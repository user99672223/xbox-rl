/*
 * proc.c - guest processes inside the one Windows process (docs/decisions.md, "Guest processes").
 *
 * A guest process is a pid, a set of guest threads, an fd table, cwd/umask/ids/rlimits, signal dispositions,
 * a view of the memory map (its mm) and a wait status. All of them live in this OS process and its one
 * address space. This file holds the process table, fork/vfork/clone-as-process, exit and wait, kill, process
 * groups and sessions, stop/continue and the controlling-tty stub. The per-process state itself is reached
 * through the calling thread (xrl_cur()->proc) by fd.c, vfs.c, signal.c and procfs.c, so their interfaces did
 * not change.
 *
 * fork() runs the child first and keeps the parent suspended in its fork call until the child execs or exits
 * ("vfork semantics"); a fork() without CLONE_VM additionally snapshots the parent's writable private memory
 * and puts it back at that point, so the parent sees what a real fork would show it. vfork()/posix_spawn
 * (CLONE_VM) keep the memory shared.
 */
#include "xrl_internal.h"

static SRWLOCK g_proc_lock = SRWLOCK_INIT;
static xrl_proc *g_procs;          /* every process, zombies included, newest first */
static int g_tty_pgrp;             /* foreground process group of the (one) terminal stand-in */

static u64 fork_snapshot_cap(void)
{
    static u64 cap = ~0ull;
    if (cap == ~0ull) {
        char v[16];
        cap = (GetEnvironmentVariableA("XRL_FORK_SNAPSHOT_MB", v, sizeof v) ? strtoull(v, NULL, 10) : 256ull) << 20;
    }
    return cap;
}

/* ------------------------------------------------------------------ mm */
static xrl_mm *mm_new(void)
{
    xrl_mm *m = (xrl_mm *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->id = xrl_mm_new();
    m->refs = 1;
    return m;
}
static void mm_ref(xrl_mm *m) { InterlockedIncrement(&m->refs); }
static void mm_free(xrl_mm *m) { xrl_mm_release(m->id); free(m->grp); free(m); }
static void mm_put(xrl_mm *m)
{
    if (m && InterlockedDecrement(&m->refs) == 0) mm_free(m);
}

/* -------------------------------------------------------- string vectors */
static char **strv_dup(char **v, int n)
{
    char **o = (char **)calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; o && i < n; i++) o[i] = _strdup(v[i]);
    return o;
}
static void strv_free(char **v, int n)
{
    if (!v) return;
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void rlim_defaults(lx_rlimit *r)
{
    for (int i = 0; i < 16; i++) r[i].rlim_cur = r[i].rlim_max = LX_RLIM_INFINITY;
    r[LX_RLIMIT_STACK].rlim_cur = 8u << 20;
    r[LX_RLIMIT_NOFILE].rlim_cur = r[LX_RLIMIT_NOFILE].rlim_max = 65536;
    r[LX_RLIMIT_CORE].rlim_cur = 0;
    r[LX_RLIMIT_NPROC].rlim_cur = r[LX_RLIMIT_NPROC].rlim_max = 32768;
    r[LX_RLIMIT_MEMLOCK].rlim_cur = r[LX_RLIMIT_MEMLOCK].rlim_max = 8u << 20;   /* the 5.16+ default */
    r[11].rlim_cur = r[11].rlim_max = 32768;     /* RLIMIT_SIGPENDING */
    r[12].rlim_cur = r[12].rlim_max = 819200;    /* RLIMIT_MSGQUEUE */
    r[13].rlim_cur = r[13].rlim_max = 0;         /* RLIMIT_NICE */
    r[14].rlim_cur = r[14].rlim_max = 0;         /* RLIMIT_RTPRIO */
}

static xrl_proc *proc_alloc(void)
{
    xrl_proc *p = (xrl_proc *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->refs = 1;
    p->thaw_ev = CreateEventW(NULL, TRUE, TRUE, NULL);
    p->start_ns = xrl_mono_ns();
    return p;
}

static void proc_free(xrl_proc *p)
{
    if (p->thaw_ev) CloseHandle(p->thaw_ev);
    strv_free(p->argv, p->argc);
    strv_free(p->envp, p->envc);
    free(p->exec_path);
    free(p);
}

void xrl_proc_ref(xrl_proc *p) { InterlockedIncrement(&p->refs); }
void xrl_proc_put(xrl_proc *p)
{
    if (!p) return;
    LONG n = InterlockedDecrement(&p->refs);
    if (n < 0) LOGE("proc %d: reference count went to %ld (a reference was dropped twice)", p->pid, n);
    if (n == 0) proc_free(p);
}

xrl_proc *xrl_proc_init_get(void) { return G.init_proc; }

xrl_proc *xrl_curproc(void)
{
    xrl_thread *t = xrl_cur();
    return t && t->proc ? t->proc : G.init_proc;
}

void xrl_proc_init(void)
{
    xrl_proc *p = proc_alloc();
    p->pid = G.pid;
    p->ppid = 1;
    p->pgid = p->sid = p->pid;
    p->is_init = 1;
    p->state = XP_RUNNING;
    p->exit_signal = LX_SIGCHLD;
    p->files = xrl_files_new();
    p->mm = mm_new();
    strcpy_s(p->cwd, sizeof p->cwd, "/");
    p->umask = 022;
    p->uid = p->euid = p->suid = xrl_env_uid(); p->gid = p->egid = p->sgid = xrl_env_gid();
    rlim_defaults(p->rlim);
    p->argc = G.argc; p->argv = strv_dup(G.argv, G.argc);
    p->envc = G.envc; p->envp = strv_dup(G.envp, G.envc);
    p->exec_path = _strdup(G.exec_path ? G.exec_path : "");
    G.init_proc = p;
    g_procs = p;
    g_tty_pgrp = p->pgid;
}

xrl_proc *xrl_proc_by_pid(int pid)
{
    xrl_proc *r = NULL;
    AcquireSRWLockShared(&g_proc_lock);
    for (xrl_proc *p = g_procs; p; p = p->next) if (p->pid == pid) { r = p; xrl_proc_ref(p); break; }
    ReleaseSRWLockShared(&g_proc_lock);
    return r;
}

int xrl_proc_list(int *out, int cap)
{
    int n = 0;
    AcquireSRWLockShared(&g_proc_lock);
    for (xrl_proc *p = g_procs; p && n < cap; p = p->next) out[n++] = p->pid;
    ReleaseSRWLockShared(&g_proc_lock);
    return n;
}

/* comm of a process: the name of its main thread (prctl(PR_SET_NAME), or the program's basename after exec) */
void xrl_proc_comm(xrl_proc *p, char out[16])
{
    xrl_thread *best = NULL;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t; t = t->next)
        if (t->proc == p && (!best || t->is_main || t->leader)) best = t;
    if (best) memcpy(out, best->name, 16);
    ReleaseSRWLockShared(&G.thread_lock);
    if (!best || !strcmp(out, "xrl-guest")) {   /* never named: the program's basename */
        const char *b = p->exec_path ? strrchr(p->exec_path, '/') : NULL;
        memset(out, 0, 16);
        strncpy_s(out, 16, b ? b + 1 : (p->exec_path ? p->exec_path : ""), _TRUNCATE);
    }
    out[15] = 0;
}

/* the child's copy of the parent's per-process state (clone flags decide what is shared) */
static xrl_proc *proc_dup(xrl_proc *pp, u64 flags)
{
    xrl_proc *c = proc_alloc();
    if (!c) return NULL;
    c->state = XP_RUNNING;
    c->exit_signal = (int)(flags & 0xff);
    c->pgid = pp->pgid; c->sid = pp->sid;
    if (flags & LX_CLONE_FILES) { c->files = pp->files; xrl_files_ref(c->files); }
    else c->files = xrl_files_clone(pp->files);
    c->mm = pp->mm;
    mm_ref(c->mm);
    memcpy(c->cwd, pp->cwd, sizeof c->cwd);
    c->umask = pp->umask;
    c->uid = pp->uid; c->euid = pp->euid; c->suid = pp->suid; c->gid = pp->gid; c->egid = pp->egid; c->sgid = pp->sgid;
    memcpy(c->rlim, pp->rlim, sizeof c->rlim);
    memcpy(c->act, pp->act, sizeof c->act);
    c->argc = pp->argc; c->argv = strv_dup(pp->argv, pp->argc);
    c->envc = pp->envc; c->envp = strv_dup(pp->envp, pp->envc);
    c->exec_path = _strdup(pp->exec_path ? pp->exec_path : "");
    if (!c->files) { proc_free(c); return NULL; }
    return c;
}

/* ----------------------------------------------------------- fork groups
 * A fork() child that does not exec runs at the same addresses as its parent. Processes that share an address
 * this way form a group: exactly one of them (the holder) runs guest code at a time, and the memory in the
 * address space is the image of one member. A member about to block in the layer (wait4, a pipe read, a sleep,
 * a futex) lets go of the baton (xrl_swap_block); when its wait ends it takes the baton back (xrl_swap_resume),
 * and if another member's image is in memory by then, that image is snapshotted into the member and this one's
 * is restored (the same snapshot code fork() uses). So a forked subshell and its parent both make progress
 * while each sees its own memory: `(sleep 1; echo x) & echo y` prints y first, as a real fork does. A member
 * that execs or exits leaves the group. Only single-threaded members take part (a thread that blocks cannot
 * stop its siblings); a multi-threaded parent keeps the child-runs-to-completion schedule. */
typedef struct swapgrp {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    xrl_proc *holder;     /* runs guest code now; NULL: nobody (everyone is blocked or waiting) */
    xrl_proc *image;      /* whose memory is in the address space */
    int image_valid;      /* 0: that process left, the memory is leftovers */
} swapgrp;

static int swap_enabled(void)
{
    static int on = -1;
    if (on < 0) { char v[4]; on = !(GetEnvironmentVariableA("XRL_FORK_SWAP", v, sizeof v) && v[0] == '0'); }
    return on;
}

static void swap_in(swapgrp *g, xrl_proc *p)   /* p is the holder; the lock is not held (copying takes milliseconds) */
{
    if (g->image == p) return;
    xrl_proc *old = g->image;
    if (old && g->image_valid && p->mm) {
        xrl_msnap *s = xrl_mem_snapshot(p->mm->id, fork_snapshot_cap());
        if (s) { xrl_mem_snap_free(old->saved); old->saved = s; }
        else LOGE("fork group: cannot save the memory of process %d (over XRL_FORK_SNAPSHOT_MB or out of memory); it will see process %d's data", old->pid, p->pid);
    }
    if (p->saved) { xrl_mem_snap_restore(p->saved); xrl_mem_snap_free(p->saved); p->saved = NULL; }
    g->image = p;
    g->image_valid = 1;
}

static void swap_acquire(swapgrp *g, xrl_proc *p, xrl_thread *t)
{
    AcquireSRWLockExclusive(&g->lock);
    while (g->holder && g->holder != p) {
        if (p->dying || (t && t->exec_kill)) break;   /* being killed: do not wait for anybody */
        SleepConditionVariableSRW(&g->cv, &g->lock, 50, 0);
    }
    if (!g->holder) g->holder = p;
    int mine = g->holder == p;
    ReleaseSRWLockExclusive(&g->lock);
    if (mine) swap_in(g, p);
}

int xrl_swap_block(void)
{
    xrl_thread *t = xrl_cur();
    if (!t || !t->proc || !t->proc->grp) return 0;
    xrl_proc *p = t->proc;
    swapgrp *g = p->grp;
    if (p->nthreads - p->ndaemons != 1) return 0;
    int rel;
    AcquireSRWLockExclusive(&g->lock);
    rel = g->holder == p;
    if (rel) { g->holder = NULL; WakeAllConditionVariable(&g->cv); }
    ReleaseSRWLockExclusive(&g->lock);
    return rel;
}

void xrl_swap_resume(void)
{
    xrl_thread *t = xrl_cur();
    if (t && t->proc && t->proc->grp) swap_acquire(t->proc->grp, t->proc, t);
}

/* p ends or execs: it leaves the group. */
static void swap_leave(xrl_proc *p)
{
    swapgrp *g = p->grp;
    if (!g) return;
    AcquireSRWLockExclusive(&g->lock);
    if (g->holder == p) g->holder = NULL;
    if (g->image == p) g->image_valid = 0;
    p->grp = NULL;
    WakeAllConditionVariable(&g->cv);
    ReleaseSRWLockExclusive(&g->lock);
    xrl_mem_snap_free(p->saved);
    p->saved = NULL;
}

/* ------------------------------------------------------- fork / clone */
/* Another process (flags have no CLONE_THREAD): see the file comment. */
slong xrl_proc_clone(u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls, u64 resume_pc)
{
    xrl_thread *p = xrl_cur();
    if (!p || !p->proc) return -LX_ENOSYS;
    if (flags & LX_CLONE_NEW_MASK) {   /* no namespaces: what an unprivileged process gets */
        static volatile LONG warned;
        if (!InterlockedExchange(&warned, 1)) LOGW("clone: namespace flags 0x%llx refused (-EPERM)", flags & LX_CLONE_NEW_MASK);
        return -LX_EPERM;
    }
    xrl_proc *pp = p->proc;
    if (flags & (LX_CLONE_SIGHAND | LX_CLONE_THREAD) && !(flags & LX_CLONE_VM)) return -LX_EINVAL;
    int share_mm = (flags & LX_CLONE_VM) != 0;
    int vfork = (flags & LX_CLONE_VFORK) != 0;
    int snapshot_fork = !share_mm;
    int sync = vfork || snapshot_fork;   /* the parent waits for the child to exec or exit */

    xrl_proc *np = proc_dup(pp, flags);
    if (!np) return -LX_ENOMEM;
    xrl_thread *t = NULL;
    slong r = xrl_thread_clone_prepare(p, np, flags, child_stack, tls, resume_pc, &t);
    if (r) { xrl_files_put(np->files); mm_put(np->mm); proc_free(np); return r; }
    xrl_heapcheck("clone: prepared");
    np->pid = t->tid;
    t->leader = 1;
    np->parent = (flags & LX_CLONE_PARENT) && pp->parent ? pp->parent : pp;
    np->ppid = np->parent->pid;

    int frozen = 0;
    swapgrp *grp = NULL;
    if (sync) {
        np->vfork_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        np->vfork_parent = p;
        /* the frames the parent returns through (the layer's, and rawsys's when the syscall was a rewritten
         * `syscall` instruction) lie below the child's initial rsp: the child's calls would overwrite them */
        u64 fr = p->frame, len = 64, rb = 0;
        if (!xrl_cfu(&rb, fr + 0x28, 8) && rb > fr && rb - fr < 2000) len = rb + 16 - fr;
        np->vfork_len = (size_t)len;
        xrl_cfu(np->vfork_frame, fr, (size_t)len);
        if (snapshot_fork) {
            int single = pp->nthreads - pp->ndaemons == 1;
            if (!single) { xrl_proc_freeze(pp, p); frozen = 1; }
            LARGE_INTEGER q0, q1, f;
            QueryPerformanceCounter(&q0);
            np->snap = xrl_mem_snapshot(pp->mm->id, fork_snapshot_cap());
            QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&f);
            if (np->snap)
                LOGI("fork: pid %d -> child %d, snapshot of the parent's writable memory taken in %.2f ms", pp->pid, np->pid,
                     (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart);
            else
                LOGW("fork: pid %d -> child %d: writable memory above XRL_FORK_SNAPSHOT_MB or out of memory; the child shares the "
                     "parent's memory (vfork semantics, what the child writes the parent sees)", pp->pid, np->pid);
            if (np->snap && single && swap_enabled()) {   /* a fork group: the parent's image is saved, the child holds the baton */
                if (!pp->mm->grp) {
                    swapgrp *ng = (swapgrp *)calloc(1, sizeof *ng);
                    InitializeSRWLock(&ng->lock);
                    InitializeConditionVariable(&ng->cv);
                    pp->mm->grp = ng;
                }
                grp = pp->mm->grp;
                AcquireSRWLockExclusive(&grp->lock);
                if (!pp->grp) { pp->grp = grp; if (!grp->holder) { grp->holder = pp; grp->image = pp; grp->image_valid = 1; } }
                np->grp = grp;
                xrl_mem_snap_free(pp->saved);
                pp->saved = np->snap; np->snap = NULL;
                grp->holder = np; grp->image = np; grp->image_valid = 1;
                ReleaseSRWLockExclusive(&grp->lock);
                CloseHandle(np->vfork_ev); np->vfork_ev = NULL; np->vfork_parent = NULL;
            }
        }
    }
    if (flags & LX_CLONE_CHILD_SETTID) xrl_ctu(ctid, &t->tid, 4);
    AcquireSRWLockExclusive(&g_proc_lock);
    np->next = g_procs; g_procs = np;
    ReleaseSRWLockExclusive(&g_proc_lock);
    LOGI("%s: pid %d -> new process %d (flags 0x%llx%s)", vfork ? "vfork" : snapshot_fork ? "fork" : "clone", pp->pid, np->pid, flags,
         share_mm ? ", shared mm" : "");
    xrl_heapcheck("clone: before start");
    xrl_thread_start(t);
    if (grp) {
        /* until the child execs, exits or blocks, the parent has nothing to run on: it takes the baton back then
         * (and its own memory, if the child's image is in the address space) */
        swap_acquire(grp, pp, p);
        xrl_heapcheck("clone: parent resumed");
    } else if (sync) {
        HANDLE ev = np->vfork_ev;
        WaitForSingleObject(ev, INFINITE);
        AcquireSRWLockExclusive(&g_proc_lock);   /* the child's release has finished with the event */
        ReleaseSRWLockExclusive(&g_proc_lock);
        CloseHandle(ev);
        np->vfork_ev = NULL;
        if (np->snap) { xrl_mem_snap_restore(np->snap); xrl_mem_snap_free(np->snap); np->snap = NULL; }
        xrl_ctu(p->frame, np->vfork_frame, np->vfork_len);
        if (frozen) xrl_proc_thaw(pp);
    }
    if (flags & LX_CLONE_PARENT_SETTID) xrl_ctu(ptid, &np->pid, 4);
    return np->pid;
}

/* The child left the parent's memory (it exec'd into a new mm, or it ended): the parent's fork call returns. */
void xrl_proc_vfork_release(xrl_proc *p)
{
    AcquireSRWLockExclusive(&g_proc_lock);
    if (p->vfork_parent) {
        p->vfork_parent = NULL;
        SetEvent(p->vfork_ev);
    }
    ReleaseSRWLockExclusive(&g_proc_lock);
}

void xrl_proc_exec_done(xrl_proc *p) { xrl_proc_vfork_release(p); }

/* execve without a kept image: a process that shares its mm (vfork/fork child, CLONE_VM) leaves it as it is and
 * takes a fresh one; the only user releases the old image in place (the mm id, and with it the brk window, stays) */
void xrl_proc_exec_mm(xrl_proc *p)
{
    xrl_mm *old = p->mm;
    xrl_shm_proc_gone(p->pid);   /* userland: exec detaches shm */
    swap_leave(p);
    if (old->refs > 1) {
        p->mm = mm_new();
        mm_put(old);
    } else xrl_mm_release(old->id);
}

/* ------------------------------------------------- stopping other threads */
int xrl_thread_listed(xrl_thread *t)
{
    int r = 0;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *x = G.threads; x; x = x->next) if (x == t) { r = 1; break; }
    ReleaseSRWLockShared(&G.thread_lock);
    return r;
}

static int proc_threads(xrl_proc *p, xrl_thread *self, xrl_thread **out, int cap, int keep_daemons)
{
    int n = 0;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t && n < cap; t = t->next)
        if (t->proc == p && t != self && !(keep_daemons && t->daemon)) out[n++] = t;
    ReleaseSRWLockShared(&G.thread_lock);
    return n;
}

/* Every other thread of p ends: cooperatively at its next syscall return (exec_kill), and a thread that is
 * running guest code is suspended and terminated (exec.c's rule: rip in executable and rsp in writable guest
 * memory means no layer lock can be held). */
void xrl_proc_kill_threads(xrl_proc *p, xrl_thread *self, int keep_daemons)
{
    xrl_thread *tg[512];
    int n = proc_threads(p, self, tg, 512, keep_daemons);
    for (int i = 0; i < n; i++) InterlockedExchange(&tg[i]->exec_kill, 1);
    InterlockedExchange(&p->frozen, 0);
    SetEvent(p->thaw_ev);
    ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < n; i++) {
            xrl_thread *t = tg[i];
            if (!t || !xrl_thread_listed(t)) continue;
            alive++;
            if (t->sig_ev) SetEvent(t->sig_ev);
            xrl_futex_interrupt(t);
            if (t->suspended) { if (t->handle) { TerminateThread(t->handle, 0); WaitForSingleObject(t->handle, 1000); } xrl_thread_reap(t); alive--; continue; }
            if (!t->in_sys && t->handle && SuspendThread(t->handle) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
                int guest = !t->in_sys && GetThreadContext(t->handle, &c) && xrl_mem_guest_ctx(c.Rip, c.Rsp) == 1;
                if (guest) { TerminateThread(t->handle, 0); WaitForSingleObject(t->handle, 1000); xrl_thread_reap(t); alive--; }
                else ResumeThread(t->handle);
            }
        }
        xrl_io_notify();
        if (!alive) return;
        if (GetTickCount64() > deadline) {
            for (int i = 0; i < n; i++)
                if (tg[i] && xrl_thread_listed(tg[i])) {
                    LOGW("proc %d: thread %d did not stop in 5 s; terminating it", p->pid, tg[i]->tid);
                    TerminateThread(tg[i]->handle, 0); WaitForSingleObject(tg[i]->handle, 1000); xrl_thread_reap(tg[i]);
                }
            return;
        }
        Sleep(2);
    }
}

/* Freeze every thread of p but `self`: a thread in a syscall parks when it returns (xrl_proc_park), a thread
 * in guest code is suspended where it is. */
void xrl_proc_freeze(xrl_proc *p, xrl_thread *self)
{
    xrl_thread *tg[512];
    ResetEvent(p->thaw_ev);
    InterlockedExchange(&p->frozen, 1);
    int n = proc_threads(p, self, tg, 512, 0);
    ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        int pending = 0;
        for (int i = 0; i < n; i++) {
            xrl_thread *t = tg[i];
            if (t->suspended || !xrl_thread_listed(t)) continue;
            if (t->in_sys) continue;   /* parks at its syscall return */
            if (t->handle && SuspendThread(t->handle) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
                int guest = !t->in_sys && GetThreadContext(t->handle, &c) && xrl_mem_guest_ctx(c.Rip, c.Rsp) == 1;
                if (guest) InterlockedExchange(&t->suspended, 1);
                else { ResumeThread(t->handle); pending++; }
            } else pending++;
        }
        if (!pending || GetTickCount64() > deadline) break;
        Sleep(1);
    }
}

void xrl_proc_thaw(xrl_proc *p)
{
    xrl_thread *tg[512];
    int n = proc_threads(p, NULL, tg, 512, 0);
    for (int i = 0; i < n; i++)
        if (InterlockedExchange(&tg[i]->suspended, 0) && tg[i]->handle) ResumeThread(tg[i]->handle);
    InterlockedExchange(&p->frozen, 0);
    SetEvent(p->thaw_ev);
}

/* syscall return of a thread whose process is frozen / stopped */
void xrl_proc_park(xrl_thread *t)
{
    xrl_proc *p = t->proc;
    if (!p->frozen || p->dying || t->exec_kill) return;
    int rel = xrl_swap_block();   /* a stopped member lets its fork group run: Linux never holds a parent for a stopped child */
    while (p->frozen && !p->dying && !t->exec_kill) WaitForSingleObject(p->thaw_ev, 20);
    if (rel) xrl_swap_resume();
}

/* ------------------------------------------------------------------ exit */
/* the nearest ancestor that is a subreaper, else init */
static xrl_proc *reaper_for(xrl_proc *p)
{
    for (xrl_proc *a = p->parent; a; a = a->parent) if (a->subreaper && a->state != XP_ZOMBIE) return a;
    return G.init_proc;
}

static void reparent_children(xrl_proc *p, int *sig_to, int *nsig, int cap)
{
    xrl_proc *reaper = reaper_for(p);
    for (xrl_proc *c = g_procs; c; c = c->next) {
        if (c->parent != p) continue;
        c->parent = reaper;
        c->ppid = reaper->pid;
        if (c->pdeathsig && c->state != XP_ZOMBIE && *nsig < cap) sig_to[(*nsig)++] = c->pid;
        if (c->state == XP_ZOMBIE) xrl_proc_signal(reaper, LX_SIGCHLD, 0);
    }
}

/* Remove a zombie nobody will wait for (g_proc_lock held). */
static void proc_unlink(xrl_proc *p)
{
    xrl_proc **pp = &g_procs;
    while (*pp && *pp != p) pp = &(*pp)->next;
    if (*pp) *pp = p->next;
}

/* End process p (called by the process itself, `self` = its calling thread, or by whoever kills it, self NULL
 * or another process's thread). The status is a wait4 status: (exit code << 8) or the fatal signal. */
void xrl_proc_exit(xrl_proc *p, int wstatus, xrl_thread *self)
{
    if (InterlockedExchange(&p->dying, 1)) return;
    xrl_proc_ref(p);   /* the parent may reap us the moment we are a zombie */
    xrl_heapcheck("exit: start");
    xrl_proc_kill_threads(p, self && self->proc == p ? self : NULL, 0);
    xrl_heapcheck("exit: threads killed");
    xrl_files *files = p->files;
    p->files = NULL;
    xrl_files_put(files);
    xrl_shm_proc_gone(p->pid);   /* userland: its shm attaches and shared mappings go */
    xrl_heapcheck("exit: files closed");
    swap_leave(p);
    xrl_heapcheck("exit: left the group");   /* before the mm (and the fork group it owns) can go */
    xrl_mm *mm = p->mm;
    p->mm = NULL;
    int pd[64], npd = 0;
    AcquireSRWLockExclusive(&g_proc_lock);
    reparent_children(p, pd, &npd, 64);
    p->wstatus = wstatus;
    p->state = XP_ZOMBIE;
    xrl_proc *par = p->parent;
    if (par) xrl_proc_ref(par);   /* it may end (and be reaped) before we have told it: a shell that wait()s returns as soon as we are a zombie */
    int reap = 0;
    if (par && par->state != XP_ZOMBIE) {
        const lx_ksigaction *a = &par->act[LX_SIGCHLD];
        if (a->handler == LX_SIG_IGN || (a->flags & LX_SA_NOCLDWAIT)) reap = 1;
    } else if (!par) reap = 1;
    if (reap) proc_unlink(p);
    ReleaseSRWLockExclusive(&g_proc_lock);
    LOGI("proc %d ended (wait status 0x%x)%s", p->pid, wstatus, reap ? ", reaped (SIGCHLD ignored)" : "");
    xrl_heapcheck("exit: zombie");
    if (mm && InterlockedDecrement(&mm->refs) == 0) mm_free(mm);
    xrl_heapcheck("exit: mm");
    for (int i = 0; i < npd; i++) {   /* children that asked to be told when their parent ends */
        xrl_proc *c = xrl_proc_by_pid(pd[i]);
        if (c) { if (c->pdeathsig) xrl_proc_signal(c, c->pdeathsig, 0); xrl_proc_put(c); }
    }
    xrl_proc_vfork_release(p);
    if (par && !reap) xrl_proc_signal(par, p->exit_signal ? p->exit_signal : LX_SIGCHLD, 0);
    xrl_proc_put(par);
    xrl_io_notify();
    xrl_heapcheck("exit: signals");
    if (reap) xrl_proc_put(p);   /* the table's reference; the thread records hold theirs until they are unlinked */
    xrl_proc_put(p);
    xrl_heapcheck("exit: end");
}

/* ---------------------------------------------------------------- signals */
static int sig_stops(int sig) { return sig == LX_SIGSTOP || sig == LX_SIGTSTP || sig == LX_SIGTTIN || sig == LX_SIGTTOU; }

static void proc_stop(xrl_proc *p, int sig)
{
    AcquireSRWLockExclusive(&g_proc_lock);
    if (p->state != XP_RUNNING) { ReleaseSRWLockExclusive(&g_proc_lock); return; }
    p->state = XP_STOPPED; p->stop_sig = sig; p->stop_reported = 0; p->cont_reported = 1;
    xrl_proc *par = p->parent;
    if (par) xrl_proc_ref(par);   /* it may end before we have told it */
    ReleaseSRWLockExclusive(&g_proc_lock);
    xrl_thread *self = xrl_cur();
    xrl_proc_freeze(p, self && self->proc == p ? NULL : NULL);
    if (par && !(par->act[LX_SIGCHLD].flags & LX_SA_NOCLDSTOP)) xrl_proc_signal(par, LX_SIGCHLD, 0);
    xrl_proc_put(par);
    xrl_io_notify();
}

static void proc_continue(xrl_proc *p)
{
    AcquireSRWLockExclusive(&g_proc_lock);
    if (p->state != XP_STOPPED) { ReleaseSRWLockExclusive(&g_proc_lock); return; }
    p->state = XP_RUNNING; p->cont_reported = 0;
    xrl_proc *par = p->parent;
    if (par) xrl_proc_ref(par);   /* it may end before we have told it */
    ReleaseSRWLockExclusive(&g_proc_lock);
    xrl_proc_thaw(p);
    if (par && !(par->act[LX_SIGCHLD].flags & LX_SA_NOCLDSTOP)) xrl_proc_signal(par, LX_SIGCHLD, 0);
    xrl_proc_put(par);
    xrl_io_notify();
}

slong xrl_proc_fatal_signal(xrl_proc *p, int sig, xrl_thread *self)
{
    if (self && self->proc == p) { xrl_do_exit_sig(sig); return 0; }
    if (p->is_init) { xrl_layer_request_exit(128 + sig); return 0; }
    xrl_proc_exit(p, sig, self);
    return 0;
}

int xrl_proc_family_stopped(xrl_proc *p) { return p->state == XP_STOPPED; }

/* Signal for a whole process: a thread that does not block it, the leader first. */
static slong proc_signal(xrl_proc *p, int sig, int from_kill);
slong xrl_proc_signal(xrl_proc *p, int sig, int from_kill)
{
    xrl_proc_ref(p);   /* a thread of p may exit and p be reaped while the signal is being queued */
    slong r = proc_signal(p, sig, from_kill);
    xrl_proc_put(p);
    return r;
}

static slong proc_signal(xrl_proc *p, int sig, int from_kill)
{
    if (sig == 0) return 0;
    if (sig < 1 || sig > LX_NSIG) return -LX_EINVAL;
    if (p->state == XP_ZOMBIE) return 0;
    xrl_thread *tg[512];
    int n = proc_threads(p, NULL, tg, 512, 0);
    xrl_thread *pick = NULL, *lead = NULL;
    for (int i = 0; i < n; i++) {
        if (tg[i]->leader || tg[i]->is_main) lead = tg[i];
        if (!(tg[i]->sigmask & (1ull << (sig - 1))) && (!pick || tg[i]->leader || tg[i]->is_main)) pick = tg[i];
    }
    if (!pick) pick = lead ? lead : (n ? tg[0] : NULL);
    if (sig == LX_SIGCONT) proc_continue(p);
    if (!pick) return -LX_ESRCH;
    return xrl_sig_send_thread(pick, sig, from_kill);
}

/* stop signals with the default action stop the target's whole process */
int xrl_proc_stop_signal(xrl_proc *p, int sig)
{
    if (!sig_stops(sig)) return 0;
    proc_stop(p, sig);
    return 1;
}

/* ------------------------------------------------------------------ kill */
slong xrl_sys_kill(int pid, int sig)
{
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    xrl_proc *self = xrl_curproc();
    xrl_proc *tg[256];
    int n = 0;
    AcquireSRWLockShared(&g_proc_lock);
    for (xrl_proc *p = g_procs; p && n < 256; p = p->next) {
        int m;
        if (pid > 0) m = p->pid == pid;
        else if (pid == 0) m = p->pgid == self->pgid && p->state != XP_ZOMBIE;
        else if (pid == -1) m = p != self && !p->is_init && p->state != XP_ZOMBIE;
        else m = p->pgid == -pid && p->state != XP_ZOMBIE;
        if (m) { xrl_proc_ref(p); tg[n++] = p; }
    }
    ReleaseSRWLockShared(&g_proc_lock);
    if (!n) return -LX_ESRCH;
    slong res = 0;
    for (int i = 0; i < n; i++) {
        slong r = xrl_proc_signal(tg[i], sig, 1);
        if (r) res = r;
        xrl_proc_put(tg[i]);
    }
    return res;
}

/* ------------------------------------------------------------------ wait */
static int status_for(xrl_proc *c, int options, int *out, int consume, int *code)
{
    if (c->state == XP_ZOMBIE) {
        *out = c->wstatus;
        *code = (c->wstatus & 0x7f) ? LX_CLD_KILLED : LX_CLD_EXITED;
        return 1;
    }
    if (c->state == XP_STOPPED && (options & LX_WSTOPPED) && !c->stop_reported) {
        *out = 0x7f | (c->stop_sig << 8);
        if (consume) c->stop_reported = 1;
        *code = LX_CLD_STOPPED;
        return 1;
    }
    if (c->state == XP_RUNNING && (options & LX_WCONTINUED) && !c->cont_reported) {
        *out = 0xffff;
        if (consume) c->cont_reported = 1;
        *code = LX_CLD_CONTINUED;
        return 1;
    }
    return 0;
}

/* Find a child to report. idtype: 0 any, 1 pid, 2 pgid (the wait4 forms map onto these). Returns the pid
 * (consumed unless WNOWAIT), 0 when children exist but none is ready (WNOHANG), -ECHILD, -EINTR. */
static slong wait_core(int idtype, int id, int options, int *status, int *code, int *uid_out)
{
    xrl_proc *me = xrl_curproc();
    for (;;) {
        LONG seen = xrl_io_epoch;
        xrl_proc *found = NULL;
        int any = 0, st = 0, cd = 0;
        AcquireSRWLockExclusive(&g_proc_lock);
        for (xrl_proc *c = g_procs; c; c = c->next) {
            if (c->parent != me) continue;
            if (idtype == LX_P_PID && c->pid != id) continue;
            if (idtype == LX_P_PGID && c->pgid != (id ? id : me->pgid)) continue;
            any = 1;
            if (status_for(c, options, &st, !(options & LX_WNOWAIT), &cd)) { found = c; break; }
        }
        slong res = 0;
        if (found) {
            res = found->pid;
            *status = st; *code = cd; *uid_out = (int)found->uid;
            if (found->state == XP_ZOMBIE && !(options & LX_WNOWAIT)) { proc_unlink(found); }
            else found = NULL;
        }
        ReleaseSRWLockExclusive(&g_proc_lock);
        if (res) {
            if (found) xrl_proc_put(found);   /* reaped: the table's reference goes; freed with its last thread record */
            xrl_heapcheck("wait: reaped");
            return res;
        }
        if (!any) return -LX_ECHILD;
        if (options & LX_WNOHANG) return 0;
        if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
    }
}

slong xrl_sys_wait4(int pid, u64 ustatus, int options, u64 urusage)
{
    if (options & ~(LX_WNOHANG | LX_WUNTRACED | LX_WCONTINUED | LX_WALL | LX_WCLONE | LX_WNOTHREAD)) return -LX_EINVAL;
    int idtype = LX_P_ALL, id = 0;
    if (pid > 0) { idtype = LX_P_PID; id = pid; }
    else if (pid == 0) { idtype = LX_P_PGID; id = 0; }
    else if (pid < -1) { idtype = LX_P_PGID; id = -pid; }
    int st = 0, cd = 0, uid = 0;
    slong r = wait_core(idtype, id, (options & (LX_WUNTRACED | LX_WNOHANG | LX_WCONTINUED)) | LX_WEXITED, &st, &cd, &uid);
    if (r <= 0) return r;
    if (ustatus && xrl_ctu(ustatus, &st, 4)) return -LX_EFAULT;
    if (urusage) { lx_rusage ru; memset(&ru, 0, sizeof ru); if (xrl_ctu(urusage, &ru, sizeof ru)) return -LX_EFAULT; }
    return r;
}

slong xrl_sys_waitid(int idtype, int id, u64 uinfo, int options, u64 urusage)
{
    if (idtype != LX_P_ALL && idtype != LX_P_PID && idtype != LX_P_PGID) return -LX_EINVAL;
    if (!(options & (LX_WEXITED | LX_WSTOPPED | LX_WCONTINUED))) return -LX_EINVAL;
    int st = 0, cd = 0, uid = 0;
    int want = options & (LX_WNOHANG | LX_WNOWAIT | LX_WSTOPPED | LX_WCONTINUED);
    slong r = wait_core(idtype, id, want | (options & LX_WEXITED ? LX_WEXITED : 0), &st, &cd, &uid);
    if (r < 0) return r;
    u8 si[128];
    memset(si, 0, sizeof si);
    if (r > 0) {
        *(s32 *)(si + 0) = LX_SIGCHLD;
        *(s32 *)(si + 8) = cd;
        *(s32 *)(si + 16) = (s32)r;
        *(u32 *)(si + 20) = (u32)uid;
        *(s32 *)(si + 24) = cd == LX_CLD_EXITED ? (st >> 8) & 0xff : cd == LX_CLD_CONTINUED ? LX_SIGCONT : (cd == LX_CLD_STOPPED ? (st >> 8) & 0xff : st & 0x7f);
    }
    if (uinfo && xrl_ctu(uinfo, si, sizeof si)) return -LX_EFAULT;
    (void)urusage;
    return 0;
}

/* ----------------------------------------------------- groups, sessions */
static xrl_proc *proc_locked(int pid)
{
    for (xrl_proc *p = g_procs; p; p = p->next) if (p->pid == pid) return p;
    return NULL;
}

slong xrl_sys_setpgid(int pid, int pgid)
{
    xrl_proc *me = xrl_curproc();
    if (pgid < 0) return -LX_EINVAL;
    slong r = 0;
    AcquireSRWLockExclusive(&g_proc_lock);
    xrl_proc *t = (pid == 0 || pid == me->pid) ? me : proc_locked(pid);
    if (!t || t->state == XP_ZOMBIE) r = -LX_ESRCH;
    else if (t != me && t->parent != me) r = -LX_ESRCH;
    else if (t->sid != me->sid) r = -LX_EPERM;
    else if (t->sid == t->pid) r = -LX_EPERM;       /* a session leader keeps its group */
    else {
        if (pgid == 0) pgid = t->pid;
        if (pgid != t->pid) {
            int ok = 0;
            for (xrl_proc *q = g_procs; q; q = q->next) if (q->pgid == pgid && q->sid == t->sid && q->state != XP_ZOMBIE) { ok = 1; break; }
            if (!ok) r = -LX_EPERM;
        }
        if (!r) t->pgid = pgid;
    }
    ReleaseSRWLockExclusive(&g_proc_lock);
    return r;
}

slong xrl_sys_getpgid(int pid)
{
    xrl_proc *me = xrl_curproc();
    if (pid == 0 || pid == me->pid) return me->pgid;
    xrl_proc *t = xrl_proc_by_pid(pid);
    slong r = t ? t->pgid : -LX_ESRCH;
    xrl_proc_put(t);
    return r;
}

slong xrl_sys_getsid(int pid)
{
    xrl_proc *me = xrl_curproc();
    if (pid == 0 || pid == me->pid) return me->sid;
    xrl_proc *t = xrl_proc_by_pid(pid);
    slong r = t ? t->sid : -LX_ESRCH;
    xrl_proc_put(t);
    return r;
}

slong xrl_sys_setsid(void)
{
    xrl_proc *me = xrl_curproc();
    slong r = me->pid;
    AcquireSRWLockExclusive(&g_proc_lock);
    if (me->pgid == me->pid) r = -LX_EPERM;     /* a process group leader cannot start a session */
    for (xrl_proc *q = g_procs; q && r > 0; q = q->next)
        if (q != me && q->pgid == me->pid && q->state != XP_ZOMBIE) { r = -LX_EPERM; break; }
    if (r > 0) { me->sid = me->pgid = me->pid; }
    ReleaseSRWLockExclusive(&g_proc_lock);
    return r;
}

/* the terminal stand-in: one foreground group (the init process's, until a shell moves it) */
int xrl_tty_pgrp(void) { return g_tty_pgrp; }
int xrl_tty_set_pgrp(int pgrp)
{
    xrl_proc *me = xrl_curproc();
    int ok = 0;
    AcquireSRWLockShared(&g_proc_lock);
    for (xrl_proc *q = g_procs; q; q = q->next) if (q->pgid == pgrp && q->sid == me->sid && q->state != XP_ZOMBIE) { ok = 1; break; }
    ReleaseSRWLockShared(&g_proc_lock);
    if (!ok) return -LX_EPERM;
    g_tty_pgrp = pgrp;
    return 0;
}

/* pty.c: the line discipline signals the foreground group (^C, ^Z, ^backslash, SIGWINCH, SIGHUP) */
int xrl_tty_signal(int pgrp, int sig) { return pgrp > 0 ? (int)xrl_sys_kill(-pgrp, sig) : 0; }

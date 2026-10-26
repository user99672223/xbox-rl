/*
 * exec.c - execve: the calling process image is replaced inside this OS process.
 *
 * What Linux does and what is done here
 *   Linux's execve keeps the process (pid, fds without CLOEXEC, ignored signals,
 *   cwd, umask) and replaces the program: other threads die, memory is unmapped,
 *   handlers go back to the default, the new ELF starts on a fresh stack. The
 *   layer is one OS process, so that is literally what happens: the other guest
 *   threads are stopped, the guest mappings are released, the new image is loaded
 *   by the same loader the boot uses and its main thread started; the calling
 *   thread ends. The new main thread has a new (host) thread id; getpid() stays.
 *
 * Keeping part of the old image (layer-private, docs/decisions.md "exec handover")
 *   Wine's server is a thread of the guest process (patch 0005). A Wine process
 *   that execs "the next Windows process" must not take the server with it, so:
 *     prctl(PR_XRL_DAEMON, 1)            the calling thread survives an exec
 *                                        (and does not count as "the last thread");
 *     prctl(PR_XRL_EXEC_KEEP, ranges, n) the next execve keeps the old image: only
 *                                        the n {base, len} ranges are released,
 *                                        everything else (the old musl, the server's
 *                                        heap and stack) stays mapped, the old
 *                                        thread-private state ends, CLOEXEC fds are
 *                                        left open (the survivors may own some).
 *   Without PR_XRL_EXEC_KEEP an execve is the plain Linux one.
 *
 * Stopping the other threads is cooperative first: each is flagged (exec_kill)
 * and woken; a thread inside the layer ends itself on the way out of its syscall,
 * a thread in guest code (rip in executable and rsp in writable guest memory, no
 * layer lock can be held) is suspended and terminated by us.
 */
#include "xrl_internal.h"

#define LX_PR_XRL_DAEMON    0x58524c01
#define LX_PR_XRL_EXEC_KEEP 0x58524c02

typedef struct xrange { u64 base, len; } xrange;
static SRWLOCK g_exec_lock = SRWLOCK_INIT;
static xrange *g_keep_free;
static int g_keep_n, g_keep_on;

slong xrl_exec_prctl(int op, u64 a2, u64 a3)
{
    xrl_thread *t = xrl_cur();
    switch (op) {
    case LX_PR_XRL_DAEMON:
        if (!t) return -LX_EINVAL;
        if (!!a2 != !!t->daemon) {
            t->daemon = a2 ? 1 : 0;
            if (a2) InterlockedIncrement(&G.ndaemons); else InterlockedDecrement(&G.ndaemons);
        }
        return 0;
    case LX_PR_XRL_EXEC_KEEP: {
        AcquireSRWLockExclusive(&g_exec_lock);
        free(g_keep_free); g_keep_free = NULL; g_keep_n = 0; g_keep_on = 0;
        slong r = 0;
        if (a2 || a3) {
            if (a3 > 65536) r = -LX_EINVAL;
            else if (a3) {
                g_keep_free = (xrange *)malloc((size_t)a3 * sizeof(xrange));
                if (!g_keep_free) r = -LX_ENOMEM;
                else if (xrl_cfu(g_keep_free, a2, (size_t)a3 * sizeof(xrange))) { free(g_keep_free); g_keep_free = NULL; r = -LX_EFAULT; }
                else g_keep_n = (int)a3;
            }
            if (!r) g_keep_on = 1;
        }
        ReleaseSRWLockExclusive(&g_exec_lock);
        return r;
    }
    }
    return -LX_EINVAL;
}

/* ------------------------------------------------------- argument vectors */
static void strv_free(char **v) { if (v) { for (int i = 0; v[i]; i++) free(v[i]); free(v); } }

static slong strv_from_user(u64 uv, char ***out, int *count)
{
    char **v = (char **)calloc(1, sizeof(char *));
    int n = 0;
    *out = NULL; *count = 0;
    if (!v) return -LX_ENOMEM;
    for (; uv; n++) {
        u64 p;
        if (n >= 8192) { strv_free(v); return -LX_E2BIG; }
        if (xrl_cfu(&p, uv + 8ull * (u64)n, 8)) { strv_free(v); return -LX_EFAULT; }
        if (!p) break;
        char *s = (char *)malloc(1 << 17);
        if (!s) { strv_free(v); return -LX_ENOMEM; }
        int r = xrl_strfu(s, p, 1 << 17);
        if (r < 0) { free(s); strv_free(v); return r == -LX_ENAMETOOLONG ? -LX_E2BIG : r; }
        char **nv = (char **)realloc(v, ((size_t)n + 2) * sizeof(char *));
        if (!nv) { free(s); strv_free(v); return -LX_ENOMEM; }
        v = nv; v[n] = s; v[n + 1] = NULL;
    }
    *out = v; *count = n;
    return 0;
}

/* ------------------------------------------------------- stopping threads */
static int thread_listed(xrl_thread *t)
{
    int r = 0;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *x = G.threads; x; x = x->next) if (x == t) { r = 1; break; }
    ReleaseSRWLockShared(&G.thread_lock);
    return r;
}

static void kill_others(xrl_thread *self, int keep_daemons)
{
    xrl_thread *tg[512];
    int n = 0;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t && n < 512; t = t->next)
        if (t != self && !(keep_daemons && t->daemon)) tg[n++] = t;
    ReleaseSRWLockShared(&G.thread_lock);
    for (int i = 0; i < n; i++) InterlockedExchange(&tg[i]->exec_kill, 1);
    ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < n; i++) {
            xrl_thread *t = tg[i];
            if (!t || !thread_listed(t)) continue;
            alive++;
            if (t->sig_ev) SetEvent(t->sig_ev);
            xrl_futex_interrupt(t);
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
                if (tg[i] && thread_listed(tg[i])) {
                    LOGW("exec: thread %d did not stop in 5 s; terminating it", tg[i]->tid);
                    TerminateThread(tg[i]->handle, 0); WaitForSingleObject(tg[i]->handle, 1000); xrl_thread_reap(tg[i]);
                }
            return;
        }
        Sleep(2);
    }
}

/* ----------------------------------------------------------------- execve */
slong xrl_sys_execve(u64 upath, u64 uargv, u64 uenvp)
{
    xrl_thread *self = xrl_cur();
    char path[1024], gpath[1024];
    slong r;
    if (!self) return -LX_ENOSYS;
    if ((r = xrl_strfu(path, upath, sizeof path)) < 0) return r;
    if (self->task) return xrl_task_exec(self, path, uargv, uenvp);
    if (!path[0]) return -LX_ENOENT;
    if ((r = xrl_vfs_resolve(LX_AT_FDCWD, path, 1, gpath, sizeof gpath)) < 0) return r;

    /* everything that can fail with an errno happens before anything is destroyed */
    {
        u8 hdr[64];
        slong fd = xrl_vfs_open(LX_AT_FDCWD, gpath, LX_O_RDONLY | LX_O_CLOEXEC, 0);
        if (fd < 0) return fd;
        xfile *f = xrl_fd_get((int)fd);
        slong got = f && f->ops->read ? f->ops->read(f, hdr, sizeof hdr, 0) : -LX_EACCES;
        xfile_unref(f);
        xrl_fd_close((int)fd);
        if (got < 0) return got;
        if (got < 20 || hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') return -LX_ENOEXEC;
        if (hdr[4] != 2 || *(u16 *)(hdr + 18) != 62) return -LX_ENOEXEC;   /* ELFCLASS64, EM_X86_64 */
    }
    char **av = NULL, **ev = NULL;
    int ac, ec;
    if ((r = strv_from_user(uargv, &av, &ac)) < 0) return r;
    if ((r = strv_from_user(uenvp, &ev, &ec)) < 0) { strv_free(av); return r; }
    if (!ac) {   /* Linux runs the program with an empty argv[0]; give it its path instead */
        free(av); av = (char **)calloc(2, sizeof(char *)); av[0] = _strdup(path); ac = 1;
    }
    if (!TryAcquireSRWLockExclusive(&g_exec_lock)) { strv_free(av); strv_free(ev); return -LX_EAGAIN; }
    int keep = g_keep_on;
    xrange *rg = g_keep_free; int nrg = g_keep_n;
    g_keep_free = NULL; g_keep_n = 0; g_keep_on = 0;
    ReleaseSRWLockExclusive(&g_exec_lock);

    LOGI("exec: pid %d tid %d -> %s (%d args, %d env)%s", G.pid, self->tid, gpath, ac, ec, keep ? ", old image kept" : "");

    /* ---- point of no return ---- */
    u64 before = xrl_mem_committed();
    kill_others(self, keep);
    if (keep) {
        for (int i = 0; i < nrg; i++) {
            u64 b = rg[i].base & ~(u64)(LX_PAGE - 1), e = ALIGN_UP(rg[i].base + rg[i].len, LX_PAGE);
            if (e > b) xrl_mem_munmap(b, e - b);
        }
    } else {
        xrl_mem_release_all();
        xrl_fd_close_on_exec();
    }
    free(rg);
    LOGI("exec: guest memory committed %llu MiB -> %llu MiB after releasing the old image", before >> 20, xrl_mem_committed() >> 20);
    /* signals: handlers go back to the default, SIG_IGN stays (Linux) */
    for (int s = 1; s <= LX_NSIG; s++) {
        lx_ksigaction a;
        if (s == LX_SIGKILL || s == LX_SIGSTOP) continue;
        if (xrl_sys_rt_sigaction(s, 0, (u64)(uintptr_t)&a, 8)) continue;
        if (a.handler > LX_SIG_IGN) { memset(&a, 0, sizeof a); xrl_sys_rt_sigaction(s, (u64)(uintptr_t)&a, 0, 8); }
    }
    memset(&self->altstack, 0, sizeof self->altstack);
    self->clear_child_tid = 0;

    /* the process now is the new program */
    G.argv = av; G.argc = ac; G.envp = ev; G.envc = ec;
    G.exec_path = _strdup(gpath);
    xrl_elf_image img;
    u64 slo = 0, shi = 0, sp = 0;
    xrl_thread *mt = NULL;
    if (xrl_elf_load(G.exec_path, &img) || !(sp = xrl_elf_build_stack(&img, &slo, &shi)) ||
        xrl_threads_start_main(img.entry, sp, &mt)) {
        LOGE("exec: cannot start %s after the old image was released; ending the process (SIGSEGV)", gpath);
        xrl_do_exit(128 + LX_SIGSEGV, 1);
        return 0;
    }
    mt->guest_stack_lo = slo; mt->guest_stack_hi = shi;
    LOGI("exec: %s running, main thread tid %d entry %llx", gpath, mt->tid, img.entry);
    xrl_thread_die(self);
    return 0;
}

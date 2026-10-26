/*
 * task.c - fork() and ptrace() for a child that alternates with its parent (XRL_FORK_TASKS=1).
 *
 * EXPERIMENT. The layer is one OS process; a real fork needs a second one (docs/decisions.md, process
 * model). This is the cheapest thing that lets a program's fork() child run, to observe what it asks of the
 * system: the child is another thread of this process with
 *   - its own fd table (a copy of the parent's, fd.c), pid, ppid, exit status;
 *   - the parent's memory (NOT a copy: what the child writes outside the stack, the parent sees);
 *   - the parent's stack addresses, kept apart by swapping the stack's contents at every hand-over
 *     (exactly one of the two runs at a time: the parent runs when the child exits or stops, the child
 *     runs when the parent waits for it).
 * ptrace is the part a child can use on its parent's behalf: TRACEME, signal-delivery-stop, syscall-stops
 * (PTRACE_SYSCALL with entry and exit), exec-stop, GETREGS/GETREGSET (what the layer knows of the child's
 * registers), SETOPTIONS, CONT/DETACH/KILL. A child that outlives the parent's waiting, runs concurrently
 * or whose memory writes the parent reads is not supported. A real fork replaces this.
 */
#include "xrl_internal.h"

#define SIGTRAP_ 5
#define STACK_MAX (1u << 20)

typedef struct xrl_task {
    int pid, ppid;
    HANDLE parent_go, child_go;
    u8 *snapP, *snapC;
    u64 lo_p, lo_c, hi;
    int exited, killed, reaped, sigdeath, status;
    int traced, opts, resumed, mode;          /* mode: 0 PTRACE_CONT, 1 PTRACE_SYSCALL */
    int stopped, reported, entry_stopped;
    int s_kind, s_sig;                         /* 0 signal-delivery-stop, 1 syscall entry, 2 syscall exit, 3 exec */
    slong s_nr, s_a[6], s_ret;
    u64 s_fr[7], s_rsp, s_tp;
    void *fdt;
    struct xrl_task *next;
} xrl_task;

static xrl_task *g_tasks;
static SRWLOCK g_task_lock = SRWLOCK_INIT;

int xrl_fork_tasks_enabled(void)
{
    static int on = -1;
    if (on < 0) { char v[4]; on = GetEnvironmentVariableA("XRL_FORK_TASKS", v, sizeof v) && v[0] == '1'; }
    return on;
}

int xrl_task_pid(xrl_thread *t, int *ppid)
{
    if (!t || !t->task) return 0;
    if (ppid) *ppid = t->task->ppid;
    return t->task->pid;
}

static u64 rsp_of(xrl_thread *t) { return t->frame + 56; }
static u64 lo_of(xrl_thread *t, u64 hi)
{
    u64 lo = (rsp_of(t) - 256) & ~15ull;
    return hi - lo > STACK_MAX ? hi - STACK_MAX : lo;
}

/* ------------------------------------------------------------ hand-overs */
static void child_yield(xrl_task *k, xrl_thread *t)
{
    k->lo_c = lo_of(t, k->hi);
    xrl_cfu(k->snapC, k->lo_c, (size_t)(k->hi - k->lo_c));
    xrl_ctu(k->lo_p, k->snapP, (size_t)(k->hi - k->lo_p));
    SetEvent(k->parent_go);
    WaitForSingleObject(k->child_go, INFINITE);
    if (k->killed) xrl_thread_die(t);
}

static void to_child(xrl_task *k, xrl_thread *p)
{
    k->lo_p = lo_of(p, k->hi);
    xrl_cfu(k->snapP, k->lo_p, (size_t)(k->hi - k->lo_p));
    xrl_ctu(k->lo_c, k->snapC, (size_t)(k->hi - k->lo_c));
    SetEvent(k->child_go);
    WaitForSingleObject(k->parent_go, INFINITE);
}

static void save_regs(xrl_task *k, xrl_thread *t, int kind, int sig, slong nr, const slong *a, slong ret)
{
    k->s_kind = kind; k->s_sig = sig; k->s_nr = nr; k->s_ret = ret;
    if (a) memcpy(k->s_a, a, sizeof k->s_a); else memset(k->s_a, 0, sizeof k->s_a);
    memcpy(k->s_fr, (const void *)t->frame, sizeof k->s_fr);
    k->s_rsp = rsp_of(t); k->s_tp = t->guest_tp;
}

static void stop(xrl_task *k, xrl_thread *t, int wstatus)
{
    k->stopped = 1; k->reported = 0; k->status = wstatus;
    child_yield(k, t);
}

/* ------------------------------------------------------------------ fork */
slong xrl_do_fork(void)
{
    xrl_thread *p = xrl_cur();
    if (!p || !xrl_fork_tasks_enabled()) return -LX_ENOSYS;
    xrl_task *k = (xrl_task *)calloc(1, sizeof *k);
    if (!k) return -LX_ENOMEM;
    u64 rsp = rsp_of(p), lo = (rsp - 256) & ~15ull, hi = (lo & ~(u64)(LX_PAGE - 1)) + LX_PAGE;
    k->snapP = (u8 *)malloc(STACK_MAX); k->snapC = (u8 *)malloc(STACK_MAX);
    if (!k->snapP || !k->snapC || xrl_cfu(k->snapP, lo, (size_t)(hi - lo))) { free(k->snapP); free(k->snapC); free(k); return -LX_EFAULT; }
    while (hi - lo < STACK_MAX - LX_PAGE && !xrl_cfu(k->snapP + (hi - lo), hi, LX_PAGE)) hi += LX_PAGE;   /* up to the end of this stack mapping */
    k->lo_p = lo; k->hi = hi;
    k->parent_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    k->child_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    k->ppid = p->task ? p->task->pid : G.pid;
    k->fdt = xrl_fd_table_clone();
    xrl_thread *t = xrl_task_thread_new(p, rsp, k);
    if (!t) return -LX_EAGAIN;
    k->pid = t->tid;
    AcquireSRWLockExclusive(&g_task_lock); k->next = g_tasks; g_tasks = k; ReleaseSRWLockExclusive(&g_task_lock);
    LOGW("fork: child task %d runs on the parent's stack and memory (XRL_FORK_TASKS; not a real fork)", k->pid);
    xrl_task_thread_start(t);
    WaitForSingleObject(k->parent_go, INFINITE);
    return k->pid;
}

/* the child thread ends (thread.c: xrl_do_exit): its exit status, the parent's stack back, then the parent goes on */
void xrl_task_on_exit(xrl_thread *t, int code)
{
    xrl_task *k = t->task;
    if (!k->sigdeath && !k->exited) k->status = (code & 0xff) << 8;
    k->exited = 1;
    xrl_ctu(k->lo_p, k->snapP, (size_t)(k->hi - k->lo_p));
    xrl_fd_table_free(k->fdt);
    k->fdt = NULL;
}
void xrl_task_wake_parent(xrl_thread *t) { SetEvent(t->task->parent_go); }
void xrl_task_signal_death(xrl_thread *t, int sig) { t->task->sigdeath = 1; t->task->status = sig; }   /* killed by a signal: wait4 status = the signal */

/* ----------------------------------------------------------------- stops */
void xrl_task_syscall_stop(xrl_thread *t, slong nr, const slong *a, slong ret, int enter)
{
    xrl_task *k = t->task;
    if (!k || !k->traced) return;
    int kind;
    if (enter) { if (k->mode != 1) { k->entry_stopped = 0; return; } k->entry_stopped = 1; kind = 1; }
    else { if (!k->entry_stopped) return; k->entry_stopped = 0; kind = 2; }
    save_regs(k, t, kind, SIGTRAP_, nr, a, enter ? -LX_ENOSYS : ret);
    stop(k, t, (((k->opts & 1) ? (SIGTRAP_ | 0x80) : SIGTRAP_) << 8) | 0x7f);
}

/* a signal the task sends itself: a traced task stops with it (signal-delivery-stop); 1 = consumed */
int xrl_task_signal(xrl_thread *t, int sig)
{
    xrl_task *k = t->task;
    if (!k || !k->traced || sig == LX_SIGKILL) return 0;
    LOGW("task %d: signal %d, traced: stops", k->pid, sig);
    save_regs(k, t, 0, sig, -1, NULL, 0);
    stop(k, t, (sig << 8) | 0x7f);
    return 1;
}

static void print_arg(const char *s)   /* login codes, account names and ids must never reach a log: only these values are shown */
{
    static const char *const ok[] = { "-epicapp=", "-epicenv=", "-epiclocale=", "-epicsandboxid=", "-AUTH_LOGIN=", "WINE", "EOS", "XRL", "PROTON", "LD_", "PATH=", "HOME=", "/", "Z:" };
    char buf[200]; size_t i;
    int show = 0;
    for (size_t k = 0; k < sizeof ok / sizeof *ok; k++) if (!strncmp(s, ok[k], strlen(ok[k]))) show = 1;
    if (show) snprintf(buf, sizeof buf, "%.190s", s);
    else { const char *e = strchr(s, '='); snprintf(buf, sizeof buf, "%.*s%s", (int)(e ? e - s : 30), s, e ? "=<value not shown>" : ""); }
    for (i = 0; buf[i]; i++) if (buf[i] == '\n') buf[i] = ' ';
    LOGW("exec:   %s", buf);
}

/* execve in a task: what the child asks to run, then the stop a tracer expects; the image is not run */
slong xrl_task_exec(xrl_thread *t, const char *path, u64 uargv, u64 uenvp)
{
    xrl_task *k = t->task;
    LOGW("exec: task %d: execve(\"%s\")", k->pid, path);
    for (int pass = 0; pass < 2; pass++) {
        u64 v = pass ? uenvp : uargv; int n = 0;
        LOGW("exec: %s:", pass ? "environment (names; values of WINE*/EOS*/XRL*/PROTON*/LD_*/PATH/HOME)" : "argv");
        while (v && n < 200) {
            u64 sp; char s[1024];
            if (xrl_cfu(&sp, v + 8ull * n, 8) || !sp || xrl_strfu(s, sp, sizeof s)) break;
            if (!pass) print_arg(s);
            else {
                const char *e = strchr(s, '=');
                if (e && (!strncmp(s, "WINE", 4) || !strncmp(s, "EOS", 3) || !strncmp(s, "XRL", 3) || !strncmp(s, "PROTON", 6) || !strncmp(s, "LD_", 3) || !strncmp(s, "PATH=", 5) || !strncmp(s, "HOME=", 5))) print_arg(s);
                else LOGW("exec:   %.*s", (int)(e ? e - s : 40), s);
            }
            n++;
        }
    }
    if (k->traced) {
        save_regs(k, t, 3, SIGTRAP_, 59, NULL, 0);
        stop(k, t, ((k->opts & 0x10) ? (SIGTRAP_ | (4 << 8)) : SIGTRAP_) << 8 | 0x7f);
    }
    xrl_do_exit(0, 1);   /* the new image would run now; it cannot in this model */
    return 0;
}

/* ------------------------------------------------------------------ wait */
static xrl_task *find_child(int pid)
{
    int me = 0, pp; me = xrl_task_pid(xrl_cur(), &pp); if (!me) me = G.pid;
    xrl_task *k = NULL;
    AcquireSRWLockShared(&g_task_lock);
    for (xrl_task *c = g_tasks; c; c = c->next)
        if (c->ppid == me && !c->reaped && (pid == -1 || pid == 0 || c->pid == pid)) { k = c; break; }
    ReleaseSRWLockShared(&g_task_lock);
    return k;
}

/* wait4(pid, &status, options): a child that exited, or stopped and not yet reported. A child the parent has
 * resumed (ptrace CONT/SYSCALL/DETACH) runs now, until it next stops or exits. */
slong xrl_task_wait(int pid, u64 ustatus, int options)
{
    xrl_thread *p = xrl_cur();
    xrl_task *k = find_child(pid);
    if (!k) return -LX_ECHILD;
    for (;;) {
        if (k->exited || (k->stopped && !k->reported && k->traced)) {
            int st = k->status, r = k->pid;
            if (ustatus && xrl_ctu(ustatus, &st, 4)) return -LX_EFAULT;
            if (k->exited) { k->reaped = 1; CloseHandle(k->parent_go); CloseHandle(k->child_go); free(k->snapP); free(k->snapC); k->snapP = k->snapC = NULL; }
            else k->reported = 1;
            return r;
        }
        if (k->resumed) { k->resumed = 0; k->stopped = 0; to_child(k, p); continue; }
        if (options & 1 /* WNOHANG */) return 0;
        LOGW("wait4(%d): the child is stopped and nobody resumed it: this would wait forever", k->pid);
        return -LX_ECHILD;
    }
}

/* kill(pid, sig) for a child of the caller; 1 = it was one */
int xrl_task_kill(int pid, int sig, slong *res)
{
    int pp, me = xrl_task_pid(xrl_cur(), &pp); if (!me) me = G.pid;
    xrl_task *k = NULL;
    AcquireSRWLockShared(&g_task_lock);
    for (xrl_task *c = g_tasks; c; c = c->next) if (c->pid == pid && c->ppid == me && !c->reaped && !c->exited) { k = c; break; }
    ReleaseSRWLockShared(&g_task_lock);
    if (!k) return 0;
    *res = 0;
    if (sig == 0) return 1;
    if (sig == LX_SIGKILL) { k->killed = 1; k->exited = 1; k->status = 9; k->resumed = 0; xrl_fd_table_free(k->fdt); k->fdt = NULL; SetEvent(k->child_go); LOGW("task %d killed", k->pid); }
    else LOGW("kill(%d, %d): signal to a task that is not running is dropped", pid, sig);
    return 1;
}

/* ptrace(request, pid, addr, data) by the parent of a traced child */
static slong ptrace_impl(int req, int pid, u64 addr, u64 data);
slong xrl_task_ptrace(int req, int pid, u64 addr, u64 data)
{
    slong r = ptrace_impl(req, pid, addr, data);
    if (req != 24 && req != 0x4204) LOGW("ptrace(req 0x%x, pid %d, addr 0x%llx, data 0x%llx) = %lld", req, pid, addr, data, r);
    return r;
}
static slong ptrace_impl(int req, int pid, u64 addr, u64 data)
{
    xrl_thread *p = xrl_cur();
    if (req == 0) { if (p && p->task) p->task->traced = 1; LOGW("ptrace(PTRACE_TRACEME) by task %d", p && p->task ? p->task->pid : 0); return p && p->task ? 0 : -LX_EPERM; }
    xrl_task *k = find_child(pid);
    if (!k || !k->traced || k->exited) return -LX_ESRCH;
    if (!k->stopped) return -LX_ESRCH;   /* not in ptrace-stop */
    switch (req) {
    case 0x4200: k->opts = (int)data; return 0;                                   /* PTRACE_SETOPTIONS */
    case 7: case 24: case 9: k->mode = req == 24; k->resumed = 1; return 0;       /* CONT, SYSCALL, SINGLESTEP (as CONT) */
    case 17: k->traced = 0; k->resumed = 1; k->mode = 0; return 0;                /* DETACH */
    case 8: { slong r; xrl_task_kill(pid, LX_SIGKILL, &r); return 0; }            /* PTRACE_KILL */
    case 12: case 0x4204: {                                                       /* GETREGS, GETREGSET(NT_PRSTATUS) */
        u64 r[27]; memset(r, 0, sizeof r);
        r[0] = k->s_fr[0]; r[1] = k->s_fr[1]; r[2] = k->s_fr[2]; r[3] = k->s_fr[3];   /* r15 r14 r13 r12 */
        r[4] = k->s_fr[5]; r[5] = k->s_fr[4];                                         /* rbp rbx */
        r[9] = (u64)k->s_a[4]; r[8] = (u64)k->s_a[5]; r[7] = (u64)k->s_a[3];            /* r8 r9 r10 */
        r[10] = (u64)k->s_ret; r[12] = (u64)k->s_a[2]; r[13] = (u64)k->s_a[1]; r[14] = (u64)k->s_a[0];   /* rax rdx rsi rdi */
        r[15] = (u64)k->s_nr; r[16] = k->s_fr[6]; r[17] = 0x33; r[18] = 0x246; r[19] = k->s_rsp; r[20] = 0x2b; r[21] = k->s_tp;
        if (req == 12) return xrl_ctu(data, r, sizeof r) ? -LX_EFAULT : 0;
        u64 iov[2]; if (xrl_cfu(iov, data, 16)) return -LX_EFAULT;
        u64 n = iov[1] < sizeof r ? iov[1] : sizeof r; iov[1] = n;
        if (xrl_ctu(iov[0], r, (size_t)n) || xrl_ctu(data, iov, 16)) return -LX_EFAULT;
        return 0; }
    case 0x4202: {                                                                 /* GETSIGINFO */
        u32 si[32]; memset(si, 0, sizeof si);
        si[0] = (u32)(k->s_kind == 0 ? k->s_sig : SIGTRAP_);
        si[2] = k->s_kind == 1 || k->s_kind == 2 ? ((k->opts & 1) ? (SIGTRAP_ | 0x80) : SIGTRAP_) : k->s_kind == 3 ? ((k->opts & 0x10) ? (SIGTRAP_ | (4 << 8)) : SIGTRAP_) : (u32)-6 /* SI_TKILL */;
        if (k->s_kind == 0) { si[3] = 0; si[4] = (u32)k->ppid; si[5] = 1000; }
        return xrl_ctu(data, si, sizeof si) ? -LX_EFAULT : 0; }
    case 0x4201: { u64 m = 0; return xrl_ctu(data, &m, 8) ? -LX_EFAULT : 0; }      /* GETEVENTMSG */
    case 1: case 2: { u64 v; if (xrl_cfu(&v, addr, 8)) return -LX_EIO; return xrl_ctu(data, &v, 8) ? -LX_EFAULT : 0; }   /* PEEKTEXT/DATA (shared memory) */
    default: LOGW("ptrace request 0x%x on task %d: not supported", req, pid); return -LX_EIO;
    }
}

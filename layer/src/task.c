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
    int exited, killed, reaped, sigdeath, execd, status;
    HANDLE proc;                               /* the host process an exec started */
    HANDLE pipe;                               /* traced exec: the tracer's end of the control pipe (tracee.c) */
    int remote, running;                       /* traced exec: the task stands for a second process; it runs between a CONT and the next stop */
    char exe[256];
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
    if (!k->execd) {
        if (!k->sigdeath && !k->exited) k->status = (code & 0xff) << 8;
        k->exited = 1;
    }
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


/* ---- execve in an untraced task: the program runs as a second layer process --------------------------------
 * The host executable is started again (same file, same root) with the program's path and arguments; the
 * guest environment travels as XRL_ENV_<name>=<value> (what the test host turns back into the guest's
 * environment). Open files other than the standard streams are not inherited yet. The task ends and the
 * parent goes on; wait4 then waits for the host process and reports its exit code. On the console the same
 * step would start the package's own executable as a child process of the app (see docs/decisions.md). */
static void quote_arg(wchar_t *out, size_t cap, const wchar_t *a, size_t *w)
{
    size_t n = *w;
    int q = !*a || wcspbrk(a, L" \t\"") != NULL;
    if (q && n + 1 < cap) out[n++] = L'"';
    for (const wchar_t *p = a; *p && n + 4 < cap; p++) {
        size_t bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * (q ? 2 : 1) && n + 2 < cap; i++) out[n++] = L'\\'; break; }
        if (*p == L'"') { for (size_t i = 0; i < bs * 2 + 1 && n + 2 < cap; i++) out[n++] = L'\\'; out[n++] = L'"'; }
        else { for (size_t i = 0; i < bs && n + 2 < cap; i++) out[n++] = L'\\'; out[n++] = *p; }
    }
    if (q && n + 1 < cap) out[n++] = L'"';
    out[n++] = L' ';
    *w = n;
}

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n + 1);
    return w;
}


/* ---- a hard cap on the processes a layer process may start --------------------------------------------------
 * A spawn path that creates OS processes must not be able to loop (a test that spawned itself once started
 * 85 host processes and used up the laptop's commit). Two limits, both refused with EAGAIN as a full process
 * table is on Linux:
 *   - a group-wide count of live spawned processes (a named semaphore shared by the first process and every
 *     descendant; XRL_MAX_CHILDREN, default 4, never above 16); a slot is given back when wait4 reaps the child
 *   - a nesting depth (XRL_SPAWN_DEPTH travels in the environment; XRL_MAX_SPAWN_DEPTH, default 2).
 * XRL_SPAWN_KILL_ON_EXIT=1 additionally puts the process and its descendants into a job object that kills
 * them when the first process ends (the tests set it; a launcher whose child must outlive it does not). */
static HANDLE g_spawn_sem;
static char g_spawn_group[64];
static int g_spawn_depth;

static int env_int(const char *name, int dflt)
{
    char v[16];
    return GetEnvironmentVariableA(name, v, sizeof v) ? atoi(v) : dflt;
}

static int spawn_slot_take(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL CALLBACK init(PINIT_ONCE io, PVOID p, PVOID *c);
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!g_spawn_sem) return -LX_EAGAIN;
    if (g_spawn_depth >= env_int("XRL_MAX_SPAWN_DEPTH", 2)) { LOGE("exec: spawn depth %d reached (XRL_MAX_SPAWN_DEPTH): EAGAIN", g_spawn_depth); return -LX_EAGAIN; }
    if (WaitForSingleObject(g_spawn_sem, 0) != WAIT_OBJECT_0) { LOGE("exec: too many live child processes (XRL_MAX_CHILDREN): EAGAIN"); return -LX_EAGAIN; }
    return 0;
}
static void spawn_slot_give(void) { if (g_spawn_sem) ReleaseSemaphore(g_spawn_sem, 1, NULL); }

BOOL CALLBACK init(PINIT_ONCE io, PVOID p, PVOID *c)
{
    (void)io; (void)p; (void)c;
    int cap = env_int("XRL_MAX_CHILDREN", 4);
    if (cap < 1) cap = 1;
    if (cap > 16) cap = 16;
    g_spawn_depth = env_int("XRL_SPAWN_DEPTH", 0);
    if (!GetEnvironmentVariableA("XRL_SPAWN_GROUP", g_spawn_group, sizeof g_spawn_group)) {
        snprintf(g_spawn_group, sizeof g_spawn_group, "xrl-spawn-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
        SetEnvironmentVariableA("XRL_SPAWN_GROUP", g_spawn_group);
        if (env_int("XRL_SPAWN_KILL_ON_EXIT", 0)) {   /* only the first process of a group makes the job; children are in it already */
            HANDLE job = CreateJobObjectW(NULL, NULL);
            if (job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
                if (!AssignProcessToJobObject(job, GetCurrentProcess())) LOGW("exec: could not put this process into a job (%lu); children are not killed with it", GetLastError());
            }
        }
    }
    g_spawn_sem = CreateSemaphoreA(NULL, cap, cap, g_spawn_group);
    return TRUE;
}


/* ---- a traced exec: the program is a second layer process and this task is its tracer-side handle ----------
 * (tracee.c has the other end.) The pipe carries ptrace requests; the process handle gives exit status. */
static int rcall(xrl_task *k, u32 op, u64 a, u64 b, const void *pl, u32 plen, void **out, u32 *outlen)
{
    u32 rop, len; u64 ra, rb; void *p = NULL;
    if (xrl_tr_send(k->pipe, op, a, b, pl, plen)) return -LX_ESRCH;
    if (xrl_tr_recv(k->pipe, &rop, &ra, &rb, &p, &len) || rop != 3 /* RESP */) { free(p); return -LX_ESRCH; }
    if (out) { *out = p; if (outlen) *outlen = len; } else free(p);
    return (int)(s64)ra;
}

/* the tracee ended: collect the exit code */
static void remote_gone(xrl_task *k)
{
    DWORD ec = 0;
    WaitForSingleObject(k->proc, 5000);
    GetExitCodeProcess(k->proc, &ec);
    if (!k->killed) k->status = (int)(ec & 0xff) << 8;
    k->exited = 1; k->running = 0;
}

static slong remote_wait(xrl_task *k, u64 ustatus, int options)
{
    for (;;) {
        if (k->exited || k->killed) {
            if (!k->exited) remote_gone(k);
            int st = k->status, r = k->pid;
            if (ustatus && xrl_ctu(ustatus, &st, 4)) return -LX_EFAULT;
            k->reaped = 1;
            CloseHandle(k->proc); k->proc = NULL; CloseHandle(k->pipe); k->pipe = NULL;
            spawn_slot_give();
            return r;
        }
        if (k->stopped && !k->reported) {
            int st = k->status;
            if (ustatus && xrl_ctu(ustatus, &st, 4)) return -LX_EFAULT;
            k->reported = 1;
            return k->pid;
        }
        if (!k->running) { LOGW("wait4(%d): the tracee is stopped and nobody continued it: this would wait forever", k->pid); return -LX_ECHILD; }
        if (options & 1) {   /* WNOHANG */
            DWORD avail = 0;
            if (PeekNamedPipe(k->pipe, NULL, 0, NULL, &avail, NULL) && !avail) {
                if (WaitForSingleObject(k->proc, 0) != WAIT_OBJECT_0) return 0;
                remote_gone(k); continue;
            }
        }
        u32 op, len; u64 a, b; void *p = NULL;
        if (xrl_tr_recv(k->pipe, &op, &a, &b, &p, &len)) { remote_gone(k); continue; }
        free(p);
        if (op == 2 /* STOP */) { k->running = 0; k->stopped = 1; k->reported = 0; k->s_kind = 4; k->s_sig = (int)a; k->status = ((int)a << 8) | 0x7f; }
    }
}

static slong remote_ptrace(xrl_task *k, int req, u64 addr, u64 data)
{
    void *out = NULL; u32 olen = 0; int e;
    switch (req) {
    case 0x4200: k->opts = (int)data; return 0;
    case 7: case 24: case 9:
        if ((e = rcall(k, 14 /* CONT */, 0, 0, NULL, 0, NULL, NULL))) return e;
        k->running = 1; k->stopped = 0; return 0;
    case 17:
        if ((e = rcall(k, 17 /* DETACH */, 0, 0, NULL, 0, NULL, NULL))) return e;
        k->running = 1; k->stopped = 0; k->traced = 0; return 0;
    case 8: { slong r; xrl_task_kill(k->pid, LX_SIGKILL, &r); return 0; }
    case 1: case 2: {
        if ((e = rcall(k, 10 /* PEEK */, addr, 8, NULL, 0, &out, &olen))) return -LX_EIO;
        if (olen != 8) { free(out); return -LX_EIO; }
        e = xrl_ctu(data, out, 8) ? -LX_EFAULT : 0; free(out); return e; }
    case 4: case 5: return rcall(k, 11 /* POKE */, addr, 0, &data, 8, NULL, NULL) ? -LX_EIO : 0;
    case 12: case 0x4204: {
        if ((e = rcall(k, 12 /* GETREGS */, 0, 0, NULL, 0, &out, &olen))) return e;
        if (req == 12) { e = xrl_ctu(data, out, olen) ? -LX_EFAULT : 0; free(out); return e; }
        u64 iov[2]; if (xrl_cfu(iov, data, 16)) { free(out); return -LX_EFAULT; }
        u64 n = iov[1] < olen ? iov[1] : olen; iov[1] = n;
        e = (xrl_ctu(iov[0], out, (size_t)n) || xrl_ctu(data, iov, 16)) ? -LX_EFAULT : 0; free(out); return e; }
    case 13: case 0x4205: {
        u64 r[27]; u64 src = data;
        if (req == 0x4205) { u64 iov[2]; if (xrl_cfu(iov, data, 16) || iov[1] < sizeof r) return -LX_EINVAL; src = iov[0]; }
        if (xrl_cfu(r, src, sizeof r)) return -LX_EFAULT;
        return rcall(k, 13 /* SETREGS */, 0, 0, r, sizeof r, NULL, NULL); }
    case 0x4202: {
        u32 si[32]; memset(si, 0, sizeof si);
        si[0] = 5;
        si[2] = k->s_kind == 3 ? ((k->opts & 0x10) ? (5 | (4 << 8)) : 5) : 0x80;
        return xrl_ctu(data, si, sizeof si) ? -LX_EFAULT : 0; }
    case 0x4201: { u64 m = 0; return xrl_ctu(data, &m, 8) ? -LX_EFAULT : 0; }
    default: LOGW("ptrace request 0x%x on remote task %d: not supported", req, k->pid); return -LX_EIO;
    }
}

/* /proc/<pid>/{maps,exe} of a traced second process, for xrl_proc_*; the text is malloc'd */
char *xrl_task_remote_proc(int pid, const char *leaf, size_t *len)
{
    xrl_task *k = NULL;
    AcquireSRWLockShared(&g_task_lock);
    for (xrl_task *c = g_tasks; c; c = c->next) if (c->pid == pid && c->remote && c->pipe && !c->reaped && !c->exited && !c->running) { k = c; break; }
    ReleaseSRWLockShared(&g_task_lock);
    if (!k) return NULL;
    if (!strcmp(leaf, "exe")) { char *s = _strdup(k->exe); *len = strlen(s); return s; }
    if (!strcmp(leaf, "maps")) {
        void *out = NULL; u32 olen = 0;
        if (rcall(k, 15 /* MAPS */, 0, 0, NULL, 0, &out, &olen)) return NULL;
        *len = olen; return (char *)out ? (char *)out : _strdup("");
    }
    return NULL;
}

static slong spawn_process(xrl_task *k, const char *path, u64 uargv, u64 uenvp, int traced)
{
    /* the program must exist and be an x86-64 ELF: the errors a caller of execve expects come before anything starts */
    char gpath[1024]; slong r;
    if ((r = xrl_vfs_resolve(LX_AT_FDCWD, path, 1, gpath, sizeof gpath)) < 0) return r;
    {
        u8 hdr[64];
        slong fd = xrl_vfs_open(LX_AT_FDCWD, gpath, LX_O_RDONLY | LX_O_CLOEXEC, 0);
        if (fd < 0) return fd;
        xfile *f = xrl_fd_get((int)fd);
        slong got = f && f->ops->read ? f->ops->read(f, hdr, sizeof hdr, 0) : -LX_EACCES;
        xfile_unref(f);
        xrl_fd_close((int)fd);
        if (got < 0) return got;
        if (got < 20 || hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F' || hdr[4] != 2 || *(u16 *)(hdr + 18) != 62) return -LX_ENOEXEC;
    }
    if ((r = spawn_slot_take()) < 0) return r;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t *root = G.vfs_root_w;
    if (!wcsncmp(root, L"\\\\?\\", 4)) root += 4;
    static wchar_t cmd[32768];
    size_t w = 0;
    quote_arg(cmd, ARRAY_LEN(cmd), exe, &w);
    quote_arg(cmd, ARRAY_LEN(cmd), root, &w);
    wchar_t *wp = widen(path);
    quote_arg(cmd, ARRAY_LEN(cmd), wp, &w); free(wp);
    for (int i = 1; i < 64; i++) {   /* argv[1..]; argv[0] is the program's own name for itself */
        u64 sp; char s[2048];
        if (xrl_cfu(&sp, uargv + 8ull * i, 8) || !sp || xrl_strfu(s, sp, sizeof s)) break;
        wchar_t *a = widen(s); quote_arg(cmd, ARRAY_LEN(cmd), a, &w); free(a);
    }
    for (int i = 0; i < G.argc; i++) if (!strcmp(G.argv[i], "--fake-host")) { quote_arg(cmd, ARRAY_LEN(cmd), L"--fake-host", &w); break; }
    cmd[w - 1] = 0;
    /* environment: this process's, without its XRL_ENV_ lines, plus the guest's as XRL_ENV_ lines */
    wchar_t *blk = (wchar_t *)malloc(1 << 20), *o = blk;
    wchar_t *cur = GetEnvironmentStringsW();
    for (const wchar_t *e = cur; *e; e += wcslen(e) + 1)
        if (wcsncmp(e, L"XRL_ENV_", 8) && wcsncmp(e, L"XRL_SPAWN_DEPTH=", 16) && wcsncmp(e, L"XRL_TRACEE=", 11) && wcsncmp(e, L"XRL_ARGV0=", 10)) { size_t l = wcslen(e) + 1; if ((size_t)(o - blk) + l + 4096 < (1u << 19)) { memcpy(o, e, l * sizeof(wchar_t)); o += l; } }
    FreeEnvironmentStringsW(cur);
    for (int i = 0; i < 256; i++) {
        u64 sp; char s[4096];
        if (xrl_cfu(&sp, uenvp + 8ull * i, 8) || !sp || xrl_strfu(s, sp, sizeof s)) break;
        wchar_t *a = widen(s); size_t l = wcslen(a);
        if ((size_t)(o - blk) + l + 16 < (1u << 19)) { memcpy(o, L"XRL_ENV_", 8 * sizeof(wchar_t)); o += 8; memcpy(o, a, (l + 1) * sizeof(wchar_t)); o += l + 1; }
        free(a);
    }
    { wchar_t dv[40]; swprintf(dv, 40, L"XRL_SPAWN_DEPTH=%d", g_spawn_depth + 1); size_t l = wcslen(dv) + 1; memcpy(o, dv, l * sizeof(wchar_t)); o += l; }
    char pname[128] = "";
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (traced) {
        snprintf(pname, sizeof pname, "\\\\.\\pipe\\xrl-tracee-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
        pipe = CreateNamedPipeA(pname, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 1 << 16, 1 << 16, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) { free(blk); spawn_slot_give(); return -LX_EAGAIN; }
        wchar_t *wn = widen(pname); size_t l = wcslen(wn);
        memcpy(o, L"XRL_TRACEE=", 11 * sizeof(wchar_t)); o += 11; memcpy(o, wn, (l + 1) * sizeof(wchar_t)); o += l + 1; free(wn);
    }
    {   u64 a0; char s0[1024];
        if (!xrl_cfu(&a0, uargv, 8) && a0 && !xrl_strfu(s0, a0, sizeof s0)) { wchar_t *wa = widen(s0); size_t l = wcslen(wa); if ((size_t)(o - blk) + l + 16 < (1u << 19)) { memcpy(o, L"XRL_ARGV0=", 10 * sizeof(wchar_t)); o += 10; memcpy(o, wa, (l + 1) * sizeof(wchar_t)); o += l + 1; } free(wa); }
    }
    *o++ = 0;
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, blk, NULL, &si, &pi);
    free(blk);
    if (!ok) { LOGE("exec: CreateProcess failed: %lu", GetLastError()); if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); spawn_slot_give(); return -LX_EAGAIN; }
    CloseHandle(pi.hThread);
    k->proc = pi.hProcess;
    if (traced) {   /* wait for the new process to reach its exec stop (HELLO) */
        int up = 0;
        for (int i = 0; i < 3000 && !up; i++) {   /* 30 s */
            if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) up = 1;
            else if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
        }
        u32 op = 0, len; u64 a = 0, b; void *p = NULL;
        DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        if (up) SetNamedPipeHandleState(pipe, &mode, NULL, NULL);
        if (!up || xrl_tr_recv(pipe, &op, &a, &b, &p, &len) || op != 1 /* HELLO */) {
            LOGE("exec: the traced process did not reach its exec stop"); free(p);
            TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); CloseHandle(pipe); k->proc = NULL; spawn_slot_give(); return -LX_EAGAIN;
        }
        k->pipe = pipe; k->remote = 1; k->running = 0;
        snprintf(k->exe, sizeof k->exe, "%s", gpath);
    }
    LOGW("exec: task %d runs \"%s\" as host process %lu", k->pid, path, pi.dwProcessId);
    return 0;
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
    slong r = spawn_process(k, path, uargv, uenvp, k->traced);
    if (r < 0) return r;                             /* execve fails in the child, as on Linux */
    if (k->traced) {                                 /* the exec stop is the tracee's HELLO: the tracer sees SIGTRAP (| EVENT_EXEC) */
        k->stopped = 1; k->reported = 0; k->s_kind = 3;
        k->status = ((k->opts & 0x10) ? (SIGTRAP_ | (4 << 8)) : SIGTRAP_) << 8 | 0x7f;
    }
    k->execd = 1;                                     /* the task ends here; its process goes on elsewhere */
    xrl_do_exit(0, 1);
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
        if (k->remote) return remote_wait(k, ustatus, options);   /* a traced exec made it a second process meanwhile */
        if (k->proc) {   /* it exec'd: a host process now */
            DWORD wr = WaitForSingleObject(k->proc, (options & 1) ? 0 : INFINITE), ec = 0;
            if (wr == WAIT_TIMEOUT) return 0;
            GetExitCodeProcess(k->proc, &ec);
            CloseHandle(k->proc); k->proc = NULL;
            spawn_slot_give();
            if (!k->killed) k->status = (int)(ec & 0xff) << 8;
            k->exited = 1;
        }
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
    if (sig == LX_SIGKILL && k->proc) { k->killed = 1; k->status = 9; TerminateProcess(k->proc, 137); LOGW("task %d: its host process killed", k->pid); }
    else if (sig == LX_SIGKILL) { k->killed = 1; k->exited = 1; k->status = 9; k->resumed = 0; xrl_fd_table_free(k->fdt); k->fdt = NULL; SetEvent(k->child_go); LOGW("task %d killed", k->pid); }
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
    if (k->remote) return remote_ptrace(k, req, addr, data);
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

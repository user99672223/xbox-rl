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
    HANDLE pipe_r, pipe_w;                     /* traced exec: the tracer's ends of the two control pipes (tracee.c): read from / write to the tracee */
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

/* XRL_INPROC_INJECT=1 (Plan C): when a traced fork child execve's the game, DON'T spawn a 2nd OS process
 * (the Xbox reaps a windowless 2nd process at ~3s). Instead run the game as a second guest in THIS OS
 * process, so the one surviving window-owning process carries the game + EAC. Increment 1 here only probes
 * whether a 2nd wine64 guest can even start in-process sharing G (the dominant unknown); ptrace-injection
 * wiring comes next once that is known. See the design note in memory. */
int xrl_inproc_inject_enabled(void)
{
    static int on = -1;
    if (on < 0) { char v[4]; on = GetEnvironmentVariableA("XRL_INPROC_INJECT", v, sizeof v) && v[0] == '1'; }
    return on;
}

/* read a NULL-terminated user char* vector into a freshly malloc'd char** (NULL-terminated); caller frees. */
static char **strv_dup_from_user(u64 uv, int *count)
{
    char **v = (char **)calloc(1, sizeof(char *));
    int n = 0;
    if (!v) return NULL;
    for (; uv; n++) {
        u64 p;
        if (n >= 8192 || xrl_cfu(&p, uv + 8ull * (u64)n, 8)) break;
        if (!p) break;
        char *s = (char *)malloc(1 << 17);
        if (!s) break;
        if (xrl_strfu(s, p, 1 << 17) < 0) { free(s); break; }
        char **nv = (char **)realloc(v, ((size_t)n + 2) * sizeof(char *));
        if (!nv) { free(s); break; }
        v = nv; v[n] = s; v[n + 1] = NULL;
    }
    if (count) *count = n;
    return v;
}

static void strv_free_local(char **v) { if (v) { for (int i = 0; v[i]; i++) free(v[i]); free(v); } }

/* The game runs in-process as a TRACED tracee: a helper thread loads nothing (the fork-child already loaded
 * the image), serves the launcher's ptrace over an IN-PROCESS pipe via the same tracee.c serve() the 2nd
 * process uses, and on CONT starts the game's guest main thread. The launcher drives it with the unchanged
 * remote_ptrace/remote_wait over k->pipe_r/w. */
struct inproc_boot { u64 entry, sp, slo, shi; void *fdt; int traced, proc_pid, proc_ppid; };
static DWORD WINAPI inproc_game_boot_thread(void *arg)
{
    struct inproc_boot *b = (struct inproc_boot *)arg;
    u64 entry = b->entry, sp = b->sp, slo = b->slo, shi = b->shi; void *fdt = b->fdt;
    int traced = b->traced, proc_pid = b->proc_pid, proc_ppid = b->proc_ppid;
    free(b);
    /* Only the EAC-traced game waits to be injected: HELLO over the in-process pipe, then serve the
     * launcher's injection until CONT. A non-traced execve is wine's own plumbing (the preloader's loader
     * re-exec, a helper, wineserver): it has no tracer and no CONT will ever come, so it must NOT wait --
     * it runs in-process immediately. Running it in-process (not as a 2nd OS process) keeps the whole wine
     * stack in the one window-owning process, so the console does not background/reap a windowless 2nd one. */
    if (traced) xrl_tracee_boot(&entry, &sp);
    xrl_thread *mt = NULL;
    /* The game gets its OWN fd table: EAC's injected code creates the in-game module as a memfd (fd 3) and
     * reopens /proc/self/fd/3 to dlopen it; sharing the launcher's table let the launcher clobber fd 3. */
    if (xrl_threads_start_main_fdt(entry, sp, fdt, proc_pid, proc_ppid, &mt) == 0 && mt) { mt->guest_stack_lo = slo; mt->guest_stack_hi = shi; }
    else LOGE("inproc: could not start the game main thread after injection");
    return 0;
}

/* Plan C: run the game as a traced 2nd guest IN THIS OS PROCESS (no 2nd process, so the Xbox has nothing
 * windowless to reap). The launcher injects it over an in-process pipe exactly as it injects a 2nd process.
 * Returns 0 with k set up as a remote (pipe-backed) tracee at its exec stop, <0 on a load error. */
static slong task_exec_inproc(xrl_task *k, const char *gpath, u64 uargv, u64 uenvp)
{
    LOGW("inproc: running \"%s\" as a %s 2nd guest in this process (shared G%s), no 2nd process",
         gpath, k->traced ? "traced" : "plain", k->traced ? ", in-process pipe" : "");

    /* The game needs its own argv/envp/exec_path. The launcher has forked and will not use its own argv
     * again, so repoint G to the game's (the stack builder reads G.argv/G.envp). */
    int ac = 0, ec = 0;
    char **av = strv_dup_from_user(uargv, &ac);
    char **ev = strv_dup_from_user(uenvp, &ec);
    if (!av || !ev) { strv_free_local(av); strv_free_local(ev); return -LX_ENOMEM; }
    if (!ac) { strv_free_local(av); av = (char **)calloc(2, sizeof(char *)); av[0] = _strdup(gpath); ac = 1; }
    char **oldav = G.argv, **oldev = G.envp; char *oldep = G.exec_path;
    G.argv = av; G.argc = ac; G.envp = ev; G.envc = ec;
    G.exec_path = _strdup(gpath);

    xrl_elf_image img;
    if (xrl_elf_load(G.exec_path, &img)) {
        LOGE("inproc: xrl_elf_load(%s) failed; reverting G", gpath);
        free(G.exec_path); G.argv = oldav; G.argc = 0; G.envp = oldev; G.exec_path = oldep;
        strv_free_local(av); strv_free_local(ev);
        return -LX_ENOEXEC;
    }
    u64 slo = 0, shi = 0, sp = xrl_elf_build_stack(&img, &slo, &shi);
    if (!sp) { LOGE("inproc: build_stack failed"); return -LX_ENOMEM; }
    strv_free_local(oldav); strv_free_local(oldev); free(oldep);
    LOGW("inproc: image loaded entry=%llx stack sp=%llx [%llx,%llx)", img.entry, sp, slo, shi);
    /* NOTE: moving the "libc.so" identity to the game's musl (xrl_boot_move_libc_to_game) was tried and
     * REGRESSED to 210 — EAC's injected dlopen needs an INITIALIZED musl, and the game's musl is dormant at
     * the exec-stop. The launcher's musl IS initialized, so EAC must keep using it; the console hang is the
     * launcher's dl lock being contended, which is fixed separately (not by re-tagging libc). */

    /* two anonymous pipes (in-process): parent reads pr / writes pw; the tracee reads cr / writes cw.
     * Only the EAC-traced game needs them (and the HELLO handshake); a plain execve just runs. */
    HANDLE pr = NULL, pw = NULL, cr = NULL, cw = NULL;
    if (k->traced) {
        if (!CreatePipe(&cr, &pw, NULL, 0) || !CreatePipe(&pr, &cw, NULL, 0)) {
            LOGE("inproc: CreatePipe failed %lu", GetLastError());
            if (cr) CloseHandle(cr); if (pw) CloseHandle(pw); if (pr) CloseHandle(pr); if (cw) CloseHandle(cw);
            return -LX_EAGAIN;
        }
        /* xrl_tracee_boot reads the child ends by value from the environment (same as the 2nd-process path) */
        char rs[32], ws[32];
        snprintf(rs, sizeof rs, "%llu", (unsigned long long)(uintptr_t)cr);
        snprintf(ws, sizeof ws, "%llu", (unsigned long long)(uintptr_t)cw);
        SetEnvironmentVariableA("XRL_TRACEE_R", rs);
        SetEnvironmentVariableA("XRL_TRACEE_W", ws);
    }

    struct inproc_boot *b = (struct inproc_boot *)calloc(1, sizeof *b);
    if (!b) { if (cr) CloseHandle(cr); if (pw) CloseHandle(pw); if (pr) CloseHandle(pr); if (cw) CloseHandle(cw); return -LX_ENOMEM; }
    b->entry = img.entry; b->sp = sp; b->slo = slo; b->shi = shi; b->traced = k->traced;
    b->proc_pid = k->pid; b->proc_ppid = k->ppid;   /* getpid() for the game = its fork-child pid; the launcher polls /tmp/eac_wine_pid_<that pid> */
    b->fdt = xrl_fd_table_clone();   /* the game's own fd table (copy of this fork-child's), isolated from the launcher */
    HANDLE th = CreateThread(NULL, 1u << 20, inproc_game_boot_thread, b, 0, NULL);
    if (!th) { LOGE("inproc: CreateThread failed %lu", GetLastError()); free(b); if (cr) CloseHandle(cr); if (pw) CloseHandle(pw); if (pr) CloseHandle(pr); if (cw) CloseHandle(cw); return -LX_EAGAIN; }
    CloseHandle(th);

    snprintf(k->exe, sizeof k->exe, "%s", gpath);
    if (!k->traced) {
        /* A plain in-process guest: it runs on the boot thread now. The fork-task thread ends here (execd),
         * so the parent's fork() returns and wine's startup proceeds, all in this one process. */
        LOGW("inproc: plain guest started (no injection)");
        return 0;
    }

    /* wait for the tracee's HELLO (image loaded, injection-ready), like spawn_process's traced branch */
    u32 op = 0, len; u64 a = 0, b2; void *p = NULL; int ready = 0;
    for (int i = 0; i < 3000 && !ready; i++) {
        DWORD avail = 0;
        if (PeekNamedPipe(pr, NULL, 0, NULL, &avail, NULL) && avail) { ready = 1; break; }
        Sleep(10);
    }
    if (!ready || xrl_tr_recv(pr, &op, &a, &b2, &p, &len) || op != 1 /* HELLO */) {
        LOGE("inproc: the in-process tracee did not reach its exec stop (op=%u)", op); free(p);
        CloseHandle(pr); CloseHandle(pw);   /* the serve loop will see the pipe break and exit */
        return -LX_EAGAIN;
    }
    free(p);
    k->pipe_r = pr; k->pipe_w = pw; k->remote = 1; k->running = 0;
    LOGW("inproc: tracee HELLO received; launcher will inject over the in-process pipe (entry=%llx sp=%llx)", a, b2);
    return 0;
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
    /* Extend up to the end of the WRITABLE stack mapping only. Stopping at readability alone over-reaches:
     * on the console the guest stack abuts EAC's module, and a readable-but-read-only guard page there got
     * pulled into the snapshot -- then xrl_task_on_exit's restore wrote that page and faulted, corrupting the
     * fork hand-off so EAC never injected. A real stack is writable throughout, so this loses no stack. */
    while (hi - lo < STACK_MAX - LX_PAGE && xrl_mem_page_writable(hi) && !xrl_cfu(k->snapP + (hi - lo), hi, LX_PAGE)) hi += LX_PAGE;
    k->lo_p = lo; k->hi = hi;
    k->parent_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    k->child_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    k->ppid = p->task ? p->task->pid : G.pid;
    k->fdt = xrl_fd_table_clone();
    xrl_thread *t = xrl_task_thread_new(p, rsp, k);
    if (!t) return -LX_EAGAIN;
    k->pid = t->tid;
    AcquireSRWLockExclusive(&g_task_lock); k->next = g_tasks; g_tasks = k; ReleaseSRWLockExclusive(&g_task_lock);
    LOGW("fork: child task %d (ppid %d) runs on the parent's stack and memory (XRL_FORK_TASKS; not a real fork); stack snap [%llx,%llx)", k->pid, k->ppid, k->lo_p, k->hi);
    xrl_task_thread_start(t);
    WaitForSingleObject(k->parent_go, INFINITE);
    LOGW("fork: parent (task %d) resumes after child %d yielded/exec'd%s", k->ppid, k->pid, k->remote ? " [child now remote]" : "");
    return k->pid;
}

/* the child thread ends (thread.c: xrl_do_exit): its exit status, the parent's stack back, then the parent goes on */
void xrl_task_on_exit(xrl_thread *t, int code)
{
    xrl_task *k = t->task;
    LOGW("task %d (ppid %d) thread ending: code=%d execd=%d traced=%d remote=%d", k->pid, k->ppid, code, k->execd, k->traced, k->remote);
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
    static const char *const ok[] = { "-epicapp=", "-epicenv=", "-epiclocale=", "-epicsandboxid=", "-AUTH_LOGIN=", "WINE", "EOS", "EAC", "GNUTLS", "XRL", "PROTON", "LD_", "PATH=", "HOME=", "/", "Z:" };
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
    if (!g_spawn_sem) { LOGE("exec: no spawn semaphore available: EAGAIN"); return -LX_EAGAIN; }
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
    if (!g_spawn_sem) {   /* an AppContainer (the console) may refuse a named kernel object; an unnamed
                           * per-process semaphore still caps this process's spawns, which is enough here */
        DWORD e = GetLastError();
        g_spawn_sem = CreateSemaphoreA(NULL, cap, cap, NULL);
        LOGW("exec: named spawn semaphore '%s' failed (%lu); using an unnamed one (%p)", g_spawn_group, e, (void *)g_spawn_sem);
    }
    return TRUE;
}


/* ---- a traced exec: the program is a second layer process and this task is its tracer-side handle ----------
 * (tracee.c has the other end.) The pipe carries ptrace requests; the process handle gives exit status. */
static int rcall(xrl_task *k, u32 op, u64 a, u64 b, const void *pl, u32 plen, void **out, u32 *outlen)
{
    u32 rop, len; u64 ra, rb; void *p = NULL;
    if (xrl_tr_send(k->pipe_w, op, a, b, pl, plen)) return -LX_ESRCH;
    if (xrl_tr_recv(k->pipe_r, &rop, &ra, &rb, &p, &len) || rop != 3 /* RESP */) { free(p); return -LX_ESRCH; }
    if (out) { *out = p; if (outlen) *outlen = len; } else free(p);
    return (int)(s64)ra;
}

/* the tracee ended: collect the exit code */
static void remote_gone(xrl_task *k)
{
    DWORD ec = 0;
    WaitForSingleObject(k->proc, 5000);
    GetExitCodeProcess(k->proc, &ec);
    if (!k->killed && !k->sigdeath) k->status = (int)(ec & 0xff) << 8;
    k->exited = 1; k->running = 0;
    LOGW("task %d: its host process ended with exit code 0x%lx (sigdeath=%d status=0x%x)", k->pid, ec, k->sigdeath, k->status);
}

static slong remote_wait(xrl_task *k, u64 ustatus, int options)
{
    for (;;) {
        if (k->exited || k->killed) {
            if (!k->exited) remote_gone(k);
            int st = k->status, r = k->pid;
            if (ustatus && xrl_ctu(ustatus, &st, 4)) return -LX_EFAULT;
            k->reaped = 1;
            CloseHandle(k->proc); k->proc = NULL; if (k->pipe_r) CloseHandle(k->pipe_r); if (k->pipe_w) CloseHandle(k->pipe_w); k->pipe_r = k->pipe_w = NULL;
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
            if (PeekNamedPipe(k->pipe_r, NULL, 0, NULL, &avail, NULL) && !avail) {
                if (WaitForSingleObject(k->proc, 0) != WAIT_OBJECT_0) return 0;
                remote_gone(k); continue;
            }
        }
        u32 op, len; u64 a, b; void *p = NULL;
        if (xrl_tr_recv(k->pipe_r, &op, &a, &b, &p, &len)) { remote_gone(k); continue; }
        free(p);
        if (op == 2 /* STOP */) { LOGW("task %d: its process stopped with signal %llu", k->pid, a); k->sigdeath = 0; k->running = 0; k->stopped = 1; k->reported = 0; k->s_kind = 4; k->s_sig = (int)a; k->status = ((int)a << 8) | 0x7f; }
    }
}

static slong remote_ptrace(xrl_task *k, int req, u64 addr, u64 data)
{
    void *out = NULL; u32 olen = 0; int e;
    switch (req) {
    case 0x4200: k->opts = (int)data; return 0;
    case 7: case 24: case 9:
        if ((e = rcall(k, 14 /* CONT */, data, 0, NULL, 0, NULL, NULL))) return e;
        if (data) { k->sigdeath = 1; k->status = (int)data & 0x7f; }
        k->running = 1; k->stopped = 0; return 0;
    case 17:
        if ((e = rcall(k, 17 /* DETACH */, 0, 0, NULL, 0, NULL, NULL))) return e;
        k->running = 1; k->stopped = 0; k->traced = 0; return 0;
    case 8: { slong r; xrl_task_kill(k->pid, LX_SIGKILL, &r); return 0; }
    case 1: case 2: {
        if ((e = rcall(k, 10 /* PEEK */, addr, 8, NULL, 0, &out, &olen))) return -LX_EIO;
        if (olen != 8) { free(out); return -LX_EIO; }
        LOGW("ptrace-peek %llx -> %016llx", (unsigned long long)addr, (unsigned long long)*(u64 *)out);   /* DIAG: what EAC reads */
        e = xrl_ctu(data, out, 8) ? -LX_EFAULT : 0; free(out); return e; }
    case 4: case 5: return rcall(k, 11 /* POKE */, addr, 0, &data, 8, NULL, NULL) ? -LX_EIO : 0;
    case 14: case 0x4204: if (req == 14 || addr == 2) {   /* GETFPREGS, GETREGSET(NT_PRFPREG) */
        if ((e = rcall(k, 18 /* GETFP */, 0, 0, NULL, 0, &out, &olen))) return e;
        if (req == 14) { e = xrl_ctu(data, out, olen) ? -LX_EFAULT : 0; free(out); return e; }
        u64 iov[2]; if (xrl_cfu(iov, data, 16)) { free(out); return -LX_EFAULT; }
        u64 n = iov[1] < olen ? iov[1] : olen; iov[1] = n;
        e = (xrl_ctu(iov[0], out, (size_t)n) || xrl_ctu(data, iov, 16)) ? -LX_EFAULT : 0; free(out); return e; }
        /* fall through */
    case 12: {
        if ((e = rcall(k, 12 /* GETREGS */, 0, 0, NULL, 0, &out, &olen))) return e;
        if (req == 12) { e = xrl_ctu(data, out, olen) ? -LX_EFAULT : 0; free(out); return e; }
        u64 iov[2]; if (xrl_cfu(iov, data, 16)) { free(out); return -LX_EFAULT; }
        u64 n = iov[1] < olen ? iov[1] : olen; iov[1] = n;
        e = (xrl_ctu(iov[0], out, (size_t)n) || xrl_ctu(data, iov, 16)) ? -LX_EFAULT : 0; free(out); return e; }
    case 15: case 0x4205: if (req == 15 || addr == 2) {   /* SETFPREGS, SETREGSET(NT_PRFPREG) */
        u8 f[512]; u64 src = data;
        if (req == 0x4205) { u64 iov[2]; if (xrl_cfu(iov, data, 16) || iov[1] < sizeof f) return -LX_EINVAL; src = iov[0]; }
        if (xrl_cfu(f, src, sizeof f)) return -LX_EFAULT;
        return rcall(k, 19 /* SETFP */, 0, 0, f, sizeof f, NULL, NULL); }
        /* fall through */
    case 13: {
        u64 r[27]; u64 src = data;
        if (req == 0x4205) { u64 iov[2]; if (xrl_cfu(iov, data, 16) || iov[1] < sizeof r) return -LX_EINVAL; src = iov[0]; }
        if (xrl_cfu(r, src, sizeof r)) return -LX_EFAULT;
        return rcall(k, 13 /* SETREGS */, 0, 0, r, sizeof r, NULL, NULL); }
    case 0x4202: {   /* GETSIGINFO: si_signo is the signal this stop is for (s_kind==4: whatever xrl_tracee_stop
                      * sent, SIGTRAP for its own int3, or the real signal for a hardware fault) -- it used to be
                      * hardcoded to SIGTRAP, wrong for every fault stop. si_code/si_addr: Linux reports the
                      * kernel's own fault classification and the faulting address, which the wire protocol does
                      * not carry; for a code-fetch fault (the case this layer's own tests exercise: a `ret` to an
                      * invalid address) the faulting address equals rip, fetched fresh as a stand-in -- not
                      * exact for a data-access fault elsewhere, but closer than the constant 0x80 this had. */
        u32 si[32]; memset(si, 0, sizeof si);
        int sig = k->s_kind == 3 ? SIGTRAP_ : k->s_sig;
        si[0] = (u32)sig;
        if (k->s_kind == 3) si[2] = (k->opts & 0x10) ? (SIGTRAP_ | (4 << 8)) : SIGTRAP_;
        else if (sig == 11 /*SIGSEGV*/ || sig == 7 /*SIGBUS*/) {
            si[2] = 1;   /* SEGV_MAPERR / BUS_ADRALN: the common case, not distinguished here */
            void *out = NULL; u32 olen = 0;
            if (!rcall(k, 12 /* GETREGS */, 0, 0, NULL, 0, &out, &olen) && olen >= 17 * 8) memcpy(&si[4], (char *)out + 16 * 8, 8);
            free(out);
        } else si[2] = 0x80;   /* SI_KERNEL: our own int3/trap stops */
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
    for (xrl_task *c = g_tasks; c; c = c->next) if (c->pid == pid && c->remote && c->pipe_r && !c->reaped && !c->exited && !c->running) { k = c; break; }
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

/* ------------------------------------------------------------ exec hand-over (XRL_EXEC_HOST=1)
 * On the console only the app's first process owns the window, gamepad and sound, and the app ends when that
 * process ends. EAC's bootstrapper execs the game from a fork child and then exits, so with the game in a
 * second process the app, and the game with it, ended when the bootstrapper did. The hand-over flips the
 * roles: the first process starts the bootstrapper as a child host process and waits; the child's first
 * traced execve is sent back to it (command line and environment, as spawn_process would start it), and the
 * first process boots that program as the tracee. The bootstrapper traces it from the child over the same
 * two pipes a spawned tracee uses, whose ends the child duplicates into the first process. */
#define EXECHOST_MAGIC 0x48584558u   /* "XEXH" */
static HANDLE g_xh_pipe, g_xh_proc;     /* the child's: write end to the first process, and its process handle */

/* in the child: the hand-over channel, for the first traced execve only */
static int exechost_take(void)
{
    static LONG taken;
    char v[32], p[32];
    if (!GetEnvironmentVariableA("XRL_EXECHOST_W", v, sizeof v) || !GetEnvironmentVariableA("XRL_EXECHOST_PROC", p, sizeof p)) return 0;
    if (InterlockedExchange(&taken, 1)) return 0;
    g_xh_pipe = (HANDLE)(uintptr_t)strtoull(v, NULL, 10);
    g_xh_proc = (HANDLE)(uintptr_t)strtoull(p, NULL, 10);
    SetEnvironmentVariableA("XRL_EXECHOST_W", NULL);
    SetEnvironmentVariableA("XRL_EXECHOST_PROC", NULL);
    return 1;
}

static int exechost_write(const void *p, DWORD n)
{
    DWORD put;
    while (n) { if (!WriteFile(g_xh_pipe, p, n, &put, NULL) || !put) return 0; p = (const char *)p + put; n -= put; }
    return 1;
}

static int exechost_read(HANDLE h, void *p, DWORD n)
{
    DWORD got;
    while (n) { if (!ReadFile(h, p, n, &got, NULL) || !got) return 0; p = (char *)p + got; n -= got; }
    return 1;
}

/* a command line split by Windows' rules (the inverse of quote_arg), as UTF-8, from argument `skip` on */
static char **split_cmdline(const wchar_t *p, int skip)
{
    char **v = (char **)calloc(1, sizeof(char *));
    wchar_t *a = (wchar_t *)malloc((wcslen(p) + 1) * sizeof(wchar_t));
    int n = 0, idx = 0;
    while (v && a && *p) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        size_t k = 0; int inq = 0;
        while (*p && (inq || (*p != L' ' && *p != L'\t'))) {
            size_t bs = 0;
            while (*p == L'\\') { bs++; p++; }
            if (*p == L'"') {
                for (size_t i = 0; i < bs / 2; i++) a[k++] = L'\\';
                if (bs % 2) a[k++] = L'"'; else inq = !inq;
                p++;
            } else {
                for (size_t i = 0; i < bs; i++) a[k++] = L'\\';
                if (*p && (inq || (*p != L' ' && *p != L'\t'))) a[k++] = *p++;
            }
        }
        a[k] = 0;
        if (idx++ < skip) continue;
        int m = WideCharToMultiByte(CP_UTF8, 0, a, -1, NULL, 0, NULL, NULL);
        char *s = (char *)malloc((size_t)m);
        char **nv = s ? (char **)realloc(v, ((size_t)n + 2) * sizeof(char *)) : NULL;
        if (!nv) { free(s); break; }
        WideCharToMultiByte(CP_UTF8, 0, a, -1, s, m, NULL, NULL);
        v = nv; v[n++] = s; v[n] = NULL;
    }
    free(a);
    return v;
}

int xrl_exec_host_run(const char *vfs_root, const char *const *argv, const char *const *envp, xrl_log_fn log, char ***argv_out, int *exit_code)
{
    char m[600];
#define XH_LOG(lvl, ...) do { snprintf(m, sizeof m, __VA_ARGS__); if (log) log(lvl, "exec-host", m); } while (0)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    static wchar_t cmd[32768];
    size_t w = 0;
    quote_arg(cmd, ARRAY_LEN(cmd), exe, &w);
    { wchar_t *a = widen(vfs_root); quote_arg(cmd, ARRAY_LEN(cmd), a, &w); free(a); }
    for (int i = 0; argv && argv[i]; i++) { wchar_t *a = widen(argv[i]); quote_arg(cmd, ARRAY_LEN(cmd), a, &w); free(a); }
    cmd[w - 1] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE pr = NULL, pw = NULL, self = NULL;
    if (!CreatePipe(&pr, &pw, &sa, 1 << 16)) { XH_LOG(3, "CreatePipe failed: %lu", GetLastError()); return -1; }
    SetHandleInformation(pr, 0x1 /* HANDLE_FLAG_INHERIT */, 0);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        XH_LOG(3, "cannot make a handle to this process for the child: %lu", GetLastError());
        CloseHandle(pr); CloseHandle(pw); return -1;
    }
    /* the child's environment: this one's, with the program's guest environment as XRL_ENV_ lines */
    wchar_t *blk = (wchar_t *)malloc(1 << 20), *o = blk;
    wchar_t *cur = GetEnvironmentStringsW();
    for (const wchar_t *e = cur; *e; e += wcslen(e) + 1)
        if (wcsncmp(e, L"XRL_ENV_", 8) && wcsncmp(e, L"XRL_SPAWN_DEPTH=", 16) && wcsncmp(e, L"XRL_EXEC_HOST=", 14) && wcsncmp(e, L"XRL_EXECHOST", 12)) {
            size_t l = wcslen(e) + 1;
            if ((size_t)(o - blk) + l + 4096 < (1u << 19)) { memcpy(o, e, l * sizeof(wchar_t)); o += l; }
        }
    FreeEnvironmentStringsW(cur);
    for (int i = 0; envp && envp[i]; i++) {
        wchar_t *a = widen(envp[i]); size_t l = wcslen(a);
        if ((size_t)(o - blk) + l + 16 < (1u << 19)) { memcpy(o, L"XRL_ENV_", 8 * sizeof(wchar_t)); o += 8; memcpy(o, a, (l + 1) * sizeof(wchar_t)); o += l + 1; }
        free(a);
    }
    o += swprintf(o, 64, L"XRL_SPAWN_DEPTH=1") + 1;
    o += swprintf(o, 64, L"XRL_EXECHOST_W=%llu", (unsigned long long)(uintptr_t)pw) + 1;
    o += swprintf(o, 64, L"XRL_EXECHOST_PROC=%llu", (unsigned long long)(uintptr_t)self) + 1;
    *o++ = 0;
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, blk, NULL, &si, &pi);
    free(blk);
    CloseHandle(pw); CloseHandle(self);   /* the child holds its own copies; EOF on pr once it exits */
    if (!ok) { XH_LOG(3, "CreateProcess failed: %lu", GetLastError()); CloseHandle(pr); return -1; }
    CloseHandle(pi.hThread);
    XH_LOG(2, "started %s as host process %lu; this process becomes the program it execs (traced)", argv && argv[0] ? argv[0] : "?", pi.dwProcessId);

    u32 h[3];
    wchar_t *ncmd = NULL, *nenv = NULL;
    if (!exechost_read(pr, h, sizeof h) || h[0] != EXECHOST_MAGIC || h[1] > (1u << 20) || h[2] > (4u << 20) ||
        !(ncmd = (wchar_t *)calloc(1, h[1] + 2)) || !(nenv = (wchar_t *)calloc(1, h[2] + 4)) ||
        !exechost_read(pr, ncmd, h[1]) || !exechost_read(pr, nenv, h[2])) {
        free(ncmd); free(nenv); CloseHandle(pr);
        DWORD ec = 0;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        XH_LOG(2, "the child ended without handing over an exec (exit code 0x%lx)", ec);
        if (exit_code) *exit_code = (int)ec;
        return 0;
    }
    CloseHandle(pr); CloseHandle(pi.hProcess);
    /* become that process: its environment (the launcher's guest environment goes), then its program */
    cur = GetEnvironmentStringsW();
    for (const wchar_t *e = cur; *e; e += wcslen(e) + 1)
        if (!wcsncmp(e, L"XRL_ENV_", 8)) {
            const wchar_t *eq = wcschr(e, L'=');
            wchar_t nm[512]; size_t l = eq ? (size_t)(eq - e) : 0;
            if (l && l < ARRAY_LEN(nm)) { memcpy(nm, e, l * sizeof(wchar_t)); nm[l] = 0; SetEnvironmentVariableW(nm, NULL); }
        }
    FreeEnvironmentStringsW(cur);
    int nset = 0;
    for (wchar_t *e = nenv; *e; e += wcslen(e) + 1) {
        wchar_t *eq = wcschr(e + 1, L'=');   /* "=C:=..." entries are the shell's; skip them */
        if (e[0] == L'=' || !eq) continue;
        *eq = 0; SetEnvironmentVariableW(e, eq + 1); *eq = L'='; nset++;
    }
    SetEnvironmentVariableW(L"XRL_EXEC_HOST", NULL);
    *argv_out = split_cmdline(ncmd, 2);   /* <exe> <rootfs> <program> <args...> */
    XH_LOG(2, "hand-over: running %s here (%d environment entries)", (*argv_out && (*argv_out)[0]) ? (*argv_out)[0] : "?", nset);
    free(ncmd); free(nenv);
    return 1;
#undef XH_LOG
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
    int handover = traced && exechost_take();   /* XRL_EXEC_HOST: the first process becomes this program */
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
        if (wcsncmp(e, L"XRL_ENV_", 8) && wcsncmp(e, L"XRL_SPAWN_DEPTH=", 16) && wcsncmp(e, L"XRL_TRACEE", 10) && wcsncmp(e, L"XRL_ARGV0=", 10) && wcsncmp(e, L"XRL_INHERIT_FDS=", 16) && wcsncmp(e, L"XRL_DBT_MEMFD=", 14) && wcsncmp(e, L"XRL_PID=", 8) && wcsncmp(e, L"XRL_EXECHOST", 12)) { size_t l = wcslen(e) + 1; if ((size_t)(o - blk) + l + 4096 < (1u << 19)) { memcpy(o, e, l * sizeof(wchar_t)); o += l; } }
    FreeEnvironmentStringsW(cur);
    for (int i = 0; i < 256; i++) {
        u64 sp; char s[4096];
        if (xrl_cfu(&sp, uenvp + 8ull * i, 8) || !sp || xrl_strfu(s, sp, sizeof s)) break;
        wchar_t *a = widen(s); size_t l = wcslen(a);
        if ((size_t)(o - blk) + l + 16 < (1u << 19)) { memcpy(o, L"XRL_ENV_", 8 * sizeof(wchar_t)); o += 8; memcpy(o, a, (l + 1) * sizeof(wchar_t)); o += l + 1; }
        free(a);
    }
    { wchar_t dv[40]; swprintf(dv, 40, L"XRL_SPAWN_DEPTH=%d", g_spawn_depth + 1); size_t l = wcslen(dv) + 1; memcpy(o, dv, l * sizeof(wchar_t)); o += l; }
    {   /* the glibc-ABI shim (libgcompat) is part of this guest userland: a process started by exec gets it loaded
         * before its first instruction unless the caller chose an LD_PRELOAD of its own, so that a glibc-built
         * library put into it from outside (the EAC module's in-game part, via dlopen) finds its symbols */
        int have = 0; lx_stat sst;
        for (int i = 0; i < 256; i++) { u64 sp; char s1[64]; if (xrl_cfu(&sp, uenvp + 8ull * i, 8) || !sp || xrl_strfu(s1, sp, sizeof s1)) break; if (!strncmp(s1, "LD_PRELOAD=", 11)) have = 1; }
        if (!have && !xrl_vfs_stat(LX_AT_FDCWD, "/lib/libgcompat.so.0", 0, &sst)) {
            const wchar_t *kv = L"XRL_ENV_LD_PRELOAD=/lib/libgcompat.so.0"; size_t l = wcslen(kv) + 1;
            memcpy(o, kv, l * sizeof(wchar_t)); o += l;
        }
    }
    {   char inh[3000]; if (xrl_fd_inherit_list(inh, sizeof inh)) { wchar_t *wi = widen(inh); size_t l = wcslen(wi); if ((size_t)(o - blk) + l + 32 < (1u << 19)) { memcpy(o, L"XRL_INHERIT_FDS=", 16 * sizeof(wchar_t)); o += 16; memcpy(o, wi, (l + 1) * sizeof(wchar_t)); o += l + 1; } free(wi); } }
    /* The tracer<->tracee control channel is TWO anonymous pipes (not a named pipe): an AppContainer
     * (the console) refuses CreateNamedPipe in the global namespace with ACCESS_DENIED, but anonymous
     * pipes have no name and their handles are inherited into the child (bInheritHandles below). The
     * parent keeps the write-to-child and read-from-child ends; the child's ends travel as handle
     * values in XRL_TRACEE_R / XRL_TRACEE_W (valid in the child because inherited). */
    HANDLE pipe_r = NULL, pipe_w = NULL;     /* parent's ends: read from the child, write to the child */
    HANDLE child_r = NULL, child_w = NULL;   /* the child's ends, closed in the parent after CreateProcess */
    if (traced) {
        { const wchar_t *kv = L"XRL_DBT_MEMFD=1"; size_t kl = wcslen(kv) + 1; memcpy(o, kv, kl * sizeof(wchar_t)); o += kl; }
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        HANDLE t2c_r = NULL, t2c_w = NULL, c2t_r = NULL, c2t_w = NULL;   /* tracer->child, child->tracer */
        if (!CreatePipe(&t2c_r, &t2c_w, &sa, 1 << 16) || !CreatePipe(&c2t_r, &c2t_w, &sa, 1 << 16)) {
            LOGE("exec: CreatePipe failed: %lu (traced spawn): EAGAIN", GetLastError());
            if (t2c_r) CloseHandle(t2c_r); if (t2c_w) CloseHandle(t2c_w); if (c2t_r) CloseHandle(c2t_r); if (c2t_w) CloseHandle(c2t_w);
            free(blk); spawn_slot_give(); return -LX_EAGAIN;
        }
        SetHandleInformation(t2c_w, 0x1 /* HANDLE_FLAG_INHERIT */, 0);   /* the parent's own ends are not inherited */
        SetHandleInformation(c2t_r, 0x1 /* HANDLE_FLAG_INHERIT */, 0);
        pipe_w = t2c_w; pipe_r = c2t_r; child_r = t2c_r; child_w = c2t_w;
        HANDLE vr = t2c_r, vw = c2t_w;   /* the handle values the tracee's process reads */
        if (handover && (!DuplicateHandle(GetCurrentProcess(), t2c_r, g_xh_proc, &vr, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                         !DuplicateHandle(GetCurrentProcess(), c2t_w, g_xh_proc, &vw, 0, FALSE, DUPLICATE_SAME_ACCESS))) {
            LOGE("exec: cannot hand the tracee pipes to the first process (%lu); starting a new process instead", GetLastError());
            handover = 0; vr = t2c_r; vw = c2t_w;
        }
        wchar_t hv[64];
        int hn = swprintf(hv, 64, L"XRL_TRACEE_R=%llu", (unsigned long long)(uintptr_t)vr); size_t hl = (size_t)hn + 1; memcpy(o, hv, hl * sizeof(wchar_t)); o += hl;
        hn = swprintf(hv, 64, L"XRL_TRACEE_W=%llu", (unsigned long long)(uintptr_t)vw); hl = (size_t)hn + 1; memcpy(o, hv, hl * sizeof(wchar_t)); o += hl;
    }
    {   u64 a0; char s0[1024];
        if (!xrl_cfu(&a0, uargv, 8) && a0 && !xrl_strfu(s0, a0, sizeof s0)) { wchar_t *wa = widen(s0); size_t l = wcslen(wa); if ((size_t)(o - blk) + l + 16 < (1u << 19)) { memcpy(o, L"XRL_ARGV0=", 10 * sizeof(wchar_t)); o += 10; memcpy(o, wa, (l + 1) * sizeof(wchar_t)); o += l + 1; } free(wa); }
    }
    {   /* execve preserves the pid: the exec'd process keeps this fork child's pid (thread.c reads XRL_PID).
         * EAC's launcher polls /tmp/eac_wine_pid_<child pid> and the game creates it under its own getpid(). */
        wchar_t pv[40]; int pn = swprintf(pv, 40, L"XRL_PID=%d", k->pid); size_t l = (size_t)pn + 1;
        if (pn > 0 && (size_t)(o - blk) + l + 4 < (1u << 19)) { memcpy(o, pv, l * sizeof(wchar_t)); o += l; }
    }
    *o++ = 0;
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = { 0 };
    BOOL ok;
    if (handover) {   /* no new process: the first one runs it, and is the tracee from now on */
        u32 h[3] = { EXECHOST_MAGIC, (u32)((wcslen(cmd) + 1) * sizeof(wchar_t)), (u32)((size_t)(o - blk) * sizeof(wchar_t)) };
        ok = exechost_write(h, sizeof h) && exechost_write(cmd, h[1]) && exechost_write(blk, h[2]);
        if (!ok) LOGE("exec: the hand-over to the first process failed: %lu", GetLastError());
        CloseHandle(g_xh_pipe); g_xh_pipe = NULL;
        if (ok) { pi.hProcess = g_xh_proc; pi.dwProcessId = GetProcessId(g_xh_proc); }
    } else {
        ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, blk, NULL, &si, &pi);
        if (!ok) LOGE("exec: CreateProcess failed: %lu", GetLastError());
        else CloseHandle(pi.hThread);
    }
    free(blk);
    if (!ok) {
        if (pipe_r) CloseHandle(pipe_r); if (pipe_w) CloseHandle(pipe_w); if (child_r) CloseHandle(child_r); if (child_w) CloseHandle(child_w);
        spawn_slot_give(); return -LX_EAGAIN;
    }
    k->proc = pi.hProcess;
    if (child_r) { CloseHandle(child_r); child_r = NULL; }   /* the child has its own inherited copies now */
    if (child_w) { CloseHandle(child_w); child_w = NULL; }
    if (traced) {   /* wait for the new process to reach its exec stop (HELLO), or exit first */
        u32 op = 0, len; u64 a = 0, b; void *p = NULL; int ready = 0;
        for (int i = 0; i < 3000 && !ready; i++) {   /* up to 30 s */
            DWORD avail = 0;
            if (PeekNamedPipe(pipe_r, NULL, 0, NULL, &avail, NULL) && avail) { ready = 1; break; }
            if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;   /* the child exited before HELLO */
        }
        if (!ready || xrl_tr_recv(pipe_r, &op, &a, &b, &p, &len)) op = 0;
        if (op != 1 /* HELLO */) {
            LOGE("exec: the traced process did not reach its exec stop"); free(p);
            if (!handover) TerminateProcess(pi.hProcess, 1);   /* never the first process: that is the app */
            CloseHandle(pi.hProcess);
            CloseHandle(pipe_r); CloseHandle(pipe_w); k->proc = NULL; spawn_slot_give(); return -LX_EAGAIN;
        }
        free(p);
        k->pipe_r = pipe_r; k->pipe_w = pipe_w; k->remote = 1; k->running = 0;
        snprintf(k->exe, sizeof k->exe, "%s", gpath);
    }
    LOGW("exec: task %d runs \"%s\" as host process %lu%s", k->pid, path, pi.dwProcessId, handover ? " (the first process, handed over)" : "");
    return 0;
}

/* execve in a task: what the child asks to run, then the stop a tracer expects; the image is not run */
slong xrl_task_exec(xrl_thread *t, const char *path, u64 uargv, u64 uenvp)
{
    xrl_task *k = t->task;
    LOGW("exec: task %d (ppid %d) execve(\"%s\") traced=%d", k->pid, k->ppid, path, k->traced);
    if (xrl_inproc_inject_enabled()) {
        slong ir = task_exec_inproc(k, path, uargv, uenvp);
        if (ir < 0) return ir;                 /* load failed: execve fails in the child, as on Linux */
        if (k->traced) {                       /* the exec stop is the tracee's HELLO: the tracer sees SIGTRAP (| EVENT_EXEC) */
            k->stopped = 1; k->reported = 0; k->s_kind = 3;
            k->status = ((k->opts & 0x10) ? (SIGTRAP_ | (4 << 8)) : SIGTRAP_) << 8 | 0x7f;
        }
        k->execd = 1;                          /* the fork-child thread ends; the game runs in-process as a traced tracee */
        xrl_do_exit(0, 1);
        return 0;
    }
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
    { static volatile LONG wc; if (InterlockedIncrement(&wc) <= 10) LOGW("wait4(pid=%d opt=%x) by task %d -> child %d%s", pid, options, xrl_task_pid(p, NULL), k ? k->pid : -1, k && k->remote ? " [remote]" : ""); }
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
    case 4: case 5: return xrl_ctu(addr, &data, 8) ? -LX_EIO : 0;   /* POKETEXT/DATA: the child shares our memory, so write it directly */
    default: LOGW("ptrace request 0x%x on task %d: not supported", req, pid); return -LX_EIO;
    }
}

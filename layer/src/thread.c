/*
 * thread.c - guest threads on Win32 threads, clone(), exit, arch_prctl, prctl.
 *
 * Model
 *   One guest thread == one Win32 thread (CreateThread). Each has two stacks:
 *     - the *host stack* (the Win32 thread stack, TEB-consistent), where all
 *       layer code runs; thunks switch to it on every guest->host transition;
 *     - the *guest stack* (an mmap'd region owned by the guest), where guest
 *       code runs.
 *   clone() is thread-only (no fork). The child "returns from the syscall"
 *   on the new stack with rax=0: xrl_syscall's thunk saved the parent's SysV
 *   callee-saved registers + return address, we copy them into the child's
 *   record and xrl_clone_resume() restores them. The patched musl __clone must
 *   therefore keep any state it needs in the child (e.g. the thread function)
 *   in a callee-saved register (rbx/rbp/r12-r15) or on the child stack.
 *
 * FS base (arch_prctl ARCH_SET_FS / CLONE_SETTLS)
 *   MEASURED on Win11 (and to be verified on the console, see notes-C.md):
 *   rdfsbase/wrfsbase are usable (CR4.FSGSBASE is on) BUT Windows does not
 *   save/restore the FS base across context switches - it is silently reset to
 *   0. Mechanism:
 *     1. thread->guest_tp holds the guest's thread pointer (authoritative).
 *     2. wrfsbase(guest_tp) on arch_prctl, on thread start, and on every
 *        return from xrl_syscall (asm thunk).
 *     3. A vectored exception handler (signal.c) catches the access violation
 *        a guest %fs: access takes after a context switch zeroed the base,
 *        re-writes the FS base from thread->guest_tp and resumes the same
 *        instruction. Cost: one cheap exception per scheduler preemption.
 *   If FSGSBASE is unavailable (probed at boot), arch_prctl(SET_FS) fails with
 *   -ENOSYS and the boot log says so; see notes-C.md for the Tier-3 plan.
 */
#include "xrl_internal.h"
#include <intrin.h>
#include <immintrin.h>

xrl_global G;
xrl_ttab_entry xrl_ttab[XRL_TTAB_SIZE];
u64 xrl_asm_fsb_flag;

#define HOST_STACK_SIZE (4u << 20)

C_ASSERT(FIELD_OFFSET(xrl_thread, host_rsp) == 0x00);
C_ASSERT(FIELD_OFFSET(xrl_thread, guest_tp) == 0x08);
C_ASSERT(FIELD_OFFSET(xrl_thread, frame) == 0x10);
C_ASSERT(FIELD_OFFSET(xrl_thread, self) == 0x18);
C_ASSERT(FIELD_OFFSET(xrl_thread, resume) == 0x20);
C_ASSERT(FIELD_OFFSET(xrl_thread, child_rsp) == 0x60);
C_ASSERT(FIELD_OFFSET(xrl_thread, teb_guest_live) == 0x70);
C_ASSERT(FIELD_OFFSET(xrl_thread, sig_jump) == 0x78);
C_ASSERT(FIELD_OFFSET(xrl_sigregs, rsp) == 0x78 && FIELD_OFFSET(xrl_sigregs, rip) == 0x80);
C_ASSERT(FIELD_OFFSET(xrl_sigregs, xmask) == 0x90 && FIELD_OFFSET(xrl_sigregs, xs) == 0xC0);

/* ------------------------------------------------------------ thread table */
static SRWLOCK g_ttab_lock = SRWLOCK_INIT;

xrl_thread *xrl_cur(void)
{
    u64 teb = __readgsqword(0x30);
    size_t i = (teb >> 12) & (XRL_TTAB_SIZE - 1);
    for (size_t n = 0; n < XRL_TTAB_SIZE; n++) {
        u64 k = xrl_ttab[i].key;
        if (k == teb) return (xrl_thread *)xrl_ttab[i].t;
        if (!k) return NULL;
        i = (i + 1) & (XRL_TTAB_SIZE - 1);
    }
    return NULL;
}

void xrl_thread_bind(xrl_thread *t)
{
    u64 teb = __readgsqword(0x30);
    AcquireSRWLockExclusive(&g_ttab_lock);
    size_t i = (teb >> 12) & (XRL_TTAB_SIZE - 1);
    while (xrl_ttab[i].key > 1) i = (i + 1) & (XRL_TTAB_SIZE - 1);
    xrl_ttab[i].t = (u64)t;
    _ReadWriteBarrier();
    xrl_ttab[i].key = teb;
    ReleaseSRWLockExclusive(&g_ttab_lock);
}

void xrl_thread_unbind(void)
{
    u64 teb = __readgsqword(0x30);
    AcquireSRWLockExclusive(&g_ttab_lock);
    size_t i = (teb >> 12) & (XRL_TTAB_SIZE - 1);
    for (size_t n = 0; n < XRL_TTAB_SIZE; n++) {
        if (xrl_ttab[i].key == teb) { xrl_ttab[i].key = 1; break; }
        if (!xrl_ttab[i].key) break;
        i = (i + 1) & (XRL_TTAB_SIZE - 1);
    }
    ReleaseSRWLockExclusive(&g_ttab_lock);
}

/* ------------------------------------------------------------- thread list */
xrl_thread *xrl_thread_new(void) { return xrl_thread_new_in(xrl_curproc()); }

/* A thread record in process `p` (the calling thread's, for clone and exec; a new process's for fork). */
xrl_thread *xrl_thread_new_in(xrl_proc *p)
{
    xrl_thread *t = (xrl_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->proc = p;
    t->self = (u64)t;
    t->tid = 0;                         /* the OS thread id, set by start_os_thread */
    t->fsb_enabled = (u64)G.fsgsbase;
    t->sigmask = 0;
    t->sig_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    strcpy_s(t->name, sizeof t->name, "xrl-guest");
    AcquireSRWLockExclusive(&G.thread_lock);
    /* a process that is being torn down (xrl_proc_exit marks it dying before it lists its threads, under this
     * lock) gets no new thread: an execve or clone racing with a kill must not outlive the process */
    if (p->dying) {
        ReleaseSRWLockExclusive(&G.thread_lock);
        if (t->sig_ev) CloseHandle(t->sig_ev);
        free(t);
        return NULL;
    }
    InterlockedIncrement(&p->nthreads);
    xrl_proc_ref(p);
    t->next = G.threads;
    if (G.threads) G.threads->prev = t;
    G.threads = t;
    InterlockedIncrement(&G.nthreads);
    ReleaseSRWLockExclusive(&G.thread_lock);
    return t;
}

static void thread_unlink(xrl_thread *t)
{
    AcquireSRWLockExclusive(&G.thread_lock);
    if (t->prev) t->prev->next = t->next; else G.threads = t->next;
    if (t->next) t->next->prev = t->prev;
    InterlockedDecrement(&G.nthreads);
    ReleaseSRWLockExclusive(&G.thread_lock);
    if (t->proc) {
        InterlockedDecrement(&t->proc->nthreads);
        xrl_proc_put(t->proc);      /* the record stays allocated, its process may not */
    }
}

xrl_thread *xrl_thread_by_tid(int tid)
{
    xrl_thread *r = NULL;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t; t = t->next) if (t->tid == tid) { r = t; break; }
    ReleaseSRWLockShared(&G.thread_lock);
    return r;
}

int xrl_thread_count(void) { return (int)G.nthreads; }

/* -------------------------------------------------- thread pointer slot */
/* The guest TP lives in TlsSlots[XRL_TP_TLS_INDEX] of each thread's own TEB
 * (see xrl_abi.h): patched guest code reads it as gs:[XRL_TP_GS_OFFSET]. */
static void tp_slot_set(u64 v)
{
    if (G.tp_slot) __writegsqword(XRL_TP_GS_OFFSET, v);
}

/* Claim exactly TLS index XRL_TP_TLS_INDEX (TlsAlloc hands out the lowest free
 * index, so allocate up to it and give the others back), then check that the
 * TEB layout matches: TlsSetValue(index) must land at gs:[XRL_TP_GS_OFFSET]. */
static int claim_tp_slot(void)
{
    DWORD got[XRL_TP_TLS_INDEX + 1];
    int n = 0, ok = 0;
    for (;;) {
        DWORD i = TlsAlloc();
        if (i == TLS_OUT_OF_INDEXES) break;
        if (i == XRL_TP_TLS_INDEX) { ok = 1; break; }
        if (i > XRL_TP_TLS_INDEX || n == XRL_TP_TLS_INDEX + 1) { TlsFree(i); break; }
        got[n++] = i;
    }
    while (n) TlsFree(got[--n]);
    if (!ok) { LOGE("threads: TLS index %d is already taken in this process", XRL_TP_TLS_INDEX); return 0; }
    TlsSetValue(XRL_TP_TLS_INDEX, (LPVOID)(uintptr_t)0x58524c54ull);
    int layout = __readgsqword(XRL_TP_GS_OFFSET) == 0x58524c54ull;
    TlsSetValue(XRL_TP_TLS_INDEX, NULL);
    if (!layout) { LOGE("threads: TlsSlots[%d] is not at gs:[0x%x] on this OS", XRL_TP_TLS_INDEX, XRL_TP_GS_OFFSET); TlsFree(XRL_TP_TLS_INDEX); }
    return layout;
}

/* ----------------------------------------------------------- thread start */
static DWORD WINAPI thread_main(LPVOID p)
{
    xrl_thread *t = (xrl_thread *)p;
    t->win_tid = GetCurrentThreadId();
    t->host_teb = __readgsqword(0x30);
    xrl_thread_bind(t);
    tp_slot_set(t->guest_tp);           /* slot 63 is outside the TEB swap set */
    xrl_teb_thread_init(t);             /* pristine TEB: capture the host view  */
    xrl_teb_enter_guest(t);
    if (t->is_main) xrl_enter_guest(t->resume[6], t->child_rsp, t);
    else xrl_clone_resume(t);
    return 0;
}

/* Linux tids are the Windows thread ids (and getpid() the Windows process id):
 * Wine takes them as its own thread/process ids, so the TEB's ClientId is the
 * same in the guest and host views (docs/gs-teb.md). The OS thread is created
 * suspended so its id is known (and published) before it runs; ids are never
 * reused in this process because thread handles are never closed. */
static int create_os_thread(xrl_thread *t)
{
    DWORD id = 0;
    t->handle = CreateThread(NULL, HOST_STACK_SIZE, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, &id);
    if (!t->handle) return -1;
    t->tid = (int)id;
    return 0;
}

static int start_os_thread(xrl_thread *t)
{
    if (create_os_thread(t)) return -1;
    ResumeThread(t->handle);
    return 0;
}

int xrl_threads_start_main(u64 entry, u64 guest_rsp, xrl_thread **out)
{
    xrl_thread *t = xrl_thread_new();
    if (!t) return -1;
    t->is_main = 1;
    t->resume[6] = entry;
    t->child_rsp = guest_rsp;
    if (start_os_thread(t)) { thread_unlink(t); free(t); return -1; }
    if (out) *out = t;
    return 0;
}

/* Start `fn` (a SysV function taking no args) on a fresh guest thread. When it
 * returns, a stub calls exit(rax). Used for the wineserver-thread hook. */
extern u64 xrl_exit_stub_addr(void);
int xrl_threads_start_fn(u64 fn, const char *name)
{
    u64 sz = 1ull << 20;
    u64 base = xrl_mem_stack_alloc(sz);
    if (!base) return -1;
    xrl_thread *t = xrl_thread_new();
    if (!t) return -1;
    t->guest_stack_lo = base; t->guest_stack_hi = base + sz;
    if (name) strncpy_s(t->name, sizeof t->name, name, _TRUNCATE);
    u64 sp = (base + sz - 64) & ~15ull;
    sp -= 8;
    *(u64 *)sp = xrl_exit_stub_addr();     /* return address; rsp+8 is 16-aligned at entry */
    t->is_main = 1;
    t->resume[6] = fn;
    t->child_rsp = sp;
    if (start_os_thread(t)) { thread_unlink(t); free(t); return -1; }
    return t->tid;
}

/* ----------------------------------------------------------------- clone */
/* Build the child's thread record from the parent's syscall frame, suspended (its tid is known and
 * published before it runs). `np` is the child's process: the parent's own for a thread, a new one for fork. */
int xrl_thread_clone_prepare(xrl_thread *p, xrl_proc *np, u64 flags, u64 child_stack, u64 tls, u64 resume_pc, xrl_thread **out)
{
    if (flags & LX_CLONE_SETTLS) {
        if (!G.fsgsbase && !G.tp_slot) { LOGE("clone: CLONE_SETTLS but neither FSGSBASE nor the TP slot is available"); return -LX_ENOSYS; }
    }
    xrl_thread *t = xrl_thread_new_in(np);
    if (!t) return p->proc->dying ? -LX_EAGAIN : -LX_ENOMEM;
    const u64 *fr = (const u64 *)p->frame;
    for (int i = 0; i < 7; i++) t->resume[i] = fr[i];
    /* B's musl passes an explicit child entry (7th arg): a call cannot return
     * twice, so the child starts at resume_pc instead of "returning" from it. */
    if (resume_pc) {
        if (!child_stack) { thread_unlink(t); free(t); return -LX_EINVAL; }
        t->resume[6] = resume_pc;
    }
    t->child_rsp = child_stack ? child_stack : (p->frame + 56);
    /* The child's stack is an ordinary (lazy) mmap to us (musl's pthread stacks
     * carry no MAP_STACK). The host kernel pushes exception frames there, which
     * a lazy page cannot take, so commit the whole mapping now (mem.c). */
    if (child_stack) xrl_mem_populate_containing(child_stack - 1);
    t->guest_tp = (flags & LX_CLONE_SETTLS) ? tls : p->guest_tp;
    t->sigmask = p->sigmask;
    if (np != p->proc) {   /* a forked process keeps its sigaltstack and its comm, a thread starts without */
        t->altstack = p->altstack;
        memcpy(t->name, p->name, sizeof t->name);
    }
    t->clone_flags = (u32)flags;
    t->guest_stack_lo = 0;
    if (create_os_thread(t)) {   /* suspended: the tid is known before the child runs */
        thread_unlink(t);
        free(t);
        return -LX_EAGAIN;
    }
    *out = t;
    return 0;
}

void xrl_thread_start(xrl_thread *t) { ResumeThread(t->handle); }

slong xrl_do_clone(u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls, u64 resume_pc)
{
    xrl_thread *p = xrl_cur();
    if (!p) return -LX_ENOSYS;
    if (!(flags & LX_CLONE_THREAD)) return xrl_proc_clone(flags, child_stack, ptid, ctid, tls, resume_pc);
    if (!(flags & LX_CLONE_VM)) return -LX_EINVAL;
    xrl_thread *t = NULL;
    slong r = xrl_thread_clone_prepare(p, p->proc, flags, child_stack, tls, resume_pc, &t);
    if (r) return r;
    if (flags & LX_CLONE_CHILD_CLEARTID) t->clear_child_tid = ctid;
    if (flags & LX_CLONE_PARENT_SETTID) xrl_ctu(ptid, &t->tid, 4);
    if (flags & LX_CLONE_CHILD_SETTID) xrl_ctu(ctid, &t->tid, 4);
    int tid = t->tid;
    xrl_thread_start(t);
    return tid;
}

/* ------------------------------------------------------------------ exit */
void xrl_wake_ctid(xrl_thread *t)
{
    if (t->clear_child_tid) {
        u32 z = 0;
        if (!xrl_ctu(t->clear_child_tid, &z, 4))
            xrl_futex_wake_addr(t->clear_child_tid, 0x7fffffff);
        t->clear_child_tid = 0;
    }
}

/* sig != 0: the process dies of that signal (the wait status says so; the init process's exit code stays
 * 128+sig as it always was) */
static slong do_exit(int code, int group, int sig)
{
    xrl_thread *t = xrl_cur();
    if (!t) ExitThread((DWORD)code);
    xrl_proc *p = t->proc;
    int last = (int)(p->nthreads - p->ndaemons) <= 1 && !t->daemon;
    if (t->daemon) { InterlockedDecrement(&p->ndaemons); t->daemon = 0; }
    int ends = group || last;
    int is_init = p->is_init;   /* p may be freed as soon as this thread's record is unlinked */
    if (ends && is_init) {
        InterlockedExchange(&G.exit_code, code);
        InterlockedExchange(&G.exiting, 1);
    }
    /* Linux order: musl's __unmapself unmaps the stack before exit, and the
     * CLEARTID wake happens in exit, so a joiner never sees the old mapping.
     * If ctid lived inside the unmapped region the store just fails, as on Linux. */
    if (t->defer_unmap_len) { xrl_mem_munmap(t->defer_unmap_addr, t->defer_unmap_len); t->defer_unmap_len = 0; }
    xrl_wake_ctid(t);
    if (t->timer) CloseHandle(t->timer);
    /* another process: it becomes a zombie (its fds close, its mm goes, the parent hears SIGCHLD, a vfork parent
     * runs again); this has to happen before our own record goes, it needs to know which thread is the caller */
    xrl_heapcheck("do_exit: start");
    if (ends && !is_init) xrl_proc_exit(p, sig ? sig : (code & 0xff) << 8, t);
    thread_unlink(t);
    xrl_heapcheck("do_exit: unlinked");
    xrl_thread_unbind();
    if (ends && is_init) SetEvent(G.exit_event);
    /* leave the record allocated: other threads may briefly hold pointers via tgkill lookup */
    ExitThread((DWORD)code);
    return 0;
}

slong xrl_do_exit(int code, int group) { return do_exit(code, group, 0); }
slong xrl_do_exit_sig(int sig) { return do_exit(128 + sig, 1, sig); }

/* A thread that ends because execve is replacing the image (or that exec.c terminated while it ran
 * guest code): no CLEARTID wake, no exit_group; just its records. */
static void thread_forget(xrl_thread *t, u64 teb)
{
    AcquireSRWLockExclusive(&g_ttab_lock);
    size_t i = (teb >> 12) & (XRL_TTAB_SIZE - 1);
    for (size_t n = 0; n < XRL_TTAB_SIZE; n++) {
        if (xrl_ttab[i].key == teb) { xrl_ttab[i].key = 1; break; }
        if (!xrl_ttab[i].key) break;
        i = (i + 1) & (XRL_TTAB_SIZE - 1);
    }
    ReleaseSRWLockExclusive(&g_ttab_lock);
    if (t->timer) { CloseHandle(t->timer); t->timer = NULL; }
    if (t->daemon) { t->daemon = 0; InterlockedDecrement(&t->proc->ndaemons); }
    thread_unlink(t);
}

void xrl_thread_reap(xrl_thread *t) { thread_forget(t, t->host_teb); }

void xrl_thread_die(xrl_thread *t)
{
    thread_forget(t, __readgsqword(0x30));
    ExitThread(0);
}

void xrl_threads_terminate_all(void)
{
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t; t = t->next) {
        if (t->handle && t->win_tid != GetCurrentThreadId()) TerminateThread(t->handle, 0);
    }
    ReleaseSRWLockShared(&G.thread_lock);
}

/* ------------------------------------------------------------ arch_prctl */
/* custom arch_prctl codes (in the 0x5800 range; not used by Linux) */
#define ARCH_XRL_GET_HOST_TEB 0x5801
#define ARCH_XRL_GET_FS_MODE  0x5802

slong xrl_sys_arch_prctl(int code, u64 addr)
{
    xrl_thread *t = xrl_cur();
    if (!t) return -LX_ENOSYS;
    switch (code) {
    case LX_ARCH_SET_FS:
        if (!G.fsgsbase && !G.tp_slot) return -LX_ENOSYS;
        if (addr >= 0x800000000000ull) return -LX_EPERM;   /* non-canonical */
        t->guest_tp = addr;
        tp_slot_set(addr);                          /* patched guests (the console path) */
        if (G.fsgsbase) _writefsbase_u64(addr);     /* unpatched guests, desktop only */
        return 0;
    case LX_ARCH_GET_FS:
        return xrl_ctu(addr, &t->guest_tp, 8);
    case LX_ARCH_SET_GS: {
        /*
         * GS base on Windows x64 is the host TEB and cannot be rebased from
         * user mode (the kernel restores it on every context switch). The only
         * supported value is the thread's own host TEB ("adoption"); see
         * notes-C.md section "GS / TEB". Anything else is refused loudly.
         */
        u64 teb = __readgsqword(0x30);
        if (addr == teb || addr == 0) { t->guest_gs = addr; return 0; }
        static volatile LONG warned;
        if (!InterlockedExchange(&warned, 1))
            LOGE("arch_prctl(ARCH_SET_GS, %llx): refused. Host GS base is the Windows TEB (%llx) and cannot be "
                 "rebased. The guest must adopt the host TEB (arch_prctl(0x5801) returns it). See notes-C.md.", addr, teb);
        return -LX_EPERM;
    }
    case LX_ARCH_GET_GS: {
        u64 teb = __readgsqword(0x30);
        return xrl_ctu(addr, &teb, 8);
    }
    case ARCH_XRL_GET_HOST_TEB: {
        u64 teb = __readgsqword(0x30);
        return xrl_ctu(addr, &teb, 8);
    }
    case ARCH_XRL_GET_FS_MODE: {
        /* bit0 = hardware FS base with fault re-arm, bit1 = TP slot gs:[XRL_TP_GS_OFFSET] */
        u64 m = (G.fsgsbase ? 1u : 0u) | (G.tp_slot ? 2u : 0u);
        return xrl_ctu(addr, &m, 8);
    }
    case LX_ARCH_GET_CPUID: return 1;      /* cpuid not faulting-capable: report enabled */
    case LX_ARCH_SET_CPUID: return -LX_ENODEV;
    default: return -LX_EINVAL;
    }
}

void xrl_threads_init(void)
{
    InitializeSRWLock(&G.thread_lock);
    G.next_tid = 1000;
    G.pid = (int)GetCurrentProcessId();   /* see create_os_thread */
    int cpuid[4];
    __cpuidex(cpuid, 7, 0);
    int has = (cpuid[1] & 1) != 0;   /* EBX bit 0: FSGSBASE */
    /* XRL_NO_FSGSBASE=1 (host env) behaves like the Xbox, which hides FSGSBASE:
     * lets the desktop test suite cover the console path. */
    char nofsb[4];
    if (GetEnvironmentVariableA("XRL_NO_FSGSBASE", nofsb, sizeof nofsb) && nofsb[0] == '1') has = 0;
    G.fsgsbase = 0;
    if (has) {
        __try {
            u64 v = _readfsbase_u64();
            _writefsbase_u64(v);
            G.fsgsbase = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { G.fsgsbase = 0; }
    }
    xrl_asm_fsb_flag = (u64)G.fsgsbase;
    G.tp_slot = claim_tp_slot();
    LOGI("threads: FSGSBASE %s (cpuid=%d); TP slot gs:[0x%x] %s -> guest thread pointer via %s",
         G.fsgsbase ? "usable" : "NOT usable", has, XRL_TP_GS_OFFSET, G.tp_slot ? "claimed" : "UNAVAILABLE",
         G.tp_slot ? (G.fsgsbase ? "TP slot (patched guests) + wrfsbase (unpatched)" : "TP slot (guests must be patched)")
                   : (G.fsgsbase ? "wrfsbase + VEH re-arm only" : "NOTHING (SET_FS will fail)"));
}

/* ----------------------------------------------------------------- prctl */
slong xrl_sys_prctl(int op, u64 a2, u64 a3, u64 a4, u64 a5)
{
    xrl_thread *t = xrl_cur();
    (void)a3; (void)a4; (void)a5;
    switch (op) {
    case LX_PR_SET_NAME: {
        char n[16] = {0};
        if (xrl_cfu(n, a2, 15)) return -LX_EFAULT;
        n[15] = 0;
        if (t) memcpy(t->name, n, 16);
        return 0;
    }
    case LX_PR_GET_NAME:
        return xrl_ctu(a2, t ? t->name : "xrl", 16);
    case LX_PR_SET_PDEATHSIG:
        if (a2 > LX_NSIG) return -LX_EINVAL;
        if (t) t->proc->pdeathsig = (int)a2;
        return 0;
    case LX_PR_GET_PDEATHSIG: { int v = t ? t->proc->pdeathsig : 0; return xrl_ctu(a2, &v, 4); }
    case LX_PR_SET_CHILD_SUBREAPER:
        if (t) t->proc->subreaper = a2 != 0;
        return 0;
    case LX_PR_GET_CHILD_SUBREAPER: { int v = t ? t->proc->subreaper : 0; return xrl_ctu(a2, &v, 4); }
    case LX_PR_SET_DUMPABLE:
    case LX_PR_SET_NO_NEW_PRIVS:
    case LX_PR_SET_TIMERSLACK:
    case LX_PR_SET_VMA:
        return 0;
    case LX_PR_GET_DUMPABLE: return 1;
    case LX_PR_GET_NO_NEW_PRIVS: return 0;
    case LX_PR_GET_TIMERSLACK: return 50000;
    default:
        return xrl_exec_prctl(op, a2, a3);
    }
}

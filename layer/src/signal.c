/*
 * signal.c - rt_sigaction/rt_sigprocmask/sigaltstack bookkeeping, the vectored
 * exception handler (FS-base re-arm, guest fault reporting) and tgkill.
 *
 * Signals are delivered with Linux rt_sigframes (see "signal delivery"
 * below): at syscall return, asynchronously to a thread running guest code,
 * and for faults in guest code that have a guest handler. A fault without a
 * handler still gets the crash report and 128+signo.
 */
#include "xrl_internal.h"
#include <intrin.h>
#include <immintrin.h>

/* Logging here: no CRT stream, no log lock (a faulting thread may hold either,
 * and a fatal signal ends the process before buffered output is written). One WriteFile per line to the process's stderr and the debugger. */
static void emerg(const char *fmt, ...)
{
    char line[600];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);
    line[n++] = '\n';
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)n, &w, NULL);
    line[n - 1] = 0;
    OutputDebugStringA(line);
}
#undef LOGE
#define LOGE(...) emerg("[E layer    ] " __VA_ARGS__)

#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
/* kernel32 exports it; the app-family headers do not declare it. The host links
 * kernel32.lib (see mem.c on VirtualAlloc). Whether the console's app container
 * lets us use it on our own threads is a run-time question. */
WINBASEAPI BOOL WINAPI SetThreadContext(HANDLE hThread, const CONTEXT *lpContext);
#endif

static lx_ksigaction g_act[LX_NSIG + 1];
static SRWLOCK g_sig_lock = SRWLOCK_INIT;
static PVOID g_veh;
volatile LONG64 xrl_fs_rearms;

static void sig_cpu_init(void);
void xrl_sig_init(void) { memset(g_act, 0, sizeof g_act); sig_cpu_init(); }

#define SIGBIT(s) (1ull << ((s) - 1))

slong xrl_sys_rt_sigaction(int sig, u64 uact, u64 uoact, u64 size)
{
    if (size != 8) return -LX_EINVAL;
    if (sig < 1 || sig > LX_NSIG) return -LX_EINVAL;
    lx_ksigaction na;
    if (uact) {
        if (sig == LX_SIGKILL || sig == LX_SIGSTOP) return -LX_EINVAL;
        if (xrl_cfu(&na, uact, sizeof na)) return -LX_EFAULT;
    }
    AcquireSRWLockExclusive(&g_sig_lock);
    lx_ksigaction old = g_act[sig];
    if (uact) g_act[sig] = na;
    ReleaseSRWLockExclusive(&g_sig_lock);
    if (uoact && xrl_ctu(uoact, &old, sizeof old)) return -LX_EFAULT;
    return 0;
}

slong xrl_sys_rt_sigprocmask(int how, u64 uset, u64 uoset, u64 size)
{
    xrl_thread *t = xrl_cur();
    if (size != 8 || !t) return -LX_EINVAL;
    u64 old = t->sigmask;
    if (uset) {
        u64 set;
        if (xrl_cfu(&set, uset, 8)) return -LX_EFAULT;
        set &= ~(SIGBIT(LX_SIGKILL) | SIGBIT(LX_SIGSTOP));
        switch (how) {
        case LX_SIG_BLOCK: t->sigmask |= set; break;
        case LX_SIG_UNBLOCK: t->sigmask &= ~set; break;
        case LX_SIG_SETMASK: t->sigmask = set; break;
        default: return -LX_EINVAL;
        }
    }
    if (uoset && xrl_ctu(uoset, &old, 8)) return -LX_EFAULT;
    return 0;
}

slong xrl_sys_sigaltstack(u64 uss, u64 uoss)
{
    xrl_thread *t = xrl_cur();
    if (!t) return -LX_EINVAL;
    lx_stack_t old = t->altstack;
    if (!old.ss_sp && !old.ss_size) old.ss_flags = LX_SS_DISABLE;
    if (uss) {
        lx_stack_t n;
        if (xrl_cfu(&n, uss, sizeof n)) return -LX_EFAULT;
        if (n.ss_flags & ~(LX_SS_DISABLE)) return -LX_EINVAL;
        if (!(n.ss_flags & LX_SS_DISABLE) && n.ss_size < 2048) return -LX_EINVAL;
        t->altstack = n;
        if (n.ss_flags & LX_SS_DISABLE) memset(&t->altstack, 0, sizeof t->altstack);
    }
    if (uoss && xrl_ctu(uoss, &old, sizeof old)) return -LX_EFAULT;
    return 0;
}

int xrl_sig_pending_for(xrl_thread *t)
{
    return (t->sigpending & ~t->sigmask) != 0;
}

extern void xrl_futex_interrupt(xrl_thread *t);

/* ======================================================= signal delivery
 *
 * A Linux rt_sigframe is built on the guest stack (or the sigaltstack) from
 * the interrupted register state, and the thread continues in the handler:
 *   - at syscall return (xrl_sig_at_return): the thread itself, before it goes
 *     back to guest code; xrl_sig_jump (thunks.asm) loads the handler state;
 *   - asynchronously (try_async): another thread suspends it while it runs
 *     guest code and rewrites its context (Wine's SIGUSR1 thread suspend);
 *   - for a fault in guest code (xrl_veh): from the exception's context.
 * rt_sigreturn reads the frame back. A syscall that returned -EINTR because of
 * an SA_RESTART handler is re-run by rt_sigreturn (the frame carries its
 * number and arguments), which is Linux's -ERESTARTSYS. */

#define SIGBIT_OK(s) ((s) >= 1 && (s) <= LX_NSIG)
#define UC_FP_XSTATE 1
#define UC_SIGCONTEXT_SS 2
#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u
#define XS_LEGACY 512u
#define XS_AVX_END 832u          /* legacy area + XSAVE header + AVX (YMM_Hi128) */
#define SIGPRIV_MAGIC 0x58524c5349475256ull
#define SI_USER 0
#define SI_TKILL (-6)
#define EFLAGS_USER 0x40fd5ull   /* CF PF AF ZF SF TF DF OF AC: what rt_sigreturn may set */

typedef struct lx_rt_sigframe { u64 pretcode; lx_ucontext uc; lx_siginfo info; } lx_rt_sigframe;
C_ASSERT(sizeof(lx_ucontext) == 304 && sizeof(lx_rt_sigframe) == 440);
/* Layer-private, right after the Linux frame: the syscall to re-run. */
typedef struct sigpriv { u64 magic; s64 nr; s64 a[6]; u64 retaddr; u64 pad; } sigpriv;

static u64 g_xmask;              /* XCR0 & (x87|SSE|AVX); 0 = no XSAVE, use fxsave */
static u32 g_mxcsr_mask = 0xffbf;

static void sig_cpu_init(void)
{
    int r[4];
    __cpuid(r, 1);
    if ((r[2] >> 27) & 1) g_xmask = _xgetbv(0) & 7;
    __declspec(align(64)) unsigned char fx[512];
    memset(fx, 0, sizeof fx);
    _fxsave64(fx);
    u32 m = *(u32 *)(fx + 28);
    if (m) g_mxcsr_mask = m;
}

static xrl_sigregs *regs_of(xrl_thread *t)
{
    if (!t->sig_regs) t->sig_regs = (xrl_sigregs *)_aligned_malloc(sizeof(xrl_sigregs), 64);
    return t->sig_regs;
}

/* The FP state a handler starts with: XSTATE_BV = 0 puts every component in
 * its init state (xrstor still loads MXCSR); FCW for fxrstor. */
static void fp_init_image(xrl_sigregs *r)
{
    memset(r->xs, 0, XS_AVX_END);
    *(u16 *)(r->xs + 0) = 0x37f;
    *(u32 *)(r->xs + 24) = 0x1f80;
}

static int restartable(slong nr)
{
    switch (nr) {
    case 0: case 1: case 2: case 16: case 17: case 18: case 19: case 20: case 43: case 44: case 45: case 46:
    case 47: case 61: case 72: case 73: case 202: case 247: case 257: case 288: case 295: case 296:
        return 1;   /* read write open ioctl pread pwrite readv writev accept sendto recvfrom sendmsg recvmsg
                     * wait4 fcntl flock futex waitid openat accept4 preadv pwritev */
    default:
        return 0;   /* poll, select, epoll_wait, nanosleep, pause, ...: -EINTR, as on Linux */
    }
}

static int default_ignored(int sig)
{
    return sig == LX_SIGCHLD || sig == LX_SIGCONT || sig == 23 /*SIGURG*/ || sig == 28 /*SIGWINCH*/ ||
           (sig >= 19 && sig <= 22) /* stop signals: no job control here */;
}

/* Take the lowest pending, unblocked signal whose action is a handler; signals
 * whose action became SIG_IGN are dropped, SIG_DFL ones terminate. 0 = none. */
static int take_signal(xrl_thread *t, lx_ksigaction *act, lx_siginfo *si)
{
    for (;;) {
        u64 p = t->sigpending, d = p & ~t->sigmask;
        if (!d) return 0;
        unsigned long b;
        _BitScanForward64(&b, d);
        if (InterlockedCompareExchange64((volatile LONG64 *)&t->sigpending, (LONG64)(p & ~(1ull << b)), (LONG64)p) != (LONG64)p)
            continue;
        int sig = (int)b + 1;
        int from_kill = (int)((InterlockedAnd64((volatile LONG64 *)&t->sig_from_kill, ~(LONG64)(1ull << b)) >> b) & 1);
        AcquireSRWLockExclusive(&g_sig_lock);
        *act = g_act[sig];
        if (act->handler > LX_SIG_IGN && (act->flags & LX_SA_RESETHAND)) g_act[sig].handler = LX_SIG_DFL;
        ReleaseSRWLockExclusive(&g_sig_lock);
        if (act->handler == LX_SIG_IGN) continue;
        if (act->handler == LX_SIG_DFL) {
            if (default_ignored(sig)) continue;
            LOGE("signal %d delivered to tid %d with the default action: terminating (128+%d)", sig, t->tid, sig);
            xrl_do_exit(128 + sig, 1);
        }
        memset(si, 0, sizeof *si);
        si->si_signo = sig;
        si->si_code = from_kill ? SI_USER : SI_TKILL;
        si->u.kill.si_pid = G.pid;
        si->u.kill.si_uid = 1000;
        return sig;
    }
}

static int on_altstack(const xrl_thread *t, u64 sp)
{
    return t->altstack.ss_size && sp > t->altstack.ss_sp && sp - t->altstack.ss_sp <= t->altstack.ss_size;
}

/* Write the frame for `sig` from the interrupted state in `r` (its FP state in
 * r->xs; a fault's trapno/err/cr2 in r->pad[0..2]), then turn `r` into the
 * handler-entry state. `below`: the frame must lie under this address (a
 * fault: the exception dispatcher's own frames are on the same stack). */
static int build_frame(xrl_thread *t, int sig, const lx_ksigaction *a, const lx_siginfo *si, xrl_sigregs *r,
                       const sigpriv *rs, u64 below)
{
    u64 sp = r->rsp;
    int was_on_alt = on_altstack(t, sp);
    if ((a->flags & LX_SA_ONSTACK) && t->altstack.ss_size && !was_on_alt) sp = t->altstack.ss_sp + t->altstack.ss_size;
    else {
        sp -= 128;   /* red zone */
        if (below && below < sp) sp = below;
    }
    u32 fsz = r->xmask ? XS_AVX_END : XS_LEGACY;
    u64 fp = (sp - fsz - 4) & ~63ull;
    u64 frame = ((fp - sizeof(sigpriv) - sizeof(lx_rt_sigframe)) & ~15ull) - 8;
    xrl_mem_populate(frame, sp - frame);

    unsigned char fpb[XS_AVX_END + 4];
    memcpy(fpb, r->xs, fsz);
    u32 *sw = (u32 *)(fpb + 464);   /* fxsave's software-available bytes: Linux's _fpx_sw_bytes */
    memset(sw, 0, 48);
    if (r->xmask) {
        sw[0] = FP_XSTATE_MAGIC1;
        sw[1] = fsz + 4;            /* extended_size */
        *(u64 *)(sw + 2) = r->xmask;
        sw[4] = fsz;                /* xstate_size */
        *(u32 *)(fpb + fsz) = FP_XSTATE_MAGIC2;
    }
    if (xrl_ctu(fp, fpb, fsz + (r->xmask ? 4 : 0))) return -1;

    lx_rt_sigframe f;
    memset(&f, 0, sizeof f);
    f.pretcode = a->restorer;
    f.uc.uc_flags = UC_SIGCONTEXT_SS | (r->xmask ? UC_FP_XSTATE : 0);
    f.uc.uc_stack = t->altstack;
    f.uc.uc_stack.ss_flags = !t->altstack.ss_size ? LX_SS_DISABLE : was_on_alt ? LX_SS_ONSTACK : 0;
    memcpy(&f.uc.uc_mcontext, r, 18 * sizeof(u64));
    f.uc.uc_mcontext.cs = 0x33;
    f.uc.uc_mcontext.ss = 0x2b;
    f.uc.uc_mcontext.trapno = r->pad[0];
    f.uc.uc_mcontext.err = r->pad[1];
    f.uc.uc_mcontext.cr2 = r->pad[2];
    f.uc.uc_mcontext.oldmask = t->sigmask;
    f.uc.uc_mcontext.fpstate = fp;
    f.uc.uc_sigmask = t->sigmask;
    f.info = *si;
    if (xrl_ctu(frame, &f, sizeof f)) return -1;
    sigpriv p;
    memset(&p, 0, sizeof p);
    if (rs) { p = *rs; p.magic = SIGPRIV_MAGIC ^ frame; }
    if (xrl_ctu(frame + sizeof f, &p, sizeof p)) return -1;

    u64 nm = t->sigmask | a->mask;
    if (!(a->flags & LX_SA_NODEFER)) nm |= SIGBIT(sig);
    t->sigmask = nm & ~(SIGBIT(LX_SIGKILL) | SIGBIT(LX_SIGSTOP));
    r->rip = a->handler;
    r->rsp = frame;
    r->rdi = (u64)sig;
    r->rsi = frame + offsetof(lx_rt_sigframe, info);
    r->rdx = frame + offsetof(lx_rt_sigframe, uc);
    r->rax = 0;
    r->eflags &= ~0x500ull;   /* DF, TF */
    fp_init_image(r);
    return 0;
}

/* The thread's own FP state (syscall return: the guest called the gateway, so
 * the vector registers are caller-saved anyway). */
static void capture_fp(xrl_sigregs *r)
{
    r->xmask = g_xmask;
    if (g_xmask) { memset(r->xs + XS_LEGACY, 0, 64); _xsave64(r->xs, g_xmask); }
    else _fxsave64(r->xs);
}

static void frame_failed(xrl_thread *t, int sig)
{
    LOGE("signal %d: cannot write a signal frame for tid %d (bad stack): terminating", sig, t->tid);
    xrl_do_exit(128 + LX_SIGSEGV, 1);
}

/* End of xrl_syscall_dispatch. `from`: the state to interrupt when it is not
 * the gateway frame (rt_sigreturn's restored context). Sets t->sig_jump. */
void xrl_sig_at_return(xrl_thread *t, slong nr, const slong *args, const u64 *gframe, slong res, xrl_sigregs *from)
{
    lx_ksigaction a;
    lx_siginfo si;
    int sig = take_signal(t, &a, &si);
    if (!sig) return;
    xrl_sigregs *r = regs_of(t);
    if (!from) {
        memset(r, 0, offsetof(xrl_sigregs, xs));
        r->r15 = gframe[0]; r->r14 = gframe[1]; r->r13 = gframe[2]; r->r12 = gframe[3];
        r->rbx = gframe[4]; r->rbp = gframe[5]; r->rip = gframe[6]; r->rsp = (u64)(gframe + 7);
        r->rax = (u64)res;
        r->rdi = (u64)nr; r->rsi = (u64)args[0]; r->rdx = (u64)args[1]; r->rcx = (u64)args[2];
        r->r8 = (u64)args[3]; r->r9 = (u64)args[4]; r->r10 = (u64)args[3];
        r->eflags = 0x202;
        capture_fp(r);
    }
    r->pad[0] = r->pad[1] = r->pad[2] = 0;
    sigpriv rs, *prs = NULL;
    if (res == -LX_EINTR && (a.flags & LX_SA_RESTART) && restartable(nr)) {
        memset(&rs, 0, sizeof rs);
        rs.nr = nr;
        memcpy(rs.a, args, sizeof rs.a);
        rs.retaddr = r->rip;
        prs = &rs;
    }
    if (build_frame(t, sig, &a, &si, r, prs, 0)) frame_failed(t, sig);
    t->sig_jump = (u64)r;
}

slong xrl_syscall_redo(slong nr, const slong *a);   /* syscall.c */

/* rt_sigreturn(&ucontext) (musl's __restore_rt passes it, see patches/musl).
 * Loads the frame's state into the thread's sigregs and sets sig_jump. */
slong xrl_sig_return(xrl_thread *t, u64 uc_addr)
{
    lx_ucontext uc;
    if (!t || xrl_cfu(&uc, uc_addr, sizeof uc)) {
        LOGE("rt_sigreturn: no signal frame at %llx: terminating", uc_addr);
        xrl_do_exit(128 + LX_SIGSEGV, 1);
        return -LX_EFAULT;
    }
    xrl_sigregs *r = regs_of(t);
    memset(r, 0, offsetof(xrl_sigregs, xs));
    memcpy(r, &uc.uc_mcontext, 18 * sizeof(u64));
    r->eflags = (r->eflags & EFLAGS_USER) | 0x202;
    fp_init_image(r);
    r->xmask = g_xmask;                            /* no usable FP state: init state */
    u64 fp = uc.uc_mcontext.fpstate;
    if (fp && !xrl_cfu(r->xs, fp, XS_LEGACY)) {
        const u32 *sw = (const u32 *)(r->xs + 464);
        u64 *hdr = (u64 *)(r->xs + XS_LEGACY);
        memset(hdr, 0, 64);
        hdr[0] = 3;                                /* x87 + SSE from the legacy area */
        if (g_xmask && (uc.uc_flags & UC_FP_XSTATE) && sw[0] == FP_XSTATE_MAGIC1 && sw[4] >= XS_AVX_END &&
            !xrl_cfu(r->xs + XS_LEGACY, fp + XS_LEGACY, XS_AVX_END - XS_LEGACY)) {
            hdr[0] &= g_xmask;                     /* XSTATE_BV as saved */
            memset(hdr + 1, 0, 56);                /* XCOMP_BV = 0 (standard format), reserved */
        } else hdr[0] = 3;
        *(u32 *)(r->xs + 24) &= g_mxcsr_mask;      /* reserved MXCSR bits #GP in (f)xrstor */
        if (!g_xmask) r->xmask = 0;
    }
    t->sigmask = uc.uc_sigmask & ~(SIGBIT(LX_SIGKILL) | SIGBIT(LX_SIGSTOP));
    if (uc.uc_stack.ss_flags & LX_SS_DISABLE) memset(&t->altstack, 0, sizeof t->altstack);
    else if (uc.uc_stack.ss_size >= 2048) { t->altstack = uc.uc_stack; t->altstack.ss_flags = 0; }

    /* an SA_RESTART handler interrupted a blocking syscall: run it again */
    u64 frame = uc_addr - 8;
    sigpriv p;
    slong nr = 15, args[6] = {0};
    if (!xrl_cfu(&p, frame + sizeof(lx_rt_sigframe), sizeof p) && p.magic == (SIGPRIV_MAGIC ^ frame) &&
        r->rip == p.retaddr && (slong)r->rax == -LX_EINTR) {
        nr = p.nr;
        memcpy(args, p.a, sizeof args);
        r->rax = (u64)xrl_syscall_redo(nr, args);
    }
    t->sig_jump = (u64)r;
    if (t->sigpending & ~t->sigmask) xrl_sig_at_return(t, nr, args, NULL, (slong)r->rax, r);
    return (slong)r->rax;
}

/* ---- asynchronous delivery: suspend the target while it runs guest code */
static HANDLE g_pump_ev;
static INIT_ONCE g_pump_once = INIT_ONCE_STATIC_INIT;

/* Fill `r` from a Windows context (FP state as an XSAVE image). */
static void regs_from_context(xrl_sigregs *r, const CONTEXT *c, void *ymm, DWORD ylen)
{
    memset(r, 0, offsetof(xrl_sigregs, xs));
    r->r8 = c->R8; r->r9 = c->R9; r->r10 = c->R10; r->r11 = c->R11; r->r12 = c->R12; r->r13 = c->R13;
    r->r14 = c->R14; r->r15 = c->R15; r->rdi = c->Rdi; r->rsi = c->Rsi; r->rbp = c->Rbp; r->rbx = c->Rbx;
    r->rdx = c->Rdx; r->rax = c->Rax; r->rcx = c->Rcx; r->rsp = c->Rsp; r->rip = c->Rip; r->eflags = c->EFlags;
    memset(r->xs, 0, XS_AVX_END);
    memcpy(r->xs, &c->FltSave, XS_LEGACY);
    r->xmask = g_xmask;
    if (g_xmask) {
        int avx = ymm && ylen >= 256;
        *(u64 *)(r->xs + XS_LEGACY) = 3 | (avx ? 4 : 0);
        if (avx) memcpy(r->xs + 576, ymm, 256);
    }
}

/* The handler-entry state in `r` back into the Windows context. */
static void regs_to_context(CONTEXT *c, const xrl_sigregs *r, void *ymm, DWORD ylen)
{
    c->Rip = r->rip; c->Rsp = r->rsp; c->Rdi = r->rdi; c->Rsi = r->rsi; c->Rdx = r->rdx; c->Rax = r->rax;
    c->EFlags = (DWORD)r->eflags;
    memcpy(&c->FltSave, r->xs, XS_LEGACY);
    if (ymm) memset(ymm, 0, ylen);
}

/* 1 delivered (or nothing left), 0 not now (layer or host code: retry), -1 cannot */
static int try_async(xrl_thread *t)
{
    if (!t->handle || t->win_tid == GetCurrentThreadId()) return -1;
    DWORD len = 0;
    DWORD flags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT | ((g_xmask & 4) ? CONTEXT_XSTATE : 0);
    InitializeContext(NULL, flags, NULL, &len);
    void *buf = malloc(len ? len : sizeof(CONTEXT) + 64);
    CONTEXT *c = NULL;
    if (!buf || !InitializeContext(buf, flags, &c, &len)) { free(buf); return 0; }
    if (flags & CONTEXT_XSTATE) SetXStateFeaturesMask(c, XSTATE_MASK_AVX);
    if (SuspendThread(t->handle) == (DWORD)-1) { free(buf); return -1; }
    int done = 0;
    if (GetThreadContext(t->handle, c) && !t->in_sys && t->teb_guest_live && !t->sig_jump &&
        xrl_mem_guest_ctx(c->Rip, c->Rsp) == 1) {
        lx_ksigaction a;
        lx_siginfo si;
        int sig = take_signal(t, &a, &si);
        done = 1;
        if (sig) {
            __declspec(align(64)) xrl_sigregs r;
            DWORD ylen = 0;
            void *ymm = (flags & CONTEXT_XSTATE) ? LocateXStateFeature(c, XSTATE_AVX, &ylen) : NULL;
            regs_from_context(&r, c, ymm, ylen);
            if (build_frame(t, sig, &a, &si, &r, NULL, 0)) {
                LOGE("signal %d: cannot write a signal frame for tid %d (bad stack): dropped", sig, t->tid);
            } else {
                regs_to_context(c, &r, ymm, ylen);
                c->ContextFlags = flags;
                if (!SetThreadContext(t->handle, c)) {
                    LOGE("signal %d: SetThreadContext(tid %d) failed (%lu)", sig, t->tid, GetLastError());
                    done = -1;
                }
            }
        }
    }
    ResumeThread(t->handle);
    free(buf);
    return done;
}

/* Retries threads that were in layer/host code when their signal arrived (a
 * thread in a syscall gets it at syscall return instead). */
static DWORD WINAPI sig_pump(LPVOID p)
{
    (void)p;
    int busy = 0;
    for (;;) {
        WaitForSingleObject(g_pump_ev, busy ? 1 : INFINITE);
        busy = 0;
        AcquireSRWLockShared(&G.thread_lock);
        for (xrl_thread *t = G.threads; t; t = t->next)
            if (t->tid && !t->in_sys && (t->sigpending & ~t->sigmask) && try_async(t) == 0) busy = 1;
        ReleaseSRWLockShared(&G.thread_lock);
    }
}

static BOOL CALLBACK pump_init(PINIT_ONCE io, PVOID prm, PVOID *ctx)
{
    (void)io; (void)prm; (void)ctx;
    g_pump_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE h = CreateThread(NULL, 64 << 10, sig_pump, NULL, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}

/* Queue `sig` for `t` and get it delivered. */
static slong send_signal(xrl_thread *t, int sig, int from_kill)
{
    AcquireSRWLockShared(&g_sig_lock);
    lx_ksigaction a = g_act[sig];
    ReleaseSRWLockShared(&g_sig_lock);
    int blocked = (t->sigmask & SIGBIT(sig)) != 0;
    if (sig != LX_SIGKILL && sig != LX_SIGSTOP) {
        if (a.handler == LX_SIG_IGN) return 0;
        if (a.handler == LX_SIG_DFL && default_ignored(sig)) return 0;
    }
    if (sig == LX_SIGKILL || (a.handler == LX_SIG_DFL && !blocked)) {
        LOGE("signal %d to tid %d with the default action: terminating process (128+%d)", sig, t->tid, sig);
        xrl_do_exit(128 + sig, 1);
    }
    if (from_kill) InterlockedOr64((volatile LONG64 *)&t->sig_from_kill, (LONG64)SIGBIT(sig));
    InterlockedOr64((volatile LONG64 *)&t->sigpending, (LONG64)SIGBIT(sig));
    if (blocked) return 0;
    /* a thread waiting in the layer: the syscall returns -EINTR and the signal
     * is delivered on the way out */
    if (t->sig_ev) SetEvent(t->sig_ev);
    xrl_futex_interrupt(t);
    xrl_io_notify();
    if (t->win_tid != GetCurrentThreadId() && !t->in_sys && try_async(t) == 0) {
        InitOnceExecuteOnce(&g_pump_once, pump_init, NULL, NULL);
        SetEvent(g_pump_ev);
    }
    return 0;
}

slong xrl_sys_tgkill(int tgid, int tid, int sig)
{
    if (tgid != G.pid && tgid != -1) return -LX_ESRCH;
    xrl_thread *t = xrl_thread_by_tid(tid);
    if (!t) return -LX_ESRCH;
    if (sig == 0) return 0;
    if (!SIGBIT_OK(sig)) return -LX_EINVAL;
    return send_signal(t, sig, 0);
}

/* kill(getpid(), sig): the main thread unless it blocks the signal, else any
 * thread that does not */
slong xrl_sig_kill_process(int sig)
{
    if (sig == 0) return 0;
    if (!SIGBIT_OK(sig)) return -LX_EINVAL;
    xrl_thread *pick = NULL, *main_t = NULL;
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t; t = t->next) {
        if (t->is_main) main_t = t;
        if (!(t->sigmask & SIGBIT(sig)) && (!pick || t->is_main)) pick = t;
    }
    if (!pick) pick = main_t ? main_t : G.threads;
    ReleaseSRWLockShared(&G.thread_lock);
    return pick ? send_signal(pick, sig, 1) : -LX_ESRCH;
}

/* A fault in guest code with a guest handler for its signal: deliver it from
 * the exception context (Wine's segv/trap/fpe handlers turn it into a Windows
 * exception, as on Linux). 1 = the context now enters the handler. */
static int deliver_fault(xrl_thread *t, PEXCEPTION_POINTERS ep, int sig, int code, u64 addr, u64 trapno, u64 err)
{
    AcquireSRWLockShared(&g_sig_lock);
    lx_ksigaction a = g_act[sig];
    ReleaseSRWLockShared(&g_sig_lock);
    if (a.handler <= LX_SIG_IGN || (t->sigmask & SIGBIT(sig))) return 0;   /* Linux: blocked/ignored fault = death */
    if (a.flags & LX_SA_RESETHAND) {
        AcquireSRWLockExclusive(&g_sig_lock);
        g_act[sig].handler = LX_SIG_DFL;
        ReleaseSRWLockExclusive(&g_sig_lock);
    }
    CONTEXT *c = ep->ContextRecord;
    DWORD ylen = 0;
    void *ymm = (c->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE ? LocateXStateFeature(c, XSTATE_AVX, &ylen) : NULL;
    __declspec(align(64)) xrl_sigregs r;
    regs_from_context(&r, c, ymm, ylen);
    if (sig == LX_SIGTRAP && ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) r.rip++;   /* Linux: after the int3 */
    r.pad[0] = trapno;
    r.pad[1] = err;
    r.pad[2] = sig == LX_SIGSEGV ? addr : 0;
    lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = code;
    si.u.fault.si_addr = addr;
    /* the exception dispatcher's frames are on this stack below the fault: stay under them */
    u64 here = (u64)_AddressOfReturnAddress();
    u64 below = here < c->Rsp && c->Rsp - here < (1ull << 20) ? (here - 4096) & ~15ull : 0;
    if (build_frame(t, sig, &a, &si, &r, NULL, below)) return 0;
    regs_to_context(c, &r, ymm, ylen);
    return 1;
}

slong xrl_sys_rt_sigreturn(void)
{
    LOGE("rt_sigreturn reached the syscall table (xrl_syscall_dispatch handles it)");
    return -LX_ENOSYS;
}

slong xrl_sys_rt_sigsuspend(u64 mask, u64 size) { (void)mask; (void)size; return -LX_ENOSYS; }
slong xrl_sys_rt_sigtimedwait(u64 set, u64 info, u64 ts, u64 size) { (void)set; (void)info; (void)ts; (void)size; return -LX_ENOSYS; }

/* =============================================================== VEH */
static const char *exc_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    default: return "?";
    }
}

static int host_stack_fault(const CONTEXT *c)
{
    u64 base = __readgsqword(0x08), limit = __readgsqword(0x10);
    return c->Rsp >= limit && c->Rsp < base;
}



static void crash_report(PEXCEPTION_POINTERS ep, xrl_thread *t)
{
    CONTEXT *c = ep->ContextRecord;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    /* everything essential in the first line: the rest may not make it (stack) */
    LOGE("=== UNHANDLED GUEST FAULT tid %d code %08lx rip %016llx addr %016llx (%llu) rsp %016llx teb-stack [%016llx,%016llx) ===",
         t->tid, r->ExceptionCode, c->Rip, r->NumberParameters >= 2 ? (u64)r->ExceptionInformation[1] : 0ull,
         r->NumberParameters >= 1 ? (u64)r->ExceptionInformation[0] : 0ull, c->Rsp, __readgsqword(0x10), __readgsqword(0x08));
    LOGE("%s (0x%08lx) at rip=%016llx  fault_addr=%016llx (%s)", exc_name(r->ExceptionCode), r->ExceptionCode, c->Rip,
         r->NumberParameters >= 2 ? (u64)r->ExceptionInformation[1] : 0ull,
         r->NumberParameters >= 1 ? (r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "exec") : "-");
    LOGE("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    LOGE("rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    LOGE("r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
    LOGE("r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c->R12, c->R13, c->R14, c->R15);
    LOGE("fs_base(guest tp)=%016llx fsbase_now=%016llx eflags=%08lx", t->guest_tp, G.fsgsbase ? (u64)_readfsbase_u64() : 0ull, c->EFlags);
    u8 code[16] = {0};
    if (!xrl_cfu(code, c->Rip, 16))
        LOGE("code @rip: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11]);
    for (int i = 0; i < 8; i++) {
        u64 v;
        if (xrl_cfu(&v, c->Rsp + 8 * i, 8)) break;
        LOGE("[rsp+%02x] %016llx", 8 * i, v);
    }
    size_t cap = 64 * 1024;
    char *maps = (char *)malloc(cap);
    if (maps) {
        size_t n = xrl_mem_format_maps(maps, cap - 1);
        maps[n] = 0;
        /* log the VMAs containing rip and the fault address */
        u64 want[2] = { c->Rip, r->NumberParameters >= 2 ? (u64)r->ExceptionInformation[1] : 0 };
        for (int k = 0; k < 2; k++) {
            for (char *p = maps; *p;) {
                char *nl = strchr(p, '\n');
                u64 s = 0, e = 0;
                if (sscanf(p, "%llx-%llx", &s, &e) == 2 && want[k] >= s && want[k] < e) {
                    if (nl) *nl = 0;
                    LOGE("%s vma: %s", k ? "fault_addr" : "rip", p);
                    if (nl) *nl = '\n';
                }
                if (!nl) break;
                p = nl + 1;
            }
        }
        free(maps);
    }
}

/* XRL_TRACE_FAULTS=1: one line per hardware exception, including the ones in
 * layer/host code that are left to SEH (expected for xrl_cfu probes; fatal
 * and otherwise silent when no __try covers them). */
static int g_trace_faults;
static void trace_fault(PEXCEPTION_POINTERS ep, xrl_thread *t)
{
    CONTEXT *c = ep->ContextRecord;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)c->Rip, &mod) && mod) {
        GetModuleFileNameA(mod, name, sizeof name);
        const char *b = strrchr(name, '\\');
        if (b) memmove(name, b + 1, strlen(b));
    }
    LOGW("veh: %s rip=%016llx (%s+%llx) addr=%016llx tid=%d in_sys=%d host_stack=%d rsp=%016llx",
         exc_name(r->ExceptionCode), c->Rip, name, mod ? c->Rip - (u64)mod : 0ull,
         r->NumberParameters >= 2 ? (u64)r->ExceptionInformation[1] : 0ull,
         t ? t->tid : -1, t ? t->in_sys : -1, host_stack_fault(c), c->Rsp);
}

static LONG CALLBACK xrl_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    DWORD code = r->ExceptionCode;
    /* Only hardware-ish codes concern us. */
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_BREAKPOINT: case EXCEPTION_SINGLE_STEP:
    case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_INT_OVERFLOW: case EXCEPTION_STACK_OVERFLOW:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    xrl_thread *t = xrl_cur();
    /* Lazy commit (mem.c): first touch of a reserved page in a lazy mapping, from
     * any thread or code (guest, layer, D3D reading a guest buffer). Host view
     * while we take locks and call VirtualAlloc. */
    if (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        u64 stk[2];
        int g = xrl_teb_enter_host(t);
        xrl_teb_stack_here(stk);   /* we may be on a guest stack (teb.c) */
        int ok = xrl_mem_lazy_fault((u64)r->ExceptionInformation[1], (int)r->ExceptionInformation[0]);
        xrl_teb_stack_back(stk);
        xrl_teb_restore(t, g);
        if (ok) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_trace_faults) trace_fault(ep, t);
    if (!t) return EXCEPTION_CONTINUE_SEARCH;
    /* Whose fault is it? Guest code runs with the guest TEB view live, from
     * executable guest memory, on a writable guest stack. (The TEB's stack range
     * cannot tell: in Wine's view it is Wine's stack, so every fault in Wine
     * code used to look like a host-stack fault and went unhandled.) Layer and
     * host code (the exception dispatcher included) is left to SEH. */
    {
        const CONTEXT *c = ep->ContextRecord;
        int guest = t->in_sys == 0 && t->teb_guest_live && xrl_mem_guest_ctx(c->Rip, c->Rsp) == 1;
        if (!guest) return EXCEPTION_CONTINUE_SEARCH;
    }
    /* A guest fault: the guest TEB view is live; the handler below is host code. */
    int was_guest = xrl_teb_enter_host(t);
    u64 stk[2];
    xrl_teb_stack_here(stk);

    /* Windows zeroes the FS base on context switches; the first guest access
     * afterwards faults. Restore from the authoritative copy and retry. */
    if (G.fsgsbase && t->guest_tp && code == EXCEPTION_ACCESS_VIOLATION) {
        if ((u64)_readfsbase_u64() != t->guest_tp) {
            _writefsbase_u64(t->guest_tp);
            InterlockedIncrement64(&xrl_fs_rearms);
            xrl_teb_stack_back(stk);
            xrl_teb_restore(t, was_guest);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    int sig = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ? LX_SIGSEGV :
              code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ? LX_SIGILL :
              code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ? LX_SIGTRAP :
              code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_DATATYPE_MISALIGNMENT ? LX_SIGBUS : LX_SIGFPE;
    {
        /* Linux's view of the fault: si_code, the x86 trap number and error code
         * (Wine's handlers read them from the sigcontext) */
        u64 addr = (u64)r->ExceptionAddress, trapno = 13, err = 0;
        int si_code = 0x80;   /* SI_KERNEL */
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
            if (r->NumberParameters >= 2) {
                u64 kind = r->ExceptionInformation[0];
                addr = (u64)r->ExceptionInformation[1];
                trapno = 14;
                err = 4 | (kind == 1 ? 2 : 0) | (kind == 8 ? 16 : 0);   /* user, write, instruction fetch */
                si_code = xrl_mem_is_mapped(addr) ? 2 /*SEGV_ACCERR*/ : 1 /*SEGV_MAPERR*/;
                if (code == EXCEPTION_IN_PAGE_ERROR) { sig = LX_SIGBUS; si_code = 2; /*BUS_ADRERR*/ }
            }
            break;
        case EXCEPTION_STACK_OVERFLOW: trapno = 14; err = 6; si_code = 1; addr = ep->ContextRecord->Rsp; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: trapno = 6; si_code = 1 /*ILL_ILLOPN*/; break;
        case EXCEPTION_PRIV_INSTRUCTION: trapno = 13; si_code = 0x80; addr = 0; break;
        case EXCEPTION_BREAKPOINT: trapno = 3; si_code = 0x80; break;
        case EXCEPTION_SINGLE_STEP: trapno = 1; si_code = 2 /*TRAP_TRACE*/; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: trapno = 0; si_code = 1 /*FPE_INTDIV*/; break;
        case EXCEPTION_INT_OVERFLOW: trapno = 4; si_code = 2 /*FPE_INTOVF*/; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: trapno = 17; si_code = 1 /*BUS_ADRALN*/; break;
        default: trapno = 19; si_code = 0; break;   /* SIMD floating point */
        }
        if (deliver_fault(t, ep, sig, si_code, addr, trapno, err)) {
            xrl_teb_stack_back(stk);
            xrl_teb_restore(t, was_guest);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    crash_report(ep, t);
    xrl_do_exit(128 + sig, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

void xrl_sig_install_veh(void)
{
    char v[8];
    g_trace_faults = GetEnvironmentVariableA("XRL_TRACE_FAULTS", v, sizeof v) && v[0] == '1';
    g_veh = AddVectoredExceptionHandler(1, xrl_veh);
    LOGI("signal: VEH %s", g_veh ? "installed" : "FAILED");
}

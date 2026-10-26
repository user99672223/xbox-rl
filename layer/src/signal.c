/*
 * signal.c - rt_sigaction/rt_sigprocmask/sigaltstack bookkeeping, the vectored
 * exception handler (FS-base re-arm, guest fault reporting) and tgkill.
 *
 * STATUS: handlers are recorded and masks are tracked; hardware faults in guest
 * code are NOT yet turned into Linux signal frames (the VEH logs a full crash
 * report and terminates the process with 128+signo). Signal delivery
 * (synchronous faults -> SIGSEGV/SIGILL/SIGFPE/SIGBUS/SIGTRAP handlers, and
 * asynchronous tgkill-style delivery on syscall return) is the next work item;
 * see notes-C.md "Signals".
 */
#include "xrl_internal.h"
#include <intrin.h>
#include <immintrin.h>

static lx_ksigaction g_act[LX_NSIG + 1];
static SRWLOCK g_sig_lock = SRWLOCK_INIT;
static PVOID g_veh;
volatile LONG64 xrl_fs_rearms;

void xrl_sig_init(void) { memset(g_act, 0, sizeof g_act); }

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

slong xrl_sys_tgkill(int tgid, int tid, int sig)
{
    if (tgid != G.pid && tgid != -1) return -LX_ESRCH;
    xrl_thread *t = xrl_thread_by_tid(tid);
    if (!t) return -LX_ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > LX_NSIG) return -LX_EINVAL;
    AcquireSRWLockShared(&g_sig_lock);
    lx_ksigaction a = g_act[sig];
    ReleaseSRWLockShared(&g_sig_lock);
    if (a.handler == LX_SIG_IGN) return 0;
    if (a.handler == LX_SIG_DFL) {
        switch (sig) {
        case LX_SIGCHLD: case LX_SIGCONT: return 0;
        default:
            LOGE("tgkill: sig %d to tid %d with default action: terminating process (128+%d)", sig, tid, sig);
            xrl_do_exit(128 + sig, 1);
        }
    }
    LOGW("tgkill: sig %d -> tid %d recorded as pending but asynchronous delivery is NOT implemented yet", sig, tid);
    InterlockedOr64((volatile LONG64 *)&t->sigpending, (LONG64)SIGBIT(sig));
    xrl_futex_interrupt(t);
    xrl_io_notify();
    return 0;
}

slong xrl_sys_rt_sigreturn(void)
{
    LOGE("rt_sigreturn called but signal frames are not delivered by this layer yet");
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

/* Crash-path logging: no CRT stream, no log lock (the faulting thread may hold
 * either). One WriteFile per line to the process's stderr and the debugger. */
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
    if (t->in_sys > 0 || host_stack_fault(ep->ContextRecord)) return EXCEPTION_CONTINUE_SEARCH;   /* layer/host code: SEH handles it */
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

    crash_report(ep, t);
    int sig = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ? LX_SIGSEGV :
              code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ? LX_SIGILL :
              code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ? LX_SIGTRAP :
              code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_DATATYPE_MISALIGNMENT ? LX_SIGBUS : LX_SIGFPE;
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

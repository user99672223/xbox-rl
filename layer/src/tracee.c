/*
 * tracee.c - the process a ptrace tracer controls from outside (XRL_FORK_TASKS=1, see task.c).
 *
 * A program that is exec'd by a traced fork child runs as a second layer process. Its tracer is in another
 * OS process, so Linux's ptrace calls become messages on a named pipe (XRL_TRACEE=<pipe name> in the
 * environment of that process):
 *   tracee -> tracer   HELLO   the new image is loaded, nothing has run (the exec stop); a = entry, b = rsp
 *                      STOP    the tracee stopped (int3 in guest code = SIGTRAP); a = signal
 *   tracer -> tracee   PEEK a=addr b=len -> bytes,  POKE a=addr + bytes,  GETREGS -> 27 qwords (user_regs_struct),
 *                      SETREGS + 27 qwords,  MAPS -> the text of /proc/self/maps,  CONT,  DETACH,  KILL
 * Each request is answered with RESP (a = 0 or -errno, payload). While stopped the tracee's thread serves
 * requests; CONT resumes it with the registers as the tracer left them. At the exec stop the registers are the
 * image's entry point and stack; at an int3 they are the guest thread's CONTEXT. Not provided: single-step,
 * syscall stops, stops other than int3 and the exec stop, more than one stopped thread at a time.
 */
#include "xrl_internal.h"

enum { TR_HELLO = 1, TR_STOP, TR_RESP, TR_PEEK = 10, TR_POKE, TR_GETREGS, TR_SETREGS, TR_CONT, TR_MAPS, TR_KILL, TR_DETACH, TR_GETFP, TR_SETFP };

int xrl_tr_send(HANDLE h, u32 op, u64 a, u64 b, const void *p, u32 len)
{
    u64 hdr[3] = { (u64)op | ((u64)len << 32), a, b };
    DWORD n;
    if (!WriteFile(h, hdr, sizeof hdr, &n, NULL) || n != sizeof hdr) return -1;
    if (len && (!WriteFile(h, p, len, &n, NULL) || n != len)) return -1;
    return 0;
}

static int read_all(HANDLE h, void *b, DWORD len)
{
    DWORD n;
    char *c = (char *)b;
    while (len) {
        if (!ReadFile(h, c, len, &n, NULL) || !n) return -1;
        c += n; len -= n;
    }
    return 0;
}

/* payload (malloc, may be NULL) is the caller's to free */
int xrl_tr_recv(HANDLE h, u32 *op, u64 *a, u64 *b, void **p, u32 *len)
{
    u64 hdr[3];
    if (read_all(h, hdr, sizeof hdr)) return -1;
    *op = (u32)hdr[0]; *len = (u32)(hdr[0] >> 32); *a = hdr[1]; *b = hdr[2];
    *p = NULL;
    if (*len) {
        if (*len > (16u << 20)) return -1;
        *p = malloc(*len);
        if (!*p || read_all(h, *p, *len)) { free(*p); *p = NULL; return -1; }
    }
    return 0;
}

static HANDLE g_tr;
static volatile LONG g_tr_on;
int xrl_tracee_active(void) { return g_tr_on; }

static int poke(u64 addr, const void *src, size_t n)
{
    DWORD old = 0, tmp;
    u64 lo = addr & ~(u64)(LX_PAGE - 1), hi = ALIGN_UP(addr + n, LX_PAGE);
    if (!VirtualProtect((void *)lo, (SIZE_T)(hi - lo), PAGE_EXECUTE_READWRITE, &old)) return -LX_EIO;
    int r = 0;
    __try { memcpy((void *)addr, src, n); } __except (EXCEPTION_EXECUTE_HANDLER) { r = -LX_EIO; }
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    VirtualProtect((void *)lo, (SIZE_T)(hi - lo), old, &tmp);
    return r;
}

/* SETREGS as Linux's genregs_set/putreg (arch/x86/kernel/ptrace.c) apply it to a 64-bit tracee: field by field
 * in user_regs_struct order, the first refused field ends the request with -EIO (the ones before it stay set);
 * selectors are u16 and must be 0 or RPL 3 (cs/ss not 0); eflags takes only FLAG_MASK (CF PF AF ZF SF TF DF OF
 * NT RF AC) from the tracer, the rest (IF, the reserved bit 1, ID, ...) stays the thread's; fs_base/gs_base
 * below TASK_SIZE_MAX. */
#define LX_FLAG_MASK 0x54dd5ull
#define LX_TASK_SIZE_MAX 0x7ffffffff000ull
static int bad_sel(u64 v) { return v != 0 && (v & 3) != 3; }
static slong set_regs(u64 regs[27], const u64 *n)
{
    for (int i = 0; i < 27; i++) {
        u64 v = n[i];
        switch (i) {
        case 17: case 20: v &= 0xffff; if (bad_sel(v) || v == 0) return -LX_EIO; break;            /* cs, ss */
        case 23: case 24: case 25: case 26: v &= 0xffff; if (bad_sel(v)) return -LX_EIO; break;   /* ds es fs gs */
        case 18: v = (regs[18] & ~LX_FLAG_MASK) | (v & LX_FLAG_MASK); break;
        case 21: case 22: if (v >= LX_TASK_SIZE_MAX) return -LX_EIO; break;
        }
        regs[i] = v;
    }
    return 0;
}

/* SETFPREGS as Linux's xfpregs_set: an MXCSR with a bit outside the CPU's MXCSR mask is refused (-EINVAL),
 * otherwise the 512 bytes are taken as they are. */
static u32 g_mxcsr_mask = 0xffbf;   /* the FXSAVE default when the CPU reports 0; set from the first stop's state */

/* answer requests until the tracer continues (or detaches/kills) the tracee. regs: user_regs_struct */
static void serve(u64 regs[27], u8 fp[512])
{
    for (;;) {
        u32 op, len; u64 a, b; void *p;
        if (xrl_tr_recv(g_tr, &op, &a, &b, &p, &len)) ExitProcess(137);   /* the tracer is gone: a tracee dies with it (PTRACE_O_EXITKILL's effect) */
        switch (op) {
        case TR_PEEK: {
            u32 n = (u32)(b > (1u << 20) ? (1u << 20) : b);
            u8 *buf = (u8 *)malloc(n ? n : 1);
            int e = xrl_cfu(buf, a, n);
            xrl_tr_send(g_tr, TR_RESP, (u64)(s64)e, 0, buf, e ? 0 : n);
            free(buf);
            break; }
        case TR_POKE: xrl_tr_send(g_tr, TR_RESP, (u64)(s64)poke(a, p, len), 0, NULL, 0); break;
        case TR_GETREGS: xrl_tr_send(g_tr, TR_RESP, 0, 0, regs, 27 * 8); break;
        case TR_SETREGS: xrl_tr_send(g_tr, TR_RESP, (u64)(len == 27 * 8 ? set_regs(regs, (const u64 *)p) : -LX_EINVAL), 0, NULL, 0); break;
        case TR_GETFP: xrl_tr_send(g_tr, TR_RESP, 0, 0, fp, 512); break;
        case TR_SETFP: {
            slong e = len != 512 ? -LX_EINVAL : (*(u32 *)((u8 *)p + 24) & ~g_mxcsr_mask) ? -LX_EINVAL : 0;
            if (!e) memcpy(fp, p, 512);
            xrl_tr_send(g_tr, TR_RESP, (u64)e, 0, NULL, 0);
            break; }
        case TR_MAPS: {
            size_t cap = 1 << 20; char *t = (char *)malloc(cap);
            size_t n = xrl_mem_format_maps(t, cap);
            xrl_tr_send(g_tr, TR_RESP, 0, 0, t, (u32)n);
            free(t);
            break; }
        case TR_KILL: ExitProcess(137);
        case TR_DETACH: InterlockedExchange(&g_tr_on, 0); /* fall through */
        case TR_CONT: xrl_tr_send(g_tr, TR_RESP, 0, 0, NULL, 0); free(p); return;
        default: xrl_tr_send(g_tr, TR_RESP, (u64)(s64)-LX_EIO, 0, NULL, 0); break;
        }
        free(p);
    }
}

/* boot.c: before the first guest instruction, if this process was started to be traced */
void xrl_tracee_boot(u64 *entry, u64 *sp)
{
    char name[128];
    if (!GetEnvironmentVariableA("XRL_TRACEE", name, sizeof name)) return;
    for (int i = 0; i < 200 && g_tr == NULL; i++) {
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) g_tr = h; else Sleep(50);
    }
    if (!g_tr) { LOGE("tracee: cannot reach the tracer's pipe %s", name); ExitProcess(2); }
    InterlockedExchange(&g_tr_on, 1);
    if (G.trace_syscalls >= 2) xrl_systrace_arm();
    u64 regs[27] = { 0 };
    regs[15] = 59; regs[16] = *entry; regs[17] = 0x33; regs[18] = 0x202; regs[19] = *sp; regs[20] = 0x2b;   /* orig_rax: Linux reports the execve syscall number (59) at the exec stop, confirmed on WSL2 */
    u8 fp[512] = { 0 };
    *(u16 *)fp = 0x37f; *(u32 *)(fp + 24) = 0x1f80; *(u32 *)(fp + 28) = 0xffff;   /* the state a new process starts with */
    xrl_tr_send(g_tr, TR_HELLO, *entry, *sp, NULL, 0);
    serve(regs, fp);
    *entry = regs[16]; *sp = regs[19];
    LOGW("tracee: continues at %llx, stack %llx", *entry, *sp);
}

/* The stop itself: report it, serve the tracer, write what it set back into c (the thread's saved CONTEXT). */
static void stop_and_serve(CONTEXT *c, xrl_thread *t, int sig)
{
    u64 r[27] = { 0 };
    if (c->FltSave.MxCsr_Mask) g_mxcsr_mask = c->FltSave.MxCsr_Mask;
    r[0] = c->R15; r[1] = c->R14; r[2] = c->R13; r[3] = c->R12; r[4] = c->Rbp; r[5] = c->Rbx; r[6] = c->R11; r[7] = c->R10;
    r[8] = c->R9; r[9] = c->R8; r[10] = c->Rax; r[11] = c->Rcx; r[12] = c->Rdx; r[13] = c->Rsi; r[14] = c->Rdi;
    r[15] = (u64)-1; r[16] = c->Rip + (sig == LX_SIGTRAP ? 1 : 0);   /* after the int3; a fault reports the faulting address */ r[17] = 0x33; r[18] = c->EFlags; r[19] = c->Rsp; r[20] = 0x2b; r[21] = t->guest_tp;
    {   /* the tracer sees original addresses, not code-cache addresses */
        u64 rev = xrl_dbt_reverse(r[16]);
        if (rev) { LOGW("tracee: rip %llx reverse-translated to %llx", r[16], rev); r[16] = rev; }
    }
    u8 fp[512];
    memcpy(fp, &c->FltSave, sizeof fp);   /* XMM_SAVE_AREA32 is the FXSAVE layout, as Linux's user_fpregs_struct */
    xrl_tr_send(g_tr, TR_STOP, (u64)sig, 0, NULL, 0);
    serve(r, fp);
    memcpy(&c->FltSave, fp, sizeof fp);
    c->MxCsr = c->FltSave.MxCsr;   /* the CONTEXT has two copies; NtContinue loads this one */
    if (r[17] != 0x33 || r[20] != 0x2b) LOGW("tracee: the tracer set cs=%llx ss=%llx: kept 0x33/0x2b (only 64-bit code runs here)", r[17], r[20]);
    c->R15 = r[0]; c->R14 = r[1]; c->R13 = r[2]; c->R12 = r[3]; c->Rbp = r[4]; c->Rbx = r[5]; c->R11 = r[6]; c->R10 = r[7];
    c->R9 = r[8]; c->R8 = r[9]; c->Rax = r[10]; c->Rcx = r[11]; c->Rdx = r[12]; c->Rsi = r[13]; c->Rdi = r[14];
    c->Rip = r[16]; c->EFlags = (DWORD)r[18]; c->Rsp = r[19];
    {   /* the tracer may send the thread into a translated image (dbt.c): NtContinue to a page that is not executable for the host fails, so go to the translation */
        u64 tr = xrl_dbt_translate(r[16]);
        if (tr) c->Rip = tr;
        LOGW("tracee: rip %llx -> %llx (translation %llx)", r[16], (u64)c->Rip, tr);
    }
    LOGW("tracee: resumes at rip=%llx rsp=%llx eflags=%llx rdi=%llx rsi=%llx fs_base=%llx mxcsr=%x cw=%x", r[16], r[19], r[18], r[14], r[13], r[21], c->MxCsr, *(u16 *)fp);
    {   MEMORY_BASIC_INFORMATION mi, ms;
        VirtualQuery((void *)c->Rip, &mi, sizeof mi); VirtualQuery((void *)c->Rsp, &ms, sizeof ms);
        LOGW("tracee: CONTEXT flags=%lx cs=%x ss=%x ds=%x es=%x fs=%x gs=%x eflags=%lx mxcsr=%x/%x rip-page prot=%lx state=%lx rsp-page prot=%lx state=%lx rax=%llx rcx=%llx rdx=%llx rbx=%llx rbp=%llx r8=%llx",
             c->ContextFlags, c->SegCs, c->SegSs, c->SegDs, c->SegEs, c->SegFs, c->SegGs, c->EFlags, c->MxCsr, c->FltSave.MxCsr, mi.Protect, mi.State, ms.Protect, ms.State, c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rbp, c->R8);
    }
    if (r[21] != t->guest_tp) t->guest_tp = r[21];
}

/* Where a stopped tracee waits for its tracer. On Linux a ptrace-stopped thread keeps its registers in the
 * kernel: nothing of it lives in its user stack, so a tracer may write anything below the stopped rsp (an
 * injected call's argument block and fake return address go exactly there). On Windows the exception that
 * makes the stop is dispatched ON that stack: the CONTEXT, its CONTEXT_EX and XSAVE area, the
 * EXCEPTION_RECORD and the dispatcher's and the VEH's frames all sit in the 0x700+ bytes below the faulting
 * rsp. Serving the tracer there let its POKEDATAs overwrite the CONTEXT_EX/XSAVE header NtContinue was going
 * to load (it refused the context: STATUS_INVALID_PARAMETER 0xc000000d, the process ended -- the EAC
 * launch's third injected call, a 0x328-byte block right below rsp) or the VEH's own frames.
 * So: copy the CONTEXT (with its XSTATE) out of the stack, leave the exception dispatch at once into
 * tr_park_main on this thread's host stack (unused while the thread is in guest code), serve the tracer there
 * and resume the guest from the copy with NtContinue. The guest stack is the tracer's alone. */
typedef struct tr_park { xrl_thread *t; int sig; CONTEXT *c; void *buf; DWORD cap; } tr_park;
typedef LONG (NTAPI *nt_continue_fn)(CONTEXT *, BOOLEAN);
static nt_continue_fn p_NtContinue;

static __declspec(noreturn) void tr_park_main(tr_park *pk)
{
    xrl_thread *t = pk->t;
    int g = xrl_teb_enter_host(t);
    stop_and_serve(pk->c, t, pk->sig);
    xrl_teb_restore(t, g);
    LONG s = p_NtContinue(pk->c, FALSE);
    xrl_teb_enter_host(t);
    LOGE("tracee: NtContinue refused the registers the tracer left: %08lx", (unsigned long)s);
    ExitProcess((UINT)s);
}

/* signal.c: an int3 or a fault in guest code of a traced process, from the VEH (host TEB view, on the guest
 * stack). c is the faulting thread's CONTEXT; the tracer sees the registers as Linux reports them (rip after
 * an int3) and may change them before it continues. */
void xrl_tracee_stop(CONTEXT *c, xrl_thread *t, int sig)
{
    if (!p_NtContinue) p_NtContinue = (nt_continue_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtContinue");
    DWORD flags = c->ContextFlags & (CONTEXT_ALL | CONTEXT_XSTATE), len = 0;
    tr_park *pk = (tr_park *)t->tr_park;
    if (!pk) t->tr_park = pk = (tr_park *)calloc(1, sizeof *pk);
    InitializeContext(NULL, flags, NULL, &len);   /* the size, for the features this CPU has */
    if (pk && len > pk->cap) { free(pk->buf); pk->buf = malloc(len); pk->cap = pk->buf ? len : 0; }
    CONTEXT *dc = NULL;
    DWORD l2 = pk ? pk->cap : 0;
    BOOL ok = pk && pk->buf && p_NtContinue && t->host_rsp && InitializeContext(pk->buf, flags, &dc, &l2);
    if (ok && (flags & CONTEXT_XSTATE) == CONTEXT_XSTATE) { DWORD64 m = 0; ok = GetXStateFeaturesMask(c, &m) && SetXStateFeaturesMask(dc, m); }
    if (ok) ok = CopyContext(dc, flags, c);
    if (!ok) {
        LOGW("tracee: cannot copy the CONTEXT (error %lu): stopped on the guest stack, the tracer must not write below rsp", GetLastError());
        stop_and_serve(c, t, sig);
        return;
    }
    pk->t = t; pk->sig = sig; pk->c = dc;
    u64 sp = (t->host_rsp & ~15ull) - 0x48;   /* as after a call: a return address slot (0) and 32 bytes of home space above */
    *(u64 *)sp = 0;
    c->Rsp = sp; c->Rip = (u64)(uintptr_t)tr_park_main; c->Rcx = (u64)(uintptr_t)pk;
    c->EFlags &= ~(0x100u | 0x400u | 0x40000u);   /* TF DF AC: what the guest had is in the copy */
    c->MxCsr = 0x1f80; c->FltSave.MxCsr = 0x1f80; c->FltSave.ControlWord = 0x27f;
}

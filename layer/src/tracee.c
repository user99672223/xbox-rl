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
        case TR_SETREGS: if (len == 27 * 8) { memcpy(regs, p, len); xrl_tr_send(g_tr, TR_RESP, 0, 0, NULL, 0); } else xrl_tr_send(g_tr, TR_RESP, (u64)(s64)-LX_EINVAL, 0, NULL, 0); break;
        case TR_GETFP: xrl_tr_send(g_tr, TR_RESP, 0, 0, fp, 512); break;
        case TR_SETFP: if (len == 512) { memcpy(fp, p, 512); xrl_tr_send(g_tr, TR_RESP, 0, 0, NULL, 0); } else xrl_tr_send(g_tr, TR_RESP, (u64)(s64)-LX_EINVAL, 0, NULL, 0); break;
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
    regs[15] = (u64)-1; regs[16] = *entry; regs[17] = 0x33; regs[18] = 0x202; regs[19] = *sp; regs[20] = 0x2b;
    u8 fp[512] = { 0 };
    *(u16 *)fp = 0x37f; *(u32 *)(fp + 24) = 0x1f80; *(u32 *)(fp + 28) = 0xffff;   /* the state a new process starts with */
    xrl_tr_send(g_tr, TR_HELLO, *entry, *sp, NULL, 0);
    serve(regs, fp);
    *entry = regs[16]; *sp = regs[19];
    LOGW("tracee: continues at %llx, stack %llx", *entry, *sp);
}

/* signal.c: an int3 in guest code of a traced process. The CONTEXT is the faulting thread's; the tracer sees
 * the registers as Linux reports them (rip after the int3) and may change them before it continues. */
void xrl_tracee_stop(CONTEXT *c, xrl_thread *t, int sig)
{
    u64 r[27] = { 0 };
    r[0] = c->R15; r[1] = c->R14; r[2] = c->R13; r[3] = c->R12; r[4] = c->Rbp; r[5] = c->Rbx; r[6] = c->R11; r[7] = c->R10;
    r[8] = c->R9; r[9] = c->R8; r[10] = c->Rax; r[11] = c->Rcx; r[12] = c->Rdx; r[13] = c->Rsi; r[14] = c->Rdi;
    r[15] = (u64)-1; r[16] = c->Rip + (sig == LX_SIGTRAP ? 1 : 0);   /* after the int3; a fault reports the faulting address */ r[17] = 0x33; r[18] = c->EFlags; r[19] = c->Rsp; r[20] = 0x2b; r[21] = t->guest_tp;
    u8 fp[512];
    memcpy(fp, &c->FltSave, sizeof fp);   /* XMM_SAVE_AREA32 is the FXSAVE layout, as Linux's user_fpregs_struct */
    xrl_tr_send(g_tr, TR_STOP, (u64)sig, 0, NULL, 0);
    serve(r, fp);
    memcpy(&c->FltSave, fp, sizeof fp);
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

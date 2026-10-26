/*
 * guestctx.c - the structure the guest finds via AT_XRL_CTX, and the
 * SysV->Win64 stubs behind every function pointer in it.
 *
 * The host fills xrl_gfx_ops / xrl_audio_ops / xrl_input_ops / xrl_log_fn with
 * ordinary Win64 function pointers. Wine calls them with the SysV ABI (the
 * header says so). For each non-NULL pointer we generate a 32-byte stub:
 *
 *      mov  r11, <host fn>          ; Win64 target
 *      mov  rax, <xrl_downcall_tramp>
 *      jmp  rax
 *
 * and hand the guest a COPY of the ops table containing the stubs. The common
 * trampoline switches to the thread's host stack and re-marshals up to 8
 * integer/pointer arguments (all ABI v1 ops are integer-class).
 */
#include "xrl_internal.h"

void *xrl_exec_alloc(size_t n);

static u8 *g_stub_page;
static size_t g_stub_used, g_stub_cap = 16384;

static u64 emit_stub(const u8 *code, size_t n)
{
    if (!g_stub_page) g_stub_page = (u8 *)xrl_exec_alloc(g_stub_cap);
    if (!g_stub_page || g_stub_used + 32 > g_stub_cap) return 0;
    u8 *p = g_stub_page + g_stub_used;
    memset(p, 0xCC, 32);
    memcpy(p, code, n);
    g_stub_used += 32;
    return (u64)(uintptr_t)p;
}

u64 xrl_stub_for(void *host_fn)
{
    if (!host_fn) return 0;
    u8 c[32];
    u64 tramp = (u64)(uintptr_t)&xrl_downcall_tramp;
    u64 fn = (u64)(uintptr_t)host_fn;
    c[0] = 0x49; c[1] = 0xBB; memcpy(c + 2, &fn, 8);        /* mov r11, imm64 */
    c[10] = 0x48; c[11] = 0xB8; memcpy(c + 12, &tramp, 8);  /* mov rax, imm64 */
    c[20] = 0xFF; c[21] = 0xE0;                              /* jmp rax        */
    return emit_stub(c, 22);
}

static u64 g_exit_stub;
u64 xrl_exit_stub_addr(void)
{
    if (g_exit_stub) return g_exit_stub;
    /* mov esi, eax ; mov edi, 60 ; mov rax, xrl_syscall ; call rax ; ud2 */
    u8 c[32];
    u64 sc = (u64)(uintptr_t)&xrl_syscall;
    size_t n = 0;
    c[n++] = 0x89; c[n++] = 0xC6;
    c[n++] = 0xBF; c[n++] = 60; c[n++] = 0; c[n++] = 0; c[n++] = 0;
    c[n++] = 0x48; c[n++] = 0xB8; memcpy(c + n, &sc, 8); n += 8;
    c[n++] = 0xFF; c[n++] = 0xD0;
    c[n++] = 0x0F; c[n++] = 0x0B;
    g_exit_stub = emit_stub(c, n);
    return g_exit_stub;
}

/* All members after struct_size in the three op tables are function pointers. */
static void *copy_ops(const void *src, size_t compiled_size, uint32_t host_size, int *nstubs)
{
    size_t sz = host_size && host_size < compiled_size ? host_size : compiled_size;
    u8 *d = (u8 *)calloc(1, compiled_size);
    memcpy(d, src, sz);
    for (size_t off = 8; off + 8 <= sz; off += 8) {
        u64 v; memcpy(&v, d + off, 8);
        if (v) { u64 s = xrl_stub_for((void *)(uintptr_t)v); memcpy(d + off, &s, 8); (*nstubs)++; }
    }
    return d;
}

int xrl_guest_ctx_build(void)
{
    xrl_guest_ctx *g = (xrl_guest_ctx *)calloc(1, sizeof *g);
    int ns = 0;
    g->struct_size = sizeof *g;
    g->magic = XRL_GUEST_CTX_MAGIC;
    g->abi_version = XRL_ABI_VERSION;
    g->flags = (G.fsgsbase ? 1u : 0u) | (G.tp_slot ? 2u : 0u);
    g->syscall = (u64)(uintptr_t)&xrl_syscall;
    g->tls_slot_gs_off = G.tp_slot ? XRL_TP_GS_OFFSET : 0;
    g->log = (xrl_log_fn)(uintptr_t)xrl_stub_for((void *)(uintptr_t)G.ctx.log);
    g->gfx = (const xrl_gfx_ops *)copy_ops(G.ctx.gfx, sizeof(xrl_gfx_ops), G.ctx.gfx->struct_size, &ns);
    g->audio = (const xrl_audio_ops *)copy_ops(G.ctx.audio, sizeof(xrl_audio_ops), G.ctx.audio->struct_size, &ns);
    g->input = (const xrl_input_ops *)copy_ops(G.ctx.input, sizeof(xrl_input_ops), G.ctx.input->struct_size, &ns);
    g->guest_mem_budget = G.mem_budget;
    G.guest_ctx = g;
    LOGI("guestctx: %d SysV stubs generated for host op tables (ctx=%p)", ns, (void *)g);
    return g->log || ns ? 0 : -1;
}

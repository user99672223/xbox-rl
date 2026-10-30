/*
 * dbt.c - run a foreign ELF image without changing one byte of it.
 *
 * rewrite.c patches `syscall` and %fs: operands in place. An image that checks its own code (an
 * anti-cheat module) sees the patches. Here the image's pages stay exactly as the file has them, and
 * are made non-executable for the host (still readable, still r-xp in /proc/self/maps). Execution is
 * a translation of the code into a cache next to the image:
 *   - the first jump or call into an image address from outside faults (instruction fetch from a
 *     non-executable page); the fault handler translates the code at that address (and, following
 *     direct jumps and calls, everything reachable from it) into the cache and resumes there;
 *   - instructions are copied unchanged, except: `syscall` and %fs: operands (the sequences rewrite_core
 *     already uses), RIP-relative operands (the displacement is recomputed so they still reach the
 *     same original address), and control flow;
 *   - control flow keeps ORIGINAL addresses wherever the program can see them: a call into the image
 *     pushes the original return address, `lea fn(%rip),%rax` yields the original address, `ret` and
 *     indirect jumps and calls look the original target up (dbt.asm) and continue in the cache. Calls to
 *     code outside the image (musl, Wine) are plain calls from the cache and return into it directly.
 * Calls from outside into the image (a callback, a function pointer Wine got from dlsym) fault once per
 * target and are fast after that only for returns: each such entry costs an exception.
 *
 * Limits, stated: self-modifying code in a translated ELF image is not followed (it would
 * be executed from original pages: a fault, and so a SIGSEGV for the guest; RWX pages a program of the policy
 * generates itself are handled separately, see "generated code" below); `ret imm16`, far
 * transfers, and %fs operands in branch instructions are not translated (the site becomes ud2 and
 * is logged); the unwinder sees return addresses of the original image for calls inside the image but
 * cache addresses for frames that called native code; a fault inside translated code reports the cache
 * address; unmapping an image does not drop its translations. The cache is a visible anonymous
 * mapping.
 *
 * Selected by XRL_DBT: unset = images whose path contains "easyanticheat" (the anti-cheat's modules: they
 * check their own code); 0 = none (everything is rewritten in place); 1 = every image rewrite.c would
 * rewrite; else a comma-separated list of path substrings.
 */
#include "xrl_internal.h"
#include "rewrite_core.h"
#include "x86dec.h"

extern void xrl_dbt_ijmp(void), xrl_dbt_icall(void), xrl_dbt_ret(void), xrl_rawsys(void);

#define TAB_N 262144u
u64 xrl_dbt_tab[(TAB_N + 4) * 2];        /* {original, translated}: read by dbt.asm */
u64 xrl_dbt_rng[32];                     /* {lo, size} x 16 (MAX_IMG = DBT_NRNG in dbt.asm): read by dbt.asm */

#define MAX_IMG 16
typedef struct dimg {
    u64 lo, hi;                          /* the mapping that is translated */
    u8 *cache; u64 cache_va, cap, used;
    char name[64];
    int gen;                             /* generated code (a page the program wrote): a write drops the translation */
} dimg;
static dimg g_img[MAX_IMG];
static int g_nimg;
static SRWLOCK g_lock = SRWLOCK_INIT;
static SRWLOCK g_genlock = SRWLOCK_INIT;   /* generated-code bookkeeping; taken before g_lock */
static PVOID g_veh;
#define MAX_WATCH 16
static struct { u64 lo, hi; } g_watch[MAX_WATCH];   /* RWX pages of the program, not executable for the host until first fetched */
static volatile int g_nwatch;
static volatile LONG64 g_nfault, g_ntrans;

/* ---------------------------------------------------------------- tables */
static u32 th(u64 t) { return (u32)((t * 0x9e3779b97f4a7c15ull) >> 46); }   /* = DBT_HASH in dbt.asm */

static void tab_put(u64 orig, u64 tr)
{
    for (u32 i = 0; i < 4; i++) {
        u64 *e = &xrl_dbt_tab[(th(orig) + i) * 2];
        if (e[0] == orig) { e[1] = tr; return; }
        if (!e[0] || e[0] == 1) {         /* free, or dropped (1: probing in dbt.asm continues past it) */
            e[1] = tr;
            MemoryBarrier();
            e[0] = orig;                  /* published last */
            return;
        }
    }
    static volatile LONG warned;
    if (!InterlockedExchange(&warned, 1)) LOGW("dbt: lookup table crowded at %llx: that target will take the slow path", orig);
}

/* every translation, published or not (translation runs under g_lock) */
#define CM_BITS 18
static struct { u64 o, t; } g_cmap[1u << CM_BITS];
static struct { u64 o, t; } g_pend[1u << 16];
static int g_npend;

static u64 cm_get(u64 o)
{
    for (u32 i = (u32)((o * 0x9e3779b97f4a7c15ull) >> (64 - CM_BITS));; i = (i + 1) & ((1u << CM_BITS) - 1)) {
        if (g_cmap[i].o == o) return g_cmap[i].t;
        if (!g_cmap[i].o) return 0;
    }
}

static void cm_forget(u64 o)
{
    for (u32 i = (u32)((o * 0x9e3779b97f4a7c15ull) >> (64 - CM_BITS));; i = (i + 1) & ((1u << CM_BITS) - 1)) {
        if (g_cmap[i].o == o) { g_cmap[i].t = 0; return; }
        if (!g_cmap[i].o) return;
    }
}

static u32 g_ncm;
static int g_cm_full;     /* the map is too full to take more: the translation in progress fails */

static void cm_put(u64 o, u64 t)
{
    if (g_ncm >= (3u << CM_BITS) / 4) { if (!g_cm_full) LOGE("dbt: translation map full (%u entries)", g_ncm); g_cm_full = 1; return; }
    for (u32 i = (u32)((o * 0x9e3779b97f4a7c15ull) >> (64 - CM_BITS));; i = (i + 1) & ((1u << CM_BITS) - 1)) {
        if (g_cmap[i].o == o) { g_cmap[i].t = t; break; }
        if (!g_cmap[i].o) { g_cmap[i].o = o; g_cmap[i].t = t; g_ncm++; break; }
    }
    if (g_npend < (int)(sizeof g_pend / sizeof g_pend[0])) { g_pend[g_npend].o = o; g_pend[g_npend].t = t; g_npend++; }
}

static dimg *img_of(u64 a)
{
    for (int i = 0; i < g_nimg; i++) if (a >= g_img[i].lo && a < g_img[i].hi) return &g_img[i];
    return NULL;
}

/* ------------------------------------------------------------- emission */
typedef struct fixup { u8 *site; u64 va_end; u64 target; } fixup;
#define MAX_FIX 32768
static fixup g_fix[MAX_FIX];
static int g_nfix;
#define MAX_WL 16384
static u64 g_wl[MAX_WL];
static int g_nwl;

static u64 cur(const dimg *im) { return im->cache_va + im->used; }
static void emit(dimg *im, const void *b, size_t n) { memcpy(im->cache + im->used, b, n); im->used += n; }
static void e8(dimg *im, unsigned v) { im->cache[im->used++] = (u8)v; }
static void e32(dimg *im, u32 v) { memcpy(im->cache + im->used, &v, 4); im->used += 4; }
static void e64(dimg *im, u64 v) { memcpy(im->cache + im->used, &v, 8); im->used += 8; }

static void enqueue(u64 target)
{
    if (cm_get(target)) return;
    if (g_nwl < MAX_WL) g_wl[g_nwl++] = target;
}

/* jmp rel32 whose target is the translation of `target` (patched when the work list is done) */
static void emit_jmp_orig(dimg *im, u64 target)
{
    e8(im, 0xe9);
    u64 t = cm_get(target);
    if (t) { e32(im, (u32)(int32_t)(t - (cur(im) + 4))); return; }
    if (g_nfix < MAX_FIX) { g_fix[g_nfix].site = im->cache + im->used; g_fix[g_nfix].va_end = cur(im) + 4; g_fix[g_nfix].target = target; g_nfix++; }
    e32(im, 0);
    enqueue(target);
}

static void emit_jcc_orig(dimg *im, int cc, u64 target)
{
    e8(im, 0x0f); e8(im, 0x80 | cc);
    u64 t = cm_get(target);
    if (t) { e32(im, (u32)(int32_t)(t - (cur(im) + 4))); return; }
    if (g_nfix < MAX_FIX) { g_fix[g_nfix].site = im->cache + im->used; g_fix[g_nfix].va_end = cur(im) + 4; g_fix[g_nfix].target = target; g_nfix++; }
    e32(im, 0);
    enqueue(target);
}

/* jmp *0(%rip) ; .quad target: a jump to an absolute address, no register used */
static void emit_jmp_abs(dimg *im, u64 target) { e8(im, 0xff); e8(im, 0x25); e32(im, 0); e64(im, target); }

static void emit_slot_jmp(dimg *im, int slot)      /* jmp *slot(%rip); slots sit at the start of the cache */
{
    e8(im, 0xff); e8(im, 0x25);
    e32(im, (u32)(int32_t)((im->cache_va + 8u * (u32)slot) - (cur(im) + 4)));
}
static void emit_slot_call(dimg *im, int slot)     /* call *slot(%rip) */
{
    e8(im, 0xff); e8(im, 0x15);
    e32(im, (u32)(int32_t)((im->cache_va + 8u * (u32)slot) - (cur(im) + 4)));
}

static int in_any_image(u64 a) { return img_of(a) != NULL; }

/* `mov <the operand of this indirect branch>,%r11`; 0 if the operand cannot be rebuilt */
static int emit_mov_r11(dimg *im, const u8 *ins, const x86i *in, u64 pc, int rsp_moved)
{
    if (!in->has_modrm || in->addr32 || in->vex || (in->seg && in->seg != 0x3e)) return 0;
    /* a stack-relative operand would be off by the pad and the saved r11 */
    if (rsp_moved && (in->modrm >> 6) != 3 && (in->modrm & 7) == 4 && in->sib_off && (in->sib & 7) == 4 && !(in->rex & 1)) return 0;
    u8 b[16]; int n = 0;
    b[n++] = (u8)(0x4c | (in->rex & 3));            /* W, R (r11), X, B of the original */
    b[n++] = 0x8b;
    b[n++] = (u8)((in->modrm & 0xc7) | (3 << 3));
    if (in->sib_off) b[n++] = in->sib;
    if (in->disp_size) { memcpy(b + n, ins + in->disp_off, in->disp_size); n += in->disp_size; }
    if (in->flags & X86_RIPREL) {
        int32_t d; memcpy(&d, ins + in->disp_off, 4);
        int64_t nd = (int64_t)(pc + in->len) + d - (int64_t)(cur(im) + (u64)n);
        if (nd != (int32_t)nd) return 0;
        int32_t v = (int32_t)nd;
        memcpy(b + n - 4, &v, 4);
    }
    emit(im, b, (size_t)n);
    return 1;
}

/* r11 = target: call it. A target inside an image gets the original return address, anything else the
 * continuation in the cache (dbt.asm xrl_dbt_icall). */
static void emit_icall_r11(dimg *im, u64 orig_ret)
{
    /* lea cont(%rip),%r10 ; jmp *icall_slot(%rip) ; .quad orig_ret ; cont: */
    e8(im, 0x4c); e8(im, 0x8d); e8(im, 0x15); e32(im, 14);
    emit_slot_jmp(im, 2);
    e64(im, orig_ret);
    cm_put(orig_ret, cur(im));
}

/* the jump table or PLT stub at `t` is `jmp *got(%rip)` (optionally behind endbr64 / bnd / notrack): its GOT slot, else 0 */
static u64 plt_got(u64 t)
{
    const u8 *p = (const u8 *)t;
    if (p[0] == 0xf3 && p[1] == 0x0f && p[2] == 0x1e && p[3] == 0xfa) p += 4;
    if (p[0] == 0xf2 || p[0] == 0x3e) p++;
    if (p[0] != 0xff || p[1] != 0x25) return 0;
    int32_t d; memcpy(&d, p + 2, 4);
    return (u64)(uintptr_t)(p + 6) + (u64)(int64_t)d;
}

static void emit_ud2(dimg *im, u64 pc, const char *why)
{
    LOGW("dbt: %s: cannot translate %s at %llx (ud2 emitted)", im->name, why, pc);
    e8(im, 0x0f); e8(im, 0x0b);
}

static int is_fs_unit(const x86i *in)
{
    if (in->seg != 0x64) return 0;
    if (in->map == 0 && in->opcode == 0x8d) return 0;
    if (in->has_modrm && (in->modrm >> 6) != 3) return 1;
    if (in->map == 0 && in->opcode >= 0xa0 && in->opcode <= 0xa3) return 1;
    return 0;
}

/* one trace: from `pc` until an unconditional transfer */
static int translate_trace(dimg *im, u64 pc)
{
    if (im->hi - im->lo <= 4096) {
        const u8 *d = (const u8 *)pc;
        LOGW("dbt: translate_trace(%s, %llx): first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             im->name, pc, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
    }
    cm_put(pc, cur(im));
    for (int count = 0;; count++) {
        if (im->used + 4096 > im->cap) { LOGE("dbt: %s: code cache full", im->name); return -1; }
        if (count) {
            u64 known = cm_get(pc);
            if (known && known != cur(im)) { e8(im, 0xe9); e32(im, (u32)(int32_t)(known - (cur(im) + 4))); return 0; }
        }
        if (!in_any_image(pc)) { emit_jmp_abs(im, pc); return 0; }   /* ran off the image: continue natively */
        const u8 *p = (const u8 *)pc;
        u64 end = img_of(pc)->hi;
        x86i in;
        /* an instruction may straddle the end of the translated range: read on while the memory is mapped */
        size_t avail = (size_t)(end - pc);
        if (avail < 15) { avail = 15; while (avail > (size_t)(end - pc) && !xrl_mem_is_mapped(pc + avail - 1)) avail--; }
        int len = x86_decode(p, avail, &in);
        if (!len) { emit_ud2(im, pc, "an undecodable instruction"); return 0; }
        u64 next = pc + (u64)len;
        u8 b[640]; int n;

        if (in.seg == 0x64 && is_fs_unit(&in)) {
            if ((n = xrw_emit_fs(XRL_TP_GS_OFFSET, b, p, &in)) < 0) { emit_ud2(im, pc, "a %fs operand form"); return 0; }
            emit(im, b, (size_t)n);
            pc = next;
            continue;
        }
        if (!in.vex && in.map == 1 && in.opcode == 0x05) {         /* syscall */
            n = xrw_emit_syscall(b, cur(im), im->cache_va);
            emit(im, b, (size_t)n);
            pc = next;
            continue;
        }

        u8 op = in.opcode;
        if (!in.vex && in.map == 0) {
            if (op == 0xe8) {                                       /* call rel32 */
                u64 t = next + (u64)(int64_t)in.rel;
                u64 got;
                if (in_any_image(t) && (got = plt_got(t))) {        /* a PLT stub: call through its GOT slot */
                    int64_t d = (int64_t)got - (int64_t)(cur(im) + 7);
                    if (d != (int32_t)d) { emit_ud2(im, pc, "a PLT call out of reach"); return 0; }
                    e8(im, 0x4c); e8(im, 0x8b); e8(im, 0x1d); e32(im, (u32)(int32_t)d);   /* mov got(%rip),%r11 */
                    emit_icall_r11(im, next);
                } else if (in_any_image(t)) {                       /* push $next (no register: a local callee may share r10/r11) ; jmp target */
                    e8(im, 0x68); e32(im, (u32)next);                                        /* push imm32: low half, sign-extended */
                    e8(im, 0xc7); e8(im, 0x44); e8(im, 0x24); e8(im, 0x04); e32(im, (u32)(next >> 32));   /* movl $hi,4(%rsp) */
                    emit_jmp_orig(im, t);
                    cm_put(next, cur(im));
                } else {                                            /* call *2(%rip) ; jmp +8 ; .quad target */
                    e8(im, 0xff); e8(im, 0x15); e32(im, 2);
                    e8(im, 0xeb); e8(im, 0x08);
                    e64(im, t);
                    cm_put(next, cur(im));                          /* the instruction after the literal */
                }
                pc = next;
                continue;
            }
            if (op == 0xe9 || op == 0xeb) {                         /* jmp rel */
                u64 t = next + (u64)(int64_t)in.rel;
                if (in_any_image(t)) emit_jmp_orig(im, t); else emit_jmp_abs(im, t);
                return 0;
            }
            if (op >= 0x70 && op <= 0x7f) {                         /* jcc rel8 */
                u64 t = next + (u64)(int64_t)in.rel;
                if (in_any_image(t)) emit_jcc_orig(im, op & 15, t);
                else { e8(im, 0x70 | ((op & 15) ^ 1)); e8(im, 14); emit_jmp_abs(im, t); }
                pc = next;
                continue;
            }
            if (op >= 0xe0 && op <= 0xe3) {                         /* loopne loope loop jrcxz */
                u64 t = next + (u64)(int64_t)in.rel;
                if (in.addr32) e8(im, 0x67);
                e8(im, op); e8(im, 2);
                if (in_any_image(t)) { e8(im, 0xeb); e8(im, 5); emit_jmp_orig(im, t); }
                else { e8(im, 0xeb); e8(im, 14); emit_jmp_abs(im, t); }
                pc = next;
                continue;
            }
            if (op == 0xc3) { emit_slot_jmp(im, 3); return 0; }     /* ret */
            if (op == 0xc2 || op == 0xca || op == 0xcb) { emit_ud2(im, pc, "ret imm16 / far ret"); return 0; }
            if (op == 0xcc || op == 0xf4 || op == 0xcf) { emit(im, p, (size_t)len); return 0; }   /* int3 hlt iret */
            if (op == 0xff && in.has_modrm && (((in.modrm >> 3) & 7) == 2 || ((in.modrm >> 3) & 7) == 4)) {
                int is_call = ((in.modrm >> 3) & 7) == 2;
                if (is_call) {
                    if (!emit_mov_r11(im, p, &in, pc, 0)) { emit_ud2(im, pc, "an indirect branch operand"); return 0; }
                    emit_icall_r11(im, next);
                    pc = next;
                    continue;
                }
                /* jmp *X can be a switch in the middle of a function: no register may change (dbt.asm) */
                e8(im, 0x48); e8(im, 0x8d); e8(im, 0x64); e8(im, 0x24); e8(im, 0x80);   /* lea -0x80(%rsp),%rsp */
                e8(im, 0x41); e8(im, 0x53);                                             /* push %r11 */
                if (!emit_mov_r11(im, p, &in, pc, 1)) { emit_ud2(im, pc, "an indirect branch operand"); return 0; }
                e8(im, 0x4c); e8(im, 0x87); e8(im, 0x1c); e8(im, 0x24);                 /* xchg %r11,(%rsp) */
                emit_slot_jmp(im, 1);
                return 0;
            }
            if (op == 0xff && in.has_modrm && (((in.modrm >> 3) & 7) == 3 || ((in.modrm >> 3) & 7) == 5)) {
                emit_ud2(im, pc, "a far call/jump");
                return 0;
            }
        } else if (!in.vex && in.map == 1) {
            if (op >= 0x80 && op <= 0x8f) {                         /* jcc rel32 */
                u64 t = next + (u64)(int64_t)in.rel;
                if (in_any_image(t)) emit_jcc_orig(im, op & 15, t);
                else { e8(im, 0x70 | ((op & 15) ^ 1)); e8(im, 14); emit_jmp_abs(im, t); }
                pc = next;
                continue;
            }
            if (op == 0x0b) { emit(im, p, (size_t)len); return 0; } /* ud2 */
            if (op == 0xa2 || op == 0x31 || op == 0x01) { /* cpuid, rdtsc, xgetbv: executed as they are (CPUID answers
                                                              translated code as it answers code run directly) */ }
        }

        /* an ordinary instruction: copied, with a RIP-relative displacement recomputed */
        if (in.flags & X86_RIPREL) {
            u8 c[16]; memcpy(c, p, (size_t)len);
            int32_t d; memcpy(&d, c + in.disp_off, 4);
            int64_t nd = (int64_t)next + d - (int64_t)(cur(im) + (u64)len);
            if (nd != (int32_t)nd) { emit_ud2(im, pc, "a RIP-relative operand out of reach"); return 0; }
            int32_t v = (int32_t)nd; memcpy(c + in.disp_off, &v, 4);
            emit(im, c, (size_t)len);
        } else emit(im, p, (size_t)len);
        pc = next;
    }
}

/* ---------------------------------------------------------------- driver */
static u64 translate_locked(u64 pc)
{
    u64 r = cm_get(pc);
    if (r) return r;
    dimg *im = img_of(pc);
    if (!im) return 0;
    if (!strncmp(im->name, "/memfd:", 7))
        xrl_mem_sync_shared_page(pc & ~(u64)(LX_PAGE - 1), LX_PAGE);
    u64 used0 = im->used;
    g_npend = 0; g_nfix = 0; g_nwl = 0;
    enqueue(pc);
    int ok = 1;
    while (g_nwl && ok) {
        u64 t = g_wl[--g_nwl];
        if (cm_get(t)) continue;
        if (!img_of(t)) continue;
        if (translate_trace(im, t)) ok = 0;
    }
    if (ok) {
        for (int i = 0; i < g_nfix; i++) {
            u64 t = cm_get(g_fix[i].target);
            if (!t) { LOGE("dbt: %s: no translation for branch target %llx", im->name, g_fix[i].target); ok = 0; break; }
            int32_t d = (int32_t)(t - g_fix[i].va_end);
            memcpy(g_fix[i].site, &d, 4);
        }
    }
    if (g_cm_full) ok = 0;
    if (!ok) {
        LOGE("dbt: %s: translation of %llx failed", im->name, pc);
        for (int i = 0; i < g_npend; i++) cm_forget(g_pend[i].o);               /* what was half built */
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), im->cache + used0, (SIZE_T)(im->used - used0));
    for (int i = 0; i < g_npend; i++) tab_put(g_pend[i].o, g_pend[i].t);
    InterlockedAdd64(&g_ntrans, g_npend);
    {   /* XRL_DBT_DUMP=prefix: the cache (prefix.bin, from its start) and orig -> cache pairs (prefix.map), for objdump */
        static char pre[260]; static int init;
        if (!init) { init = 1; if (!GetEnvironmentVariableA("XRL_DBT_DUMP", pre, sizeof pre)) pre[0] = 0; }
        if (pre[0]) {
            char fn[300]; FILE *f;
            snprintf(fn, sizeof fn, "%s.bin", pre);
            if (!fopen_s(&f, fn, "wb")) { fwrite(im->cache, 1, (size_t)im->used, f); fclose(f); }
            snprintf(fn, sizeof fn, "%s.map", pre);
            if (!fopen_s(&f, fn, "a")) {
                fprintf(f, "# cache %llx image %llx-%llx\n", im->cache_va, im->lo, im->hi);
                for (int i = 0; i < g_npend; i++) fprintf(f, "%llx %llx\n", g_pend[i].o - im->lo, g_pend[i].t - im->cache_va);
                fclose(f);
            }
        }
    }
    return cm_get(pc);
}

u64 xrl_dbt_translate(u64 pc)
{
    AcquireSRWLockExclusive(&g_lock);
    u64 r = translate_locked(pc);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

/* cache address → original address (for the tracee: present the tracer with addresses it recognises) */
u64 xrl_dbt_reverse(u64 ca)
{
    AcquireSRWLockExclusive(&g_lock);
    dimg *hit = NULL;
    for (int i = 0; i < g_nimg; i++)
        if (ca >= g_img[i].cache_va && ca < g_img[i].cache_va + g_img[i].cap) { hit = &g_img[i]; break; }
    if (!hit) { ReleaseSRWLockExclusive(&g_lock); return 0; }
    u64 best_o = 0, best_t = 0;
    for (u32 i = 0; i < (1u << CM_BITS); i++) {
        u64 o = g_cmap[i].o, t = g_cmap[i].t;
        if (o && t && o >= hit->lo && o < hit->hi && t <= ca && t > best_t) { best_o = o; best_t = t; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return best_o;
}

/* from dbt.asm through the syscall path (a computed jump to an address of an image without a translation) */
slong xrl_dbt_enter_sys(u64 pc)
{
    u64 r = xrl_dbt_translate(pc);
    return (slong)(r ? r : pc);
}

/* generated code (below) */
static int gen_fetch(u64 pc);
static int gen_write(u64 a);
static volatile LONG g_has_gen;

/* instruction fetch from a translated image (or a watched page of generated code); a write to a page of
 * generated code that is translated */
/* Diagnostic: an exception in guest code the DBT is about to let escape (to Wine's handler, then
 * winedbg). CALLED FROM THE VEH (exception context), so it must be deadlock-safe: NO locks, NO logger
 * (the logger takes a lock a faulting thread may already hold -- that froze the console), NO memory
 * queries. It only does plain stores into a static array and a within-page read of the instruction
 * bytes at rip (rip's page is present, as it is the code that was executing). xrl_dbt_dump_faults()
 * prints the array later from a safe context (boot exit). */
#define FCAP_N 64
typedef struct { LONG seq; u32 code, why, tid; u64 rip, acc, addr; u8 nb, b[16]; u64 reg[16]; u8 snaplen[16], snap[16][48]; } dbt_fault_rec;
/* Safe lock-free read of up to `cap` bytes at guest address p (VEH context): VirtualQuery confirms the
 * page is committed+readable and bounds the read to its region, so no nested fault. Returns bytes read. */
static u8 dbt_snap_at(u64 p, u8 *out, u8 cap)
{
    if (p < 0x1000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return 0;
    u64 avail = (u64)mbi.BaseAddress + mbi.RegionSize - p;
    if (avail > cap) avail = cap;
    const u8 *s = (const u8 *)p;
    for (u64 i = 0; i < avail; i++) out[i] = s[i];
    return (u8)avail;
}
static dbt_fault_rec g_fcap[FCAP_N];
/* XRL_FAULT_RIP=<hex>: the first real fault at exactly that rip is also kept here (with what every register points
 * to), so a crash site among thousands of handled faults keeps its registers until the dump. */
static dbt_fault_rec g_fwatch;
static volatile LONG g_fwatch_set;
static u64 g_fwatch_rip = 1;   /* 1: not read yet; 0: off */
static u64 g_fwatch_addr;      /* its access address */
static volatile LONG g_nfcap;    /* number of real (error-severity) faults seen */
static volatile LONG g_nskip;    /* benign exceptions skipped (OutputDebugString etc.) */
static void dbt_log_escape(PEXCEPTION_POINTERS ep, int why)
{
    if (!policy_on()) return;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    /* Skip benign/continuable exceptions that are NOT crashes: the EAC module raises thousands of
     * DBG_PRINTEXCEPTION_C (0x40010006, OutputDebugString), its wide form, and thread-name (0x406D1388).
     * Only error-severity codes (0xC.......: access violation, illegal/privileged instruction, ...) are
     * real faults. Keep the LAST FCAP_N of those in a ring, so the one that actually crashes is captured. */
    if (((u32)r->ExceptionCode & 0xF0000000u) != 0xC0000000u) { InterlockedIncrement(&g_nskip); return; }
    LONG seq = InterlockedIncrement(&g_nfcap) - 1;
    dbt_fault_rec *f = &g_fcap[seq & (FCAP_N - 1)];
    f->seq  = seq;
    f->code = (u32)r->ExceptionCode;
    f->why  = (u32)why;
    f->tid  = GetCurrentThreadId();
    f->rip  = ep->ContextRecord ? (u64)ep->ContextRecord->Rip : 0;
    f->acc  = r->NumberParameters >= 1 ? (u64)r->ExceptionInformation[0] : (u64)-1;
    f->addr = r->NumberParameters >= 2 ? (u64)r->ExceptionInformation[1] : 0;
    f->nb   = 0;
    if (f->rip) {
        u32 avail = (u32)(LX_PAGE - (f->rip & (LX_PAGE - 1)));
        if (avail > 16) avail = 16;
        const u8 *b = (const u8 *)f->rip;          /* within rip's own (present) page: no fault, no query */
        for (u32 k = 0; k < avail; k++) f->b[k] = b[k];
        f->nb = (u8)avail;
    }
    if (g_fwatch_rip == 1) {
        char v[24];
        g_fwatch_rip = GetEnvironmentVariableA("XRL_FAULT_RIP", v, sizeof v) ? _strtoui64(v, NULL, 16) : 0;
    }
    CONTEXT *c = ep->ContextRecord;
    if (c && g_fwatch_rip && f->rip == g_fwatch_rip && !InterlockedExchange(&g_fwatch_set, 1)) {
        g_fwatch = *f;
        g_fwatch_addr = f->addr;
        u64 g[16] = { c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp,
                      c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15 };
        for (int k = 0; k < 16; k++) { g_fwatch.reg[k] = g[k]; g_fwatch.snaplen[k] = dbt_snap_at(g[k], g_fwatch.snap[k], 48); }
        g_fwatch.addr = 0;   /* the dump prints registers and snapshots for addr < 0x10000 */
    }
    if (c) { u64 g[16] = { c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp,
                           c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15 };
             for (int k = 0; k < 16; k++) f->reg[k] = g[k];
             if (f->addr < 0x10000)   /* a NULL-ish deref: snapshot what each register points to (strings name the object) */
                 for (int k = 0; k < 16; k++) f->snaplen[k] = dbt_snap_at(g[k], f->snap[k], 48);
    }
}
/* Print the captured faults in sequence order; call only from a safe context (not the VEH). */
void xrl_dbt_dump_faults(void)
{
    static const char *whys[] = { "non-AV", "AV-data", "AV-exec-unwatched", "no-translation" };
    LONG total = g_nfcap;
    LONG first = total > FCAP_N ? total - FCAP_N : 0;   /* ring kept the last FCAP_N */
    if (g_fwatch_set)
        LOGW("dbt-fault: XRL_FAULT_RIP %llx was hit (fault #%ld, access address %llx); its record follows as the first entry",
             g_fwatch_rip, (long)g_fwatch.seq, g_fwatch_addr);
    LOGW("dbt-fault: %ld real fault(s), %ld benign exception(s) skipped; last fault is the crash:", (long)total, (long)g_nskip);
    for (LONG s = g_fwatch_set ? first - 1 : first; s < total; s++) {
        dbt_fault_rec *f = s < first ? &g_fwatch : &g_fcap[s & (FCAP_N - 1)];
        char hex[64]; int o = 0;
        for (u8 k = 0; k < f->nb && o < (int)sizeof hex - 3; k++) o += snprintf(hex + o, sizeof hex - o, "%02x ", f->b[k]);
        LOGW("dbt-fault[%ld] tid=%x %s: code=%08x rip=%llx acc=%lld addr=%llx bytes: %s",
             (long)f->seq, f->tid, f->why < 4 ? whys[f->why] : "?", f->code, f->rip, (long long)f->acc, f->addr, hex);
        if (f->addr < 0x10000) {   /* a NULL-ish deref: print the registers so what computed the bad pointer is visible */
            LOGW("dbt-fault[%ld] regs rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx",
                 (long)f->seq, f->reg[0], f->reg[1], f->reg[2], f->reg[3], f->reg[4], f->reg[5], f->reg[6], f->reg[7]);
            LOGW("dbt-fault[%ld] regs r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx",
                 (long)f->seq, f->reg[8], f->reg[9], f->reg[10], f->reg[11], f->reg[12], f->reg[13], f->reg[14], f->reg[15]);
            static const char *rn[16] = { "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15" };
            for (int k = 0; k < 16; k++) {
                if (!f->snaplen[k]) continue;
                char hx[160]; int o = 0; char as[56]; int ao = 0;
                for (u8 j = 0; j < f->snaplen[k] && o < (int)sizeof hx - 3; j++) {
                    o += snprintf(hx + o, sizeof hx - o, "%02x", f->snap[k][j]);
                    if (ao < (int)sizeof as - 1) as[ao++] = (f->snap[k][j] >= 32 && f->snap[k][j] < 127) ? (char)f->snap[k][j] : '.';
                }
                as[ao] = 0;
                LOGW("dbt-fault[%ld] [%s=%llx]: %s | %s", (long)f->seq, rn[k], f->reg[k], hx, as);
            }
        }
    }
}

/* --- emulate a read from the unbacked null page as reading zero ----------------------------
 * EAC's in-process init path dereferences a pointer that is NULL on this host (a value it
 * elsewhere treats as "empty" -- the faulting cmp's own fall-through is its empty-value branch).
 * The read of address 0 cannot be backed (Windows reserves the low 64K), so without this it
 * escapes to Wine's crash handler and the process dies at ~2s. We decode the faulting
 * instruction, supply 0 for the loaded byte/word/dword/qword (zeroing the destination register,
 * or setting flags for cmp/test), and step past it. Only the common load/compare forms are
 * handled; anything else returns 0 and escapes exactly as before. */
static int policy_on(void);
static u64 *emu_gpr(CONTEXT *c, int idx)
{
    switch (idx) {
    case 0:  return &c->Rax; case 1:  return &c->Rcx; case 2:  return &c->Rdx; case 3:  return &c->Rbx;
    case 4:  return &c->Rsp; case 5:  return &c->Rbp; case 6:  return &c->Rsi; case 7:  return &c->Rdi;
    case 8:  return &c->R8;  case 9:  return &c->R9;  case 10: return &c->R10; case 11: return &c->R11;
    case 12: return &c->R12; case 13: return &c->R13; case 14: return &c->R14; case 15: return &c->R15;
    }
    return 0;
}
static u64 emu_mask(int size) { return size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1); }
static int emu_parity(u8 v) { int p = 1; for (int b = 0; b < 8; b++) p ^= (v >> b) & 1; return p; }
static void emu_setf(CONTEXT *c, int cf, int pf, int af, int zf, int sf, int of)
{
    u64 f = c->EFlags & ~0x8d5ull;   /* CF(1) PF(4) AF(10) ZF(40) SF(80) OF(800) */
    if (cf) f |= 0x1; if (pf) f |= 0x4; if (af) f |= 0x10; if (zf) f |= 0x40; if (sf) f |= 0x80; if (of) f |= 0x800;
    c->EFlags = (DWORD)f;
}
static void emu_flags_sub(CONTEXT *c, u64 a, u64 b, int size)   /* flags for (a - b) */
{
    u64 m = emu_mask(size); a &= m; b &= m;
    u64 r = (a - b) & m, sign = 1ull << (size * 8 - 1);
    emu_setf(c, a < b, emu_parity((u8)r), ((a ^ b ^ r) & 0x10) != 0, r == 0,
             (r & sign) != 0, (((a ^ b) & (a ^ r)) & sign) != 0);
}
static void emu_flags_log(CONTEXT *c, u64 r, int size)          /* flags for a logical result (test) */
{
    u64 m = emu_mask(size); r &= m; u64 sign = 1ull << (size * 8 - 1);
    emu_setf(c, 0, emu_parity((u8)r), 0, r == 0, (r & sign) != 0, 0);
}
static int emulate_null_read(CONTEXT *c)
{
    const u8 *p = (const u8 *)c->Rip;
    int i = 0, rex = 0, op66 = 0;
    for (;;) {
        u8 b = p[i];
        if (b == 0x66) { op66 = 1; i++; }
        else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                 b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) i++;
        else break;
        if (i > 8) return 0;
    }
    if ((p[i] & 0xF0) == 0x40) { rex = p[i]; i++; }
    int twob = 0; u8 op = p[i++];
    if (op == 0x0F) { twob = 1; op = p[i++]; }
    u8 modrm = p[i++];
    int mod = modrm >> 6, reg = ((modrm >> 3) & 7) + ((rex & 4) ? 8 : 0), rm = modrm & 7;
    if (mod == 3) return 0;                                 /* register operand, not a memory read */
    if (rm == 4) { u8 sib = p[i++]; if ((sib & 7) == 5 && mod == 0) i += 4; }   /* SIB */
    else if (rm == 5 && mod == 0) i += 4;                   /* rip-relative disp32 */
    if (mod == 1) i += 1; else if (mod == 2) i += 4;
    int byteop = 0, size = op66 ? 2 : 4; if (rex & 8) size = 8;
    if (!twob) {
        switch (op) {
        case 0x8A: byteop = 1;                              /* mov r8, r/m8 */
        case 0x8B: {                                        /* mov r, r/m */
            u64 *d = emu_gpr(c, reg); if (!d) return 0;
            if (byteop) *d = *d & ~0xffull; else if (size == 2) *d = *d & ~0xffffull; else *d = 0;
            c->Rip += i; return 1;
        }
        case 0x38: byteop = 1;                              /* cmp r/m8, r8 */
        case 0x39: { u64 *s = emu_gpr(c, reg); if (!s) return 0;   /* cmp r/m, r : mem(0) - reg */
            emu_flags_sub(c, 0, *s, byteop ? 1 : size); c->Rip += i; return 1; }
        case 0x3A: byteop = 1;                              /* cmp r8, r/m8 */
        case 0x3B: { u64 *s = emu_gpr(c, reg); if (!s) return 0;   /* cmp r, r/m : reg - mem(0) */
            emu_flags_sub(c, *s, 0, byteop ? 1 : size); c->Rip += i; return 1; }
        case 0x84: byteop = 1;                              /* test r/m8, r8 */
        case 0x85: emu_flags_log(c, 0, byteop ? 1 : size); c->Rip += i; return 1;   /* 0 & reg = 0 */
        case 0x80: byteop = 1;                              /* grp1 r/m8, imm8 */
        case 0x83:                                          /* grp1 r/m, imm8 (sign-ext) */
        case 0x81: {                                        /* grp1 r/m, imm16/32 */
            int opsz = byteop ? 1 : size, grp = (modrm >> 3) & 7; u64 imm;
            if (op == 0x80) { imm = p[i]; i += 1; }
            else if (op == 0x83) { imm = (u64)(long long)(signed char)p[i]; i += 1; }
            else if (op66) { imm = (u16)(p[i] | (p[i + 1] << 8)); i += 2; }
            else { imm = (u32)(p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((u32)p[i + 3] << 24)); i += 4; }
            if (grp == 7) { emu_flags_sub(c, 0, imm & emu_mask(opsz), opsz); c->Rip += i; return 1; }  /* cmp */
            return 0;                                       /* add/or/and/sub/xor also write memory: don't emulate */
        }
        default: return 0;
        }
    }
    switch (op) {                                           /* two-byte 0F */
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {            /* movzx/movsx r, r/m8|r/m16 : 0 either way */
        u64 *d = emu_gpr(c, reg); if (!d) return 0; *d = 0; c->Rip += i; return 1;
    }
    default: return 0;
    }
}

static LONG CALLBACK dbt_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) { dbt_log_escape(ep, 0); return EXCEPTION_CONTINUE_SEARCH; }
    u64 acc = (u64)r->ExceptionInformation[0];
    u64 pc = (u64)r->ExceptionInformation[1];
    if (acc != 8 && !(acc == 1 && g_has_gen)) {
        /* A read of the null page (addr < 64K, which no VMA can back on Windows): emulate it as
         * reading zero and step over it, so EAC's init doesn't die on a null deref its own
         * fall-through already treats as an empty value. Everything else -> the lazy-commit handler. */
        if (acc == 0 && pc < 0x10000 && policy_on() && emulate_null_read(ep->ContextRecord)) {
            InterlockedIncrement64(&g_nfault);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        dbt_log_escape(ep, 1); return EXCEPTION_CONTINUE_SEARCH;
    }
    if (acc == 8 && !img_of(pc) && !g_nwatch) { dbt_log_escape(ep, 2); return EXCEPTION_CONTINUE_SEARCH; }
    xrl_thread *t = xrl_cur();
    u64 stk[2];
    int g = xrl_teb_enter_host(t);
    xrl_teb_stack_here(stk);
    int retry = 0;
    u64 tr = 0;
    if (acc == 1) retry = gen_write(pc);
    else if (img_of(pc)) tr = xrl_dbt_translate(pc);
    else retry = gen_fetch(pc);
    xrl_teb_stack_back(stk);
    xrl_teb_restore(t, g);
    if (retry) return EXCEPTION_CONTINUE_EXECUTION;
    if (!tr) { dbt_log_escape(ep, 3); return EXCEPTION_CONTINUE_SEARCH; }
    InterlockedIncrement64(&g_nfault);
    ep->ContextRecord->Rip = tr;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ---------------------------------------------------------------- policy */
int xrl_dbt_wanted(const char *path)
{
    static int mode = -1;
    static char list[256];
    if (mode < 0) {
        char v[256];
        DWORD n = GetEnvironmentVariableA("XRL_DBT", v, sizeof v);
        if (!n) { mode = 2; strcpy_s(list, sizeof list, "easyanticheat,_eac"); }   /* the default: the anti-cheat's own modules */
        else if (n >= sizeof v || !strcmp(v, "0")) mode = 0;
        else if (!strcmp(v, "1")) mode = 1;
        else { mode = 2; strcpy_s(list, sizeof list, v); }
    }
    if (mode == 0) return 0;
    if (mode == 1) return 1;
    char buf[256], low[512]; strcpy_s(buf, sizeof buf, list);
    size_t n = 0;
    for (; path && path[n] && n < sizeof low - 1; n++) low[n] = (char)((path[n] >= 'A' && path[n] <= 'Z') ? path[n] + 32 : path[n]);
    low[n] = 0;
    for (char *tok = buf, *ctx = NULL, *t = strtok_s(buf, ",", &ctx); t; t = strtok_s(NULL, ",", &ctx)) {
        char lt[64]; size_t k = 0;
        for (; t[k] && k < sizeof lt - 1; k++) lt[k] = (char)((t[k] >= 'A' && t[k] <= 'Z') ? t[k] + 32 : t[k]);
        lt[k] = 0;
        if (k && strstr(low, lt)) return 1;
        (void)tok;
    }
    return 0;
}

/* Code mapped from a file: translated if the policy selects its path, or if it comes from a memfd while the
 * program being run is one the policy selects (the EAC bootstrapper stores the module it downloaded in a memfd
 * and dlopen()s it: the module is the anti-cheat's own code and must stay byte-identical). */
static int policy_on(void);
int xrl_dbt_wanted_file(const char *gpath)
{
    static int memfd_env = -1;   /* XRL_DBT_MEMFD=1: set for a process its tracer started (task.c): the library its tracer puts into it
                                  * through a memfd is the same kind of module as the one the bootstrapper downloaded */
    if (memfd_env < 0) { char v[4]; memfd_env = GetEnvironmentVariableA("XRL_DBT_MEMFD", v, sizeof v) && v[0] == '1'; }
    return xrl_dbt_wanted(gpath) || (gpath && !strncmp(gpath, "/memfd:", 7) && (policy_on() || memfd_env));
}

/* A Windows PE image (the EAC bootstrapper): Windows code executes `syscall` as a Windows system call, but
 * this one issues raw LINUX system calls (memfd_create, write, mmap...: it loads the downloaded module
 * itself). Only the pages that hold a `syscall` instruction are translated, so that those calls reach the
 * layer; the rest of the image runs natively (translated frames have no unwind data, which Windows
 * exceptions need). `[addr, addr+len)` is a mapped executable section, readable now. */
void xrl_dbt_attach_pe(const char *name, u64 addr, u64 len)
{
    if (img_of(addr)) return;     /* already translated */
    u32 npg = (u32)((len + LX_PAGE - 1) / LX_PAGE);
    u8 *mark = (u8 *)calloc(npg ? npg : 1, 1);
    if (!mark) return;
    const u8 *p = (const u8 *)addr;
    size_t i = 0, found = 0;
    while (i + 2 <= len) {
        x86i in;
        size_t avail = len - i < 15 ? len - i : 15;
        int l = x86_decode(p + i, avail, &in);
        if (!l) { i++; continue; }
        if (!in.vex && in.map == 1 && in.opcode == 0x05) { mark[i / LX_PAGE] = 1; found++; }
        i += (size_t)l;
    }
    for (u32 pg = 0; pg < npg;) {
        if (!mark[pg]) { pg++; continue; }
        u32 e = pg;
        while (e < npg && mark[e]) e++;
        LOGI("dbt: %s: %zu `syscall` instruction(s) in a Windows image; translating pages %llx-%llx", name, found, addr + (u64)pg * LX_PAGE, addr + (u64)e * LX_PAGE);
        xrl_dbt_attach(name, addr + (u64)pg * LX_PAGE, (u64)(e - pg) * LX_PAGE);
        pg = e;
    }
    free(mark);
}

static int policy_on(void);

/* Wine loads a PE image into anonymous memory (pread, not mmap) and then mprotects the sections. When a range
 * becomes executable inside a Windows image (an "MZ" header a few pages below) and the program being run is
 * one of the anti-cheat's (its command line matches the XRL_DBT policy), look for raw `syscall`s. Returns 1 if the
 * range is part of a Windows image. */
int xrl_dbt_exec_hint(u64 addr, u64 len)
{
    if (!policy_on() || len < LX_PAGE || len > (64u << 20) || (addr & (LX_PAGE - 1))) return 0;
    for (int back = 1; back <= 8; back++) {
        u64 hdr = addr - (u64)back * LX_PAGE;
        if (!xrl_mem_is_mapped(hdr)) return 0;
        const u8 *h = (const u8 *)hdr;
        if (h[0] == 'M' && h[1] == 'Z') {
            u32 lfa = *(const u32 *)(h + 0x3c);
            if (lfa < LX_PAGE - 4 && !memcmp(h + lfa, "PE\0\0", 4)) {
                char name[160];
                snprintf(name, sizeof name, "%s (Windows image at %llx)", G.argv[1], hdr);
                xrl_dbt_attach_pe(name, addr, len);
            }
            return 1;
        }
    }
    return 0;
}

/* A non-PE anonymous region that becomes executable but NOT writable: the anti-cheat's own module
 * (an ELF payload the bootstrapper downloaded) unpacks itself into memory and mprotect()s the code
 * R+X. Like a Windows image's syscall pages, this native code issues raw Linux `syscall`s that must
 * reach the layer, but it carries no "MZ" header (so xrl_dbt_exec_hint does not see it) and is not
 * writable (so xrl_dbt_generated's write-watch does not cover it) -- it simply ran untranslated and
 * faulted. Scan it for raw `syscall`s and translate the pages that have them (the rest runs native);
 * no write-watch is needed as a read-only-executable region is not rewritten in place (a later
 * mprotect back to writable re-enters the generated-code path). */
void xrl_dbt_attach_exec(u64 addr, u64 len)
{
    if (!policy_on() || len < LX_PAGE || len > (64u << 20) || (addr & (LX_PAGE - 1))) return;
    if (img_of(addr) || !xrl_mem_is_mapped(addr)) return;
    u32 npg = (u32)((len + LX_PAGE - 1) / LX_PAGE);
    u8 *mark = (u8 *)calloc(npg ? npg : 1, 1);
    if (!mark) return;
    const u8 *p = (const u8 *)addr;
    size_t i = 0, found = 0;
    while (i + 2 <= len) {
        x86i in;
        size_t avail = len - i < 15 ? len - i : 15;
        int l = x86_decode(p + i, avail, &in);
        if (!l) { i++; continue; }
        if (!in.vex && in.map == 1 && in.opcode == 0x05) { mark[i / LX_PAGE] = 1; found++; }
        i += (size_t)l;
    }
    for (u32 pg = 0; pg < npg;) {
        if (!mark[pg]) { pg++; continue; }
        u32 e = pg;
        while (e < npg && mark[e]) e++;
        char name[96];
        snprintf(name, sizeof name, "exec-unpacked at %llx", addr);
        LOGI("dbt: %zu raw `syscall`(s) in an R+X region; translating pages %llx-%llx", found, addr + (u64)pg * LX_PAGE, addr + (u64)e * LX_PAGE);
        xrl_dbt_attach(name, addr + (u64)pg * LX_PAGE, (u64)(e - pg) * LX_PAGE);
        pg = e;
    }
    free(mark);
}

/* The mapping [addr, addr+len) of `name` becomes translated code. */
int xrl_dbt_attach(const char *name, u64 addr, u64 len)
{
    int rc = -1;
    AcquireSRWLockExclusive(&g_lock);
    int slot = g_nimg;
    for (int i = 0; i < g_nimg; i++) if (!g_img[i].hi) { slot = i; break; }      /* a dropped image's slot */
    if (slot >= MAX_IMG) { LOGE("dbt: more than %d translated images", MAX_IMG); goto out; }
    u64 cap = len * 8; if (cap < (1u << 20)) cap = 1u << 20; if (cap > (64u << 20)) cap = 64u << 20;
    cap = ALIGN_UP(cap, LX_PAGE);
    len = ALIGN_UP(len, LX_PAGE);
    /* RIP-relative operands and direct jumps need the cache within 2 GiB of the image: right after it, else anywhere near */
    slong cm = -1;
    /* up in 4 MiB steps from the image, then down: a Windows image sits far from the guest arena, in space
     * Wine manages with fixed addresses, and the first free stretch near it is what is wanted */
    for (int k = 0; k < 512 && cm < 0; k++) {
        u64 step = (u64)(k & 255) * (4u << 20), a;
        if (k < 256) a = ALIGN_UP(addr + len, 0x10000) + step;
        else a = addr > cap + step + 0x10000 ? addr - cap - step : 0;
        if (a < 0x10000) continue;
        cm = xrl_mem_mmap(a, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                          LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE | LX_MAP_FIXED_NOREPLACE, -1, 0);
        if (cm >= 0 && ((u64)cm > addr ? (u64)cm + cap - addr : addr + len - (u64)cm) > 0x70000000ull) { xrl_mem_munmap((u64)cm, cap); cm = -1; }
    }
    if (cm < 0) cm = xrl_mem_mmap(0, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                                  LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE, -1, 0);
    if (cm < 0) { LOGE("dbt: %s: no memory for the code cache (%lld)", name, cm); goto out; }
    u64 far_ = (u64)cm > addr ? (u64)cm + cap - addr : addr + len - (u64)cm;
    if (far_ > 0x70000000ull) {
        LOGE("dbt: %s: the code cache %llx is too far from the image %llx for RIP-relative operands", name, (u64)cm, addr);
        xrl_mem_munmap((u64)cm, cap);
        goto out;
    }
    dimg *im = &g_img[slot];
    memset(im, 0, sizeof *im);
    im->lo = addr; im->hi = addr + len;
    im->cache = (u8 *)cm; im->cache_va = (u64)cm; im->cap = cap; im->used = 64;
    strncpy_s(im->name, sizeof im->name, name, _TRUNCATE);
    memset(im->cache, 0xcc, (size_t)cap);
    u64 *slots = (u64 *)im->cache;
    slots[0] = (u64)(uintptr_t)&xrl_rawsys;
    slots[1] = (u64)(uintptr_t)&xrl_dbt_ijmp;
    slots[2] = (u64)(uintptr_t)&xrl_dbt_icall;
    slots[3] = (u64)(uintptr_t)&xrl_dbt_ret;
    DWORD old;
    if (xrl_host_protect((void *)addr, (size_t)len, PAGE_READONLY, &old)) {
        LOGE("dbt: %s: cannot make the image non-executable", name);
        xrl_mem_munmap((u64)cm, cap);
        goto out;
    }
    xrl_dbt_rng[slot * 2] = addr;
    MemoryBarrier();
    xrl_dbt_rng[slot * 2 + 1] = len;
    if (slot == g_nimg) g_nimg++;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, dbt_veh);
    LOGI("dbt: %s: %llu bytes at %llx are translated, not executed (cache %llu KiB at %llx); the image's bytes are untouched",
         name, len, addr, cap >> 10, (u64)cm);
    rc = 0;
out:
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}

/* ------------------------------------------------- generated code
 * A program that carries its own system-call stubs (the EAC bootstrapper copies a 21-byte `syscall` wrapper
 * into a VirtualAlloc'd RWX page and calls it) executes `syscall` from memory it wrote itself. The
 * image-page scan above never sees it. So, while the program is one the policy selects, an anonymous
 * mapping that is writable *and* executable is a candidate: its host protection is read-write only (the
 * guest still sees rwx in /proc/self/maps), and the first instruction fetch from it faults. The handler
 * looks at the page then: with a `syscall` in it the page is translated like an image (and a later write to
 * it drops the translation and starts over); without, it simply becomes executable again, and the program
 * runs it natively as before. Limits, stated: code written into a page after its first execution is not
 * looked at when the page had no `syscall`; at most MAX_WATCH ranges of at most 256 KiB are watched (more,
 * or bigger, run natively and are logged); a thread executing a translation while another thread rewrites
 * the page is not stopped. */
#define GEN_MAX (256u << 10)

static int policy_on(void)
{
    static int mode = -1;
    if (mode < 0) {
        mode = G.argc > 1 && G.argv[1] && xrl_dbt_wanted(G.argv[1]);
        /* A process its tracer injects anti-cheat code into (XRL_DBT_MEMFD=1, set for every
         * spawned/traced process in task.c) runs that code even though its own program name
         * (e.g. RocketLeague.exe) does not match the policy.  The injected module calls through
         * Wine's native-syscall pages, which must be translated too, so turn the policy fully on. */
        if (!mode) { char v[4]; mode = GetEnvironmentVariableA("XRL_DBT_MEMFD", v, sizeof v) && v[0] == '1'; }
    }
    return mode;
}

static int page_has_syscall(u64 pg)
{
    const u8 *p = (const u8 *)pg;
    size_t i = 0;
    while (i + 2 <= LX_PAGE) {
        x86i in;
        size_t avail = LX_PAGE - i < 15 ? LX_PAGE - i : 15;
        int l = x86_decode(p + i, avail, &in);
        if (!l) { i++; continue; }
        if (!in.vex && in.map == 1 && in.opcode == 0x05) return 1;
        i += (size_t)l;
    }
    return 0;
}

static void watch_add(u64 lo, u64 hi)      /* g_genlock held */
{
    if (g_nwatch >= MAX_WATCH) { LOGW("dbt: more than %d watched ranges of generated code: %llx runs natively", MAX_WATCH, lo); return; }
    DWORD old;
    if (xrl_host_protect((void *)lo, (size_t)(hi - lo), PAGE_READWRITE, &old)) { LOGE("dbt: cannot watch generated code at %llx", lo); return; }
    g_watch[g_nwatch].lo = lo; g_watch[g_nwatch].hi = hi;
    MemoryBarrier();
    g_nwatch++;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, dbt_veh);
}

static void watch_remove_at(int i) { g_watch[i] = g_watch[g_nwatch - 1]; g_nwatch--; }

/* the translations of a generated-code image go; its cache is freed and its slot can be reused. g_lock held. */
static void img_drop_locked(dimg *im)
{
    for (u32 i = 0; i < (1u << CM_BITS); i++) {
        u64 o = g_cmap[i].o;
        if (!o || o < im->lo || o >= im->hi) continue;
        if (g_cmap[i].t) {      /* the map entry stays as a dead key so that probe chains stay intact */
            for (u32 k = 0; k < 4; k++) {
                u64 *e = &xrl_dbt_tab[(th(o) + k) * 2];
                if (e[0] == o) { e[1] = 0; MemoryBarrier(); e[0] = 1; break; }
            }
        }
        g_cmap[i].t = 0;
    }
    int idx = (int)(im - g_img);
    xrl_dbt_rng[idx * 2 + 1] = 0;
    MemoryBarrier();
    xrl_dbt_rng[idx * 2] = 0;
    xrl_mem_munmap(im->cache_va, im->cap);
    im->lo = im->hi = 0;
}

/* [addr, addr+len) is a writable and executable anonymous mapping (mmap or mprotect) */
void xrl_dbt_generated(u64 addr, u64 len)
{
    if (!policy_on() || !len) return;
    u64 lo = addr & ~(LX_PAGE - 1), hi = ALIGN_UP(addr + len, LX_PAGE);
    if (hi - lo > GEN_MAX) {
        static volatile LONG warned;
        if (!InterlockedExchange(&warned, 1)) LOGW("dbt: %llu KiB of writable+executable memory at %llx is too big to watch for system-call stubs: it runs natively", (hi - lo) >> 10, lo);
        return;
    }
    AcquireSRWLockExclusive(&g_genlock);
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nimg; i++)                        /* made writable again: whatever was translated is stale */
        if (g_img[i].gen && g_img[i].hi && g_img[i].lo < hi && g_img[i].hi > lo) img_drop_locked(&g_img[i]);
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nwatch; i++)
        if (g_watch[i].lo < hi && g_watch[i].hi > lo) { if (g_watch[i].lo < lo) lo = g_watch[i].lo; if (g_watch[i].hi > hi) hi = g_watch[i].hi; watch_remove_at(i--); }
    if (xrl_mem_pin_stack(lo, hi - lo)) LOGW("dbt: cannot commit generated code at %llx", lo);   /* committed, and never swapped out */
    else {
        watch_add(lo, hi);
        LOGI("dbt: generated code: %llu KiB at %llx is writable and executable; watched for `syscall` (host: read-write until first fetched)", (hi - lo) >> 10, lo);
    }
    ReleaseSRWLockExclusive(&g_genlock);
}

/* [addr, addr+len) is unmapped or remapped */
void xrl_dbt_unmapped(u64 addr, u64 len)
{
    if (!g_nwatch && !g_has_gen) return;
    u64 hi = addr + len;
    AcquireSRWLockExclusive(&g_genlock);
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nimg; i++)
        if (g_img[i].gen && g_img[i].hi && g_img[i].lo < hi && g_img[i].hi > addr) img_drop_locked(&g_img[i]);
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nwatch; i++)
        if (g_watch[i].lo < hi && g_watch[i].hi > addr) watch_remove_at(i--);
    ReleaseSRWLockExclusive(&g_genlock);
}

/* instruction fetch from a watched page: look at it. 1: retry the instruction */
static int gen_fetch(u64 pc)
{
    u64 lo = 0, hi = 0;
    AcquireSRWLockExclusive(&g_genlock);
    for (int i = 0; i < g_nwatch; i++)
        if (pc >= g_watch[i].lo && pc < g_watch[i].hi) { lo = g_watch[i].lo; hi = g_watch[i].hi; watch_remove_at(i); break; }
    if (!hi) {      /* another thread got here first (or it was never ours): retry if the page has changed state */
        MEMORY_BASIC_INFORMATION mbi;
        int r = img_of(pc) != NULL || (VirtualQuery((void *)pc, &mbi, sizeof mbi) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)));
        ReleaseSRWLockExclusive(&g_genlock);
        return r;
    }
    char name[96];
    snprintf(name, sizeof name, "generated code at %llx", lo);
    for (u64 pg = lo; pg < hi;) {
        int has = page_has_syscall(pg);
        u64 e = pg + LX_PAGE;
        while (e < hi && page_has_syscall(e) == has) e += LX_PAGE;
        if (has && !xrl_dbt_attach(name, pg, e - pg)) {
            AcquireSRWLockExclusive(&g_lock);
            dimg *im = img_of(pg);
            if (im) im->gen = 1;
            ReleaseSRWLockExclusive(&g_lock);
            g_has_gen = 1;
            LOGI("dbt: %s: `syscall` in pages %llx-%llx of generated code; translated", name, pg, e);
        } else {
            DWORD old;
            xrl_host_protect((void *)pg, (size_t)(e - pg), PAGE_EXECUTE_READWRITE, &old);   /* no `syscall`: native, as before */
        }
        pg = e;
    }
    ReleaseSRWLockExclusive(&g_genlock);
    return 1;
}

/* a write to a translated page of generated code: the translation is stale. 1: retry the write */
static int gen_write(u64 a)
{
    int r = 0;
    AcquireSRWLockExclusive(&g_genlock);
    AcquireSRWLockExclusive(&g_lock);
    dimg *im = img_of(a);
    u64 lo = 0, hi = 0;
    if (im && im->gen) { lo = im->lo; hi = im->hi; img_drop_locked(im); }
    ReleaseSRWLockExclusive(&g_lock);
    if (hi) {
        watch_add(lo, hi);          /* host: read-write again; the next fetch looks at it again */
        LOGI("dbt: generated code at %llx was written again: its translation is dropped", lo);
        r = 1;
    } else {            /* not ours, or another thread dropped it meanwhile */
        MEMORY_BASIC_INFORMATION mbi;
        r = VirtualQuery((void *)a, &mbi, sizeof mbi) && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
    }
    ReleaseSRWLockExclusive(&g_genlock);
    return r;
}


void xrl_dbt_stats(u64 *faults, u64 *translated) { *faults = (u64)g_nfault; *translated = (u64)g_ntrans; }

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
 * Limits, stated: self-modifying code and code the program generates are not followed (they would
 * be executed from original pages: a fault, and so a SIGSEGV for the guest); `ret imm16`, far
 * transfers, and %fs operands in branch instructions are not translated (the site becomes ud2 and
 * is logged); the unwinder sees return addresses of the original image for calls inside the image but
 * cache addresses for frames that called native code; a fault inside translated code reports the cache
 * address; unmapping an image does not drop its translations. The cache is a visible anonymous
 * mapping.
 *
 * Selected by XRL_DBT: unset/0 off, 1 every image rewrite.c would rewrite, else a comma-separated list
 * of path substrings.
 */
#include "xrl_internal.h"
#include "rewrite_core.h"
#include "x86dec.h"

extern void xrl_dbt_ijmp(void), xrl_dbt_icall(void), xrl_dbt_ret(void), xrl_rawsys(void);

#define TAB_N 262144u
u64 xrl_dbt_tab[(TAB_N + 4) * 2];        /* {original, translated}: read by dbt.asm */
u64 xrl_dbt_rng[8];                      /* {lo, size} x 4: read by dbt.asm */

#define MAX_IMG 4
typedef struct dimg {
    u64 lo, hi;                          /* the mapping that is translated */
    u8 *cache; u64 cache_va, cap, used;
    char name[64];
} dimg;
static dimg g_img[MAX_IMG];
static int g_nimg;
static SRWLOCK g_lock = SRWLOCK_INIT;
static PVOID g_veh;
static volatile LONG64 g_nfault, g_ntrans;

/* ---------------------------------------------------------------- tables */
static u32 th(u64 t) { return (u32)((t * 0x9e3779b97f4a7c15ull) >> 46); }   /* = DBT_HASH in dbt.asm */

static void tab_put(u64 orig, u64 tr)
{
    for (u32 i = 0; i < 4; i++) {
        u64 *e = &xrl_dbt_tab[(th(orig) + i) * 2];
        if (e[0] == orig) { e[1] = tr; return; }
        if (!e[0]) {
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

static void cm_put(u64 o, u64 t)
{
    for (u32 i = (u32)((o * 0x9e3779b97f4a7c15ull) >> (64 - CM_BITS));; i = (i + 1) & ((1u << CM_BITS) - 1)) {
        if (g_cmap[i].o == o || !g_cmap[i].o) { g_cmap[i].o = o; g_cmap[i].t = t; break; }
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
        int len = x86_decode(p, (size_t)(end - pc < 15 ? end - pc : 15), &in);
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
            if (op == 0xa2 || op == 0x31 || op == 0x01) { /* cpuid, rdtsc, xgetbv...: executed as they are */ }
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
    if (!ok) { LOGE("dbt: %s: translation of %llx failed", im->name, pc); return 0; }
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

/* from dbt.asm through the syscall path (a computed jump to an address of an image without a translation) */
slong xrl_dbt_enter_sys(u64 pc)
{
    u64 r = xrl_dbt_translate(pc);
    return (slong)(r ? r : pc);
}

/* instruction fetch from a translated image */
static LONG CALLBACK dbt_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 8) return EXCEPTION_CONTINUE_SEARCH;
    u64 pc = (u64)r->ExceptionInformation[1];
    if (!img_of(pc)) return EXCEPTION_CONTINUE_SEARCH;
    xrl_thread *t = xrl_cur();
    u64 stk[2];
    int g = xrl_teb_enter_host(t);
    xrl_teb_stack_here(stk);
    u64 tr = xrl_dbt_translate(pc);
    xrl_teb_stack_back(stk);
    xrl_teb_restore(t, g);
    if (!tr) return EXCEPTION_CONTINUE_SEARCH;
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
        if (!n || n >= sizeof v || !strcmp(v, "0")) mode = 0;
        else if (!strcmp(v, "1")) mode = 1;
        else { mode = 2; strcpy_s(list, sizeof list, v); }
    }
    if (mode == 0) return 0;
    if (mode == 1) return 1;
    char buf[256]; strcpy_s(buf, sizeof buf, list);
    char *ctx = NULL;
    for (char *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx))
        if (*tok && path && strstr(path, tok)) return 1;
    return 0;
}

/* The mapping [addr, addr+len) of `name` becomes translated code. */
int xrl_dbt_attach(const char *name, u64 addr, u64 len)
{
    int rc = -1;
    AcquireSRWLockExclusive(&g_lock);
    if (g_nimg >= MAX_IMG) { LOGE("dbt: more than %d translated images", MAX_IMG); goto out; }
    u64 cap = len * 8; if (cap < (1u << 20)) cap = 1u << 20; if (cap > (64u << 20)) cap = 64u << 20;
    cap = ALIGN_UP(cap, LX_PAGE);
    len = ALIGN_UP(len, LX_PAGE);
    /* RIP-relative operands and direct jumps need the cache within 2 GiB of the image: right after it, else anywhere near */
    slong cm = xrl_mem_mmap(addr + len, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                            LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE | LX_MAP_FIXED_NOREPLACE, -1, 0);
    if (cm < 0) cm = xrl_mem_mmap(0, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                                  LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE, -1, 0);
    if (cm < 0) { LOGE("dbt: %s: no memory for the code cache (%lld)", name, cm); goto out; }
    u64 far_ = (u64)cm > addr ? (u64)cm + cap - addr : addr + len - (u64)cm;
    if (far_ > 0x70000000ull) {
        LOGE("dbt: %s: the code cache %llx is too far from the image %llx for RIP-relative operands", name, (u64)cm, addr);
        xrl_mem_munmap((u64)cm, cap);
        goto out;
    }
    dimg *im = &g_img[g_nimg];
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
    xrl_dbt_rng[g_nimg * 2] = addr;
    MemoryBarrier();
    xrl_dbt_rng[g_nimg * 2 + 1] = len;
    g_nimg++;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, dbt_veh);
    LOGI("dbt: %s: %llu bytes at %llx are translated, not executed (cache %llu KiB at %llx); the image's bytes are untouched",
         name, len, addr, cap >> 10, (u64)cm);
    rc = 0;
out:
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}

void xrl_dbt_stats(u64 *faults, u64 *translated) { *faults = (u64)g_nfault; *translated = (u64)g_ntrans; }

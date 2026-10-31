/*
 * rewrite_core.c - see rewrite_core.h. Pure C99; also built by layer/tests/rewrite_test.c.
 */
#include "rewrite_core.h"
#include "x86dec.h"
#include <stdlib.h>
#include <string.h>

enum { K_PLAIN = 0, K_SYSCALL = 1, K_FS = 2, K_UNIT = 3, K_DONE = 0x80 };

typedef struct eb { uint8_t b[640]; int n; } eb;

static void put(eb *e, const void *p, int n) { memcpy(e->b + e->n, p, (size_t)n); e->n += n; }
static void put1(eb *e, unsigned v) { e->b[e->n++] = (uint8_t)v; }
static void put32(eb *e, uint32_t v) { memcpy(e->b + e->n, &v, 4); e->n += 4; }
static void put64(eb *e, uint64_t v) { memcpy(e->b + e->n, &v, 8); e->n += 8; }

/* ------------------------------------------------------------------ units */
static int is_seg_unit(const x86i *in, int seg)
{
    if (in->seg != seg) return 0;
    if (in->map == 0 && in->opcode == 0x8d) return 0;               /* lea ignores the override */
    if (in->has_modrm && (in->modrm >> 6) != 3) return 1;
    if (in->map == 0 && in->opcode >= 0xa0 && in->opcode <= 0xa3) return 1;   /* moffs forms */
    return 0;
}
static int is_syscall(const x86i *in) { return !in->vex && in->map == 1 && in->opcode == 0x05; }

/* ------------------------------------------------------------ stub pieces */
static const uint8_t k_pad[] = { 0x48, 0x8d, 0x64, 0x24, 0x80 };                       /* lea -0x80(%rsp),%rsp */
static const uint8_t k_unpad[] = { 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00 };   /* lea  0x80(%rsp),%rsp (always 8 bytes: rawsys relies on it) */

static void emit_syscall(eb *e, uint64_t stub_va, uint64_t slot)
{
    put(e, k_pad, 5);
    put1(e, 0xff); put1(e, 0x15);
    put32(e, (uint32_t)(int32_t)(slot - (stub_va + (uint64_t)e->n + 4)));              /* call *slot(%rip) */
    put(e, k_unpad, 8);
}

static void emit_push(eb *e, int r) { if (r >= 8) put1(e, 0x41); put1(e, 0x50 + (r & 7)); }
static void emit_pop(eb *e, int r) { if (r >= 8) put1(e, 0x41); put1(e, 0x58 + (r & 7)); }
/* mov %gs:off,%r64 */
static void emit_load_tp(eb *e, int r, uint32_t off)
{
    put1(e, 0x65); put1(e, 0x48 | (r >= 8 ? 4 : 0)); put1(e, 0x8b);
    put1(e, ((r & 7) << 3) | 4); put1(e, 0x25); put32(e, off);
}

/* The instruction `ins` (decoded as `in`) with its segment override dropped and its memory
 * operand rebuilt as [base_new + index_new*2^scale + disp] under ModRM mod `nmod`. */
static void emit_insn_with_operand(eb *e, const uint8_t *ins, const x86i *in, int nmod, int scale, int index_new, int base_new)
{
    for (int i = 0; i < in->nprefix; i++) if (ins[i] != in->seg) put1(e, ins[i]);
    unsigned rex = 0x40 | (in->rex & 0x0c) | ((index_new >= 8) ? 2 : 0) | (base_new >= 8 ? 1 : 0);
    if (rex != 0x40 || in->rex) put1(e, rex);
    int ops = in->op_off - (in->map == 1 ? 1 : in->map >= 2 ? 2 : 0);
    put(e, ins + ops, in->modrm_off - ops);
    put1(e, (nmod << 6) | (in->modrm & 0x38) | 4);
    put1(e, (scale << 6) | ((index_new & 7) << 3) | (base_new & 7));
    if (in->disp_size) put(e, ins + in->disp_off, in->disp_size);
    if (in->imm_size) put(e, ins + in->imm_off, in->imm_size);
}

/* a %fs (or, with gs_slot_off, %gs) memory operand rebuilt relative to the base kept in that segment's TEB slot */
static int emit_fs(eb *e, const xrw *w, const uint8_t *ins, const x86i *in)
{
    if (in->vex || !in->has_modrm || in->addr32 || (in->flags & (X86_RIPREL | X86_STACK | X86_CF))) return -1;
    uint32_t slot = (uint32_t)(in->seg == 0x65 ? w->gs_slot_off : w->tp_gs_off);
    unsigned mod = in->modrm >> 6, rm = in->modrm & 7;
    int reg = ((in->modrm >> 3) & 7) | ((in->rex & 4) ? 8 : 0);
    int has_sib = rm == 4, no_index = 1, no_base = 0, base = 0, index = 0, scale = 0;
    if (has_sib) {
        int bl = in->sib & 7;
        scale = in->sib >> 6;
        index = ((in->sib >> 3) & 7) | ((in->rex & 2) ? 8 : 0);
        no_index = ((in->sib >> 3) & 7) == 4 && !(in->rex & 2);
        no_base = mod == 0 && bl == 5;
        base = bl | ((in->rex & 1) ? 8 : 0);
    } else base = rm | ((in->rex & 1) ? 8 : 0);

    /* Fast path: a plain 32/64-bit load from tp+disp32 into reg R becomes
     *   mov %gs:tp,%R ; mov disp32(%R),%R     (no scratch, no stack) */
    int load = !in->opsz16 && ((in->map == 0 && (in->opcode == 0x8b || in->opcode == 0x63)) ||
                               (in->map == 1 && (in->opcode == 0xb6 || in->opcode == 0xb7 || in->opcode == 0xbe || in->opcode == 0xbf)));
    if (load && no_base && no_index && has_sib && mod == 0) {
        emit_load_tp(e, reg, slot);
        emit_insn_with_operand(e, ins, in, 2, 0, 4, reg);
        return 0;
    }

    /* General path: borrow a scratch register for the tp, restoring it afterwards. */
    unsigned used = (1u << reg) | (no_base ? 0 : (1u << base)) | (no_index ? 0 : (1u << index));
    static const int hi[] = { 11, 10, 9, 8 }, lo[] = { 3, 6, 7, 1, 2, 0 };
    int S = -1;
    int hi_ok = in->rex || ((reg & 7) < 4);        /* a new REX would turn %ah..%bh into %spl..%dil */
    if (hi_ok) for (int i = 0; i < 4 && S < 0; i++) if (!(used & (1u << hi[i]))) S = hi[i];
    if (S < 0) for (int i = 0; i < 6 && S < 0; i++) if (!(used & (1u << lo[i]))) S = lo[i];
    if (S < 0) return -1;

    put(e, k_pad, 5);
    emit_push(e, S);
    emit_load_tp(e, S, slot);
    int nmod = (int)mod, nscale = scale, nindex, nbase;
    if (!has_sib || (!no_base && no_index)) {
        /* base only: tp + base (rsp cannot be an index register) */
        if (base == 4) { nbase = 4; nindex = S; } else { nbase = S; nindex = base; }
        nscale = 0;
    } else if (no_base && no_index) {
        nmod = 2; nbase = S; nindex = 4; nscale = 0;
    } else if (no_base) {
        nmod = 2; nbase = S; nindex = index;
    } else {
        /* base + index: S += base via lea (flags untouched), operand becomes (S,index,scale) */
        int ib = base == 4 ? S : base, bb = base == 4 ? 4 : S;   /* lea (bb,ib,1),S */
        put1(e, 0x48 | (S >= 8 ? 4 : 0) | (ib >= 8 ? 2 : 0) | (bb >= 8 ? 1 : 0));
        put1(e, 0x8d); put1(e, ((S & 7) << 3) | 4); put1(e, ((ib & 7) << 3) | (bb & 7));
        nbase = S; nindex = index;
    }
    emit_insn_with_operand(e, ins, in, nmod, nscale, nindex, nbase);
    emit_pop(e, S);
    put(e, k_unpad, 8);
    return 0;
}

/* The same replacement sequences for the translator (dbt.c), written at any address.
 * Both return the length written (at most 640 bytes); xrw_emit_fs returns -1 for an operand form it
 * cannot rebuild (RIP-relative, VEX, stack or branch instructions with %fs). */
int xrw_emit_syscall(uint8_t *out, uint64_t out_va, uint64_t slot)
{
    eb e; e.n = 0;
    emit_syscall(&e, out_va, slot);
    memcpy(out, e.b, (size_t)e.n);
    return e.n;
}

int xrw_emit_gs(uint64_t gs_slot_off, uint8_t *out, const uint8_t *ins, const x86i *in)
{
    eb e; e.n = 0;
    xrw w; memset(&w, 0, sizeof w);
    w.gs_slot_off = gs_slot_off;
    if (in->seg != 0x65 || emit_fs(&e, &w, ins, in)) return -1;
    memcpy(out, e.b, (size_t)e.n);
    return e.n;
}

int xrw_emit_fs(uint64_t tp_gs_off, uint8_t *out, const uint8_t *ins, const x86i *in)
{
    eb e; e.n = 0;
    xrw w; memset(&w, 0, sizeof w);
    w.tp_gs_off = tp_gs_off;
    if (emit_fs(&e, &w, ins, in)) return -1;
    memcpy(out, e.b, (size_t)e.n);
    return e.n;
}

/* mov %fs:0x0,%r64 (64 REX.W 8b modrm 25 00000000), the form tools/xrl-tpfix.py patches */
static int is_tp_load(const uint8_t *ins, int len)
{
    return len == 9 && ins[0] == 0x64 && (ins[1] == 0x48 || ins[1] == 0x4c) && ins[2] == 0x8b &&
           (ins[3] & 0xc7) == 0x04 && ins[4] == 0x25 && !ins[5] && !ins[6] && !ins[7] && !ins[8];
}

/* ---------------------------------------------------------------- regions */
typedef struct rgn {
    uint32_t *off;       /* instruction offsets within the range */
    uint8_t  *len, *kind;
    size_t n, cap;
    uint8_t *tgt;        /* bit per byte offset from the region start: branch target */
    size_t tgt_cap;
    size_t base;         /* region start offset */
} rgn;

static int rg_push(rgn *r, uint32_t off, int len, int kind)
{
    if (r->n == r->cap) {
        size_t c = r->cap ? r->cap * 2 : 4096;
        uint32_t *o = (uint32_t *)realloc(r->off, c * 4);
        if (!o) return -1;
        r->off = o;
        uint8_t *l = (uint8_t *)realloc(r->len, c);
        if (!l) return -1;
        r->len = l;
        uint8_t *k = (uint8_t *)realloc(r->kind, c);
        if (!k) return -1;
        r->kind = k;
        r->cap = c;
    }
    r->off[r->n] = off; r->len[r->n] = (uint8_t)len; r->kind[r->n] = (uint8_t)kind; r->n++;
    return 0;
}

static int is_target(const rgn *r, size_t i)
{
    size_t b = (size_t)r->off[i] - r->base;
    return (r->tgt[b >> 3] >> (b & 7)) & 1;
}

/* Branches the stub can re-emit with a fresh rel32: jcc, jmp. (call would put a
 * stub address on the stack, where an unwinder cannot find it; loop/jrcxz have no
 * rel32 form.) */
static int is_reloc_branch(const x86i *t)
{
    if (!(t->flags & X86_REL) || t->nprefix || t->vex) return 0;
    if (t->map == 0) return (t->opcode >= 0x70 && t->opcode <= 0x7f) || t->opcode == 0xeb || t->opcode == 0xe9;
    return t->map == 1 && t->opcode >= 0x80 && t->opcode <= 0x8f;
}

/* Can instruction i be moved into a stub? Units are movable too (they are
 * replaced there); plain instructions may depend on their own address only
 * through a RIP-relative operand or a jcc/jmp displacement, which compose()
 * re-bases. */
static int movable(const uint8_t *code, const rgn *r, size_t i)
{
    x86i t;
    if (r->kind[i] & K_DONE) return 0;
    if (r->kind[i] & K_UNIT) return 1;
    if (x86_decode(code + r->off[i], r->len[i], &t) != r->len[i]) return 0;
    if (is_seg_unit(&t, 0x64)) return 0;
    if (t.flags & X86_REL) return is_reloc_branch(&t);
    if (t.flags & X86_CF) {
        if (t.map == 0 && (t.opcode == 0xc2 || t.opcode == 0xc3)) return 1;                          /* ret */
        if (t.map == 0 && t.opcode == 0xff && ((t.modrm >> 3) & 7) >= 4) return 1;                   /* jmp * */
        return 0;
    }
    return 1;
}

/* Control never falls through this instruction, so what follows it is probably an entry point: a function
 * reached through a pointer, a PLT or a jump table, which no direct branch in the image shows. A window
 * must not span the boundary (its jmp would overwrite the entry's first bytes), see movable()'s callers. */
static int ends_flow(const uint8_t *code, const rgn *r, size_t i)
{
    x86i t;
    if (x86_decode(code + r->off[i], r->len[i], &t) != r->len[i] || t.vex) return 0;
    if (t.map == 1 && t.opcode == 0x05) {
        /* syscall right after `mov $15/$60/$231, %eax|%rax`: rt_sigreturn (musl's __restore_rt) and exit never return, and
         * the next bytes are another function (musl puts __sigsetjmp there: its first instruction was overwritten). */
        if (i == 0 || r->off[i - 1] + r->len[i - 1] != r->off[i]) return 0;
        const uint8_t *p = code + r->off[i - 1];
        uint32_t nr;
        if (r->len[i - 1] == 7 && p[0] == 0x48 && p[1] == 0xc7 && p[2] == 0xc0) memcpy(&nr, p + 3, 4);
        else if (r->len[i - 1] == 5 && p[0] == 0xb8) memcpy(&nr, p + 1, 4);
        else return 0;
        return nr == 15 || nr == 60 || nr == 231;
    }
    if (t.map == 1) return t.opcode == 0x0b;                                                         /* ud2 */
    if (t.map != 0) return 0;
    switch (t.opcode) {
    case 0xc2: case 0xc3: case 0xca: case 0xcb: case 0xcc: case 0xcf: case 0xe9: case 0xeb: case 0xf4: return 1;   /* ret, int3, iret, jmp, hlt */
    case 0xff: return ((t.modrm >> 3) & 7) == 4 || ((t.modrm >> 3) & 7) == 5;                         /* jmp * */
    }
    return 0;
}

#define LOGF(...) do { if (w->log) w->log(__VA_ARGS__); } while (0)

static void jmp_abs(eb *e, uint64_t target)
{
    put1(e, 0xff); put1(e, 0x25); put32(e, 0); put64(e, target);     /* jmp *0(%rip); .quad target */
}

/* Append the stub bytes to the pool (or just count them); returns its guest address. */
static int pool_commit(xrw *w, const eb *e, uint64_t *va)
{
    size_t at = (w->pool_used + 15) & ~(size_t)15;
    *va = w->pool_vaddr + at;
    w->pool_used = at + (size_t)e->n;
    if (!w->pool) return 0;
    if (w->pool_used > w->pool_cap) return -1;
    memcpy(w->pool + at, e->b, (size_t)e->n);
    return 0;
}
static uint64_t pool_next_va(const xrw *w) { return w->pool_vaddr + ((w->pool_used + 15) & ~(size_t)15); }

/* Compose the stub for instructions [lo..hi]. Returns 0 or -1 (a unit we cannot rewrite). */
static int compose(eb *e, xrw *w, const uint8_t *code, uint64_t vaddr, const rgn *r, size_t lo, size_t hi, uint64_t stub_va)
{
    e->n = 0;
    for (size_t i = lo; i <= hi; i++) {
        const uint8_t *ins = code + r->off[i];
        x86i t;
        if (x86_decode(ins, r->len[i], &t) != r->len[i]) return -1;
        if (!(r->kind[i] & K_UNIT)) {
            uint64_t here = vaddr + r->off[i], there = stub_va + (uint64_t)e->n;
            if (is_reloc_branch(&t)) {
                uint64_t target = here + r->len[i] + (int64_t)t.rel;
                int cc = t.map == 0 ? (t.opcode == 0xeb || t.opcode == 0xe9 ? -1 : t.opcode & 15) : t.opcode & 15;
                int n = cc < 0 ? 5 : 6;
                int64_t rel = (int64_t)target - (int64_t)(there + (uint64_t)n);
                if (rel < -0x7ff00000ll || rel > 0x7ff00000ll) return -1;
                if (cc < 0) put1(e, 0xe9); else { put1(e, 0x0f); put1(e, 0x80 + cc); }
                put32(e, (uint32_t)(int32_t)rel);
            } else if (t.flags & X86_RIPREL) {
                int64_t disp;
                int32_t d32; memcpy(&d32, ins + t.disp_off, 4); disp = d32;
                int64_t nd = (int64_t)(here + r->len[i]) + disp - (int64_t)(there + r->len[i]);
                if (nd < -0x7ff00000ll || nd > 0x7ff00000ll) return -1;
                put(e, ins, r->len[i]);
                int32_t n32 = (int32_t)nd; memcpy(e->b + e->n - r->len[i] + t.disp_off, &n32, 4);
            } else put(e, ins, r->len[i]);
            if (e->n > (int)sizeof e->b - 200) return -1;
            continue;
        }
        if (r->kind[i] & K_SYSCALL) emit_syscall(e, stub_va, w->rawsys_slot);
        else if (emit_fs(e, w, ins, &t)) {
            LOGF("rewrite: unsupported %%fs form at %llx: %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned long long)(vaddr + r->off[i]),
                 ins[0], ins[1], ins[2], ins[3], r->len[i] > 4 ? ins[4] : 0, r->len[i] > 5 ? ins[5] : 0,
                 r->len[i] > 6 ? ins[6] : 0, r->len[i] > 7 ? ins[7] : 0);
            return -1;
        }
        if (e->n > (int)sizeof e->b - 200) return -1;
    }
    jmp_abs(e, vaddr + r->off[hi] + r->len[hi]);
    return 0;
}

static int process_region(xrw *w, uint8_t *code, uint64_t vaddr, size_t total, size_t off0, size_t off1, rgn *r)
{
    x86i in;
    r->n = 0; r->base = off0;
    size_t o = off0;
    while (o < off1) {
        size_t avail = total - o;
        int l = x86_decode(code + o, avail < 15 ? avail : 15, &in);
        if (!l) {
            w->st.decode_fail++;
            LOGF("rewrite: cannot decode at %llx (%02x %02x %02x %02x): rest of [%llx,%llx) left untouched",
                 (unsigned long long)(vaddr + o), code[o], o + 1 < total ? code[o + 1] : 0, o + 2 < total ? code[o + 2] : 0,
                 o + 3 < total ? code[o + 3] : 0, (unsigned long long)(vaddr + off0), (unsigned long long)(vaddr + off1));
            break;
        }
        if (o + (size_t)l > off1) break;               /* straddles the next function start: padding/data */
        int kind = K_PLAIN;
        if (is_syscall(&in)) { kind = K_SYSCALL; w->st.syscalls++; }
        else if (is_seg_unit(&in, 0x64)) { kind = K_FS; w->st.fs_sites++; }
        else if (w->gs_slot_off && is_seg_unit(&in, 0x65)) { kind = K_FS; w->st.gs_sites++; }
        else if (in.flags & X86_SYSCALL) w->st.legacy_sys++;
        if (rg_push(r, (uint32_t)o, l, kind)) return -1;
        w->st.insns++;
        o += (size_t)l;
    }
    if (!r->n) return 0;

    size_t need = ((off1 - off0) + 7) / 8;
    if (need > r->tgt_cap) {
        free(r->tgt); r->tgt = (uint8_t *)malloc(need); r->tgt_cap = need;
        if (!r->tgt) { r->tgt_cap = 0; return -1; }
    }
    memset(r->tgt, 0, need);
    for (size_t i = 0; i < r->n; i++) {
        if (r->kind[i] != K_PLAIN) continue;
        if (x86_decode(code + r->off[i], r->len[i], &in) != r->len[i] || !(in.flags & X86_REL)) continue;
        int64_t t = (int64_t)r->off[i] + r->len[i] + in.rel;
        if (t >= (int64_t)off0 && t < (int64_t)off1) { size_t b = (size_t)t - off0; r->tgt[b >> 3] |= (uint8_t)(1u << (b & 7)); }
    }

    for (size_t u = 0; u < r->n; u++) {
        if (!(r->kind[u] & K_UNIT) || (r->kind[u] & K_DONE)) continue;
        uint64_t site = vaddr + r->off[u];
        uint8_t *p = code + r->off[u];

        if ((r->kind[u] & K_FS) && is_tp_load(p, r->len[u])) {      /* in place, no stub */
            if (w->pool) { p[0] = 0x65; uint32_t g = (uint32_t)w->tp_gs_off; memcpy(p + 5, &g, 4); }
            w->st.inplace++;
            r->kind[u] |= K_DONE;
            continue;
        }

        /* window: this instruction plus neighbours until there is room for a 5-byte jmp */
        size_t lo = u, hi = u;
        int len = r->len[u];
        while (len < 5) {
            int canR = hi + 1 < r->n && r->off[hi + 1] == r->off[hi] + r->len[hi] && movable(code, r, hi + 1) && !is_target(r, hi + 1) &&
                       !ends_flow(code, r, hi);
            int canL = lo > 0 && r->off[lo] == r->off[lo - 1] + r->len[lo - 1] && movable(code, r, lo - 1) && !is_target(r, lo) &&
                       !ends_flow(code, r, lo - 1);
            if (canR) { hi++; len += r->len[hi]; }
            else if (canL) { lo--; len += r->len[lo]; }
            else break;
        }
        int windowed = len >= 5;
        if (!windowed) lo = hi = u;

        eb e;
        uint64_t stub_va = pool_next_va(w);
        int64_t rel = (int64_t)stub_va - (int64_t)(vaddr + r->off[lo] + 5);
        if (windowed && !(rel > -0x7ff00000ll && rel < 0x7ff00000ll)) { windowed = 0; lo = hi = u; }
        if (compose(&e, w, code, vaddr, r, lo, hi, stub_va)) {
            if (lo != hi) {                                /* retry alone, as a trap site */
                windowed = 0; lo = hi = u;
                if (compose(&e, w, code, vaddr, r, lo, hi, stub_va)) { w->st.unsupported++; r->kind[u] |= K_DONE; continue; }
            } else { w->st.unsupported++; r->kind[u] |= K_DONE; continue; }
        }
        if (pool_commit(w, &e, &stub_va)) return -1;

        if (windowed) {
            if (w->pool) {
                size_t wlen = (size_t)(r->off[hi] + r->len[hi] - r->off[lo]);
                uint8_t *q = code + r->off[lo];
                int32_t r32 = (int32_t)rel;
                q[0] = 0xe9; memcpy(q + 1, &r32, 4);
                memset(q + 5, 0xcc, wlen - 5);
            }
            w->st.jumps++;
            w->st.moved += (uint32_t)(hi - lo);
        } else {
            if (w->pool) {
                memset(p, 0xcc, r->len[u]);
                if (w->ntraps == w->trap_cap) {
                    size_t c = w->trap_cap ? w->trap_cap * 2 : 256;
                    xrw_trap *t = (xrw_trap *)realloc(w->traps, c * sizeof *t);
                    if (!t) return -1;
                    w->traps = t; w->trap_cap = c;
                }
                w->traps[w->ntraps].site = site; w->traps[w->ntraps].stub = stub_va; w->ntraps++;
            }
            w->st.traps++;
            if (w->verbose) LOGF("rewrite: trap site %llx (%s, no room for a jmp)", (unsigned long long)site, (r->kind[u] & K_SYSCALL) ? "syscall" : "%fs");
        }
        for (size_t i = lo; i <= hi; i++) r->kind[i] |= K_DONE;
    }
    return 0;
}

int xrw_rewrite_range(xrw *w, uint8_t *code, uint64_t vaddr, size_t size, const uint64_t *starts, size_t nstarts)
{
    rgn r; memset(&r, 0, sizeof r);
    if (w->pool_used < XRW_SLOT_BYTES) w->pool_used = XRW_SLOT_BYTES;
    w->rawsys_slot = w->pool_vaddr;
    size_t cur = 0, si = 0;
    int rc = 0;
    while (cur < size) {
        size_t next = size;
        while (si < nstarts && starts[si] <= vaddr + cur) si++;
        if (si < nstarts && starts[si] < vaddr + size) next = (size_t)(starts[si] - vaddr);
        if (process_region(w, code, vaddr, size, cur, next, &r)) { rc = -1; break; }
        cur = next;
    }
    free(r.off); free(r.len); free(r.kind); free(r.tgt);
    return rc;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t *xrw_eh_starts(const uint8_t *hdr, size_t hdr_len, uint64_t hdr_vaddr, size_t *n)
{
    *n = 0;
    if (hdr_len < 12 || hdr[0] != 1) return NULL;
    uint8_t ptr_enc = hdr[1], cnt_enc = hdr[2], tab_enc = hdr[3];
    if (ptr_enc != 0x1b || cnt_enc != 0x03 || tab_enc != 0x3b) return NULL;   /* pcrel sdata4, udata4, datarel sdata4 */
    uint32_t count;
    memcpy(&count, hdr + 8, 4);
    if (count == 0 || 12 + (size_t)count * 8 > hdr_len) return NULL;
    uint64_t *v = (uint64_t *)malloc((size_t)count * sizeof *v);
    if (!v) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        int32_t loc;
        memcpy(&loc, hdr + 12 + (size_t)i * 8, 4);
        v[i] = hdr_vaddr + (uint64_t)(int64_t)loc;
    }
    qsort(v, count, sizeof *v, cmp_u64);
    *n = count;
    return v;
}

/*
 * x86dec.c - x86-64 instruction length decoder. See x86dec.h.
 *
 * Only what the rewriter needs: where an instruction ends, where its ModRM /
 * SIB / displacement / immediate bytes are, whether it touches RIP, branches,
 * or moves %rsp. Covers legacy, REX, VEX (C4/C5), EVEX (62) and XOP (8F) forms.
 * Verified against objdump over libc/libstdc++/ld.so (layer/tests/rewrite_test.c).
 */
#include "x86dec.h"
#include <string.h>

enum {
    F_M     = 1 << 0,    /* ModRM follows the opcode */
    F_I8    = 1 << 1,
    F_I16   = 1 << 2,
    F_IZ    = 1 << 3,    /* imm32, imm16 under 0x66 */
    F_IV    = 1 << 4,    /* imm64 under REX.W (B8..BF) */
    F_REL8  = 1 << 5,
    F_REL32 = 1 << 6,
    F_MOFFS = 1 << 7,
    F_ENTER = 1 << 8,    /* imm16 + imm8 */
    F_G3_8  = 1 << 9,    /* F6: test has an imm8 (reg 0,1) */
    F_G3_Z  = 1 << 10,   /* F7: test has an immz (reg 0,1) */
    F_GFF   = 1 << 11,   /* FF: reg selects inc/dec/call/jmp/push */
    F_INV   = 1 << 12,
    F_CF    = 1 << 13,
    F_STK   = 1 << 14,
    F_SYS   = 1 << 15
};

static unsigned op1_flags(uint8_t b)
{
    if (b < 0x40) {
        switch (b & 7) {
        case 0: case 1: case 2: case 3: return F_M;
        case 4: return F_I8;
        case 5: return F_IZ;
        default: return F_INV;      /* 06 07 0E 16 17 1E 1F 27 2F 37 3F: invalid in 64-bit mode */
        }
    }
    if (b < 0x50) return F_INV;                      /* REX, consumed earlier */
    if (b < 0x60) return F_STK;                      /* push/pop reg */
    switch (b) {
    case 0x60: case 0x61: return F_INV;
    case 0x63: return F_M;
    case 0x68: return F_IZ | F_STK;
    case 0x69: return F_M | F_IZ;
    case 0x6a: return F_I8 | F_STK;
    case 0x6b: return F_M | F_I8;
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: return 0;
    case 0x80: return F_M | F_I8;
    case 0x81: return F_M | F_IZ;
    case 0x82: return F_INV;
    case 0x83: return F_M | F_I8;
    case 0x8f: return F_M | F_STK;
    case 0x9a: return F_INV;
    case 0x9c: case 0x9d: return F_STK;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: return F_MOFFS;
    case 0xa8: return F_I8;
    case 0xa9: return F_IZ;
    case 0xc0: case 0xc1: return F_M | F_I8;
    case 0xc2: return F_I16 | F_CF | F_STK;
    case 0xc3: return F_CF | F_STK;
    case 0xc6: return F_M | F_I8;
    case 0xc7: return F_M | F_IZ;
    case 0xc8: return F_ENTER | F_STK;
    case 0xc9: return F_STK;
    case 0xca: return F_I16 | F_CF;
    case 0xcb: return F_CF;
    case 0xcc: return F_CF;
    case 0xcd: return F_I8 | F_CF;
    case 0xce: return F_INV;
    case 0xcf: return F_CF;
    case 0xd4: case 0xd5: case 0xd6: return F_INV;
    case 0xd7: return 0;
    case 0xe8: return F_REL32 | F_CF | F_STK;
    case 0xe9: return F_REL32 | F_CF;
    case 0xea: return F_INV;
    case 0xeb: return F_REL8 | F_CF;
    case 0xf4: return F_CF;
    case 0xf6: return F_M | F_G3_8;
    case 0xf7: return F_M | F_G3_Z;
    case 0xfe: return F_M;
    case 0xff: return F_M | F_GFF;
    default: break;
    }
    if (b >= 0x70 && b <= 0x7f) return F_REL8 | F_CF;
    if (b >= 0x84 && b <= 0x8e) return F_M;
    if (b >= 0xb0 && b <= 0xb7) return F_I8;
    if (b >= 0xb8 && b <= 0xbf) return F_IV;
    if (b >= 0xd0 && b <= 0xd3) return F_M;
    if (b >= 0xd8 && b <= 0xdf) return F_M;
    if (b >= 0xe0 && b <= 0xe3) return F_REL8 | F_CF;
    if (b >= 0xe4 && b <= 0xe7) return F_I8;
    return 0;                      /* 90..99 9b..9f a4..a7 aa..af ec..ef f5 f8..fd */
}

static unsigned op2_flags(uint8_t b)
{
    switch (b) {
    case 0x04: case 0x0a: case 0x0c: case 0x24: case 0x25: case 0x26: case 0x27: case 0x36:
    case 0x39: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: case 0xa6: case 0xa7:
        return F_INV;
    case 0x05: return F_CF | F_SYS;               /* syscall */
    case 0x06: case 0x08: case 0x09: case 0x0e: case 0x30: case 0x31: case 0x32: case 0x33: case 0x37:
    case 0x77: case 0xa2:
        return 0;
    case 0x07: case 0x0b: case 0x35: case 0xaa: return F_CF;   /* sysret ud2 sysexit rsm */
    case 0x34: return F_CF | F_SYS;               /* sysenter */
    case 0x0f: return F_M | F_I8;                 /* 3DNow! suffix opcode */
    case 0xa0: case 0xa1: case 0xa8: case 0xa9: return F_STK;
    case 0xa4: case 0xac: case 0xba: return F_M | F_I8;
    case 0xc2: case 0xc4: case 0xc5: case 0xc6: return F_M | F_I8;
    case 0x70: case 0x71: case 0x72: case 0x73: return F_M | F_I8;
    default: break;
    }
    if (b >= 0x80 && b <= 0x8f) return F_REL32 | F_CF;
    if (b >= 0xc8 && b <= 0xcf) return 0;         /* bswap */
    return F_M;
}

int x86_decode(const uint8_t *p, size_t avail, x86i *o)
{
    memset(o, 0, sizeof *o);
    size_t i = 0;
    unsigned fl = 0;
    unsigned immsz = 0;
#define NEED(n) do { if (i + (size_t)(n) > avail || i + (size_t)(n) > 15) return 0; } while (0)

    for (;;) {                                 /* legacy prefixes */
        NEED(1);
        uint8_t b = p[i];
        if (b == 0x66) o->opsz16 = 1;
        else if (b == 0x67) o->addr32 = 1;
        else if (b == 0xf0) o->lock = 1;
        else if (b == 0xf2) { o->repne = 1; o->rep = 0; }
        else if (b == 0xf3) { o->rep = 1; o->repne = 0; }
        else if (b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65) o->seg = b;
        else break;
        o->nprefix++; i++;
    }
    NEED(1);
    if ((p[i] & 0xf0) == 0x40) { o->rex = p[i]; i++; NEED(1); }

    uint8_t b = p[i];
    o->op_off = (uint8_t)i;
    if (b == 0xc5 || b == 0xc4 || b == 0x62 || (b == 0x8f && i + 1 < avail && (p[i + 1] & 0x1f) >= 8)) {
        /* VEX / EVEX / XOP */
        unsigned map;
        if (b == 0xc5) {
            NEED(3);
            o->vex = 1; map = 1;
            o->rex = 0x40 | ((p[i + 1] & 0x80) ? 0 : 4);
            i += 2;
        } else if (b == 0xc4) {
            NEED(4);
            o->vex = 1; map = p[i + 1] & 0x1f;
            o->rex = 0x40 | ((p[i + 2] & 0x80) ? 8 : 0) | ((p[i + 1] & 0x80) ? 0 : 4) | ((p[i + 1] & 0x40) ? 0 : 2) | ((p[i + 1] & 0x20) ? 0 : 1);
            i += 3;
        } else if (b == 0x62) {
            NEED(5);
            o->vex = 2; map = p[i + 1] & 7;
            o->rex = 0x40 | ((p[i + 2] & 0x80) ? 8 : 0) | ((p[i + 1] & 0x80) ? 0 : 4) | ((p[i + 1] & 0x40) ? 0 : 2) | ((p[i + 1] & 0x20) ? 0 : 1);
            i += 4;
        } else {
            NEED(5);
            o->vex = 3; map = p[i + 1] & 0x1f;
            o->rex = 0x40 | ((p[i + 2] & 0x80) ? 8 : 0);
            i += 3;
        }
        NEED(1);
        o->map = (uint8_t)map; o->opcode = p[i]; o->op_off = (uint8_t)i; i++;
        fl = F_M;
        if (o->vex == 3) {
            if (map == 8) fl |= F_I8;
            else if (map == 0xa) { fl |= F_IZ; o->opsz16 = 0; }
        } else if (map == 1) {
            uint8_t oc = o->opcode;
            if (oc == 0x77 && o->vex == 1) fl = 0;                       /* vzeroupper / vzeroall */
            else if (oc == 0x70 || oc == 0x71 || oc == 0x72 || oc == 0x73 || oc == 0xc2 || oc == 0xc4 || oc == 0xc5 || oc == 0xc6) fl |= F_I8;
        } else if (map == 3) fl |= F_I8;
        else if (map == 2 || map == 5 || map == 6) {}
        else if (o->vex != 2) return 0;
    } else {
        i++;
        o->map = 0; o->opcode = b;
        if (b == 0x0f) {
            NEED(1);
            uint8_t b2 = p[i++];
            if (b2 == 0x38) { NEED(1); o->map = 2; o->opcode = p[i++]; fl = F_M; }
            else if (b2 == 0x3a) { NEED(1); o->map = 3; o->opcode = p[i++]; fl = F_M | F_I8; }
            else { o->map = 1; o->opcode = b2; fl = op2_flags(b2); }
            o->op_off = (uint8_t)(i - 1);
        } else fl = op1_flags(b);
    }
    if (fl & F_INV) return 0;

    if (fl & F_M) {
        NEED(1);
        uint8_t m = p[i];
        o->has_modrm = 1; o->modrm_off = (uint8_t)i; o->modrm = m; i++;
        unsigned mod = m >> 6, rm = m & 7, dsz = 0;
        if (mod != 3) {
            if (rm == 4) {
                NEED(1);
                o->sib_off = (uint8_t)i; o->sib = p[i]; i++;
                if (mod == 0 && (o->sib & 7) == 5) dsz = 4;
            }
            if (mod == 0 && rm == 5) { dsz = 4; o->flags |= X86_RIPREL; }
            else if (mod == 1) dsz = 1;
            else if (mod == 2) dsz = 4;
        }
        if (dsz) { NEED(dsz); o->disp_off = (uint8_t)i; o->disp_size = (uint8_t)dsz; i += dsz; }
        if (fl & F_G3_8) { if (((m >> 3) & 7) < 2) fl |= F_I8; }
        if (fl & F_G3_Z) { if (((m >> 3) & 7) < 2) fl |= F_IZ; }
        if (fl & F_GFF) {
            unsigned reg = (m >> 3) & 7;
            if (reg == 7) return 0;
            if (reg == 2 || reg == 3) fl |= F_CF | F_STK;
            else if (reg == 4 || reg == 5) fl |= F_CF;
            else if (reg == 6) fl |= F_STK;
        }
        if (o->map == 0 && o->opcode == 0x8f && ((m >> 3) & 7) != 0) return 0;     /* only /0 (pop) */
    }

    if (fl & F_I8) immsz = 1;
    else if (fl & F_I16) immsz = 2;
    else if (fl & F_IZ) immsz = (o->opsz16 && !(o->rex & 8)) ? 2 : 4;
    else if (fl & F_IV) immsz = (o->rex & 8) ? 8 : o->opsz16 ? 2 : 4;
    else if (fl & F_REL8) immsz = 1;
    else if (fl & F_REL32) immsz = 4;
    else if (fl & F_MOFFS) immsz = o->addr32 ? 4 : 8;
    else if (fl & F_ENTER) immsz = 3;
    if (fl & F_IZ && fl & F_I8) immsz = 1;     /* (never both; F6/F7 set one) */
    if (immsz) {
        NEED(immsz);
        o->imm_off = (uint8_t)i; o->imm_size = (uint8_t)immsz;
        if (fl & (F_REL8 | F_REL32)) {
            o->flags |= X86_REL;
            if (immsz == 1) o->rel = (int8_t)p[i];
            else { int32_t r; memcpy(&r, p + i, 4); o->rel = r; }
        }
        i += immsz;
    }
    /* xbegin (C7 F8 rel32) is a relative branch the table sees as mov imm */
    if (o->map == 0 && o->opcode == 0xc7 && o->has_modrm && o->modrm == 0xf8) {
        o->flags |= X86_REL | X86_CF; memcpy(&o->rel, p + o->imm_off, 4);
    }
    if (fl & F_CF) o->flags |= X86_CF;
    if (fl & F_STK) o->flags |= X86_STACK;
    if (fl & F_SYS) o->flags |= X86_SYSCALL;
    if (o->map == 0 && o->opcode == 0xcd && p[o->imm_off] == 0x80) o->flags |= X86_SYSCALL;
    o->len = (uint8_t)i;
    return (int)i;
#undef NEED
}

/*
 * x86dec.h - x86-64 instruction length decoder (no semantics, no tables of
 * mnemonics). Pure C99, no OS dependencies: the layer's load-time rewriter
 * (rewrite.c) uses it to find instruction boundaries, and layer/tests/
 * rewrite_test.c checks it against objdump over real libraries.
 */
#ifndef XRL_X86DEC_H
#define XRL_X86DEC_H
#include <stddef.h>
#include <stdint.h>

#define X86_RIPREL  0x01   /* memory operand is RIP-relative                         */
#define X86_REL     0x02   /* relative branch/call/xbegin: immediate is a displacement */
#define X86_CF      0x04   /* transfers control (jmp/jcc/call/ret/int/syscall/hlt/ud2...) */
#define X86_SYSCALL 0x08   /* syscall, sysenter, int $0x80                           */
#define X86_STACK   0x10   /* push/pop/call/ret/enter/leave class (changes %rsp)     */

typedef struct x86i {
    uint8_t len;
    uint8_t flags;          /* X86_* */
    uint8_t nprefix;        /* legacy prefix bytes (66 67 f0 f2 f3 2e 36 3e 26 64 65) */
    uint8_t rex;            /* REX byte value, 0 if absent (VEX/EVEX folded in: W R X B bits as REX) */
    uint8_t vex;            /* 0 none, 1 VEX, 2 EVEX, 3 XOP */
    uint8_t map;            /* 0 one-byte, 1 0F, 2 0F38, 3 0F3A, other: vex/evex map */
    uint8_t opcode;
    uint8_t op_off;         /* offset of the opcode byte */
    uint8_t has_modrm;
    uint8_t modrm_off, modrm, sib_off, sib;   /* sib_off == 0: no SIB */
    uint8_t disp_off, disp_size;
    uint8_t imm_off, imm_size;
    uint8_t seg;            /* 0, or the last segment override: 0x64 (fs) / 0x65 (gs) / 0x2e ... */
    uint8_t opsz16, addr32, rep, repne, lock;
    int32_t rel;            /* relative displacement when X86_REL */
} x86i;

/* Returns the instruction length (1..15) or 0 if the bytes are not a valid
 * x86-64 instruction (or run past `avail`). */
int x86_decode(const uint8_t *p, size_t avail, x86i *out);

#endif

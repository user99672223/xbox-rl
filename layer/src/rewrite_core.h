/*
 * rewrite_core.h - the pure-C half of the layer's load-time ELF rewriter.
 *
 * Input: the bytes of one executable ELF segment (already mapped, writable for
 * the duration of the call) plus the function start addresses that segment
 * has (from .eh_frame_hdr). Output: the segment patched in place, a
 * trampoline pool, and the list of trap sites. No OS calls: layer/tests/
 * rewrite_test.c runs it over real libraries on any host; rewrite.c wires it
 * to the layer.
 *
 * What gets rewritten (docs/decisions.md, "Easy Anti-Cheat" entry):
 *   - `syscall` (0F 05): becomes a jump into a stub that calls the layer's
 *     `xrl_rawsys` (Linux syscall register ABI in, result in rax) with the red
 *     zone skipped;
 *   - every `%fs:` memory operand: the same operand relative to the thread
 *     pointer kept in the host TEB slot (%gs:tp_gs_off), because the console
 *     has no FSGSBASE; `mov %fs:0,%r64` is patched in place (tools/xrl-tpfix.py form);
 *   - with gs_slot_off set, every `%gs:` memory operand the same way, relative to
 *     the guest's GS base kept in its own TEB slot (Linux code that sets a GS base
 *     with arch_prctl, such as Firefox's wasm2c sandboxes: %gs is the host TEB here).
 * A patch needs 5 bytes for `jmp rel32`; neighbouring instructions are moved
 * into the stub (the "window") when the instruction is shorter. If no window
 * exists the instruction becomes `int3` and signal-free handler code in
 * rewrite.c redirects the thread to the stub (the trap list).
 */
#ifndef XRL_REWRITE_CORE_H
#define XRL_REWRITE_CORE_H
#include <stddef.h>
#include <stdint.h>

typedef struct xrw_trap { uint64_t site, stub; } xrw_trap;

typedef struct xrw_stats {
    uint32_t insns;           /* instructions decoded                               */
    uint32_t syscalls;        /* `syscall` sites found                              */
    uint32_t fs_sites;        /* %fs memory operands found                          */
    uint32_t inplace;         /* patched without a stub (mov %fs:0,%r64)            */
    uint32_t jumps;           /* patched with jmp rel32 into a stub                 */
    uint32_t moved;           /* neighbouring instructions moved into stubs         */
    uint32_t traps;           /* int3 sites                                         */
    uint32_t unsupported;     /* units we could not rewrite (logged)                */
    uint32_t decode_fail;     /* regions abandoned at an undecodable byte           */
    uint32_t legacy_sys;      /* sysenter / int 0x80 seen (not rewritten)           */
    uint32_t gs_sites;        /* %gs memory operands found (gs_slot_off set)        */
} xrw_stats;

typedef struct xrw {
    uint64_t tp_gs_off;       /* %gs offset of the thread-pointer slot              */
    uint8_t *pool;            /* trampoline pool bytes (NULL: dry run, sizes only)  */
    uint64_t pool_vaddr;      /* guest address of pool[0]                           */
    size_t   pool_cap, pool_used;   /* pool[0..8) is the rawsys pointer slot        */
    uint64_t rawsys_slot;     /* == pool_vaddr (filled by the caller)               */
    xrw_trap *traps; size_t ntraps, trap_cap;
    xrw_stats st;
    int verbose;              /* log every trap site                                */
    void (*log)(const char *fmt, ...);
    uint64_t gs_slot_off;     /* %gs offset of the guest's GS base slot; 0: %gs left alone */
} xrw;

#define XRW_SLOT_BYTES 16

/* Rewrite one executable range. `code` points at the mapped bytes (writable),
 * `vaddr` is their guest address. `starts` (sorted, may be NULL) are function
 * start addresses inside the range: decoding resynchronises at each. Returns
 * 0, or -1 if the pool is too small (pool_used then says how much is needed
 * so far; call again with a bigger pool). */
int xrw_rewrite_range(xrw *w, uint8_t *code, uint64_t vaddr, size_t size,
                      const uint64_t *starts, size_t nstarts);

/* Function start addresses from an .eh_frame_hdr image (`hdr` bytes, mapped at
 * `hdr_vaddr`): the binary-search table of (initial_loc, fde) pairs. Only the
 * encodings every toolchain emits (datarel|sdata4 table) are understood.
 * Returns a malloc'd array (caller frees) sorted ascending, count in *n; NULL
 * (and *n = 0) if the table is absent or in another encoding. */
uint64_t *xrw_eh_starts(const uint8_t *hdr, size_t hdr_len, uint64_t hdr_vaddr, size_t *n);

/* For the translator (dbt.c): the replacement sequences of a `syscall` and of an instruction with a %fs
 * memory operand, written to `out` as if it were at out_va. Return the length, xrw_emit_fs -1 if the
 * operand form cannot be rebuilt. Needs x86dec.h's x86i. */
struct x86i;
int xrw_emit_syscall(uint8_t *out, uint64_t out_va, uint64_t slot);
int xrw_emit_fs(uint64_t tp_gs_off, uint8_t *out, const uint8_t *ins, const struct x86i *in);

#endif

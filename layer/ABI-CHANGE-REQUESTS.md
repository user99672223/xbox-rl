# ABI change requests (from Workstream C)

`include/xrl_abi.h` is frozen and owned by the integrator. Nothing below was
applied; each item says what the layer does today so nothing blocks.

1. **`long` is 32-bit under MSVC (LLP64).** `xrl_syscall` is declared with
   `long` parameters/return. On the Linux/musl side that is 64-bit; on the
   Windows side it is 32-bit. The layer never calls `xrl_syscall` from C (it is
   an assembly thunk), so there is no bug today, but anything on the Windows
   side that includes the header and touches the prototype is misleading. Ask:
   change to `int64_t xrl_syscall(int64_t nr, int64_t a1, ... int64_t a6)` and
   add a comment "SysV ABI; assembly thunk; do not call from Windows code".
   (The layer was bitten by exactly this internally: every `long` is now
   `slong` = `int64_t`.)

2. **Name the ELF to load.** The layer loads `argv[0]` as a *guest* path
   resolved through the VFS. Ask: add `const char *exec_path` (guest path) to
   `xrl_boot_context` so argv[0] can be the conventional program name.

3. **Entropy seed.** The console has no RDRAND; decisions.md says "seed from the
   host". The ABI has no field for it. Today the layer seeds from
   `BCryptGenRandom(system RNG)` (expected to work in the app container, NOT yet
   verified on the console) and mixes in `XRL_RNG_SEED=<64 hex chars>` if the host
   puts it in `envp`. Ask: add `const uint8_t *rng_seed; uint32_t rng_seed_len;`
   (host fills from `Windows.Security.Cryptography.CryptographicBuffer`).

4. **Promote the guest-side contract into the header.** The layer hands the
   guest (a) the syscall entry and (b) SysV-callable copies of the host op
   tables. Today that is defined only in `layer/src/xrl_internal.h`
   (`AT_XRL_SYSCALL 0x5801`, `AT_XRL_CTX 0x5802`, `AT_XRL_ABI 0x5803`,
   `struct xrl_guest_ctx`, magic `XRL_GUEST_CTX_MAGIC`). B needs these to write
   its side. Ask: move them into `xrl_abi.h` (or a sibling `xrl_guest.h`).

5. **Calling convention of the op tables, stated precisely.** The header says
   "System V ... the layer owns the thunk". Implemented as: the host fills the
   tables with ordinary Win64 function pointers; the layer gives the guest a
   *copy* of each table whose function pointers are generated SysV stubs
   (integer/pointer args only, up to 8). Ask: add to the header that op-table
   functions must take only integer/pointer-class arguments (no float/double, no
   structs by value) and at most 8 parameters, and that op callbacks run on the
   calling guest thread's host stack (so a D3D call may block that thread).

6. **TEB / GS base (needs a decision, may become an ABI item).** See
   notes-C.md "GS / TEB". Wine's PE side addresses its TEB through `%gs`; on a
   Windows host `%gs` is the host TEB and cannot be re-based. The layer
   exposes `arch_prctl(0x5801, &teb)` (returns the host TEB) and refuses
   `ARCH_SET_GS` unless the value is that TEB. B/integrator must choose the
   adoption design; if it needs layer help (TEB-field swap on transitions), that
   is a new layer capability, not a header change.

7. **Thread-safety / level statement for `xrl_log_fn`.** The layer serialises its
   own calls to `log` with a lock, and the guest's SysV `log` stub is NOT
   serialised against it. Ask: state in the header that `log` must be
   thread-safe.

8. **`xrl_layer_boot` is single-shot per process** (global state). Ask: say so in
   the header comment (host must not call it twice).

# Integration queue (integrator-owned)

Running log of cross-workstream items to reconcile when agents report. ABI edits
are batched into ONE coordinated `xrl_abi.h` v2 bump after all four report, so
the in-flight agents don't desync from a header changing under them. Not
committed until the integration pass.

## Status
- A host — **DONE** (2026-10-26, resumed run). Probe + full host build/package/sign clean (0/0). Probe logic verified on laptop as desktop exe; NOT run as UWP (laptop Dev Mode off → 0x80073CFF). Cleaned own build artifacts.
- B wine — **DONE** (2026-10-26, resumed). Valve Wine 11.0 (GE-Proton11-11 pin) builds on patched musl with **0 `syscall`/`sysenter`/`int 0x80` in all 802 shipped binaries** (glibc control fails the check). Patches in patches/musl + patches/wine (0001–0009): musl __syscall→xrl_syscall, ntdll/win32u PE stubs via __wine_syscall_dispatcher, wineserver-as-thread, single-process CreateProcess stubs, new dlls/xrlhost PE→host seam, xinput + d3d11/d3d12/dxgi forwarders. Verified under WSL against a pass-through shim: wineboot, wine.inf, `cmd /c ver`, reg query, notepad window. Baked prefix: 892 files/280 MB. Build: `wine/scripts/build-wine.sh` (needs ~4 GB free on C:).
- C layer — **DONE** (2026-10-26, resumed). Desktop + UWP flavors build clean (link `layer/build/uwp/xrl_layer.lib` + WindowsApp/bcrypt/synchronization). First-wave syscalls real (futex ~100k rt/s, clone-threads, mmap family, file/VFS, pipe/eventfd/poll/epoll, socketpair+SCM_RIGHTS, clocks, arch_prctl, getrandom, uname). Signals stored but NO handler runs yet (SIGSEGV delivery is next — first real Wine run will hit it). fork/vfork/execve = ENOSYS by design. futex_waitv→ENOSYS on purpose (makes Wine disable fsync).

## UNANIMOUS across A, B, D (+ C built the mechanism): handles = directly-callable COM
Every graphics-touching workstream independently demanded it: a gfx/audio handle
must BE the native COM interface pointer, callable from Wine via MS-x64 vtable
calls. B-Q2 is blunt: this decides whether the D3D forwarders are ~300 lines or
~10,000. ABI v2 MUST state: (a) every op-table fn pointer + xrl_log_fn is MS-x64;
(b) handles are the raw COM pointer; (c) the layer's generated SysV→MS-x64 stubs
(C already built these) are how Wine calls them. RESOLUTION: adopt C's thunk
design, state it in the header. Closes A-1, D-R1, B-2.

## UNANIMOUS BLOCKER: GS/TEB swap (B-Q1 + C GS/TEB)
Wine PE code needs %gs = Wine TEB; host code needs the real %gs. Every transition
PE↔host (and any host code on a guest stack) must swap it. B owns the Wine-side
transition points; C owns the layer thunk. Neither built it yet. This is the #1
co-design task and the most likely thing to block the first real Wine-on-layer run.

## Op-table access — RECONCILE B vs C (integrator decision)
B proposes NEW pseudo-syscalls 0x10001–0x10004 to fetch gfx/audio/input/log.
C already exposes the boot context (which HOLDS those pointers) to the guest via
auxv `AT_XRL_CTX`. PREFER C's path: B's dlls/xrlhost reads the op-tables from the
AT_XRL_CTX boot-context struct — no new syscall surface. Needs the boot-context +
auxv layout moved into xrl_abi.h (C-4) so B can include it. Fold B-1 into this.

## ABI v2 edit list (do in ONE pass now that all four are in)
- `xrl_syscall` params `long` → `int64_t` (C-1; MSVC long is 32-bit — real bug).
- State handles=COM-pointer + MS-x64 calling convention + layer thunks (above).
- Move boot-context/auxv (`AT_XRL_SYSCALL`, `AT_XRL_CTX`) defs into the header (C-4, B-1).
- Add `get_adapter_info`, swapchain `resize`/`current_index`, `last_hresult` to gfx (D-R2/R3, A-5, B-3).
- Add audio `voice_get_state` (B-4, A-2) + the missing voice controls (A-2).
- Add entropy-seed + `exec_path` fields to boot context (A-4, C-3, C-2).
- Add lifecycle (suspend/resume) + guest→host fatal-error channel (A-4).
- State: op-table fns take ≤8 integer/pointer args (C-5); `log` thread-safe (C-6);
  `xrl_layer_boot` single-shot (C-7). Fix the "address-space hole" doc ref (A-6)
  using the probe's measured ~4.5 GiB commit ceiling.
- Keyboard/mouse input op (A-3) — defer (controller-first), but reserve the slot.

## Deferred build gaps (post-ABI-v2, for the first real bring-up)
- B: in-memory ring-buffer channel still over pipes/socketpair; D3D11 swapchain
  wrapper in dxgi; audio driver winexrl.drv; measure fsync-off round-trip cost (B-Q5).
- C: signal delivery (SIGSEGV first — Wine will hit it); lazy-commit for the
  ~4.5 GiB ceiling; verify FSGSBASE/BCryptGenRandom/hi-res timers on console.

## Calling-convention TOP item — largely RESOLVED by C
C owns and built the thunk: `xrl_syscall` is a SysV asm thunk that switches to the thread's Windows stack; host op-table fns get generated SysV stubs. Guest finds it via auxv `AT_XRL_SYSCALL`/`AT_XRL_CTX`. So "who provides the thunk" (A-1/D-R1) = the layer. ABI v2 just needs to STATE this + the constraints C lists. BUT a new, bigger risk replaces it (GS/TEB, below).

## NEW top risk from C: GS/TEB (needs a B Wine patch)
Wine's PE code reads its TEB via `%gs`, but on Windows `%gs` is the host TEB and the kernel restores it every context switch, so a naive rebase silently fails. C's workable design: Wine adopts the host TEB; the layer swaps host-owned TEB fields on each transition. Needs a Wine patch from B + a small layer thunk addition — neither exists yet. This is the #1 thing B and C must co-design.

## From C (layer) — layer/ABI-CHANGE-REQUESTS.md
1. **`xrl_syscall` params `long` → `int64_t`** — MSVC `long` is 32-bit; real ABI bug in the header. FIX in v2.
2. Add explicit `exec_path` (today argv[0] is the guest ELF path).
3. Add entropy-seed field (today BCryptGenRandom + optional XRL_RNG_SEED env).
4. Move auxv/guest-context defs into the header so B can use them.
5. State op-table fns take only integer/pointer args, max 8.
6. State `log` must be thread-safe.
7. State `xrl_layer_boot` is single-shot per process.
8. GS/TEB decision (see above) — may become an ABI item.

C open qs to carry: is FSGSBASE usable on console (read `threads: FSGSBASE` in first boot log)? Windows commits memory eagerly (may exhaust 5 GB sooner — lazy-commit planned, not built). BCryptGenRandom in AppContainer? hi-res timers? `\\?\`+CreateFile2 under LocalState? Prefix baker must write `<vfs_root>\.xrl-symlinks` for dosdevices.
- D gfx — **DONE** (2026-10-26), builds on dev PC (desktop + UWP-family link check), not run on console.

## Cross-cutting TOP item (raised independently by D-R1 and A-1)
**Calling-convention / COM-call thunk.** Handles are raw COM pointers, but the
ABI never says how SysV Wine code calls MS-x64 vtable methods on them, nor who
provides the thunk. D wants it a stated guarantee + thunks; A wants every op-table
fn pointer and xrl_log_fn declared MS-x64 with the thunk owner named. Resolve in
ABI v2 before B's forwarding DLLs can really call through. HIGHEST priority.

## From A (host) — host/ABI-CHANGE-REQUESTS.md
1. **Calling convention + COM calls** (see TOP item above).
2. **Audio:** `flags` undefined; bits=32 ambiguous (A treats as float); missing
   voice state, buffer-end notify, volume, freq ratio, flush, loop.
3. **Input:** no keyboard/mouse op, no XInput packet number, no trigger motors.
4. **Lifecycle/entropy:** no suspend/resume signal; no entropy seed in boot ctx
   (needed for /dev/urandom); no guest→host fatal-error channel.
5. **Window/swapchain:** no window size/resize/HDR query; no swapchain resize or
   back-buffer index; xrl_status drops the HRESULT. (overlaps D-R2.)
6. **Docs:** xrl_abi.h references an "address-space hole" section not yet in
   decisions.md — the probe's VA map should supply the data. guest_mem_budget
   default 4096 MiB is a placeholder.

A open questions worth carrying: will the launcher accept the Full package (SDL
runs the view, XboxRL.App exists only in winmd)? which bucket does install land
in? real Device Portal endpoint behavior + 20+GB upload. VCLibs present on console?

## ABI-change requests

### From D (gfx) — gfx/ABI-CHANGE-REQUESTS.md
- **R1 (BLOCKER, affects B): vtable thunks + handle==COM-pointer guarantee.**
  The current `xrl_gfx_ops` hands back opaque handles but gives the Wine side no
  way to call methods *on* the device/queue/resource. D's position: make
  "handle == the native COM interface pointer" a *stated ABI guarantee*, and add
  thunks so Wine's d3d12/dxgi can invoke methods. This is the real Wine-side
  graphics blocker — reconcile with B's forwarding-DLL design directly. TOP item.
- **R2:** append `swapchain_get_current_index`, `swapchain_resize`,
  `last_hresult` to `xrl_gfx_ops` (already implemented as extra entries).
- **R3:** add `get_adapter_info` op so Wine's dxgi can answer adapter queries.
- **R4:** doc clarifications on existing ops.

### Convergence to note (good)
- A had already written `host/src/gfx_seam.h` expecting a strong
  `xrl_gfx_ops_get(const xrl_gfx_host_services*)` export, creates on the host's
  gfx thread, raw COM pointers as handles. D reworked gfx/ to match A's seam
  rather than its own table-handle idea. => A and D already converged on the
  seam; the ABI header text should be updated to bless it (ties into R1).

## Decisions to make at integration (integrator's call)
- **DXGI scaling:** A's host default = `DXGI_SCALING_NONE`; D's swapchain uses
  `DXGI_SCALING_STRETCH`. Pick one. Lean: match the console's fixed output —
  decide when wiring A+D.
- **CoreWindow agility:** A claims the CoreWindow pointer is usable from the gfx
  thread; D flags this as unverified, with `XRL_GFX_FLAG_SWAPCHAIN_INLINE` as the
  escape hatch if wrong. Verify on first UWP run.

## Opportunities surfaced
- **D's shortcut:** a UWP probe run on *this* Win11 PC could validate the
  `CreateSwapChainForCoreWindow` path before the console. Cheap de-risk; worth
  folding into A's probe build. Flag to the user as an early-validation option.

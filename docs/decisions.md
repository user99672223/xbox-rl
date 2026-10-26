# Decisions & seam log

Every non-obvious decision and every seam bug goes here, newest first within a
section. Keep entries short: what, why, and what it costs.

## Fixed architecture (from the project brief)

- Host: C++ UWP app on SDL2's WinRT backend (window, gamepad, keyboard, audio).
  Manifest caps: `codeGeneration`, `expandedResources`. Logs to
  `LocalState/log.txt`.
- Wine: Proton-GE wine at a pinned tag. Unix side built against musl; musl's
  `__syscall` replaced by a direct call into our syscall layer (`xrl_syscall`),
  Wine's few raw syscall sites patched the same way. Resulting binary contains
  **no `syscall` instructions** and runs native.
- Single process: wineserver runs as a thread with an in-memory request/reply
  channel; `CreateProcess` is an in-process exec; wineboot/services/explorer
  stubbed; prefix pre-baked and shipped.
- Graphics: Wine's `d3d12.dll` + `dxgi` forward to native Xbox D3D12 via
  `CreateSwapChainForCoreWindow`; `d3d11.dll` forwards to D3D11On12. DXVK ->
  Dozen only if D3D11On12 fails on the console.
- Audio/input: Wine's `xaudio2` + `xinput` forward to native XAudio2 and
  Windows.Gaming.Input. Controller first; KB/M later.
- Loader/syscalls: Blink's ELF loader maps Wine's unix side + musl. Syscall
  backend from Cosmopolitan Libc's Windows sources. VFS over LocalState with
  `/proc` and `/dev` stubs. Syscall table designed to later grow into a general
  Linux-program layer — but we do not build that generality now.
- Game files copied from the laptop install via `tools/push-game.ps1`.

## Measured facts about the target (Xbox Series S, Dev Mode)

Source: the earlier capability probe (`Documents/win10xbox`, report dated
2026-10-21, 243 probes x 3 environments, run on the actual console). These are
measurements, not assumptions. Build to them.

- **CPU/mem**: Zen 2, 8 logical / 4 physical cores, AVX2+BMI2+SHA, **no AVX-512,
  no RDRAND**. 5120 MiB total physical, ~5107 MiB available to the app. User VA
  range 64 KiB .. 128 TiB.
- **Hypervisor**: CPUID leaf 1 ECX bit 31 reads 0 (suppressed), but leaf
  0x40000000 returns `Microsoft Xb`. A guest that sniffs the hypervisor bit sees
  "none"; nothing to patch for RL.
- **Dynamic code (cat A)**: `VirtualAlloc(PAGE_EXECUTE_READWRITE)` works; RWX
  exec works; RW->RX flip works; `VirtualAlloc2` placeholders work; JIT from a
  worker thread works. `codeGeneration` capability confirmed. This is the whole
  foundation of the ELF loader + JIT — it is GREEN on the console.
- **Exceptions (cat D)**: VEH install works; access-violation delivered to VEH
  with readable RIP; **privileged-instruction `#GP` delivered to user-mode VEH,
  RIP mutable, execution resumable**; `Get/SetThreadContext` on a child thread
  works. Trap-and-emulate for a user-mode fake kernel is viable.
- **Syscall stubs (cat B/C)**: ntdll stubs have the standard x64 shape
  (`mov r10,rcx; mov eax,<nr>; test SharedUserData; syscall`). `SystemCallMode`
  = 0 (use `syscall`, no int 2eh). Numbers match the standard build
  (NtClose=0x0F, NtCreateFile=0x55, NtOpenFile=0x33). Relevant only if we ever
  hook ntdll directly; our design avoids native NT syscalls entirely.
- **Filesystem (cat F)**: **only TEMP and package LocalState are writable.**
  `C:\`, ProgramData, user profile, Windows, System32 are all read-blocked or
  write-blocked. System32 *directory listing* is denied, but individual files
  open by name. => VFS must live entirely under LocalState. The pre-baked
  prefix and the game copy both land under LocalState.
- **Registry (cat G)**: HKLM/HKU reads OK; **all writes DENIED** (virtualized
  AppContainer hive). => the layer virtualizes the registry for the guest; do
  not expect real HKCU writes to persist.
- **Graphics (cat I)**: DXGI factory OK (adapter `SraKmd_arden`, vendor 0x1414,
  dev 0xD000, 128 MiB dedicated / 1280 MiB shared). `D3D11CreateDevice` OK (FL
  11_0). `D3D12CreateDevice` OK (max FL 11_0, binding tier 3, heap tier 2).
  **`D3D11On12CreateDevice` exported and present** -> primary d3d11 path is
  viable. `d3d9on12` present but `d3d9.dll` absent; `opengl32`, `vulkan-1`,
  `gdiplus` all ABSENT. => DXVK->Dozen fallback would need us to ship Vulkan;
  there is no system Vulkan. Keep to D3D11On12 unless it proves broken.
  **FL caps at 11_0** for the AppContainer view even though the GPU is higher —
  verify RL is content with FL 11_0.
- **Audio (cat J)**: WASAPI + `xaudio2_9.dll` fully available; mastering voice
  48 kHz stereo 32-bit OK.
- **Input/WinRT (cat N)**: `Windows.Gaming.Input.Gamepad`, `CoreWindow`,
  `ApplicationView`, `DisplayInformation` activation factories all OK.
- **Processes (cat L)**: suspended children of `cmd.exe`/`dllhost.exe`/self can
  be created on the Game bucket (more permissive than laptop AppContainer), but
  our single-process design does not rely on this.
- **Networking (cat K)**: full sockets incl. listen/loopback/raw. Not needed for
  offline RL; leave it.
- **Firmware (cat M)**: SMBIOS/ACPI/enum all DENIED. Synthesize anything that
  depends on firmware tables.

### Launch/activation facts (from the probe host work)

- The Xbox launcher refuses an exe without matching WinRT metadata **before any
  process is created, with no dump**. The UWP app needs a real activation class:
  a `.winmd` declaring the `App` class + an activation factory in the exe. This
  requires the **VS UWP C++ workload** and a real UWP `.vcxproj` (hand-driven
  midlrt in the Native Desktop workload failed with MIDL5027). Host build must
  be a proper C++/WinRT UWP project.
- `D3D11CreateDevice`/`D3D12` on the **UI thread** of a Game-bucket AppContainer
  app **blocks indefinitely**. The probe host had to go headless and create the
  device off the UI thread. => The host must marshal device/swapchain creation
  off the CoreWindow UI thread. This is why `xrl_gfx_ops` lives host-side and
  the Wine builtin only holds handles.
- Device Portal `taskmanager`/app-launch REST returns **403**; the app can be
  deployed/registered via Device Portal but must be **started by pressing its
  tile in Dev Home** on the console. Deploy scripts register; a human presses
  Start. Ask once, continue on "done".

## Measured facts — THIS app's first console run (2026-10-26)

Probe `xbox-rl.host` v1.0.63.0, installed in the **Game bucket**, run on the
Series S. `results/log-latest.txt` is the full log. `PROBE_RESULT … required=PASS`
— every required capability works from our own app/manifest, confirming the
2026-10-21 win10xbox probe still holds here:
- **RWX/JIT:** execute-in-place, RW→RX flip, RX→RW patch→RX, and execute from a
  worker thread all OK. The loader+JIT foundation is green on this app.
- **4 GiB alloc:** committed and touched all 1,048,576 pages in 588 ms, readback OK.
- **NEW — commit ceiling ≈ 4.5 GiB.** VirtualAlloc *commit*: 4.5 GiB OK, **5.0/6/8
  GiB DENIED (ERROR_COMMITMENT_LIMIT 1455)**. *Reserve-only* (no commit) OK to
  1024 GiB. => mmap can reserve huge VA freely, but total **committed** guest
  memory must stay under ~4.5 GiB. Sets `guest_mem_budget` and makes C's
  lazy-commit (reserve-then-commit-on-touch) a real requirement, not optional.
  Nominal AppMemoryUsageLimit still reports 5120 MiB; the hard wall is lower.
- **D3D12:** device OK, adapter SraKmd_arden (128 MiB dedicated / 1280 shared),
  **FL 11_0** (as expected for the AppContainer view), binding tier 3, heap tier
  2, shader model 6.4, **UMA + cache-coherent-UMA**, wave lane count 64, GPU VA 40
  bits, raytracing tier 0.
- **D3D11On12:** device+context OK, CreateTexture2D/RTV/Clear/Flush all OK. The
  primary d3d11 path is confirmed on the console — no DXVK→Dozen needed.
- **NEW — CoreWindow swapchain works:** `CreateSwapChainForCoreWindow` (flip,
  2 buffers, fmt 28/BGRA8, 1920x1080), created off the UI thread on the gfx MTA
  thread, **presented 120/120 frames at 59.2 fps (~60 vsync)**. This is the path
  D built that had never run on hardware. It works at full framerate.
- **Audio:** XAudio2 engine + mastering voice (2ch/48kHz) OK, beep played.
- **Input:** Windows.Gaming.Input ready; a controller connected mid-run →
  "gamepad connected -> slot 0".
- Device family Windows.Xbox, OS 10.0.26100, CPU AuthenticAMD Zen2, avx2=1
  bmi2=1 avx512=0 rdrand=0, hypervisor-bit=0 (leaf 0x40000000 = 'Microsoft Xb').
- Install paths: pkg at `S:\Program Files\WindowsApps\…`, LocalState at
  `Q:\Users\UserMgr1\AppData\Local\Packages\xbox-rl.host_…\LocalState` (the VFS
  root; `Q:` is writable, confirmed by the log file being written there).

### Deploy tooling: real-Device-Portal fixes (2026-10-26)
The mock Device Portal hid three issues fixed in `tools/_common.ps1` + `deploy.ps1`:
1. **CSRF:** WDP needs a primed cookie session + `X-CSRF-Token` header on POST/
   DELETE (`Initialize-DpSession`/`Get-DpCsrf`). Without it, cert POST → 403.
2. **Multipart:** PowerShell `-Form` isn't shaped the way `/packagemanager/package`
   parses (→ 400 "Missing .msix"). Hand-built multipart in `Invoke-DpMultipart`.
3. **Name match:** WDP `.Name` is the *display* name ("xbox-rl"); match the
   identity via `PackageFullName -like "xbox-rl.host_*"`.
(Note `Set-StrictMode` requires `$script:DpSession` be declared before first read.)

## Project decisions (newest first)

- **2026-10-26 — Real network sockets (layer/src/net.c): AF_INET/AF_INET6
  TCP and UDP over Winsock.** The layer had only pipes and socketpairs; EAC's
  module talks to Epic's servers and any Linux program needs sockets. Shape:
  every Winsock socket is non-blocking and registered with `WSAEventSelect` on
  one shared event; a helper thread waits on it and bumps the layer's I/O epoch
  (ipc.c), the word poll/epoll and the pipe waits already sleep on, so
  `poll`/`epoll_wait` needed no change (a socket's `poll` op is one `WSAPoll`
  with a zero timeout). Blocking is the layer's own loop (try, sleep on the
  epoch, retry; interruptible; `SO_RCVTIMEO`/`SO_SNDTIMEO`), so nothing ever
  sits in the Windows kernel and exit/tgkill always win. Guest buffers go
  through a 64 KiB bounce buffer, never to Winsock directly (lazy-committed
  guest memory). Chosen over overlapped I/O + completion ports (IOCP would
  need a completion thread per outstanding op and still needs the epoch for
  poll) and over `select`-in-a-thread (64 fds, same epoch needed). Linux
  behaviour kept where Windows differs: connect to 0.0.0.0/:: reaches
  loopback, AF_INET6 sockets are dual-stack, getsockname on an unbound socket
  answers with the family and zeros, `SO_REUSEADDR` on TCP does not get
  Windows' port-stealing meaning (UDP does: several sockets on one port),
  EPIPE instead of SIGPIPE, `connect()` to a refusing loopback port fails in
  milliseconds (SYN retransmission is switched off for loopback peers only;
  otherwise Windows takes 2 s). Tested in the layer's guest suite
  (layer/tests/guest/guest.c, section "network"): TCP echo, non-blocking
  connect, accept, half close, blocking accept/read across threads, 3 MiB with
  back-pressure, poll/epoll, UDP with addresses/peek/truncation/sendmsg, IPv6,
  options, `SO_RCVTIMEO`, refused connections. **Not done:** AF_UNIX by path,
  raw/packet/netlink sockets (`getifaddrs` needs netlink), ancillary data,
  `/etc/resolv.conf` synthesis (stage-rootfs.sh writes one; musl resolves over
  UDP to it). **Console:** the app manifest has no network capability on
  purpose today (host/Package.appxmanifest: "runs offline"); sockets there need
  the general capabilities `internetClient`, `internetClientServer` and
  `privateNetworkClientServer` (not device capabilities). Not changed here:
  untested on the console, and a manifest the shell rejects would break
  deploys.

- **2026-10-26 — Easy Anti-Cheat: run a glibc-built native module inside the
  musl/no-`syscall`/no-`%fs` world. Chosen: (c) load-time rewriting of ELF
  images in the layer + (b) a small glibc-ABI shim over musl. Rejected: (a).**
  Rocket League's EAC is the closed Linux module `easyanticheat_x64.so` from
  Valve's "Proton EasyAntiCheat Runtime", which Wine's unix side `dlopen`s
  (`PROTON_EAC_RUNTIME`). It is built against glibc, may issue raw `syscall`
  instructions and uses `%fs` (TLS, `%fs:0x28` stack-protector canary), none of
  which can run here: Wine's unix side is musl, a `syscall` instruction would
  enter the *Windows* kernel with a Linux number, and the console has no
  FSGSBASE so `%fs` carries nothing (the thread pointer lives in TEB slot 63,
  see the FSGSBASE entry below). Three ways to make that work:

  | | (a) Wine unix side on patched glibc | (b) glibc-ABI shim over musl (gcompat style) | (c) rewrite `syscall`/`%fs` at load time |
  |---|---|---|---|
  | Fixes closed `.so` symbol/ABI needs (`libc.so.6`, `__memcpy_chk`, `__isoc23_*`, versioned names) | yes, natively | yes, for the functions we list | no |
  | Fixes closed `.so` raw `syscall` and `%fs` | no: the *Wine-side* glibc could be patched (we build it), the closed `.so` still carries both | no | yes, whatever the libc it was built with |
  | Work / risk | Rebuild Wine's whole unix side on glibc; patch glibc's ~hundreds of inline `syscall` sites and every direct `%fs` form (errno, canary, TLS IE/LE) which glibc's build does not allow to be turned off; redo patches/musl equivalents; lose the working, tested Wine build | A few hundred lines of C (the `_chk` family, `__isoc99/23_*`, `__xstat`, `gnu_get_libc_version`, a `libc.so.6`/`libm.so.6`/`libdl.so.2`/`libpthread.so.0` set that depends on musl); musl's loader already ignores symbol versions | A length decoder, a trampoline pool and a tiny asm entry (`rawsys`); about 1500 lines; testable on any ELF (we diff its findings against `objdump`) |
  | Serves the general Linux layer (unmodified Linux programs) | no: only Wine's own unix side | partly (glibc programs that load through musl), but a glibc *program* still needs its own libc rewritten | **yes**: it is the one mechanism that makes any prebuilt x86-64 ELF (static glibc, Alpine, Go, a closed `.so`) safe to run without a kernel |
  | Failure mode | a mixed-libc process does not exist; we would be rebuilding the project's foundation | missing symbol = clean `dlopen` error naming it (we ship a checker listing them) | an instruction we cannot move falls back to a trap (`int3` + handler), a decode failure is logged and the function is left alone |

  Decision: **(c) is the foundation, (b) is the thin adapter, (a) is dropped.**
  `layer/src/rewrite.c` rewrites every file-backed ELF mapping that the layer
  sees becoming executable (the layer's own loader for the main program and its
  interpreter, and the `mmap(PROT_EXEC)` of any later `dlopen`) before a guest
  instruction of it runs. A `syscall` becomes a jump into a trampoline that
  calls `xrl_syscall` through `rawsys` (register state and red zone preserved,
  every Linux syscall-ABI register kept as the kernel would); each `%fs:` memory
  operand becomes the same operand relative to the TEB-slot thread pointer
  (`mov %gs:0x1678,%r11; <insn with %fs dropped and r11 added as base>`; the
  tpfix form `mov %fs:0,%reg` is patched in place). Images the build already
  verified clean (`tools/xrl-tpfix.py` stamps `e_flags`, Wine's whole unix side)
  are skipped, so Wine pays nothing. `%fs`-less glibc features (IFUNC, `cpuid`,
  `rdtsc`) are left alone: they run natively. The shim (`wine/glibc-compat/`,
  built by `wine/scripts/build-glibc-compat.sh` into the guest's
  `/lib/libgcompat.so.0`) is only needed for a glibc `.so` dlopen'ed into the
  musl process; it is deliberately small and list-driven
  (`tools/glibc-compat-check.sh LIB.so` prints every undefined symbol musl and
  the shim cannot satisfy), because we cannot read the closed module until the
  runtime is installed. One musl behaviour shaped it: musl's loader turns a
  DT_NEEDED of `libc.so.N`, `libm`, `libdl`, `libpthread`, `librt`, `libutil`
  into *itself* (ldso/dynlink.c "block attempts to reload the implementation"),
  so a shim named `libc.so.6` is never loaded. Wine patch 0017 therefore
  `dlopen`s `libgcompat.so.0` RTLD_GLOBAL once, right before the first native
  unix library (`XRL_GLIBC_COMPAT` overrides the path, empty disables); the
  only other libraries a glibc module lists that musl cannot alias
  (`ld-linux-x86-64.so.2`, `libresolv.so.2`) are shipped as empty stubs.
  Verified on the laptop harness: an unmodified glibc static-PIE program
  (`layer/tests/glibc/glibc_test.c`, 30 checks) and a glibc-built stand-in
  module dlopen'ed from a musl program the way Wine does it
  (`layer/tests/glibc/standin_eac.c`: raw `syscall`, `%fs` TLS and canary,
  threads, glibc-only symbols), both with FSGSBASE hidden.
  Costs, stated plainly: the rewritten code differs from the vendor's bytes in
  memory (a module that hashes its own `.text` will notice; we do not hide
  that), every guest `%fs` access in glibc code costs
  a jump pair, and unusual encodings (`%fs` on `push/pop/call/jmp` memory
  operands, VEX-encoded `%fs`) are reported, not guessed. Not done and not
  needed for the EAC module: instruction-level handling of self-modifying code,
  text relocations (`DT_TEXTREL`), `mprotect(PROT_EXEC)` on anonymous memory.

- **2026-10-26 — Rocket League starts on the laptop through the whole stack**
  (console Wine build, real layer, gfx/ on the laptop GPU via
  `XRL_REAL_GFX=1`): it reports "Host OS: Linux (Wine 11.0)", creates its
  D3D11 device (D3D11On12 over D3D12), loads global shaders and script
  packages. Open: loading is ~20x slow, commit grows past the console budget
  (6.6 GiB at 5 min, still rising), no swapchain yet, no audio driver. What it
  took:
  - **Wine 0014, session shared memory.** win32u maps the server's session
    section to read window/class/desktop objects; the layer's MAP_SHARED is
    copy-in, so the client saw a stale copy ("Failed to get shared session
    object", no desktop window). The server is a thread of this process, so
    it publishes its session blocks (`xrl_server_session_block`, append-only
    array) and win32u reads them through ntdll's `xrl_session_block`.
    Chosen over coherent shared file mappings in the layer (placeholders +
    `MapViewOfFile3` restructure the whole memory manager and are unverified
    on the console); that work is still needed for the general Linux layer.
  - **Wine 0015, PE-side debug buffer.** The PE ntdll had the same
    `TEB+0x2000` debug-buffer arithmetic as the unix side (fixed in 0013):
    reading past the host TEB. It now reads the block from TEB+0x3e0
    (`ntdll_thread_data.xrl_block`, offset asserted on the unix side).
  - **Layer: guest->host COM calls (gs-teb Stage 2), `layer/src/com.c`.**
    The guest calls D3D methods directly; they ran on the guest stack with
    the guest TEB view live, so D3D12 `CreateCommandQueue` saw Wine's PEB
    heap and returned E_OUTOFMEMORY (the game then fell back to an older API
    and crashed). Every vtable entry of a host COM object that reaches the
    guest is patched in place to `mov r11, orig; jmp xrl_com_tramp`; the
    tramp switches to the host stack + host view only when the guest view is
    live, so host code (D3D11On12 calling D3D12) passes straight through.
    Discovery scans downcall and COM-call arguments after return for
    pointers to objects whose vtable is outside guest memory and starts with
    three host-image code pointers. Heap vtables included: D3D11 builds its
    context dispatch table at run time. Chosen over IDL-generated wrapper
    objects: no unwrapping of in-parameters, no per-interface code, and
    over-hooking is harmless. Not handled: host code calling back into guest.
  - **Layer: select/pselect6** over the poll core (Wine's desktop wait spun on
    ENOSYS).
  - Tooling: `layer/tests/catch.c` (debugger wrapper, `XRL_CATCH=1`) sees
    faults the VEH never gets; `XRL_TRACE_FAULTS`, `XRL_COM_TRACE`,
    `XRL_PROFILE`; commit high-water lines in the log.
  - Seen, not yet understood: a hardware fault in guest code on a Wine
    thread never reaches the layer's VEH (the kernel goes straight to second
    chance). Must be solved before SEH-using game code can fault safely.

- **2026-10-26 — First Windows program runs: `cmd /c ver` -> "Microsoft
  Windows 10.0.19045"** (laptop, console conditions emulated: no FSGSBASE,
  3.5 GiB budget; results/desktop-wine-cmd-ver-1.txt). Took, in order:
  `wine64 --version` (ld-musl + gateway + TP slot work), then three Wine fixes:
  - 0011/0012: `virtual_alloc_first_teb` also reserved KUSER_SHARED_DATA, and
    read `XRL_HOST_USD` four lines too late (`xrl_init_flags()` ran after it).
    Flags now read first; their TRACE moved after `dbg_init` (it needs a TEB).
  - 0013 TEB adoption (docs/gs-teb.md): Wine prepares a TEB in its 64 KiB
    block as usual, then the thread moves the swap-set fields
    (XRL_TEB_SWAP_RANGES, now shared in xrl_abi.h) and Wine's private areas
    (GdiTebBatch, Win32ClientInfo) into its host TEB and fixes the
    self-pointers. Signal stack, TEB32 and debug info are located from a new
    `ntdll_thread_data.xrl_block` instead of from the TEB address; the
    block's Self points at the live TEB for signal-stack lookups. Exiting
    threads hand Wine their block, not the host TEB (Windows frees that with
    the OS thread; the old deferred free would have read freed memory).
    Wine's PE TlsAlloc reserves index 63.
  - Seam bug found on the way: Wine's per-thread debug buffer was at
    `NtCurrentTeb()+0x2000+sizeof(TEB32)`, i.e. past the end of the *host*
    TEB: every `err:` line Wine printed before 0013 wrote into foreign memory.
  - Seam bug: build-wine.sh copied include/xrl_abi.h into Wine's tree only at
    the `patch` stage, so incremental builds compiled against ABI v1. Now
    refreshed (on change only) before every `build`.
  - Layer: swap set gained the embedded ActivationContextStack (0x290) and
    RealClientId (0x7d8) -> 91 qwords; preadv2/pwritev2 implemented.
  - Seam gap bridged: the prefix baker lists links as `L path -> target`, the
    VFS reads `.xrl-symlinks`; wine/scripts/stage-rootfs.sh assembles the
    guest tree (Wine + /prefix + link table + /etc/machine-id).
  - Laptop loop: tools/run-desktop-wine.sh (seconds per try).

- **2026-10-26 — Console has NO FSGSBASE → guest thread pointer moves to a TEB
  slot.** First layer run on the Series S (results/console-layer-test-1.txt):
  68 checks passed (ELF load, mmap/budget, VFS under LocalState, /proc, /dev,
  pipes, epoll, socketpair+SCM_RIGHTS, BCrypt RNG, and a real
  gfx.get_adapter_info downcall from a guest thread: 0x1414, FL 11_0, 128 MiB).
  But CPUID.7.EBX[0] = 0: the hypervisor hides FSGSBASE, so `wrfsbase` is
  unavailable and CLONE_SETTLS / ARCH_SET_FS failed. Since Windows also never
  preserves a user FS base, FS cannot carry the thread pointer at all.
  Decision: the TP lives in TlsSlots[63] of each thread's host TEB, read as
  gs:[0x1678] (XRL_TP_TLS_INDEX / XRL_TP_GS_OFFSET in xrl_abi.h). Guest ELFs
  are built with -mno-tls-direct-seg-refs and no TLS stack-protector guard;
  `tools/xrl-tpfix.py` rewrites every `mov %fs:0,%reg` to the same-length
  `mov %gs:0x1678,%reg` (objdump-located, fails on any other %fs form). The
  layer claims index 63 at boot, verifies TlsSetValue lands at gs:[0x1678],
  and keeps the slot equal to SET_FS/CLONE_SETTLS. Cost: zero per access (one
  gs-relative load, no faults). Wine's PE TlsAlloc must reserve index 63.
  `XRL_NO_FSGSBASE=1` makes the desktop suite behave like the console.
  **Verified on the console** (results/console-layer-test-2.txt): 94 checks,
  0 failures; 6 cloned threads keep their TP across preemption; futex
  ping-pong 1.25 M round trips/s (≈10x the laptop); clean self-exit.
  Rejected: trap-and-emulate of %fs faults (µs per access, needs a decoder);
  hash lookup via gs:[0x30] (slower, same rebuild cost).

- **2026-10-26 — ABI v2 (append-only).** Folded the convergent requests of all
  four workstreams into `include/xrl_abi.h`: `xrl_syscall` takes `int64_t` (MSVC
  `long` is 32-bit — real v1 bug); op-table functions are native MS-x64 and the
  guest only ever calls layer-generated SysV stubs; handles naming COM objects
  ARE the raw interface pointers; pseudo-syscalls 0x10001–4 hand out the stubbed
  tables (chosen over auxv because Wine's xrlhost already used them and the
  layer already built the tables — less churn); gfx gains
  `swapchain_get_current_index`, `swapchain_resize`, `last_hresult`,
  `get_adapter_info`; audio gains `voice_get_state`; boot ctx gains `exec_path`.
  The layer accepts v1 or v2 hosts and copies only `struct_size` bytes of the
  boot context (v1 code read past a smaller struct). Deferred: keyboard/mouse,
  suspend/resume, guest→host fatal channel.
- **2026-10-26 — B↔C contract reconciled in the layer** (gateway symbol write,
  clone `resume_pc`, exit region, pseudo-syscalls). Spec in syscall-abi.md;
  covered by new tests in the layer's guest suite.
- **Seam bug — exit(60) garbage args.** Honouring `exit(status, base, size)`
  naively unmapped a garbage range (a3 = 0x7ffffffffffffffc) and crashed the
  main thread intermittently. Fixed with plausibility checks; also moved the
  stack unmap before the CLEARTID wake to match Linux. Found by bisecting
  against the committed baseline.
- **Build bug — PowerShell splits `-DCMAKE_SYSTEM_VERSION=10.0.26100.0`** at the
  first dot (CMake saw "10"). gfx's UWP configure only worked from a cached
  build dir; a clean clone would fail. Quoted the arguments.
- **Repo — line endings.** core.autocrlf=true would turn WSL scripts and
  patches into CRLF on a Windows clone. Root `.gitattributes` pins LF for
  `*.sh`, `*.S`, pins files, and `-text` for `*.patch`.

- **2026-10-26 — ABI v1 frozen.** `include/xrl_abi.h` defines three seams:
  the syscall gateway (`xrl_syscall`, Linux return convention), the boot context
  (`xrl_layer_boot`), and the host op-table (gfx/audio/input as opaque-handle C
  vtables). Handles are opaque on the Wine side so wine/ needs no Windows or
  WinRT headers. Rationale: keeps B buildable with the musl cross toolchain in
  WSL with zero Xbox SDK dependency; all native types stay in host/ + gfx/.
- **2026-10-26 — repo skeleton + four workstreams** created. See README.

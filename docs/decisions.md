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

- **2026-10-26 — ABI v1 frozen.** `include/xrl_abi.h` defines three seams:
  the syscall gateway (`xrl_syscall`, Linux return convention), the boot context
  (`xrl_layer_boot`), and the host op-table (gfx/audio/input as opaque-handle C
  vtables). Handles are opaque on the Wine side so wine/ needs no Windows or
  WinRT headers. Rationale: keeps B buildable with the musl cross toolchain in
  WSL with zero Xbox SDK dependency; all native types stay in host/ + gfx/.
- **2026-10-26 — repo skeleton + four workstreams** created. See README.

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

- **2026-10-26 — Easy Anti-Cheat module byte-identical: chosen (b), translation
  into a code cache next to the image (`layer/src/dbt.c`, `layer/asm/dbt.asm`),
  selected with `XRL_DBT`. Rewriting in place stays the default for everything
  else.** Why a change was needed: the rewriter patches the module's code, and an
  anti-cheat checks its own code; the stand-in measured 69 of 8192 bytes changed.
  On Linux an unmodified program simply has its vendor's bytes; so the aim here is
  the same, not a way to hide anything. Why the code cannot run as it is: a raw
  `syscall` executed on Windows enters the *Windows* kernel with a Linux number
  (no exception, no way to intercept), and the console has no FSGSBASE, so `%fs` is
  base 0 (a `%fs:0x28` canary read hits the null page). Compared:

  | | (a) x86-64 interpreter for the module's threads | (b) translation into a code cache | (c) other: make only the two instructions fault |
  |---|---|---|---|
  | Image bytes | untouched | untouched (pages stay readable, r-xp in /proc/self/maps) | untouched |
  | `syscall`, `%fs` | emulated | replaced by the rewriter's own sequences, in the cache | `%fs`: an AV in the null page per access, decoded and emulated by the exception handler (one exception, microseconds, each; one or two per stack-protected call; not measured); `syscall`: **cannot** be made to fault on Windows, so single-stepping every instruction (TF) is the only way: an interpreter that is slower than (a) |
  | What has to be written | a whole x86-64 + SSE/AVX2 interpreter (Blink is not vendored; it is built around its own page tables, not our identity-mapped memory, so pointers Wine and the module exchange would need a translation layer) | a length decoder (we have it), an emitter for the few instruction classes that are not copied verbatim, three helpers (about 700 lines of C and 190 of asm) | an emulator for the `%fs` memory forms plus (a) for `syscall` |
  | Calls between Wine (native) and the module | Wine calling a module function pointer must enter the interpreter; the module calling musl needs a native-call gateway for every import | none needed: translated code is native code; a call from the module to musl is a plain call; a call from Wine into the image enters by an instruction-fetch fault (once per target) | as (b) |
  | Return addresses / stack walks | whatever the interpreter keeps | a call *inside the image* pushes the **original** return address (the unwinder, `backtrace()` and the module itself see original addresses; C++ exceptions through the module work, tested); a call to native code pushes the cache continuation | original |
  | Speed (measured, `glibc/bench.c`, same process) | typically 10-50x slower than native (the usual figure for interpreters; not measured here) | straight-line code 1.0x (hash loop 59 ms vs 58 ms), 100k `getpid` syscalls 16 -> 19 ms, 400k-element `qsort` (7 M indirect calls through the lookup helper) 36 -> 106 ms (3x) | `%fs`-heavy code: very slow |
  | Serves the general Linux layer | yes, but only as a slow fallback | yes: any prebuilt x86-64 program can run unmodified, with its code untouched | no |

  How (b) works. The mapping of the module's code stays as the file has it but is
  made non-executable for the host. A jump or call into it from outside (Wine got a
  function pointer by `dlsym`, a thread start routine, a callback) faults on the
  instruction fetch; the handler (`dbt_veh`, first in the chain) translates the code
  at that address and, following direct jumps and calls, everything reachable from it
  (a trace ends at an unconditional transfer), writes it into a cache that sits right
  after the image (RIP-relative operands need it within 2 GiB; it is a visible
  anonymous RWX mapping) and resumes there. Instructions are copied as they are,
  except `syscall` and `%fs:` operands (`rewrite_core.c`'s sequences, now emitted at
  any address), RIP-relative operands (displacement recomputed so they still reach
  the same original address: `lea fn(%rip),%rax` still yields the original function
  address) and control flow: `jmp`/`jcc`/`call` to image addresses become jumps in the
  cache (a call pushes the original return address with `push imm32; movl $hi,4(%rsp)`:
  no register, because gcc `-fipa-ra` keeps values in r10/r11 across calls to local
  functions; found the hard way: the first version corrupted `__printf_fp`), a call
  through a PLT stub becomes an indirect call through its GOT slot, and `ret`, `jmp *`,
  `call *` look the original target up in a hash table (`dbt.asm`; a hit continues in
  the cache; `jmp *` and `ret` preserve every register, since a jump table can sit in
  the middle of a function; a miss on `jmp *` into the image asks the layer to translate
  through the syscall path, not by faulting, because a fault's exception frame would
  land on the red zone). Targets outside the image are plain jumps/calls (natives
  return straight into the cache).
  Verified (`layer/tests/dbt-test.sh`): the stand-in module's self-checksum sees **69
  changed bytes with the rewriter and 0 translated**, its raw `syscall`, `%fs`
  canary/TLS and 3-thread checks pass; an unmodified static glibc program (30
  checks: threads, setjmp, IFUNC, stdio, qsort) and a C++ program (exceptions through 6
  frames with destructors, virtual calls, `std::function`, `backtrace()`) pass with the
  whole image translated; the Proton EAC bridge chain on Wine runs with **Valve's real
  `easyanticheat_x64.so` bridge and the stand-in module both translated**.
  Limits, stated: self-modifying code and generated code are not followed (executing
  original pages faults, which the guest sees as SIGSEGV); `ret imm16`, far transfers,
  and `%fs` operands inside branch instructions or with `%rsp` as base are not
  translated (the site becomes `ud2` and is logged); a fault inside translated code
  reports the cache address; frames of calls *into native code* show cache addresses to
  an unwinder (an exception propagating out of native code into the module would
  fail); entering the image from native code costs one exception per target (a
  callback invoked 100 000 times from musl is slow; calls inside the image are not);
  unmapping an image does not drop its translations; a guest `mprotect` of the
  module's code makes it executable again untranslated; the table has four probe slots
  per hash (crowding is logged, the target then takes the slow path); the cache is
  8x the image, 1-64 MiB, committed up front. `XRL_DBT=1` translates every image the
  rewriter would patch, `XRL_DBT=a,b` only paths containing `a` or `b`
  (`XRL_DBT=easyanticheat_x64.so` for the EAC module); `XRL_DBT_DUMP=prefix` writes the
  cache and the original->cache map for `objdump -D -b binary -mi386:x86-64`.

- **2026-10-26 — Exec handover: the process a Windows program creates replaces it
  inside the same OS process (layer `execve` + Wine 0020).** Rocket League starts
  EAC only through `RocketLeague_EAC.exe`, the Epic "EOS bootstrapper"
  (v1.9.3, 3.9 MB, libcurl + SDL inside), which `CreateProcess`es
  `RocketLeague.exe`. One OS process holds one Windows process's address space
  (PE bases such as 0x140000000, Wine's reserved ranges and ntdll's unix state are
  per process), so parent and child cannot run side by side here.
  What the bootstrapper does (static reading of the PE, plus a run on our stack
  with `anticheatlauncher.log`, which it writes to
  `C:\users\<u>\AppData\Roaming\EasyAntiCheat\<productid>\<deploymentid>\`):
  - reads `EasyAntiCheat\Settings.json` next to itself: `title`, `executable`
    (`RocketLeague.exe`, relative), `productid`, `sandboxid`, `deploymentid`,
    `requested_splash`, `wait_for_game_process_exit` (false here), `hide_bootstrapper`,
    `hide_gui`; takes `EOS_ANTICHEAT_BOOTSTRAPPER_*` env switches; single instance by a
    named semaphore (`EasyAntiCheatLauncherSemaphore`);
  - detects Wine (it looks up `wine_get_version` in ntdll and reads `/proc/self/maps`
    for libc, to `dlopen` what it downloaded), names the system **`linux64`**, and
    downloads the anti-cheat module over HTTPS from
    `https://modules-cdn.eac-prod.on.epicgames.com/modules/<productid>/<deploymentid>/linux64`
    (cached with `etag`/`sha1`, certificate-checked with `WinVerifyTrust`);
  - **on our Wine that download fails in TLS** (curl error 35, "no schannel support":
    Wine was built without gnutls; the TCP connect to the CDN works, so the layer's
    sockets are fine), and the bootstrapper then sets
    `EOS_USE_ANTICHEATCLIENTNULL=1` in its own environment and launches the game
    *without* EAC ("launching with null client": its own fallback, not ours). So **TLS
    for Wine (gnutls on musl) is a prerequisite for ever seeing the real module**, and
    what the real path does after the download (does the module live in the bootstrapper
    process next to the game? what does it pass?) could not be observed. Not guessed;
    to be read from the log once TLS works;
  - starts the game with `CreateProcessW(app = "<dir>\RocketLeague.exe", cmdline = the
    quoted path alone, no handle inheritance, flags CREATE_DEFAULT_ERROR_MODE |
    CREATE_UNICODE_ENVIRONMENT, environment = NULL i.e. inherited)`. No pipe, no
    duplicated handle and no argument goes to the child in that path; only the
    environment (`SteamNoOverlayUIDrawing`, and `EOS_USE_ANTICHEATCLIENTNULL` in the null
    path). Afterwards it logs "waiting for game window to become visible" (imports:
    `GetWindowThreadProcessId`, `Process32First/Next`, `OpenProcess`, `PeekNamedPipe`,
    `DuplicateHandle`, semaphores/events; what each is used for in the real path is
    unconfirmed) and ends with "Terminating with: 0".

  Decision (with the user): the child **replaces** the parent, like Linux `execve`.
  - **Layer (`layer/src/exec.c`).** A real `execve`: other guest threads are stopped
    (flagged and woken; a thread inside the layer ends itself on the way out of its
    syscall, a thread in guest code is suspended and terminated: no layer lock can be
    held there), guest memory is released, CLOEXEC descriptors closed, signal handlers
    reset (SIG_IGN stays), the new ELF is loaded by the boot loader and its main thread
    started; pid is unchanged, the new main thread has a new host thread id; errors that
    can be known up front come back as errno (`ENOENT`, `ENOEXEC`) before anything is
    destroyed. This is the general Linux-layer feature; tested alone
    (`layer/tests/exec-test.sh`: a parent with three threads in a spin loop, a futex
    wait and a nanosleep, an open CLOEXEC and a plain pipe, a handler and an ignored
    signal, execs a child that checks all of it).
  - **Keeping the server (`prctl` 0x58524c01 / 0x58524c02, layer-private).** Wine's
    wineserver is a thread of the guest (patch 0005), so a Wine process that execs the
    next Windows process must not take it along. The server thread marks itself
    `PR_XRL_DAEMON` (it survives and does not count as the last thread) and the Wine
    side calls `PR_XRL_EXEC_KEEP(ranges, n)` before `execve`: the old image then stays
    mapped except the `n` ranges (every Wine view, which Wine lists itself:
    `xrl_virtual_view_ranges`), CLOEXEC descriptors stay open (the survivors own
    some). The new loader starts a *second* musl/Wine instance next to the first (the
    old one only serves the server thread; 25 MiB of the bootstrapper's 36 MiB stay
    resident after the handover in the test). Rejected: re-initialising Wine's unix
    state in place (hundreds of init-once globals, no way to know it is complete), and
    a second guest process model with its own address space (the layer has one
    address space; PE bases collide).
  - **Wine 0020.** `NtCreateUserProcess` under `XRL_NO_SPAWN` ran stubs for Wine's
    system helpers and failed for everything else; now everything else is handed
    over: the child's server objects exist (`new_process`), its first thread is resumed
    (kernelbase would do that after `NtCreateUserProcess` returns, which it never
    does), the parent ends the way `ExitProcess` ends it on the server
    (`terminate_process` for the other threads, then for itself), and instead of
    `exit()` the loader of the child is exec'ed in place with `WINESERVERSOCKET`,
    `WINELOADERNOEXEC` and `XRL_SESSION_BLOCK_FN` (the address of the server's
    session-block lookup in the old image; the new Wine instance did not start the
    server, so win32u reaches it through that pointer). The command line, working
    directory and `WINEDEBUG` of the child are the ones `spawn_process` would have used.
  - **Test (`layer/tests/exec-win.sh`, `exec-win/parent.c`, `child.c`):** a Windows
    program `CreateProcess`es a second one (three arguments, one with a space; an
    environment variable set just before) and exits. One layer boot, one `exec:` line
    with the old image kept, the child prints its pid, its arguments and the inherited
    variable and exits 0; the parent never comes back from `CreateProcess`. Verified
    on the laptop harness.

  Costs, stated plainly: a program that `CreateProcess`es and then keeps working or
  waits for the child (a shell, an installer, `cmd /c`, any launcher that does
  something after the child starts) loses everything after the call; the child's
  parent id names a process that is gone; the old unix side is never freed (about
  25 MiB per handover); `CREATE_SUSPENDED` is ignored (the child runs). For the EAC
  bootstrapper this is exactly the shape of the null-client path. For the real path it
  is an open question whether the module the bootstrapper loads into its own process
  has to stay next to the game; that cannot be decided before the module can be
  downloaded.

- **2026-10-26 — Linux signal delivery.** The layer recorded handlers but never
  ran them; Wine's thread suspend (SIGUSR1 via tgkill) waited forever, which is
  where Rocket League hung after loading. Now real rt_sigframes (ucontext,
  siginfo, XSAVE fpstate with Linux's magic words, uc_stack, sigmask) go on the
  guest stack or the sigaltstack, three ways:
  - **at syscall return**, by the thread itself; `xrl_sig_jump` (thunks.asm)
    loads the whole register state with `xrstor` + `iretq`, so nothing below
    the target rsp (the red zone) is touched;
  - **asynchronously** to a thread running guest code: the sender suspends it,
    checks rip is in executable and rsp in writable guest memory (a try-lock on
    the VMA list: a thread in layer code is never waited for), writes the frame
    and redirects the context. Threads in host code are retried by a pump
    thread; threads in a syscall get it at syscall return (waits are woken:
    futex, I/O, ntsync, nanosleep);
  - **faults in guest code** with a guest handler (SIGSEGV/BUS/ILL/TRAP/FPE with
    Linux's si_code, trapno and error code, which Wine's handlers read), instead
    of the crash report. The frame stays below the exception dispatcher's frames.
  rt_sigreturn restores everything and re-runs a syscall that an SA_RESTART
  handler interrupted (Linux's -ERESTARTSYS; the frame carries a private
  record). kill(getpid()) picks a thread that does not block the signal (it
  looked up tid == pid, which stopped existing with host thread ids). Guest
  suite: 15 checks incl. all registers intact across an async signal.
  Unverified on the console: Suspend/Get/SetThreadContext in the app container.
  Note for glibc programs (the EAC branch's rewriter): the layer takes
  rt_sigreturn's ucontext from a1, as musl's patched `__restore_rt` passes it.

- **2026-10-26 — Rocket League loads past the shader cache; its own peak is
  6.4 GiB.** With a 12 GiB test budget it gets through script packages, audio
  init (no driver), display modes and a second D3D11 device, then hangs: Wine
  suspends a thread with SIGUSR1 (tgkill) and the layer cannot yet deliver a
  signal to a running thread. The stack walk (`dbghelp`) just before looks like
  RL's crash-dump path; most likely the Epic login critical error that RL also
  hits natively without the launcher (not confirmed). What it took:
  - **ntsync (Wine 0017, `layer/src/ntsync.c`).** The layer implements
    `/dev/ntsync` (Linux 6.14 uapi: semaphores, mutexes with abandonment,
    events, wait-any/all with alert), so Wine's waits no longer queue on the
    wineserver thread: startup to the game window went 141 s -> 60 s.
    `XRL_NO_NTSYNC=1` falls back. ioctl requests are now taken as unsigned
    32-bit (musl passes an `int`; bit 31 of `_IOR` sign-extended).
  - **Host thread ids (Wine 0018).** Wine's tids/pid are the real Windows ids
    (the layer's Linux tids are `GetThreadId`, pid `GetCurrentProcessId`), so
    the TEB's ClientId is the same in the guest and host views. With mismatched
    ids the host exception dispatcher lost wake-ups (an ntsync mutex owned by
    one id, released by another) and RL deadlocked while loading.
  - **Lazy mappings keep their identity.** A new thread commits the whole
    mapping holding its stack (the kernel pushes exception frames there), but
    adjacent anonymous mappings had been merged, so every thread start
    committed RL's 1.6 GiB heap neighbour. VMAs now carry the id of the mmap
    that made them and only merge within it.
  - **MAP_FIXED_NOREPLACE over host memory -> EEXIST** (was ENOMEM): Wine's
    free-area search stopped at KUSER_SHARED_DATA (0x7ffe0000) instead of
    stepping over it.
  - **Measured natively on the laptop** (same exe, Windows): private bytes
    3.3 GB at 5 s, **6.5 GB peak** during the shader-cache load (464 k shaders),
    ~4-5 GB after. The layer's 6.4 GiB matches: RL really writes that much
    (write faults 3.8 GiB of the first 4.2). The console's ~4.5 GiB commit
    wall therefore needs swap in the layer (next), not tighter accounting.
  - **`/proc/cpuinfo` from the real CPU (`layer/src/cpuinfo.c`).** It was a
    fixed "AMD Custom APU 0405" text whose flags disagreed with CPUID (listed
    fsgsbase, svm). Guests execute CPUID directly, so the file must say the
    same: built from CPUID/XGETBV with Linux's flag names, Windows topology for
    core ids and `/sys/.../topology`. Hostname is the machine's own name.
    Nothing about the CPU or the hypervisor is added or hidden.

- **2026-10-26 — The Proton EAC bridge runs on the layer (verified with a
  stand-in client module).** What the runtime really is: `v2/lib64/` holds a
  Wine builtin PE (`easyanticheat_x64.dll`, exports the whole EAC client API:
  `CreateGameClient`, `GameClient_*`, `ClientAuth_*`, `NetProtectClient_*`) and
  an 18 KB glibc-built unix half (`easyanticheat_x64.so`, NEEDED ntdll.so /
  libc.so.6 / libdl / libpthread, no `syscall`, no `%fs`, exports
  `__wine_unix_call_funcs`). The PE half's DllMain asks Wine for the bridge
  unix lib, then passes the unix path of its own module to `eac_init_library`,
  which replaces the file name with `easyanticheat_x64.so` and `dlopen`s it:
  **the real EAC client module is a second `easyanticheat_x64.so` that must sit
  next to the game's own Windows `easyanticheat_x64.dll`** (Wine only picks the
  bridge when that `.so` exists there; `get_load_order`). It is not in the
  runtime and not in the laptop's Rocket League folder (only
  `EasyAntiCheat_EOS_Setup.exe`, certificates and `Settings.json`), so the real
  module could not be run. `layer/tests/glibc/run-eac-bridge.sh` runs the same
  chain with `standin_eac.c` as that module: LoadLibrary of the DLL ->
  bridge PE -> bridge unix half -> dlopen of a glibc module with rewriting and
  the shim -> `CreateGameClient` -> a client object. Found and fixed on the way:
  - **`membarrier` (324) was ENOSYS** and musl's `dlopen` of any library with a
    TLS segment in a multi-threaded process calls it (PRIVATE_EXPEDITED), falling
    back to signalling every thread, which the layer cannot deliver: the process
    hung. Now answered with Windows' `FlushProcessWriteBuffers`. This hits every
    TLS-bearing `dlopen` under Wine (the real client is C++), not only EAC.
  - **The shim was never loaded** on that path in the first build: my Wine tree's
    Makefiles still pointed at the other agent's sources (a copied build tree
    records absolute paths) so patch 0019 was not compiled in. `build-wine.sh
    configure` in the copy fixes it (about a minute, nothing rebuilds).
  - **`/proc/self/maps` lied**: private file maps showed offset 0, inode 0 and
    only the basename. A module that locates and checksums its own code (the
    stand-in does, as an anti-cheat would) read the wrong bytes. Maps now carry
    the full guest path, the file offset and the inode (mem.c: three fields on
    `vma`). With that, the stand-in measures honestly that the mapped copy of
    its code differs from the file in 69 of 8192 bytes: the rewriter's patches.
    **A module that hashes its own `.text` against the file will see exactly
    this; nothing here hides it.**
  - `XRL_TRACE=1` now also logs every path the guest opens (vfs.c).
  Also found: `stage-rootfs.sh` died on a first-time stage (a `tr | head`
  pipeline under `pipefail`).

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
  so a shim named `libc.so.6` is never loaded. Wine patch 0019 therefore
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

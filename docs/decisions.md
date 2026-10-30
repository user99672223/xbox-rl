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

- **2026-10-30 — A Debian 13 (glibc) userland on the layer: first fixes.** Branch `claude/desktop-debian`
  (docs/desktop-brief.md; progress in that branch's `REPORT.md`).
  - **Case collisions are skipped, not fatal.** `tools/xrl-rootfs-unpack.py` keeps the first of two names that differ
    only in case and lists the second; the CI rootfs carries a dpkg `path-exclude` for man/info/doc/locale, where
    nearly all such names live, so later `apt install`s inside the layer do not unpack them either. Names that matter
    (terminfo, some language trees) will need a VFS answer.
  - **glibc forks with `clone(flags, stack=0)`.** `asm/rawsys.asm` passes a6 = 0 (no resume pc) for a raw clone with
    no new stack, so it takes the same path as `fork` (proc.c); with a stack it still passes its child entry. The
    `XRL_FORK_TASKS` path is unchanged (it is taken before a6 is read).
  - **mremap growth stays one mapping.** An in-place extension takes the old mapping's id and merges, and the old range
    may span pieces of one mmap: APT grows its cache by 1 MiB at a time, and the second in-place growth used to fail
    with EFAULT ("Dynamic MMap ran out of room").

- **2026-10-26 — Keyboard, mouse and a framebuffer for a Linux desktop: ABI v3, evdev devices, `/dev/fb0`.**
  Branch `claude/linux-display`. The spec for the guest-visible parts is in `docs/syscall-abi.md` ("ABI v3").
  - **Events speak Linux.** `xrl_input_event` uses evdev's types and codes (`KEY_*`/`BTN_*`, `REL_*`, `ABS_*`) rather
    than SDL scancodes or Win32 virtual keys: the layer's `/dev/input/event*` then pass them through almost
    verbatim, Wine's xrlhost needs one table (Linux key -> PS/2 scancode) for `SendInput`, and each front-end
    needs one table into the common vocabulary (SDL: `host/src/input_keymap.h`, USB HID usage -> Linux key, which
    is positional like the codes; Win32 test window: set-1 scancode, identical to Linux below 0x59, a short table
    for the E0 keys). Text (`XRL_IEV_TEXT`) is separate because Linux guests have their own keymap (X) and Wine makes
    `WM_CHAR` itself. SDL text input is not started on the console (it would raise the on-screen keyboard);
    TEXT events appear only if a front-end sends them.
  - **A queue with one consumer, and the state kept in the queue.** Key repeat, held-key release on focus loss and
    motion coalescing/dropping live in `input_queue.cpp`, once, so every front-end and consumer gets the same
    behaviour. `wait_events` gives the guest something to block on without the host knowing about guest threads;
    the layer's pump thread turns it into `xrl_io_notify()` so `poll`/`epoll` on the evdev fds work. The cost of a
    single consumer is that evdev and Wine's xrlhost cannot both be live; Wine on the same desktop as Linux apps
    would need the layer to be the one consumer and Wine to read evdev too (not needed yet).
  - **Two pointer devices, mode chosen by who is listening.** A relative mouse (`event1`) needs the host to capture
    the cursor; an absolute pointer (`event2`, a VM tablet's shape) follows the host cursor and is what a desktop
    wants. The layer puts the host in relative mode only while `event1` is open and `event2` is not; opening
    `event2` (or neither) keeps the cursor free. Left Ctrl+Left Alt releases a capture so the user can leave the
    window. Absolute positions are normalized over the aspect-fitted picture (`set_content_size`), so the guest sees
    pixel-exact pointing whatever the window size or letterboxing.
  - **`/dev/fb0` as live memory, presented by polling.** The layer's file mmaps are copies (read at map time,
    written back at unmap), useless for a framebuffer. Two small hooks (`xfile_ops.mapped/unmapped`, called from
    `map_range`/`vma_unmap_range` under the memory lock) tell the device where the guest's mapping is, and a
    presenter thread then reads that memory directly. There is no scan-out to ask about dirty pixels (Windows'
    write-watch needs memory allocated with a flag; ours is the layer's arena), so the thread compares the visible
    rows with its last snapshot (8 MB of reads per 8 ms at 1080p while the picture is idle) and presents only on a
    change; `present_framebuffer` waits for vblank, so a moving picture is paced at the refresh rate. The snapshot is
    taken under the device lock so `munmap` never frees pixels the presenter is reading, then the lock is released
    before the vblank wait. Read faults (the guest `mprotect`ed the mapping) are caught and drop the mapping.
  - **The host blit is a native D3D11 fullscreen triangle** (`gfx/src/fbpresent.cpp`, two shader-model-4.0 blobs
    compiled offline with fxc and embedded, `gfx/src/fb_shaders.h`): same swapchain code as the guest's native
    D3D11 path (`HostSwapchainCreate11`, CoreWindow on the console, the test HWND on the laptop), bilinear, black
    bars computed in the viewport. Chosen over D3D12 (needs queues, fences, root signatures for the same result) and
    over `DXGI_SCALING_ASPECT_RATIO_STRETCH` (CoreWindow only; the laptop path would differ). The window has one
    swapchain: a Windows game's own swapchain and this cannot share a window.
  - **Verified on the laptop (not on the console):** `layer/tests/fbdemo-test.sh` (XRL_REAL_GFX=1 window; the fbdemo
    guest checks the device ioctls, mmap/pread/pwrite coherence, and the exact event sequences injected through
    `XRL_INPUT_SCRIPT`) passes with TOTAL FAILS=0, and a screenshot of the window shows the picture with the pointer
    where the events put it (`results/display-fbdemo.png`); `xrl_inq_test` covers the queue and the HID map; the
    layer's regression suite stays at 0 fails; the UWP builds of the layer and gfx compile and link; `sdl_main.cpp`
    compiles against the vendored SDL headers. The console path (SDL input, CoreWindow swapchain for the blit) has
    not been run.
  - **Windows games (Wine): `patches/wine/0027-kbm-input-pump.patch`.** The desktop thread of 0026 starts one more
    thread per process (`wine_xrl_input`, in user32) that loads `xrlhost.dll`, waits on the host queue
    (`xrl_input_ops.wait_events`, 100 ms slices) and replays the events with `NtUserSendInput`: keys become PS/2
    scancodes (Linux code == set-1 scancode for the main block, a table for the E0 keys) plus the layout's virtual
    key, so the wineserver does focus routing, key repeat (the pump ignores the host's value-2 repeats), `WM_CHAR`
    and `WM_INPUT` itself; `ABS` events (0..32767) become `MOUSEEVENTF_ABSOLUTE` (0..65535), `REL` a relative move,
    wheels `WHEEL_DELTA` notches. The pump puts the host in relative mode while the application hides the cursor
    (`GetCursorInfo` flags, polled every 100 ms; the usual state of a game with mouse look) and in absolute mode
    otherwise (`XRL_MOUSE=abs|rel` forces one, `XRL_KBM=0` switches the pump off). Not done: ClipCursor/SetCursorPos
    do not reach the host (no cursor warp), no IME/TEXT use, no gamepad change. **Needs 0026** (it hangs off the
    desktop thread) and a restaged rootfs: the shared `~/xrl-work/rootfs` predates 0026, so the pump is dormant
    there until the next stage. `wine/tests/kbm-test.sh` (XRL_ROOTFS=<tree with the 0026/0027 build>) runs a Windows
    window that logs what it receives while the test host injects events: key A with `WM_CHAR`, Shift+B, an extended
    key, a wheel notch, a click at the centre of the screen from an absolute position, a relative move arriving as
    raw input - 10/10 on the laptop (`XRL_DISPLAY` 1920x1200) with the build tree's win32u/user32/xrlhost
    overlaid on a hard-linked copy of the shared rootfs. Console not run.
  - **An X server on `/dev/fb0` (`layer/tests/guest/xbridge.c`, `layer/tests/xbridge-test.sh`).** One single-threaded guest, no libc, no Xlib:
    it polls an Xvfb `-fbdir` screen file (XWD: big-endian header, `header_size + ncolors*12` to the pixels, 32 bpp BGRX = our framebuffer's
    format, so rows are copied verbatim; it first resizes /dev/fb0 to the X screen and writes only runs of changed rows), and reads
    event0/event2 to send XTest `FakeInput` requests it encodes itself (key code = evdev + 8, buttons 1/2/3, wheels as buttons 4-7, ABS -> an
    absolute warp scaled to the X screen, REL -> relative motion), after `QueryExtension("XTEST")`. It talks TCP today (the layer's Winsock),
    AF_UNIX once the layer has it. Chosen over linking Xlib/libXtst because the guest needs nothing but the syscall gateway, and over Xorg's
    fbdev + evdev drivers because those need udev, `/sys/class/input` and `/proc/bus/input/devices`, which the layer does not have (nor the
    fork/exec an Xorg session needs; a second process for the bridge likewise waits for the process model). Laptop proof with the X server in
    WSL (Alpine's Xvfb 21.1.25, twm, xterm, unpacked from the verified minirootfs into RAM through `apk`): 23 injected requests, QueryPointer
    returned the injected position (300,250), the xterm showed the typed "hello world", and `/dev/fb0` read back inside the layer held the
    X desktop (`results/display-xbridge-fb0.png`). Notes: Xvfb cannot mmap its `-fbdir` file on a Windows drive (SIGBUS), so the screen file
    is copied Windows-side by a loop; the guest reopens it each poll because a writer replacing it in place fails while it is held open
    (Windows sharing); the test host's GPU init took more than a minute once while another agent ran its own tests (not reproduced).

- **2026-10-27 - The X11 desktop runs on the layer: Xvfb + twm + xterm as guest processes, a screenshot of it
  (`results/userland-xvfb-twm-xterm.png`).** Needs both the procs branch (fork/exec) and this one (AF_UNIX, pty,
  shm, modes/owners, timers). Checked on a scratch merge of `claude/linux-procs` and `claude/linux-userland`
  (6 of 6 runs; `layer/tests/xsession-test.sh`, `tools/xwd2png.py`): xdpyinfo lists 22 extensions incl. MIT-SHM,
  xwininfo shows twm's frames and xterm's window, xterm's shell prints `id` through the pty. Found by running it
  and fixed in this branch: (1) **EPOLLET** lost a wake-up when a socket was drained and refilled between two
  `epoll_wait`s (the old code compared ready masks; now a file with an edge counter reports when it moved, the
  rest when the I/O epoch did) - xorg's `ospoll` uses it for every client; (2) **ITIMER_REAL/alarm are per process**
  (Xvfb's SIGALRM killed the shell); (3) the load-time rewriter let a window run from `__restore_rt`'s `syscall`
  into `__sigsetjmp`, so every `sigsetjmp()` crashed (xterm's child); (4) `setgroups`/`setfsuid`/`setfsgid` were
  ENOSYS and xterm's pty child exits 28 on that. **To merge the two branches** (conflicts are all in
  `syscall.c` identity code, `CMakeLists.txt`, `build.sh`, `decisions.md`): keep procs' `getuid..getresgid/umask/
  getpid` bodies; make `xrl_cur_uid/gid/umask/pid` in util.c read `xrl_curproc()` (euid, egid, umask, pid),
  `xrl_signal_pid` call `xrl_sys_kill`, give proc.c's first process `xrl_env_uid()/xrl_env_gid()` (XRL_UID/XRL_GID)
  and define `xrl_tty_signal(pgrp, sig)` there as `xrl_sys_kill(-pgrp, sig)`. The exact script that does it,
  with every conflict rule, is `xrl-work/linux-agents/userland/remerge.py`.
- **2026-10-26 - Alpine 3.24 is the userland rootfs; packages come from `apk`.** `alpine-minirootfs` 3.24.3
  (x86_64, sha256 checked against the mirror's `.sha256`) is unpacked by `tools/xrl-rootfs-unpack.py` (tar
  stream or file -> NTFS tree + `.xrl-symlinks` + `.xrl-meta`; hard links become copies) into a tree outside
  the repo (`xrl-work/alpine/rootfs`). More packages are installed with Alpine's own `apk` signature checks,
  either inside the guest (`apk add --no-scripts`: works over the layer's Winsock + TLS, verified with
  `apk add figlet` / `apk del`; post-install scripts need fork/exec, i.e. proc.c) or with apk in a WSL chroot
  whose result is piped through the unpacker. Musl PIE binaries need nothing special: the load-time rewriter
  (rewrite.c) patches `syscall` and `%fs:` sites; one bug found by Alpine's tiny musl wrappers (no FDEs) is
  fixed in rewrite_core.c (a window never spans a `ret`/`jmp`).
- **2026-10-26 - AF_UNIX is its own module (`unix.c`), process-independent.** A socket is a refcounted `usock`;
  the open file description holds one reference, so do a connected peer, the accept queue, the abstract-name
  table and the VFS node of a bound path. Data is queued in the receiver as chunks (one send, one datagram,
  one record) that carry the passed fds (open-file-description references: no fd number is in flight, so it
  works between threads and between guest processes alike, whichever fd table receives it), the sender's
  credentials and for datagrams the sender's address. One global lock; waits sleep on the layer's I/O epoch
  like pipes. It replaced the pipe-based socketpair of ipc.c. Pathname sockets are **vnodes** (vfs.c): rows
  in a table that `stat`/`getdents`/`unlink`/`rename`/`open` consult, not host files, so a crashed run
  leaves nothing behind and `/tmp` stays clean. Costs: a closed peer's object lives until its partner closes
  too (getpeername/SO_PEERCRED keep working, as on Linux); fds in flight are not garbage collected if a
  socket is passed over itself. `cur_cred()` in unix.c is the one place that names the caller's pid/uid/gid.
- **2026-10-26 - Unix modes and owners over NTFS: the `.xrl-meta` journal, on only where the file exists.**
  NTFS holds neither, so vfs.c keeps a table (`path -> mode, uid, gid`, hashed) loaded from
  `<vfs_root>\.xrl-meta` and appended to on every change (a later line wins, `-<TAB>path` forgets), with a
  `#default<TAB>uid<TAB>gid` line for unlisted files. With it: stat/fstat report it, chmod/chown change it
  under Linux's rules (owner or root; chown to another user only for root; setuid/setgid dropped), files and
  directories are created `mode & ~umask` owned by the caller, rename (also of a directory tree), unlink and
  link carry entries, `open`/`access` check the bits for a non-root caller. Without the file (the Wine prefix,
  trees baked before) nothing changes - chmod stays a no-op and everything is 1000:1000 - so Rocket League's
  prefix keeps today's behaviour. The caller's ids are `XRL_UID`/`XRL_GID` (default 1000) until proc.c's
  per-process ids replace `xrl_cur_uid()`/`xrl_cur_gid()`. Alpine runs as root (Xvfb refuses to create
  `/tmp/.X11-unix` otherwise). Not checked: directory search/write permission along a path.
- **2026-10-26 - Shared memory by address sharing, not by Windows sections (`shm.c`).** mem.c owns one reserved
  arena with a VMA list, lazy commit and a swap file, and the guest processes share one address space; a
  Windows section view cannot be placed inside the reserved arena without redoing all of that. So the
  pages of a SysV segment, a memfd or a `/dev/shm` file are one ordinary `MAP_SHARED` anonymous region, and every
  attach/mapping of the object returns the same address (Linux allows any address). Cannot: two addresses
  for one object (MAP_FIXED over it, `shmat` at a chosen address, a ring buffer mapped twice) - those get
  -EINVAL or the old private-copy mapping. Protection is per object. A memfd's pages live while a description
  or a mapping does, a `/dev/shm` file's while mapped and are written back at the last munmap (up to 64 MiB);
  The pages of a file object are reserved with 8x headroom (1 MiB..1 GiB, committed only when touched) so the file can grow
  after its first mapping (a Wayland-style pool resize); beyond that mmap fails -ENOMEM. A MAP_FIXED mapping over a region is
  refused (-EINVAL). read()/write() on the file while it is mapped do not see the mapping's writes. **For proc.c / integration:** a
  process that exits or execs must not unmap these regions while another process maps them; `xrl_shm_munmap`
  claims the range and keeps the pages (its answer is `XRL_NOT_NET` for anything else). Bug found on the way
  (old): munmap/msync of a writable shared *file* mapping deadlocked on mem.c's own lock (the write-back's
  populate); fixed.

- **2026-10-26 — Guest processes: many Linux processes inside the one Windows process (branch `claude/linux-procs`).**
  The layer was one guest process (threads only; `fork`/`vfork` returned `-ENOSYS`). The console app is UWP and cannot
  create Windows processes, so every guest process must live in our one OS process and one address space. Design:
  - **Objects (`layer/src/proc.c`).** `xrl_proc`: pid, ppid, pgid, sid, state (running / stopped / zombie), wait status,
    its threads (`xrl_thread.proc`; the thread list stays global), children (a parent pointer + the global process table,
    walked under one lock), its **fd table** (`xrl_files`, refcounted: `CLONE_FILES` shares it, fork copies the entries,
    sharing the `xfile` objects, which is what makes inherited pipe ends keep a pipe open), cwd, umask, credentials,
    **signal dispositions** (`act[]`; per-thread masks and pending sets stay in `xrl_thread`), rlimits, argv/envp/exec path,
    the controlling-tty stub, and an **mm**. `fd.c`, `vfs.c` (cwd), `signal.c` (`g_act`), `syscall.c` (umask, ids) and
    `procfs.c` act on the *calling thread's* process, so the API the other modules use does not change (the userland
    agent's AF_UNIX/SysV code keeps calling `xrl_fd_install` etc. and gets per-process tables for free). The process
    that boot starts keeps the Windows process id as its pid (Wine takes it as its own; unchanged) and is the "init":
    orphans are reparented to it, and its exit ends the layer exactly as before (so Rocket League's path is the old one).
    Every other pid is the Windows thread id of the process's first thread (unique, never reused: handles stay open).
  - **Address space.** One arena, one VMA list, so processes cannot overlap by construction when they use `mmap(NULL)` (top
    down `find_gap`). Every VMA carries an owner (`mm` id): `mm` is the Linux object (`proc->mm`, shared by a `vfork`
    child and its parent, by `CLONE_VM`). `/proc/<pid>/maps` shows the owner's VMAs; exec/exit unmap the owner's VMAs when
    the last user of the mm leaves; `MAP_FIXED` over another mm's mapping fails with `-ENOMEM` and a log line (a program
    that hard-codes addresses cannot stomp a neighbour). PIE and ld-musl (Alpine's binaries) load at a free base, one base
    per process. A non-PIE `ET_EXEC` image whose fixed range another live process uses fails exec with `-ENOMEM` and
    logs which range and which pid. `brk`: the boot process keeps the arena's brk window; every other mm gets a lazily
    reserved window of its own (musl's malloc falls back to mmap when brk fails anyway).
  - **`fork` and friends.** A child is a new guest thread (a new Windows thread) that "returns from the syscall" with
    `rax = 0` using the same mechanism as a thread clone (`xrl_do_clone`'s no-resume-pc path), with a new `xrl_proc`.
    1. `vfork` / `clone(CLONE_VM|CLONE_VFORK)` (posix_spawn): the child runs on the parent's memory and the parent is
       suspended until the child calls `execve` or exits. Exact vfork semantics; shared memory stays shared (posix_spawn
       reports exec errors through it). The thunk frame the parent will return through lies on the guest stack below the
       child's initial rsp, so the layer saves and restores those 64 bytes.
    2. `fork` / `clone(SIGCHLD)` (no `CLONE_VM`): the same schedule (child first, parent suspended until the child
       execs or exits) **plus a snapshot**: at fork the layer copies the committed pages of every writable private
       mapping of the parent's mm (stack, data, heap, malloc arenas, TLS) and remembers the VMA list; when the child
       execs or exits it puts them back (unmaps what the child mapped, recreates anonymous mappings it unmapped, copies the
       contents back). The parent therefore sees what a real fork would show it: a shell's `(x=2)` or `$(cmd)` leaves its
       variables, heap and `rootshell` alone. This covers fork+exec, fork+builtin+exit (every shell stage), `system()`,
       `popen()`. A mm whose writable footprint exceeds `XRL_FORK_SNAPSHOT_MB` (256) falls back to plain vfork
       semantics with a warning (Firefox-sized parents: see below).
       **Fork groups (built, `XRL_FORK_SWAP=0` turns it off).** "Child first, parent suspended" would deadlock or
       reorder output whenever a forked child that does not exec waits for something the parent does (a background
       subshell `(sleep 1; echo x) &`, a pipe the parent reads, a signal only the parent sends). So processes that share
       addresses this way form a group (`mm->grp`): one *holder* runs guest code at a time, and the address space holds
       one member's image. A member that is about to block in the layer (`xrl_wait_addr` and `xrl_sleep_ns` cover wait4,
       pipe reads, poll/epoll, futexes, sleeps) lets go of the baton; when its wait ends it takes the baton back, and if
       another member's image is in memory by then, that image is snapshotted into the member and this one's is restored
       (the same snapshot code, a few milliseconds for a shell). The parent resumes from its fork call as soon as the child
       execs, exits or blocks. Verified with Alpine's `ash`: `(sleep 1; echo bg) & echo before; wait` prints `before`
       first. Only single-threaded processes join (a thread that blocks cannot stop its siblings); a multi-threaded parent
       keeps the child-runs-to-completion schedule with its other threads frozen. Blocking primitives outside the two hooks
       (Winsock waits in net.c, ntsync's waits) do not release the baton: a forked child blocked there keeps the parent
       waiting.
    3. `clone(CLONE_VM)` without `CLONE_THREAD`/`CLONE_VFORK`: a new process on the shared mm, running concurrently.
  - **Real concurrent copy-on-write fork (not built).** The fork groups above are cooperative: members of one group never
    run at the same instant. What that cannot do: a fork-without-exec child that computes while its parent computes
    (Firefox's fork server, Chromium's zygote, a forking daemon with CPU-bound children), a multi-threaded process that
    forks without exec, and big processes (the swap copies the whole writable footprint). Options: (b) section objects:
    back private memory with pagefile sections and give the child a copy-on-write view, which Windows can only map at
    *different* addresses, so it needs (c) address translation (Blink-style or the `XRL_DBT` code cache) with a
    per-process offset; (d) on the laptop only, a second Windows process; (e) make the baton finer: swap at every
    syscall boundary of a *runnable* member, which turns the group into a time-slicing scheduler (a few ms per slice for
    a shell, far too slow for a browser).
  - **`execve`** (extends exec.c): the old image goes (in place if the mm has one user, else the process takes a fresh mm
    and drops its reference), `FD_CLOEXEC` descriptors close in this process's table, handlers reset (SIG_IGN stays),
    other threads of *this* process stop, the new image starts on a new main thread. The Wine hand-over prctls are
    unchanged. `#!` scripts are run by execve itself (interpreter plus one argument, up to 4 levels), so `env`, make and
    `execvp` callers work without relying on the shell's ENOEXEC fallback.
  - **Measured (laptop, Alpine busybox ash, `layer/tests/alpine-procs.sh` and a 700-process loop).** fork snapshot of a
    shell's writable memory 1.3-2.6 ms (median 1.5), the same again to put it back; a fork+exec+wait of
    `/bin/true` from ash about 14 ms end to end (200 spawns, 100 two-stage pipelines and 200 `$(...)` in 9.9 s, the
    process's mappings stayed at 20 lines and VmSize at 75 MB). Races found and fixed while testing: a parent that
    `wait()`s returns as soon as its child is a zombie and can end and be reaped before the child has signalled it (the
    child now holds a reference); a kill that lands while the victim is inside execve (a dying process gets no new
    thread); both showed up as intermittent `STATUS_HEAP_CORRUPTION` at the end of a busybox script.
  - **Exit, wait, signals.** Exit (last thread, `exit_group`, fatal signal) stops the process's threads, closes its fd table
    (so pipe readers see EOF), releases the mm, reparents children to init, becomes a zombie holding the wait status, sends
    SIGCHLD to the parent (the zombie is reaped at once when the parent ignores SIGCHLD or sets `SA_NOCLDWAIT`) and wakes
    waiters. `wait4`/`waitid` (pid, -1, 0, -pgid; `WNOHANG`, `WUNTRACED`, `WCONTINUED`, `__WALL`) block in the layer, are
    interruptible and restart like any other. `kill(pid | 0 | -pgid | -1)`, `tgkill`/`tkill` check the thread's process.
    A default-action fatal signal terminates the *target* process (it used to end the layer). SIGSTOP/SIGTSTP/SIGCONT stop
    and continue a process: the state and the parent's wait report are exact; the threads freeze at their next syscall
    boundary (a thread in a long blocking call stops when it returns). `setpgid`, `getpgid`, `getpgrp`, `setsid`, `getsid`;
    `ioctl(TIOCGPGRP/TIOCSPGRP/TIOCSCTTY/TIOCGSID)` on the tty stand-in give shells a foreground pgrp.
  - **Not done on purpose:** real preemptive concurrency of a forked-but-not-exec'd child, ptrace, `CLONE_NEWPID`
    namespaces, per-process resource accounting beyond what `getrusage` already fakes.

- **2026-10-26 — `XRL_D3D11=native`: the game's D3D11 can run on a native D3D11 device instead of D3D11On12;
  measured on the laptop at RL's title menu it halves the render thread's CPU and cuts Present ~4x. Opt-in; the
  default (D3D11On12) is unchanged.** Branch `claude/d3d11-native` (gfx commit da923fc).
  - **Design.** A host-process switch, read once in `xrl_gfx_init` (`gfx/src/core.cpp`) and logged as
    `xrl_gfx init: D3D11 device path = NATIVE` / `= D3D11On12`. Laptop: an environment variable of the shell
    that starts `tools/run-rl.sh` / `run-desktop-wine.sh` (inherited by `xrl_test_host.exe`; it is not an
    `XRL_ENV_` guest variable). Console: `host_env=XRL_D3D11=native` in `LocalState\launch.cfg` (applied in
    `load_config()`, before gfx starts). In native mode `d3d12_create_device` hands the guest a stand-in
    ID3D12Device + queue (`gfx/src/stub12.cpp`; no D3D12 device or queue exists, no memory spent), and
    `d3d11on12_create` makes a plain `D3D11CreateDevice` (FL11_0, BGRA support, same adapter pick) on the gfx
    thread; the guest's D3D11 calls then go straight to that device. `d3d11_create_swapchain` sees a device
    without `ID3D11On12Device` and makes a D3D11 flip swapchain from it (`CreateSwapChainForHwnd` on the
    laptop, `ForCoreWindow` on the console); Present copies/resolves the game's back buffer into its buffer 0
    on the same context (no wrapped resources, no D3D12 queue fence), Drain uses an event query. Wine's
    `d3d11`/`dxgi`/`xrlhost` are unchanged: the layer's ABI is the same in both modes.
  - **Why.** D3D11On12 translates every D3D11 call onto D3D12 on the calling (render) thread; the console has a
    native D3D11 runtime at FL11_0 (the probe), so the translation is pure overhead if native D3D11 works there.
    It also removes D3D11On12's own memory (its D3D12 heaps/allocators) from the 5 GiB budget.
  - **Measured (laptop, Intel Graphics, RL title menu, Epic login failing; `XRL_PROFILE=1 XRL_RL_REUSE_ARGS=1
    XRL_SWAP_RESERVE_MB=700 XRL_REAL_GFX=1 XRL_TEST_BUDGET_MB=1792 WINEDEBUG=err+all`, 130 s and 170 s runs,
    10-s `perf:` windows once frames flow).** The GPU caps both at ~55 fps, so CPU is the number that matters.
    The render thread is the unnamed `xrl-guest/<tid>` whose top syscall is `rt_sigprocmask` (~13k syscalls/s
    in both modes).

    | | default (D3D11On12), 4 runs | `XRL_D3D11=native`, 4 runs |
    |---|---|---|
    | fps (steady) | 54.2-55.1 | 54.0-55.6 |
    | worst gap (steady) | 32.0-45.2 ms | 32.6-39.2 ms |
    | Present, avg | 0.26-0.34 ms (0.42-0.55 first window) | 0.06-0.08 ms (0.09-0.13 first window) |
    | render thread CPU, per window | 23-47 %, median 34 % | 9-37 %, median 20 % (18 % without run 3) |
    | guest COM calls/s | 275-281k (one run 441k/872k on a later screen) | 259-269k |
    | whole guest CPU | 57-113 % of a core | 35-78 % (run 3: 90-212 %) |

    Run 3 of native was a noisy one (every thread, the idle websocket/HTTP ones included, ~2-3x its usual CPU,
    a network thread spinning on `getsockopt` at the end): the laptop was busy. One extra default run hung during
    loading (main thread spinning in `getrusage`/`sched_yield`, one guest thread at 100 % with no syscalls, 5 COM
    calls in total, i.e. before any D3D work) and is not counted; it is not mode-related. No `[E` lines in any
    run, the same two device creations and the same swapchain (1024x768 fmt 28, buffer_count 1 clamped to 2)
    in both modes. RL does not call ResizeBuffers at the menu; resize is covered by the gfx harness
    (`XRL_D3D11=native gfx/build-desktop/Release/xrl_gfx_harness.exe`: BGRA, sRGB, 4xMSAA, RGBA, each presented,
    screen pixel checked, resized to 640x360; the default harness still passes too).
  - **Costs / open.** Not yet run on the console: whether the Xbox's native D3D11 runtime takes RL's shaders and
    formats the way D3D11On12 does is unmeasured. If it fails, remove the `host_env` line (no rebuild). The
    device is created without the game's own creation flags (as in the D3D11On12 path). Default stays
    D3D11On12 until the console run says otherwise.

- **2026-10-26 — An in-process desktop thread stands in for explorer.exe's (`patches/wine/0026-desktop-thread.patch`).**
  Every thread's first user32/win32u display call loads the user driver (`load_desktop_driver()`), which first
  does `send_message( desktop, WM_NULL )` and waits for the desktop window's owner to answer; a mode change sends
  it `WM_DISPLAYCHANGE`. In Wine that owner is explorer's desktop thread. We have no explorer (one process,
  `XRL_NO_SPAWN`): the server created the desktop window with `force`, but its handle entry kept the id of
  whichever program thread asked first, so later senders waited on a game thread. Rocket League froze on the
  Xbox on a fullscreen switch to 2560x1440 and on a windowed 2560x1440 start (render thread waits on WM_NULL,
  game thread waits for the render thread). Laptop repro: a second thread enumerating modes while the main
  thread calls `ChangeDisplaySettingsEx` never finished its first loop.
  - **What it does.** win32u's `get_desktop_window()`, when the desktop has no windows yet and `XRL_NO_SPAWN` is
    set, calls a new user32 callback (`NtUserStartDesktop`) instead of spawning explorer. It starts one thread
    (`wine_xrl_desktop`) that does what `manage_desktop()` does for the null driver: zero display GUID,
    `Video\{0000...}\0000\GraphicsDriver=null` (left alone when the pre-baked prefix has it), creates the desktop
    window with that GUID (so `__wine_display_device_guid` is set as explorer sets it) and the `Message` window,
    sizes the desktop to the virtual screen, then pumps messages for the life of the process. Callers wait for it
    (up to 10 s, like explorer's `WaitForInputIdle`) and cache the windows only once both exist. Callers that
    must not wait (the desktop thread itself before its window exists, a holder of the loader lock, a holder of
    win32u's user lock, anyone after a 10 s timeout) get the current state without caching and ask again later;
    `load_display_driver()` then skips the WM_NULL to a desktop that is not pumping yet (the zero GUID's key
    names the driver anyway). `WM_DISPLAYCHANGE` is handled on that thread by win32u's desktop proc (resizes
    the desktop). If the thread cannot start, the server's desktop window is the fallback as before.
  - **Not done:** wallpaper, shell window, systray, explorer's display-settings restorer (one process: nothing
    to restore for another). **Cost:** one more thread blocked in `GetMessage`; it dies with `ExitProcess` like
    any thread, but a program that ends by `ExitThread` on its last own thread would now be kept alive by it
    (Rocket League exits through `ExitProcess`). Global hooks set by the program run on that thread.
  - **Tested on the laptop** (desktop host, guest `rootfs-console`, `XRL_DISPLAY=2560x1440@120`): the modeset
    repro without arguments completes 80 mode changes with the enum thread running (6610 enum loops) and exits 0;
    with `trace+win` each change shows `desktop_window_proc ... change from ... to ...` on the desktop thread.
    Rocket League windowed 2560x1440: swapchain created, `perf: frames` 11-15 fps. Fullscreen 1920x1080:
    swapchain created, `perf: frames` 29-44 fps for 3 minutes (240 s run); no `handle_syscall_fault`, no musl
    crash. One of three fullscreen runs stalled before its swapchain (main thread ~2M syscalls/s, one unnamed
    thread at 100% user CPU without syscalls, no `desktop thread not ready` error); not
    reproduced in the next run with `XRL_PROFILE=1 XRL_HANG_DUMP=170`, cause unknown. If it recurs, rerun with
    those two variables to see where the threads are.

- **2026-10-26 — EAC launch: what the module asks of the system, how it starts the game, and the process model
  that needs.** Branch `claude/eac-launch`. Observation of the system interface only (decoded syscall trace);
  nothing of the module's internals was read, nothing was added to hide the layer, Wine or the Xbox.
  - **Method.** `XRL_TRACE=2` (`layer/src/systrace.c`) logs every call decoded: paths, the bytes a read returned
    (whole text for /proc, /etc, /sys), sockaddrs, uname, limits, stat results, errno names. It starts at the first
    `memfd_create` (the module mapping); Wine's futex/clock/pipe noise is left out (`XRL_TRACE_NOISE=1` keeps it).
    Traces: `docs/eac-trace/module-before.txt` (before the fixes below), `module-after-fork-probe.txt` (after).
    The `unknown(7ff4...)` lines of the raw trace are not Linux system calls: they are the translator's own
    entry (`XRL_SYS_DBT_ENTER`, dbt.asm) for an indirect jump into module code that has no translation yet; the
    argument is the target address, the result its translation.
  - **Part 1: what the module asked, what a Linux answers, what we answered.** The bootstrapper's published
    behaviour: `error_launch_ex = "Launch Error: {0}"` in `EasyAntiCheat/Localization/en_us.cfg` is the generic
    message for a code that has no text of its own; 256 is in no table the launcher ships. Everything below is
    from the trace of the run that ended in 256.

    | asked | we answered | a Linux (6.x, Debian, Proton) | verdict, action |
    |---|---|---|---|
    | `/proc/<pid>/status` | 28 of ~60 fields (no NStgid, NSpid, VmLck, RssAnon, VmPTE, CapBnd, CapAmb, NoNewPrivs, Seccomp, Seccomp_filters, Speculation_*, Mems_allowed, THP_enabled...), SigIgn/SigCgt always 0, Groups empty | the full 6.1 layout, SigCgt/SigIgn from the real dispositions | **gap, fixed**: `procfs.c` prints the 6.1 layout; Sig* from `rt_sigaction`; test `test_procfs_rlimits` |
    | `/proc/1/comm` (PPid is 1) | ENOENT | process 1 exists, comm readable by anyone | **gap, fixed**: `/proc/1/{comm,cmdline,status,stat,statm,cgroup,limits,oom_score*}` describe a nominal init as WSL1's does; what a non-root user cannot read of pid 1 (exe, cwd, root, environ, maps, mem, fd) is EACCES. It is the layer's init, named `init`, uid 0, one thread |
    | `/proc/self/task/<tid>/status` | the *stat* text (bug) | status | **bug, fixed** (also `statm`) |
    | `getrlimit/prlimit64` after a set | `setrlimit` ignored, `prlimit64` set ignored; `/proc/self/limits` 4 lines | what was set; 16 lines; EINVAL soft>hard, EPERM raising the hard limit | **gap, fixed** (`misc.c`, table of 16 limits, Debian defaults except NOFILE 65536) |
    | `/proc/mounts`, mountinfo, filesystems | `rootfs /` and `proc` only | /sys, /dev too | **fixed**: lists what the layer provides (rootfs, proc, sysfs, devtmpfs) |
    | `/proc/version` vs `uname -v` | different strings | the same | **fixed** (`XRL_UTS_VERSION`); `/proc/sys/kernel/{version,domainname,threads-max,core_pattern,perf_event_paranoid,kptr_restrict,random/uuid}`, `/proc/self/{cgroup,oom_score,oom_score_adj,personality}` added |
    | `/tmp/eac.flag`, `/run/host/*`, `/.dockerenv`, `/.flatpak-info`, `/run/.containerenv` | ENOENT | ENOENT on a normal desktop | right as it is |
    | `/sys/class/dmi/id/*`, `/sys/devices/virtual/dmi/id/*`, `/proc/bus/pci/devices` | ENOENT | present on a PC | **not a gap we can close honestly**: the console denies SMBIOS/ACPI (cat M), and nothing is invented |
    | `/etc/os-release` | `NAME="xbox-rl"`, ID, PRETTY_NAME | the distribution's | what this system is; unchanged |
    | `uname` | Linux 6.1.0-xrl, `#1 SMP xbox-rl layer 0.1.0`, x86_64, `(none)` | Linux, a distribution kernel | right in form, the values say what we are |
    | `userfaultfd` (Wine's probe) | ENOSYS | EPERM/EINVAL or a descriptor | harmless, Wine falls back |
    | `gettid()` of the main thread | the Windows thread id, not `getpid()` | equal | **known difference, kept**: Wine takes both as its own ids and the TEB's ClientId has to match in both views (docs/gs-teb.md); `/proc/<pid>/task/<pid>` is accepted |
    | DNS (UDP 53, resolv.conf), TCP 443, TLS 1.2 to `api.epicgames.dev`, one 882-byte request | worked | same | right. This traffic is the module's report after a failure, not the launch (below) |

  - **What those fixes changed.** Not the number: the module takes a *different path* once `/proc/1/comm` exists.
    Before: survey of the system, a report to Epic, return, `Launcher finished with: 208, 'Launch Error: 256'`.
    After: it reads `/proc/1/{comm,cmdline,status}`, `/proc/mounts`, DMI, PCI, `/proc/self/exe` and then **forks**.
    With `fork` answering ENOSYS the process exits with status 1 (nothing logged); with a child but no ptrace the
    bootstrapper logs `210, 'Unexpected error. (#1)'`. So 256 was what the module said on the path where it
    saw no init process; it is not a Linux wait status of a child (hypothesis 2 of the Debian note: no child existed
    on that path). The Debian log (`debian-anticheatlauncher.log`: same bootstrapper 1.9.3, module 12081735 bytes)
    ends 0.7 s after the module mapping with `301, 'Easy Anti-Cheat successfully loaded in-game'`, "waiting for game
    window to become visible", `Terminating with: 0`.
  - **How the module starts the game (hypothesis 1: confirmed in shape).** Seen by running its fork child as a task
    (below), the system calls are: parent `fork`; child `close` of every descriptor from 3 up,
    `prctl(PR_SET_DUMPABLE, 0)`, `ptrace(PTRACE_TRACEME)`, `kill(getpid(), SIGSTOP)`; parent `wait4` (sees the
    stop), `PTRACE_SETOPTIONS`, then a loop of `PTRACE_SYSCALL` / `wait4(WNOHANG)` / `PTRACE_GETREGSET(NT_PRSTATUS)`:
    **it single-steps every system call of the child** (entry and exit stop). The child meanwhile: `mmap` 6.7 MB,
    `memfd_create(GUID)` (this becomes **fd 3**), writes a second ELF (6 672 928 bytes: the in-game part) into it,
    `fcntl`s it, opens `RocketLeague.exe` and reads its first 5 KB (the PE header), `readlink /proc/self/exe`
    (the Wine loader, `/usr/lib/wine/x86_64-unix/wine64`), then **`execve(wine64, [unix path of RocketLeague.exe,
    "Z:\game\...\RocketLeague.exe", the Epic launch arguments])`** with the environment plus `EAC_LAUNCHERDIR` and
    `GNUTLS_SYSTEM_PRIORITY_FILE`. The parent, at the exec stop (`GETSIGINFO`), reads `/proc/<child>/exe` and
    `/proc/<child>/maps`, `PEEKDATA`s the new image's ELF header and its entry point (twice) and then
    **`POKEDATA`s a `0xCC` (int3) over the first byte of `_start`**: a breakpoint at the new program's entry. The
    rest is the usual injection (run to the breakpoint, set registers so the child calls `dlopen` on
    `/proc/self/fd/3` through its libc, continue); not seen here because the layer cannot do it yet (the run
    was stopped at that point on purpose: the poke would have written into our own image). `301 ... loaded in-game`
    is that done; the bootstrapper then has nothing left to do and exits (`WaitForGameProcessExit` false).
    **So the game is not a Windows child of the bootstrapper, and the module does not stay in the bootstrapper:
    it is a real Linux process (`wine64`) created by exec from a ptrace-supervised fork child, and the in-game
    module is put into it from outside.** Static facts that fit: the bootstrapper imports no pipe, mapping or named-
    object creation (`CreateProcessW`, `DuplicateHandle`, `OpenProcess`, `PeekNamedPipe`, `CreateSemaphore`
    for its single-instance lock only); `EOSSDK-Win64-Shipping.dll` has `CreateFileMappingW`/`MapViewOfFile` and
    no module-mapping code, i.e. the game side attaches to what was injected, not to something the bootstrapper made.
  - **Part 2: process model.** What the module needs (all from the trace): `fork` (a child that runs module code,
    with its own descriptor table), `ptrace` (TRACEME, signal-stop, syscall-stops, exec-stop, GETREGS(ET),
    GETSIGINFO, PEEK/POKE, SETREGS, CONT, DETACH, KILL) between two *different address spaces*, `execve` that
    creates a new process whose `/proc/<pid>/maps` the tracer can read, an inherited memfd, `wait4`/`kill`.
    Options, against the console's constraints (one app process owns the window and graphics; a UWP app can create
    child processes of itself, probe cat L; the probe did not measure whether a child outlives its parent or can
    open another's memory):
    1. **A second layer OS process per exec'd program (chosen direction).** `execve` in a forked child starts the
       host executable again with the program, arguments and environment (`XRL_ENV_*`, as the test host already
       takes them) as a new guest process with its own address space, fd table and pid; the pre-exec child is a
       task of the parent (below). Needs, in this order: spawn + wait4 + kill (built), a control channel from
       the tracer to the tracee process (PEEK/POKE/GETREGS/SETREGS/CONT and the stop notifications over a named
       pipe; on Windows `ReadProcessMemory` for memory is also possible because guest addresses are host
       addresses), the tracee stopping at its first instruction and at int3 (signal.c: breakpoint exception
       reported to the tracer instead of the guest), `/proc/<pid>/{exe,maps,status}` of another layer
       process, inheritance of the memfd by path. **No shared wineserver is needed**: the game is a fresh `wine64`
       (the module launches it by exec, not through `CreateProcess`), so it gets its own in-process wineserver
       thread; prefix files are shared, the bootstrapper's wineserver ends with the bootstrapper. (Real Wine
       would connect it to the prefix's one wineserver; here they are two, which only matters for registry
       writes made by both.) Console: the game process needs the CoreWindow, so on the console **the game must be
       the app's main process**; the bootstrapper+module would be the child. That means the app starts the
       launcher part first as a child and the exec'd game runs in the main process (the spawn of step 1 reversed
       in who owns the window). This needs a console probe before building (can a package process read another's
       memory, does a child survive its parent, can the main process receive an exec request from its child).
    2. **Two Windows processes in one OS process:** rejected. Wine's address-space layout (low 4 GiB reserved,
       fixed system DLL bases, one musl instance's globals) is per process; the module also pokes the tracee's
       entry point at the *same virtual address* the tracer's own `wine64` has, which cannot be two different
       memories in one address space.
    3. **Exec handover with the old image kept** (what the wineserver thread uses): same problem as 2 for the
       tracee, and the old image must end before the game is meant to be alone.
    4. **A real copy-on-write `fork`:** not needed by anything observed (the only fork child runs a few hundred
       calls and execs); the cost is a host process with a copy of the guest memory *and every layer table*
       (mem.c VMAs, fd table, vfs, signal, futex, ntsync state), weeks of work. Not planned; fork children are
       tasks.
  - **What is built (first step, `XRL_FORK_TASKS=1`, off by default; `layer/src/task.c`, `fd.c`, `thread.c`).**
    - `fork`/`vfork`/`clone` without `CLONE_VM` start a **task**: another thread, its own fd table (a copy that
      shares the open files), pid (its thread id), ppid, exit status, and **the parent's memory and stack
      addresses**. Exactly one of parent and child runs at a time: the parent runs again when the child exits,
      stops (ptrace) or execs, the child runs when the parent waits for it (`wait4`), and the stack's contents are
      swapped at every hand-over (up to 1 MiB above the stack pointer). Memory outside the stack is shared.
    - `wait4` (children exited, ptrace-stopped, or exec'd), `kill(child, SIGKILL)`, `getpid/getppid` of a task, a
      task's `kill/tgkill` of itself, signal death as a wait status.
    - `ptrace`: TRACEME, SETOPTIONS (TRACESYSGOOD, TRACEEXEC), CONT, SYSCALL (entry and exit stops with the
      syscall number, arguments and return value), SINGLESTEP (as CONT), DETACH, KILL, GETREGS and GETREGSET
      (the registers the layer knows at the stop: callee-saved ones, arguments, rip after the call, rsp, fs base),
      GETSIGINFO, GETEVENTMSG, PEEKTEXT/DATA, exec-stop. Not: SETREGS, POKE*, ATTACH/SEIZE, FP registers, a tracer
      other than the parent, a child that runs while its parent runs.
    - `execve` in a task: a program that exists and is an x86-64 ELF **starts as a second host process** (the same
      executable and root, arguments, environment, stdin/stdout/stderr); `wait4` gives its exit code, `kill`
      terminates it. Descriptors other than 0-2 are not inherited. A *traced* task's `execve` is only logged
      (path, arguments with values shown only for a short allow-list, environment names) and ends the task in the
      exec stop, because the stop's tracer would now poke an image that does not exist as a separate process.
    - **A hard cap** (`XRL_MAX_CHILDREN`, default 4, at most 16, group-wide through a named semaphore; nesting
      `XRL_MAX_SPAWN_DEPTH` default 2 through `XRL_SPAWN_DEPTH`), refused with `EAGAIN`; `XRL_SPAWN_KILL_ON_EXIT=1`
      ends the whole group with its first process (job object). Why: while this was being written a test program
      that spawned itself with no limit started 85 host processes and exhausted the laptop's commit. Cause: the
      guest test's spawn step ran in the *child* too (the child's argv was not recognised yet) and each level
      spawned the next; nothing counted processes and orphans outlive their parent on Windows. Now: the cap, the
      depth, the job, `layer/tests/fork-tasks.sh` (timeout, census of `xrl_test_host.exe` before and after, kills
      and fails on leftovers, skips under 3 GB free).
    - Test: `test_fork_tasks` and `test_spawn` in `layer/tests/guest/guest.c` (skip unless `XRL_FORK_TASKS=1`),
      run by `layer/tests/fork-tasks.sh`.
    - What it does **not** do: a real fork (memory is shared; a child that writes outside its stack changes the
      parent), concurrency between parent and child, ptrace of a spawned process, signals other than SIGKILL
      between processes, pipes/sockets/memfds across the process boundary.
  - **Where it stands.** Part 1: every gap the trace showed is fixed or explained; the module now runs its
    Linux path and stops where it needs ptrace on a process of its own (it cannot be answered honestly without
    it: faking a successful trace of the child would be exactly what must not be done). Part 2: decided (option
    1), first step built and tested on the laptop, next step is the tracer/tracee channel across two layer
    processes (design above) and a console probe. Not run on the console. EAC with the game still ends before
    the game starts on this branch.
  - **Update, end of the session (state of the traced-exec work).** Built after the entry above, all behind `XRL_FORK_TASKS=1`
    and covered by `layer/tests/fork-tasks.sh`: a *traced* `execve` starts the program as a second layer process whose tracer
    controls it over a named pipe (`layer/src/tracee.c`): exec stop, PEEK/POKE, GETREGS/SETREGS, FP registers, int3 and
    fault stops, CONT/DETACH, `/proc/<pid>/maps` and `exe` of that process; descriptors that are memfds are inherited at the
    same number; the glibc shim is preloaded in the new process; musl patch 0003 leaves `.dynamic`'s hash/symbol/string
    entries absolute as glibc's ld.so does (the module reads them before any constructor can run). With these the module's
    launch protocol runs on the layer up to the point where it hands control to its in-game part inside the new process; the
    bootstrapper then reports `210, 'Unexpected error. (#2) (0)'` because that process ends with a Windows status
    (0xc000000d, raised by ntdll's exception continue path) as soon as it is resumed into the in-game code. That
    crash is not understood yet (it is in the layer's resume path, not in a system call). I stopped there on purpose: it is
    the point where the work stops being about the system interface the module asks for, so Part 1/2 end here. Exactly
    what is not done: that crash, `ptrace` single-step and syscall stops across processes, signals between processes,
    pipes/sockets across processes, a console probe, any run on the console.
  - **The 0xc000000d crash: a layer bug, fixed (commit 3f46d9f).** The CONTEXT logged at each resume (run25: flags
    0x10005f incl. CONTEXT_XSTATE, cs 33, ss 2b, eflags 202, MxCsr 1f80/1f80, committed rip/rsp pages) was the same for
    the two resumes that worked and the one that failed. What differed was the memory below the stopped rsp: the
    layer served the stop inside its VEH, on the stopped thread's own stack, where Windows had just put the
    exception's CONTEXT, CONTEXT_EX, XSAVE area and the dispatcher's frames (0x700+ bytes below the faulting rsp).
    The third injected call's argument block (0x328 bytes POKEd right below rsp: legal on Linux, where a stopped
    tracee keeps its registers in the kernel) overwrote the CONTEXT_EX/XSAVE header NtContinue then loaded, and it
    refused it with STATUS_INVALID_PARAMETER. Reproduced in `test_inject_call` (a 0x328-byte block below rsp: the
    tracee ended with exit code 0xc000000d, like the EAC run). Fix: `xrl_tracee_stop` copies the CONTEXT (with XSTATE)
    out, leaves the exception dispatch at once into `tr_park_main` on the thread's host stack, serves the tracer
    there and resumes from the copy with NtContinue; nothing of the layer stays below the stopped rsp (also checked
    with a 16 KiB block). Also as Linux: SETREGS keeps only FLAG_MASK bits of eflags (CF PF AF ZF SF TF DF OF NT RF AC),
    refuses bad selectors and fs/gs_base >= TASK_SIZE_MAX with -EIO, field by field; SETFPREGS refuses an MXCSR outside
    the CPU's mask with -EINVAL; CONTEXT.MxCsr follows FltSave.MxCsr. Not checked: whether Windows' exception
    record also lands in the 128-byte red zone right below a faulting rsp (Linux leaves it alone at a stop).
    The WSL2 side-by-side run of the same request sequence was not done in this round.
    EAC after the fix (run26): the third call runs and comes back as a SIGSEGV stop at the fake return address
    (no 0xc000000d); about a thousand more stop/resume rounds follow, the tracee makes ordinary system calls
    (mmap, munmap, mprotect, memfd_create/ftruncate/close, a read of /proc/self/maps), then the tracer kills it,
    and the bootstrapper still logs `210, 'Unexpected error. (#2) (0)'`. Not followed further: the next question
    is what the module decides from those answers, which is outside what this work looks at. A next layer-side
    step would be to log the mmap flags in systrace and compare those answers (EEXIST for address hints) with Linux.
  - **mmap answers in the traced game process (commits 4093584, 96fdf0e).** systrace now logs mmap with every
    argument and flag by name, and for an EEXIST what the guest's maps and the host hold over the range. run27:
    175232 `MAP_FIXED_NOREPLACE` requests of one page answered EEXIST. 90841 were over the process's own
    `libc.so`/`wine64` mappings: the answer Linux gives too. 84391 were just above the main program's image, where
    the guest's maps show nothing: the layer started top-down mmaps at the very end of its arena, so `wine64` ended
    exactly there and Windows had a mapped view and a private reservation right above. Linux keeps mmap_base at least
    128 MiB below the stack, so that space is free and the request gets its address. Fix: top-down mmaps (mmap,
    mremap) start 128 MiB below the arena end (at most arena/64); the gap stays inside the arena. Test
    `test_mmap_above_top` (failed before). run28: 1 such request succeeds, 83 EEXIST, all over the process's own
    mappings, none over free space. The bootstrapper still logs `210, 'Unexpected error. (#2) (0)'`; the tracee
    later stops about 1000 times with SIGSEGV at one address until its tracer kills it. Not followed further here.
    Also seen, not changed: a non-fixed mmap with a free hint address inside the arena gets another address
    (Linux gives the hint).

- **2026-10-26 — Wine module mapping: the real EAC module is mapped, initialised, and talks to Epic's
  backend; the bootstrapper then reports `Launch Error: 256` (where it stops).** Branch
  `claude/eac-module-map`. Findings, then what was built, then where it stops.
  - **What sets the global at 0x14038b340 (the earlier note said nothing did: wrong).** The launcher's start-up
    code (0x14004a7a0), only if `wine_get_version` exists in ntdll (0x14001c1b0), does
    `VirtualAlloc(0x7001_0000_0000 + n * 4 GiB, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)` for
    n < 10, keeps the first result that is >= 0x7000_0000_0000 and `memcpy`s the 21-byte `syscall3` stub
    (0x14004b9f0: `mov rax,rcx; mov rdi,rdx; push rdx; mov rsi,r8; mov rdx,r9; syscall; ...; ret`) into it; the
    global is that page (failure: "Failed to initialize Wine helper buffer"). The stub is therefore never run from
    the image: it runs from that RWX page. **Why that address: Proton-GE's Wine has the same convention.** Its
    SIGSYS handler (`dlls/ntdll/unix/signal_x86_64.c`, `hack_native_syscalls`, enabled by
    `WINE_HACK_NATIVE_SYSCALLS=1`) re-arms Linux syscall user dispatch for the range
    [0x7001_0000_0000, 0x7fff_ffff_0000) so that a `syscall` issued from there reaches the Linux kernel instead of
    being a Windows system call. Our layer's equivalent has to be "a `syscall` in Windows code is a Linux system
    call", wherever the code is. What the bootstrapper checks of Wine: `wine_get_version` (also logged:
    "Wine version: 11.0"), `wine_get_unix_file_name` (its callback for the module), nothing else found.
    The mapping routine (0x14001cc10): `memfd_create(name = a GUID, MFD_ALLOW_SEALING)`, `write` of the whole
    module, `getpid`, `/proc/<pid>/maps` (parsed: lines with six fields; the first whose path contains `/libc-` or
    `libc.so`, case-insensitively, is libc, its start address the ELF header), then its **own symbol walk** of that
    image (0x14001c470: PT_DYNAMIC, DT_HASH/DT_GNU_HASH + DT_SYMTAB + DT_STRTAB taken as absolute pointers, djb2
    hash for both tables) for `dlopen` (else `__libc_dlopen_mode`) and `dlsym` (else `__libc_dlsym`), then
    `dlopen("/proc/self/fd/N", RTLD_LAZY)` through a Windows-ABI -> SysV-ABI trampoline (0x14004ba06: `call rax`
    with rdi/rsi/rdx from rcx/r8/r9 shifted), and `dlsym` of `a`..`d` (the module also exports `e`).
  - **Raw syscalls from Windows code (`layer/src/dbt.c`, `syscall.c`).** The first version only translated PE
    pages that hold a `syscall`; the real one never executes that page. Now, while the program is one the
    `XRL_DBT` policy selects (default: `_eac` / `easyanticheat` in the program path), an anonymous mapping that
    is writable and executable (mmap or mprotect) is *watched*: made non-executable for the host (guest still sees
    `rwx`), its first instruction fetch faults, the page is scanned; with a `syscall` it is translated like an
    image, without it simply becomes executable again. A later write to a translated page drops the
    translation (slots are reusable, 16 instead of 4; `dbt.asm` scans 16 ranges) and the page is watched again.
    Limits: at most 16 watched ranges of at most 256 KiB; code written after the first fetch into a page that had
    no `syscall` is not looked at; a thread running a translation while another rewrites it is not stopped.
    Test `layer/tests/rawsys-win.sh` (PE program issuing the bootstrapper's stub from its image and from a
    VirtualAlloc page at 0x7001_0000_0000: `uname` says Linux/x86_64, `memfd_create`/`write`/`lseek`/`read`,
    `/proc/self/fd/N`, a second thread's `gettid`, `-ENOSYS`, a page rewritten 8 times; with `XRL_DBT=0` it fails
    because the Windows kernel answers).
  - **libc discovery: decision.** Two things are needed, and neither is a disguise. (1) *The name.* musl installed
    the way a musl system does: the library is `/usr/lib/libc.so`, `/lib/ld-musl-x86_64.so.1` a symlink to it
    (musl's own `make install` layout; Void Linux's musl too). `/proc/self/maps` then shows `/usr/lib/libc.so`
    because it names the resolved file, as Linux does; `build-wine.sh` installs it so, `stage-rootfs.sh` stages the
    symlink. No glibc names are invented. (2) *The dynamic section.* glibc's ld.so adds the load address to
    DT_HASH/DT_GNU_HASH/DT_SYMTAB/DT_STRTAB/DT_VERSYM in memory; musl leaves link-time offsets and the bootstrapper
    dereferences them (on a stock musl system it would fault). The glibc-ABI shim (`libgcompat.so.0`) now does what
    glibc does to those five entries of libc's own `.dynamic`, once, in a constructor (musl reads them only while
    loading, checked in dynlink.c v1.2.6; the page is made writable and put back). Wine patch 0024 loads the shim at
    start-up (it used to load only before a native Wine library) because the module `dlopen`ed by Windows code needs
    it resolved globally. The loader's *identity* is unchanged: it is musl, `gnu_get_libc_version` was already the
    shim's.
  - **What the module itself needed from the guest**, found by running it: `libgcc_s.so.1` (a musl-linked build:
    `wine/scripts/build-libgcc-s.sh` links the toolchain's static libgcc_eh.a + libgcc.a, hidden symbols made
    visible by `tools/xrl-elf-unhide.py`; the build machine's glibc libgcc_s cannot be used, musl never matches its
    own `@GCC_4.8.0`-versioned symbols; `libgcc_s_extra.c` supplies `_dl_find_object`, because our musl's stub
    returns "not found" and GCC 15's unwinder then aborts in `uw_init_context_1`), the `getcontext` family
    (musl has none: written in the shim on glibc's algorithm, layout asserted), `__wmemmove_chk`, and
    `ld-linux-x86-64.so.2` (empty stub, already there). `/etc/os-release` (the module reads it) is staged:
    `NAME="xbox-rl"`, what this system is.
  - **memfd.** Named as Linux shows them (`/memfd:NAME (deleted)`), reopenable through `/proc/self/fd/N`. Code
    mapped from a memfd in a program the policy selects is translated, not rewritten: the module is the
    vendor's own code and stays byte-identical (the stand-in module test, extended: a glibc-built `.so` loaded
    from a memfd through the bootstrapper's own algorithm; its code in memory equals the file, its constructor,
    raw `syscall`, a shim symbol, `swapcontext` coroutines and `_Unwind_Backtrace` work).
  - **Run (`tools/run-rl.sh` with `XRL_RL_EXE=RocketLeague_EAC.exe`, laptop).** Download OK; "Starting Wine module
    mapping" is no longer followed by "Failed to map": the 12 MB module (ELF shared object: PT_TLS, needs
    libgcc_s, libc.so.6, ld-linux; exports a..e) is `dlopen`ed (two executable mappings, 2.0 and 2.2 MB, translated
    without a single unsupported instruction or fault), the bootstrapper calls its `a` with a settings structure
    and a callback, and the module, in about 0.3 s: calls back for the Unix path of `RocketLeague.exe`, reads
    `/proc/<pid>/status`, probes `/proc/1/comm`, `/tmp/eac.flag`, os-release files and a few container marker
    files (all absent), reads `/etc/hosts` and `/etc/resolv.conf`, resolves a name over UDP, connects by TCP,
    does a TLS 1.2 handshake with its own TLS code, sends one 882-byte request, reads a 673-byte reply, closes,
    and returns. The bootstrapper logs `Launcher finished with: 208, 'Launch Error: 256'` and shows its error box
    (the headless run waits on it until the timeout). So **EAC initialises and reaches its service, and then
    reports error 256 (a code from the module, not one of the bootstrapper's own messages).** What it means is
    not known and was not pursued by reverse-engineering the module: it is the module's answer after the
    server's reply, from inside Epic's opaque code. Psyonix say (2026) that Proton/Linux keeps working with EAC
    on, so the server does accept genuine Linux clients; whether it refuses this one because of something the
    layer gets wrong (a `/proc` or `uname` value, a missing file) or because it recognises the environment, this
    run cannot say. Changing `/etc/os-release` did not change the code. Nothing was added to hide the layer, Wine or
    the Xbox, and nothing will be: if the refusal is EAC's own, it stays.
  - **Open, whatever 256 turns out to be.** The module lives in the *bootstrapper's* process and the game is meant
    to be a child that talks to it (the bootstrapper imports pipe/handle-duplication calls); the exec handover
    (previous entries) replaces the bootstrapper, module included, by the game. Even with a successful `a`, the
    game would then run without the module in its process. Needs a decision (a second layer process? keeping the
    old image alive next to the new one, as the server thread's does?) before RL can run with EAC enabled.

- **2026-10-26 — Rocket League reaches its online code on the laptop**
  (1.75 GiB budget, swap, Epic launch arguments from `tools/run-rl.sh`):
  PreInit, global shaders, script packages (the 6.4 GiB shader-cache peak, 224 s
  under swap against 24 s native), GUID cache, client init (hardware survey:
  "Wine Adapter", "Wine Audio Device"), **viewport created**, PsyNet starts and
  sends its first HTTPS request (config.psynet.gg). What it took, beyond swap:
  - **Fault ownership** (`signal.c`): the VEH called a fault "host" when rsp was
    inside the TEB stack range; in Wine's view that range is Wine's stack, so every
    CPU fault in Wine code went to Windows SEH, unhandled, and hung in the host's
    unhandled-exception path (RL's main thread; a worker spun in NtYieldExecution,
    308 M getrusage calls). This was the long-noted "fault on a Wine thread never
    reaches the VEH". Now: guest view live + rip/rsp in executable/writable guest
    memory = guest fault, delivered to Wine's handler.
  - Guest tree: CA bundle, resolv.conf, hosts (DNS resolves Epic's servers).
  - Open: TLS (Wine has no GnuTLS: bcrypt asymmetric keys and schannel fail; the
    EAC branch adds GnuTLS), the laptop's own limits (page file growth ate the disk
    while agents ran: 11.5 GB system-managed page file), no first frame yet.

- **2026-10-26 — Swap for lazy memory (`layer/src/mem.c`).** Rocket League
  writes 6.4 GiB while loading (natively too); the console's commit wall is
  ~4.5 GiB, the laptop's free commit ~2 GB. Cold 64 KiB spans of lazy
  anonymous memory go to a swap file in TEMP and are decommitted; a touch, or
  a layer path handing memory to the kernel (`xrl_mem_populate`), brings them
  back. Choices, each from a measurement:
  - **Clock with probing** for coldness (PAGE_NOACCESS keeps the data; a touch
    faults and marks it hot). No accessed bits on Windows; QueryWorkingSetEx /
    write watch were rejected as uncertain on the console.
  - **Pinned**: stacks and MADV_POPULATE memory (the kernel writes exception
    frames there) are never probed or evicted.
  - **Clean spans**: a span read back on a read fault keeps its record and stays
    read-only; evicting it again needs no write (shader data is read-mostly).
  - **Batching**: every protection change/decommit flushes the TLB on all cores;
    per-span calls made an eviction 1.3 ms against a 0.3 ms write. Address-sorted
    runs cut it to ~0.25 ms. Read-ahead pulls up to 7 following spans stored in
    consecutive slots in one read.
  - **Compression + sparse file**: each slot holds the span's XPRESS-compressed
    image (Windows Compression API, available to app/games partitions); the file
    is sparse and freed slots are punched out, so disk use is the compressed size.
  - **Disk guard**: the file grows only while `XRL_SWAP_RESERVE_MB` (1 GiB) stays
    free; past it eviction fails like a full swap partition.
  - Bug found by the guest suite: a span queued twice in one clock pass lost its
    record (its data came back as zeros); queued spans have their own state.
  Laptop: Rocket League ran under a 1.5 GiB budget (thrashing at that size,
  before batching/compression). Not yet run on the console (sparse files and the
  Compression API there are unverified; both fall back cleanly).
- **2026-10-26 — TLS for Wine (gnutls on musl), and what the real EAC bootstrapper
  does next: it needs raw Linux system calls from Windows code.** The bootstrapper's
  libcurl has only the schannel backend, so it needs Wine's schannel, which needs
  gnutls (dlopen'ed by secur32/bcrypt, only if Wine is configured `--with-gnutls`).
  - `wine/scripts/build-gnutls.sh` builds nettle 3.10.2 (mini-gmp, no assembler) and
    gnutls 3.8.12 (included libtasn1/unistring, no p11-kit, no zlib, no non-Suite-B curves)
    against our musl into `$WORK/deps` (pins and sha256 recorded when first fetched,
    2026-10-26); `build-wine.sh` configures `--with-gnutls` when `deps/` exists and
    installs `libgnutls.so.30`, `libnettle.so.8`, `libhogweed.so.6` into the rootfs
    (the thread-pointer check covers them). Costs: the whole unix side rebuilds once,
    about 6 MB more in the rootfs, Wine logs "Compiled without DH support".
  - Verified on the layer without Wine (`layer/tests/tls-test.sh`): gnutls does a TLS 1.3
    handshake with certificate verification against the CA bundle and an HTTPS request
    to `modules-cdn.eac-prod.on.epicgames.com` (DNS, Winsock sockets, handshake).
  - Verified through Wine: the bootstrapper's download now succeeds (curl: "No error (0)
    Response Code: 200", a 12 MB blob; it keeps it in memory, there is no file). The blob
    is not an ELF (no header; the bootstrapper unpacks and loads it itself), so nothing
    was read from it or run.
  - **Side effect, stated:** with TLS working the bootstrapper no longer takes its
    "CDN unreachable, launching with null client" fallback, so `RocketLeague_EAC.exe`
    now stops at "Failed to load the anti-cheat module" instead of starting the game.
    Epic's own switch `EOS_USE_ANTICHEATCLIENTNULL=1` (the bootstrapper's debug
    variable; `XRL_ENV_EOS_USE_ANTICHEATCLIENTNULL=1` through the test host) gives the
    old behaviour, which is what the exec-handover check on the real bootstrapper needs.
    Running `RocketLeague.exe` directly (tools/run-rl.sh) is not affected.
  - **Where it stops (the next job).** After the download the log says "Starting Wine
    module mapping, Wine version: 11.0." and, in the same millisecond, "Failed to map the
    anti-cheat module." Reading the bootstrapper (static disassembly, strings): the
    mapping routine (function at 0x140034a30 -> 0x14001cc10) checks a global (0x14038b340)
    that, when set, makes it call `syscall3(nr, ...)` wrappers that are **raw `syscall`
    instructions in the Windows executable** (wrappers at 0x14004b9f0 and neighbours:
    `mov rax,rcx; mov rdi,rdx; mov rsi,r8; mov rdx,r9; syscall`), with Linux numbers:
    `memfd_create` (319, flags MFD_ALLOW_SEALING) and `write` (1) to put the unpacked
    module into an anonymous file, then (strings) `/proc/%i/maps` to find libc ("libc.so",
    "/libc-"), `dlopen`/`dlsym`/`__libc_dlopen_mode` in it, `/proc/self/fd/%i` and
    `/tmp/eac_wine_pid_%X`. On Linux a PE `syscall` really is a Linux system call; here the
    Windows kernel would execute it, and no `memfd_create` was ever issued in our run: the
    global is null, and nothing in the executable stores to it directly (it is set through
    something else; not found). Two consequences to plan for: **(1)** raw Linux `syscall`s in
    *Windows* code must reach the layer (what `dbt.c` does for ELF code can do the same for
    the PE pages that hold a `syscall`; a first version is in: when the program being run
    matches the `XRL_DBT` policy (default `easyanticheat,_eac`), a range that becomes
    executable inside a Windows image gets its `syscall` pages translated: for the
    bootstrapper that is the page of the wrappers and one page of data that decodes as a
    `syscall`; no call through them was observed yet); **(2)** its libc discovery looks for
    a mapping named like glibc's libc, which our musl mapping (`/lib/ld-musl-x86_64.so.1`)
    is not: the module it loads is glibc-built (our glibc-ABI shim answers its symbol
    needs), so this is an ABI adaptation to settle with care, not by renaming things to
    look like glibc. Not done, and not guessed.
  - The PE translation has limits beyond the ELF ones: code in translated pages has no
    Windows unwind data (an exception inside it cannot be unwound; only the pages with a
    `syscall` are translated), and the cache for a Windows image has to sit within 2 GiB
    of it (it is searched for in 4 MiB steps from the image, in the address space Wine
    manages with fixed addresses).

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
  8x the image, 1-64 MiB, committed up front. **Default: an image whose path contains
  `easyanticheat` is translated, every other foreign image is still rewritten**;
  `XRL_DBT=0` rewrites everything, `XRL_DBT=1` translates every image the rewriter would
  patch, `XRL_DBT=a,b` only paths containing `a` or `b`; `XRL_DBT_DUMP=prefix` writes the
  cache and the original->cache map for `objdump -D -b binary -mi386:x86-64`.
  Not verified on the real client: the module is a 12 MB opaque blob that the bootstrapper
  downloads (`modules-cdn.eac-prod.on.epicgames.com/modules/<product>/<deployment>/linux64`,
  no ELF header: the bootstrapper unpacks and loads it itself), and the bootstrapper's TLS
  needs gnutls in Wine (see the exec handover entry), so no run could reach it. The
  translator was exercised on Valve's real bridge `.so`, the stand-in, a static glibc
  program (about 650 KB of code translated) and a C++ program.

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

  Verified on the real bootstrapper (laptop harness, `XRL_TEST_BUDGET_MB=1536`):
  `RocketLeague_EAC.exe` reads its settings, fails the module download in TLS (before gnutls
  was added; now use `EOS_USE_ANTICHEATCLIENTNULL=1`, see the TLS entry), takes its
  own null-client path, and its `CreateProcess` of `RocketLeague.exe` becomes the handover
  ("this process hands the OS process to ...Win64\RocketLeague.exe", 92 -> 30 MiB
  committed after the old image is released); the game then starts as the new process and
  gets as far as launching it directly does (EOS init, D3D11On12 device creation; where
  both then fail the same way on this tree: the graphics path is the other agent's).

  Costs, stated plainly: a program that `CreateProcess`es and then keeps working or
  waits for the child (a shell, an installer, `cmd /c`, any launcher that does
  something after the child starts) loses everything after the call; the child's
  parent id names a process that is gone; the old unix side is never freed (about
  25 MiB per handover); `CREATE_SUSPENDED` is ignored (the child runs). For the EAC
  bootstrapper this is exactly the shape of the null-client path. For the real path it
  is an open question whether the module the bootstrapper loads into its own process
  has to stay next to the game; that cannot be decided before the module can be
  downloaded.
- **2026-10-26 — Sound: `winexrl.drv`, an mmdevapi driver over `xrl_audio_ops`.** Wine logged
  `No driver from L"pulse,alsa,oss,coreaudio" could be initialized`, so every WASAPI client failed in
  `GetDefaultAudioEndpoint`. Design choice: a **new Wine mmdevapi driver** (Wine patch 0023,
  `dlls/winexrl.drv`, a unix library like winealsa.drv with an empty PE stub) instead of an XAudio2
  forwarder. Wine's own `xaudio2_7/8/9` are FAudio on top of WASAPI, DirectSound and winmm waveOut are
  too, and Wwise's WASAPI sink talks to mmdevapi directly: one driver under mmdevapi serves all of them
  with no XAudio2 COM-object forwarding and no callback thunking. mmdevapi's default driver list now
  starts with `xrl`.
  - **Device and formats.** One render endpoint, "Xbox Audio" (no capture endpoint), mix format
    float32 / 2 ch / 48 kHz. IEEE float 32 and PCM 16 (1-8 channels, 1-200 kHz) pass straight to the
    host's source voices, PCM 8/24/32 are converted to float on the way (XAudio2 does the rate
    conversion); anything else (more than 8 channels...) gets S_FALSE from `IsFormatSupported`, and
    mmdevapi then offers the mix format, as on Windows. Shared mode only. Volumes (`set_volumes`) are applied in software when handing data to the host.
  - **Streaming.** App -> `GetBuffer`/`ReleaseBuffer` -> a ring -> the timer thread (a PE thread inside a
    unix call, as in every Wine driver) -> `voice_submit_buffer`, one XAudio2 source voice per WASAPI
    stream on one mastering voice (2 ch, 48 kHz) made on first use. **Pacing comes from the host:** each
    period (10 ms) the thread asks `voice_get_state` (ABI v2, `SamplesPlayed`) how much the voice has
    played, keeps four periods queued at the host and signals the app's event, so padding and
    `GetPosition` follow the host's audio device and the app needs no clock of its own (a host without
    `voice_get_state` is paced by the wall clock). The queued frames are *hidden* from the app as in
    winealsa (padding = written - played - hidden, ring = buffer + hidden, `GetStreamLatency` = hidden
    = 40 ms): without it the app can never have more than its 30 ms buffer outstanding and the host
    queue ran dry between two ticks of the coarse Windows timer (15.6 ms; the layer's sleeps do not
    get finer), which starved the stream at 90% of real time (measured; fixed, ratio now 1.00).
    `Reset` makes a new voice because the host ABI cannot flush.
  - **Host side.** `host/src/audio_ops.cpp` gained `voice_get_state` (the slot existed in the ABI,
    the console host never filled it), COM initialisation on whichever guest thread creates voices
    (`CreateMasteringVoice` failed with CO_E_NOTINITIALIZED from a Wine thread), and per-voice
    counters: one log line `voice X: N buffers, M frames so far; last K buffers peak P` about every
    second of audio, so a log proves buffers arrive and how loud they are. The desktop test host now
    compiles that same file (`layer/tests/desktop_audio.cpp` + a stand-in `pch.h`) and plays through
    the laptop's XAudio2; `XRL_NO_AUDIO=1` keeps the old stubs.
  - **Test.** `wine/tests/pe/xrl_audio_test.c` (WASAPI shared: event-driven float, polling, event-driven
    PCM16, PCM24 and PCM8, a refused 12-channel format, stop/reset/start, and Wine's XAudio2 on top) all pass under Wine on
    the layer: 2 s of audio take 1.96-1.98 s of wall clock (ratio 1.00), 480-frame buffers arrive at
    the host at 100/s with peak 0.100 (the tone's amplitude); `results/desktop-wine-audio-1.txt`.
    Not listened to by a person; not run on the console.
  - **Prefix.** The baked prefix predates the module, and Wine does not find a builtin that has no
    fake DLL in `system32` (status c0000135); `stage-rootfs.sh` copies `winexrl.drv` there (the baked
    fakes are byte-identical copies of the build's files). Re-baking from a build that has the module
    makes this unnecessary.
  - **Build flags gotcha.** After editing `configure.ac` for a new directory, `config.status --recheck`
    loses the cross flags (they come from the environment: `x86_64_CFLAGS` became `-g -O2`, the
    rebuilt PE DLLs grew by 17 MB). Re-run it as `CROSSCFLAGS=-O1 ./config.status --recheck` and
    delete the objects built in between.

- **2026-10-26 — First frames on screen: a D3D11 program presents through the whole stack.**
  `wine/tests/pe/xrl_present_test.c` (Windows x64, mingw) creates a window, a D3D11 device and
  swapchains by every route a game uses (`IDXGIFactory::CreateSwapChain`,
  `IDXGIFactory2::CreateSwapChainForHwnd`, `D3D11CreateDeviceAndSwapChain`; discard, flip-discard,
  flip-sequential; R8G8B8A8 and B8G8R8A8; 1-3 buffers), clears to changing colours, presents, resizes
  once, and reads a pixel back each frame. Under Wine on the layer with `XRL_REAL_GFX=1` all of them
  work, and `tools/test-present.sh` screenshots the host window for each and finds the colour the
  program last presented (51,153,255) in the centre (`results/present-N.png`,
  `results/desktop-wine-present-1.txt`); with `PATTERN=1` the program holds four quadrants
  (red, green / blue, yellow) and the screenshot has them in the right places and channel order for
  both RGBA and BGRA (`results/present-pattern-N.png`). What was missing and is now there:
  - **A D3D11 swapchain object (`gfx/src/d3d11_swapchain.cpp`, ABI v2 append
    `xrl_gfx_ops.d3d11_create_swapchain` + `xrl_swapchain_desc`).** Wine's dxgi used to return the host's
    raw `IDXGISwapChain3`, whose `GetBuffer(IID_ID3D11Texture2D)` fails (`E_NOINTERFACE`), so no D3D11
    program could even start rendering. The new object is an `IDXGISwapChain1` implemented in gfx/
    (native vtable, handed to the guest like every other host COM object): `GetBuffer(0)` returns ONE
    stable D3D11 texture on the game's own device; `Present` copies (or resolves, for MSAA) it into the
    current buffer of the real flip-model swapchain through `ID3D11On12Device` wrapped resources
    (acquire, copy, release to PRESENT, flush) and presents that. Chosen over exposing the rotating
    buffers: D3D11 games take their render-target view once and keep it, flip model included, and a
    copy per frame also makes discard/sequential semantics, sRGB views and MSAA trivially right. Cost:
    one full-frame copy per Present (bandwidth only; to be measured on the console). Implemented in
    gfx/ rather than as a Wine-side wrapper: real headers, no hand-written COM vtables in PE C, and the
    layer already routes calls into host objects. Not IDXGISwapChain2+ on purpose (the
    frame-latency waitable object is a native handle the guest cannot wait on), buffer index > 0 is
    refused (D3D11 only exposes buffer 0), fullscreen/ResizeTarget are accepted and ignored (the host
    owns the window), `GetContainingOutput` fails (Wine's dxgi owns the outputs the game enumerated).
    The D3D12 path (game passes its own queue) still gets the raw host swapchain.
  - **Wine 0021.** `xrlhost` call `XRL_CALL_GFX_D3D11_CREATE_SWAPCHAIN`; dxgi's native-swapchain helper
    picks it for devices that `d3d11.dll` created, so does `D3D11CreateDeviceAndSwapChain`; the
    device-to-queue lookup now compares the `ID3D11Device` identity (a game may pass `IDXGIDevice`) and
    replaces a stale record when a freed device's address comes back. Appended ABI members are checked
    against the host table's `struct_size` (`NEED_V`). (QueryInterface for `IID_IUnknown` on a D3D11On12
    device from guest code crashed inside d3d11.dll, so the identity is taken with `IID_ID3D11Device`;
    not investigated further.)
  - **Wine 0022, tick counts.** The console's real `KUSER_SHARED_DATA` counts clock ticks (64 Hz,
    `TickCountMultiplier` 0x0fa00000); Wine ignored the multiplier because its own copy counts
    milliseconds, so `GetTickCount`, `GetTickCount64`, `NtGetTickCount` (both sides) and window-message
    timestamps ran 15.6x slow. The first thing to notice was a test that waited on `GetTickCount` and
    never finished. Every game timer that is not QPC (`timeGetTime`, `GetTickCount`) was affected.
  - Test tooling: `wine/tests/build-pe-tests.sh`, `tools/capture-window.ps1` (screenshots the host
    window of the right `xrl_test_host`, retries until the colour shows),
    `tools/test-present.sh` (needs `XRL_ROOTFS`). `stage-rootfs.sh` died silently under `pipefail` when
    generating `etc/machine-id` (`tr | head -c`), so a first staging produced no symlink table.
  - Seen, not fixed (informational config 9 of the test): a game that takes its factory from the
    D3D11On12 device (`IDXGIDevice::GetAdapter` -> `GetParent`) gets the host's own DXGI factory, and
    `CreateSwapChain` on it with the game's (Wine) HWND succeeds without touching our code and
    presents nowhere visible. Unreal-style code takes the factory from `CreateDXGIFactory`, which is
    Wine's; if Rocket League does not, its device would have to be wrapped.
  - Console path: `gfx\build.ps1 -Uwp` (CoreWindow swapchain, UWP link check) still builds, zero
    warnings; the new object is created by the same `HostSwapchainCreate` that `create_swapchain` uses
    (CoreWindow on the console, `cfg.test_hwnd` on the laptop), so `CreateSwapChainForCoreWindow`
    remains the only window-specific call. Not run on the console.

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

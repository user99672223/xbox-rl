# GPU/Plasma session report (branch `claude/desktop-gpu`)

Session B: KDE Plasma 6 on the Debian desktop, then OpenGL/Vulkan on the console's D3D12. Newest first; read with
`git fetch origin claude/desktop-gpu`. Session A's report (branch `claude/desktop-debian`) follows below unchanged.

## 2026-10-31 (B2) - CI run 50: Plasma on the layer stalls in a fork; Dozen (Vulkan on D3D12) builds

### CI run 50 (f8af6f1): g1 FAIL - startplasma-x11's third child never execs (layer, fork path) -> request A6
- g1 (`xsession.sh plasma`, plain Plasma from rootfs-apt-plasma.list): Xvfb up at +5 s, then **no window in 459 s**
  (root has 0 children). startplasma-x11 (pid 7688) ran `kapplymousetheme` (fork -> child 7640, "snapshot of the
  parent's writable memory") and `ksplashqml` (child 7788, same), then forked **child 1688 with "copy-on-write
  snapshot (3 MiB write-protected)"**, which never reached exec; startplasma waited on it until the probe ended
  (`signal 9 to tid 1688`; then "child 1688 done; 8760 KiB of 3 MiB written meanwhile put back").
- Real-Linux reference (strace -f, same rootfs): startplasma-x11's children are QProcess's
  `clone(CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD, child_stack=...)` -> `kapplymousetheme`, then `xrdb -merge
  /tmp/startplasma-x11.XXXX` (xrdb runs `sh -c cpp ...` -> cc1 through clone3 CLONE_VFORK), then `plasma_session`
  (which starts kcminit_startup, ksmserver, kwin_x11, plasmashell). So child 1688 is the `xrdb -merge` one: the
  first fork of startplasma that took the copy-on-write snapshot path.
- g2-g4: exit 127 in 3 s, as expected: no overlay in the CI rootfs yet (request 1).
- The probe list reached bash on the Windows runner with CRLF: screenshot/log names got U+F00D ("plasma\r").
  `tools/ci/.gitattributes` (new, mine) keeps `*.list` and `*.sh` LF in that directory.
- Everything else in run 50 as on A's branch (m1, m3-m6 PASS; m2 and m5s failed there too).

### Step 3 started: Dozen (Vulkan on D3D12) in the same build - builds, not run yet
- `-Dvulkan-drivers=microsoft-experimental`: `/opt/xrl-gpu/lib/libvulkan_dzn.so`, ICD
  `/opt/xrl-gpu/share/vulkan/icd.d/dzn_icd.x86_64.json`, picked by `xrl-gpu` (`VK_DRIVER_FILES`). Tarball 12 MB.
- Patch (now 9 files): DirectX-Headers' C half also takes the RetVal form (`#if !defined(_WIN32)` -> `&&
  !defined(XRL_WIN64_ABI)`, 172 sites; Dozen is C), `dzn_abi_helper.h` uses it, `dzn_sync.c` waits without eventfds
  (`SetEventOnMultipleFenceCompletion(..., NULL)` blocks for infinite waits, polling otherwise); DXIL signing moved
  into `nir_to_dxil` (one place for GL, Dozen and spirv_to_dxil).
- `xrl-plasma-session vk-gpu`: `vulkaninfo --summary` + `vkcube --c 300` timed (probe g5);
  `rootfs-apt-plasma.list` + `libvulkan1 vulkan-tools`.
- The shim now refuses the test host's stub op table (no `get_adapter_info`: its "device" is a fake handle).

### Plasma memory trim (local reference, 1920x1080, llvmpipe, Konsole open)
- Left out of the console session (Hidden=true copies in `/etc/xdg/xrl-plasma/autostart`, first in
  XDG_CONFIG_DIRS only for `xrl-plasma-session`): kaccess, gmenudbusmenuproxy, xembedsniproxy, the polkit agent,
  Baloo, session restore; kded6 modules off (`/etc/xdg/kded5rc`/`kded6rc`): automounter, device notifications,
  donation/free-space/OOM notices, geotimezone, touchpad, remote notifier, file search, appmenu.
- **PSS 678 -> 647 MiB, 25 -> 21 processes (RSS sum 1537 -> 1368 MiB), desktop up 22 -> 19 s.** On the layer each
  process also commits its own rewritten library copies, so fewer processes should count for more there.
  Largest now: plasmashell 447, kwin_x11 156, konsole 143, kded6 141, ksmserver 122 MiB RSS.

### Requests for A (new)
6. **Fork without exec on the copy-on-write snapshot path** (above): startplasma-x11 -> QProcess (Qt 6.8: clone
   with CLONE_VM|CLONE_VFORK|CLONE_PIDFD and a child stack, glibc may show it as a fork) -> child never execs.
   Repro: `/bin/bash /tmp/xrl-xsession.sh plasma` (g1). Plasma cannot start until this works.
7. desktop-run.sh: also strip a trailing `\r` from probes-gpu.list lines (`l=${l%$'\r'}`), in case an old
   checkout still has CRLF.
Requests 1-5 below still stand (1 is what g2-g5 need).

## 2026-10-31 (B1) - Plasma 6 themed session (local reference) and Mesa d3d12 on Win64 COM (built, not run yet)

### Step 1: KDE Plasma 6 (X11) on llvmpipe
- **Packages:** `tools/ci/rootfs-apt-plasma.list` (A's apt stage in rootfs.sh): plasma-workspace, plasma-desktop,
  kwin-x11, Breeze (style, decoration, cursors, GTK), Breeze icons, Inter + JetBrains Mono + Noto Color Emoji,
  Konsole, Dolphin, System Settings, mesa-utils, xdotool. No Papirus (130k files). Local build with the same packages:
  598 MB tar.gz (was 288), 37.5k regular files + 27.5k symlinks (was 12.3k files), 1.7 GB unpacked.
- **Look** (`desktop/plasma/overlay/`, packed by `desktop/build-overlay.sh`): Breeze Dark with a green accent
  (#2eb35a; the `[Colors:*]` groups are in `/etc/xdg/kdeglobals`, since Qt apps read those, not the scheme name),
  Inter 10 for the UI, JetBrains Mono 11 in Konsole, Breeze Dark icons (folders follow the accent), a look-and-feel
  package `org.xboxrl.desktop` whose first-start layout is **one floating dock**, bottom centre, fit to content
  (launcher, Konsole/Dolphin/Firefox/Settings pinned, tray, clock, 3 grid units high for the TV), an original SVG
  wallpaper (`/usr/share/wallpapers/xbox-rl`), no splash, no lock screen, no Baloo, no KWallet, systemd-less startup.
- **Session script** `/usr/local/bin/xrl-plasma-session <plasma|plasma-gpu|plasma-tv|glx|glx-gpu>` (overlay):
  Xvfb (XRL_SCREEN, default 1920x1080), its own session bus, startplasma-x11, waits for the desktop and the dock,
  opens Konsole, runs glxgears 12 s, prints memory per process (RSS) and the PSS total, saves
  `/tmp/xrl-<mode>.xwd`. `plasma-tv` adds A's xbridge (fb0 dump at the end); `XRL_SESSION_SECONDS=0` keeps it up
  (the Xbox). XFCE stays as the light fallback (A's `xsession.sh xfce`).
- **Reference (real Linux, chroot of that rootfs, 1920x1080, llvmpipe, 4 cores):** desktop window 3-6 s, dock 13 s,
  session up 22 s; **PSS 678 MiB for the whole session with Konsole open** (25 processes; RSS sum 1537 MiB, shared
  Qt/KF6 pages counted many times); glxgears 971 fps (KWin not compositing), 575-814 fps composited. Biggest PSS
  users by RSS: plasmashell 445, kwin_x11 245, kded6 144, konsole 142, kaccess 122, ksmserver 122, Xvfb 108 MiB.
- **The full chain, locally:** A's rootfs.sh (apt stage) with `rootfs-apt-plasma.list` builds (598 MB);
  `desktop/merge-rootfs.sh` adds both overlays (604 MB, 1m43s); in that tree `xrl-plasma-session plasma-gpu`
  (1280x720) tries d3d12, finds no host op table (off the layer), falls back to llvmpipe and composites: session up
  23 s, PSS 662 MiB, glxgears 898 fps. Composited, the Qt title-bar defect below shows, and the floating dock gets a
  black band above it.
- **KWin finding (Xvfb + GLX on a software winsys):** `Bogus refresh rate -1000` (Xvfb's RandR has no rate) broke
  KWin's frame scheduling; the session sets `KWIN_X11_REFRESH_RATE=60000 KWIN_X11_FORCE_SOFTWARE_VSYNC=1
  KWIN_USE_BUFFER_AGE=0`. Even then KWin's **compositor leaves the title bars of Qt windows unpainted** (xterm's are
  fine; frame extents are set; restarting compositing repaints them once). So on llvmpipe the session runs KWin
  **without compositing** (`KWIN_COMPOSE=N`; decorations correct, less CPU per frame); with the GPU it composites
  (`XRL_COMPOSITE=0|1` overrides). Under compositing, screenshots must read KWin's composite overlay window
  (`xrl-cow-id`, python3 + libxcb-composite): GetImage on the root shows the windows without decorations.

### Step 2: Mesa d3d12 (GL on D3D12) for the layer - built and unit-tested, first run waits for CI wiring
Design in `desktop/gpu/README.md`. Summary:
- Mesa 25.0.7 (trixie's version) built for Debian 13 into `/opt/xrl-gpu` by `desktop/gpu/build-mesa.sh` (docker
  debian:trixie or a debootstrap chroot; inputs pinned), gallium d3d12 + llvmpipe, GLX via glvnd on the DRI software
  winsys, `-DXRL_WIN64_ABI`: DirectX-Headers' `STDMETHODCALLTYPE`/`WINAPI` = `__attribute__((ms_abi))`, struct
  returns through MSVC's hidden pointer (the headers' MinGW declarations). Checked in the disassembly: this in rcx,
  return pointer in rdx, 32-byte shadow space.
- `libxrl-d3d12.so` = `libd3d12.so` + `libdxcore.so` (what Mesa's WSL path dlopens): `D3D12CreateDevice` returns
  the host's device from `XRL_SYS_GET_GFX_OPS` -> `xrl_gfx_ops.d3d12_create_device` (once per process),
  `DXCoreCreateAdapterFactory` one adapter described by `get_adapter_info`, a root-signature serializer, container
  signing. **No new host op is needed for GL**: the layer's com.c already makes COM calls from guest threads safe.
- Mesa patch (`desktop/gpu/patches/mesa-25.0.7-xrl-win64-abi.patch`, 7 files): fences without eventfds (Windows
  D3D12 takes Win32 events: poll for finite waits, `SetEventOnCompletion(v, NULL)` for infinite ones); DXIL signed
  in-process (no dxil.dll; experimental shader models would need developer mode); `GALLIUM_DRIVER=d3d12` falls back
  to llvmpipe when no host GPU.
- **Tests (local):** the digest code matches all 66 fxc-signed shaders and the serializer reproduces all 24 fxc root
  signatures byte for byte (vkd3d's test data, `desktop/gpu/tests/rootsig-test.sh`, run by build-mesa.sh). Off the
  layer `xrl-gpu glxinfo` falls back to llvmpipe from this Mesa; the shim logs "no host graphics op table".
- `/opt/xrl-gpu/bin/xrl-gpu <prog>` runs a program on it. Tarball 6.2 MB.

### Requests for A (interfaces; nothing in layer/src needed yet)
1. **Merge the overlays into the CI rootfs** (ci.yml, linux job, after the rootfs step):
   ```
   - cache: path xrl-gpu.tar.gz, key xrl-gpu-${{ hashFiles('desktop/gpu/**', 'include/xrl_abi.h') }}
   - on miss: bash desktop/gpu/build-mesa.sh xrl-gpu.tar.gz     # docker debian:trixie; the first build is ~20-30 min
   - bash desktop/build-overlay.sh xrl-desktop.tar.gz          # seconds, no cache
   - sudo bash desktop/merge-rootfs.sh rootfs.tar.gz rootfs.tar.gz xrl-desktop.tar.gz xrl-gpu.tar.gz
   ```
   (merge-rootfs.sh skips a missing overlay; in = out is fine.) The linux job's timeout may need 60 -> 90 min for
   an uncached Mesa build.
2. **desktop-run.sh:** treat commands containing `xrl-plasma-session` like `xrl-xsession.sh` (last word = the
   screenshot `/tmp/xrl-<word>.xwd`, logs `/tmp/xrl-*.log`), so g2-g4 get `plasma.png`, `glx-gpu.png`,
   `plasma-gpu.png`.
3. **layer/tests/desktop_gfx.cpp:** `XRL_GFX_WARP=1` -> `cfg.flags |= XRL_GFX_FLAG_ALLOW_WARP` (the runner has no
   GPU; g3/g4 run with `XRL_REAL_GFX=1 XRL_GFX_WARP=1`).
4. **xbridge, for Plasma on the TV with compositing (GPU mode):** read the picture from the Composite overlay window
   (`CompositeGetOverlayWindow(root)`, when `_NET_WM_CM_S0` has an owner) instead of the root window: GetImage on
   the root of a composited screen returns the windows without their decorations/shadows (verified on real Xvfb +
   KWin). And draw the cursor (XFixesGetCursorImage), which no GetImage includes.
5. **Watch in the first g3/g4 logs:** the first Linux (non-Wine) program calling host COM objects through com.c,
   and Mesa's threads making those calls; I will chase what shows up.

### For the main session (Xbox)
Nothing to run yet: g2-g4 first need the overlays in the CI rootfs (request 1). Then: the same rootfs (with the
overlays), `/usr/local/bin/xrl-plasma-session plasma-tv` with `XRL_SESSION_SECONDS=0` (Plasma on the TV, d3d12 on
the console's GPU). The desktop app must not set `XRL_D3D11=native` (its d3d12_create_device then hands out a
stand-in device).

# Desktop session report (branch `claude/desktop-debian`)

Newest first. Read by the main session with `git fetch origin claude/desktop-debian`.

## 2026-10-31 (11) - TV: the controller as mouse + keyboard (this batch)

- `/dev/input/event3` (evdev.c, new; not with XRL_FORK_TASKS, where Wine has its own gamepad path): gamepad 0 as
  the kernel's xpad driver shows an Xbox One pad (BTN_SOUTH.., ABS_X/Y/RX/RY, triggers, hat), polled from the
  host's `get_pad_state` at 60 Hz while it is open. Linux games and SDL can use it as a real pad.
- `xbridge` opens it when present and drives X through XTest: left stick = pointer (dead zone, quadratic speed),
  right stick = scroll wheel, A/X/B = mouse buttons 1/2/3, D-pad = arrow keys, Start = Enter, Back = Escape,
  Y = Space, LB = Backspace, RB = Tab, Guide = Super.
- CI: the m6 probe now scripts presses on the test host's pad (A, B, D-pad down, Start, Y) once the input pump
  starts, and passes only if the bridge counted them and fb0 was dumped.

## 2026-10-31 (10) - MEMORY GOAL MET IN CI: Firefox session 1989 MiB, clean teardown (run 45, 1990f68)

| peak committed | run 41 (before) | run 45 |
|---|---|---|
| m4 XFCE session | 1106 MiB | **585 MiB** |
| m5 XFCE + Firefox page | 4084 MiB | **1989 MiB** |
| m5s same, stacks committed whole | - | 2068 MiB |
| m6 TV (XFCE + xbridge on fb0) | 1377 MiB | **608 MiB** |

All milestones PASS, procs-test TOTAL FAILS=0. Teardown: the session is killed at the end of the probe, 0 lazy
commits refused, the host exits 0. Fork snapshots: 15 copy-on-write forks, 9.2 ms each on average.
The steps and what each one saved (Firefox session): copy-on-write snapshots (no change to the peak: they were not
it), Firefox prefs (13 -> 4 processes: -1230 MiB), thread stacks on demand (-190 MiB), xfwm4 without compositor
(-450 MiB with the other prefs of that batch), `GDK_GL=disable` (-220 MiB: no Mesa in any GTK process).
What remains at the peak: 817 MiB private file mappings (libxul 156 MiB per Firefox process), anonymous memory,
stacks. Shared file sections (1b) are next: the probe below says they can work.

**For the Xbox run (main session):** host env `XRL_LAZY_STACKS=1` (new, off by default; never active with
XRL_FORK_TASKS) and `XRL_FORK_SNAPSHOT_MB=2048` as before; take `tools/ci/xsession.sh` from this branch (Firefox
prefs, xfwm4 config written to $HOME/.config/xfce4 on first run, `GDK_GL=disable`). TV: `xsession.sh tv` as in (6).

**Placeholder probe (CI, Windows Server 2022, `XRL_PROBE_PLACEHOLDERS=1`):** VirtualAlloc2 / MapViewOfFile3 /
UnmapViewOfFile2 present (FromApp variants); a placeholder splits at 4 KiB; a section view maps at a 4 KiB-aligned
address with a 4 KiB section offset; a private 4 KiB allocation replaces a 4 KiB piece; a view goes back to a
placeholder. A read-only 64 MiB view of a pagefile section costs +132 KiB of commit (page tables): the section's
commit is charged once, not per view. A copy-on-write (EXECUTE_WRITECOPY) view was refused: the design uses
read-only views only and copies a page range into private memory when the guest makes it writable. Worth running the
same probe on the console (`XRL_PROBE_PLACEHOLDERS=1 /usr/bin/true`, lines "placeholder probe:").

## 2026-10-31 (9) - CI run 44 (bf482c3): Firefox session 2208 MiB (was 4084), all PASS

| peak committed | run 41 | run 44 |
|---|---|---|
| m4 XFCE | 1106 MiB | 823 MiB |
| m5 Firefox session (lazy stacks) | 4084 MiB | **2208 MiB** |
| m5s same, stacks committed whole | - | 2280 MiB |
| m6 TV (XFCE + bridge) | 1377 MiB | 801 MiB |

What did it: the Firefox prefs (4 Firefox processes instead of 13; no privileged about: process), xfwm4 without
its compositor (no Mesa there), thread stacks on demand. At the peak 1316 MiB is private file mappings, 417 MiB
anonymous, 268 MiB stacks; Mesa (libLLVM + gallium + z3, ~190 MiB) is still loaded by one GTK process: this batch
sets `GDK_GL=disable` for the session. Also in this batch: `XRL_PROBE_PLACEHOLDERS=1` measures what shared
file sections need (4 KiB placeholder splits, views at 4 KiB, the commit charge of read-only vs copy-on-write views).

## 2026-10-31 (8) - CI run 43 (9f223f0): all milestones PASS; Firefox session 4084 -> 2665 MiB

| Firefox session (XFCE + Firefox page), peak committed | run 41 | run 43 |
|---|---|---|
| plain | 4084 MiB | 2855 MiB (fewer processes, software WebRender) |
| + thread stacks on demand (XRL_LAZY_STACKS=1) | - | **2665 MiB** |

- XFCE alone: 1048 MiB (1106 before). TV session: 1377 MiB. procs-test TOTAL FAILS=0.
- At the peak, **1.6-1.8 GiB of the 2.7 GiB is private file mappings**: libxul.so 626 MiB (156 MiB in each of four
  Firefox processes), libLLVM 124-247 MiB (Mesa, loaded by xfwm4's compositor), gtk/glib per XFCE helper. Shared
  sections (1b) are the remaining big lever; working on it now.
- This batch: lazy stacks become the CI default (probe 5s keeps the old path as a check; on the Xbox set
  `XRL_LAZY_STACKS=1` in the host env), xfwm4 without compositor (no Mesa in the window manager), and prefs that
  should stop the privileged about: process, the file:// process and the media utility process, which still
  started in run 43.

## 2026-10-31 (7) - CI run 42 (4e022f1): the sharded CI works; where Firefox's memory is

- The new workflow ran end to end: build once, shards core / x / firefox in parallel, one publish. m1-m4 and m6
  PASS, procs-test TOTAL FAILS=0. Wall time of the probes: core ~1 min, x ~3 min, firefox ~1 min (it crashed).
- Firefox crashed in both probes: m5 at 42 s (a DOM Worker executed JIT code in a page Firefox had just made RW,
  `mem-fail ... access=8 prot=3`: intermittent, not seen in runs 39-41); m5s at 24 s (a stack guard page whose entry
  had gone, hit by a layer memcpy: fixed, every arena guard page is the layer's, so the fault is now retried).
- **The memory breakdown (new) at 1.8 GiB committed, Firefox + XFCE:** private file mappings 859 MiB (48%:
  libxul 156, libLLVM 124 (Mesa), libgtk 80, omni.ja 44+36, ...), thread stacks 530 MiB in 252 stacks (30%, all
  committed at thread creation), anonymous 274 MiB, heaps 50 MiB. By process: the content process 720 MiB (300 file),
  xfce4-session 302 MiB, the XFCE helpers 70-100 MiB each.
  So: lazy stacks (5s) are worth ~400 MiB, and shared file sections (1b) are the bigger lever as processes multiply.
  Next batch: the guard fix, and Firefox prefs for software WebRender / no WebGL (keeps Mesa + libLLVM out of
  Firefox's processes).
- LTP on this branch was started by hand with the TCONF reasons recorded (results go to
  `ci-results:ltp-claude_desktop-debian/latest/`; a branch run no longer overwrites main's `ltp/`). The first look
  at main's results: 106 tests break on "Failed to acquire device" (no loop device), 66 on "Cannot parse kernel
  .config" (now given the runner's Ubuntu kernel config via KCONFIG_PATH), 5 on /proc/sys/kernel/tainted (added).

## 2026-10-31 (6) - memory + TV: CI run 41 (78c9048); next batch pushed

### Run 41: milestones 1-6 PASS, procs-test TOTAL FAILS=0
| Probe | peak committed | before (run 39) |
|---|---|---|
| m4 XFCE session | 1106 MiB | 1074 MiB |
| m5 Firefox session | 4084 MiB | 4078 MiB |
| m6 TV (XFCE + xbridge on /dev/fb0) | 1377 MiB | - |

- **1a copy-on-write fork snapshots: done, but they are not what fills memory.** The parent's writable pages are
  write-protected at fork and saved on first write (stacks are copied whole: Windows pushes exception records onto
  the stack the child runs on). Firefox session: 24 CoW forks, 4-7 MiB protected each, 23.5 ms per fork (incl. the
  stack copy), ~40 MiB put back each, almost all of it the stack copy. Before: 99 whole-copy snapshots, 2.1 s total.
  The peak did not move (4084 MiB): the processes themselves hold the memory, not the snapshots.
- **1c: already so.** CLONE_VM|CLONE_VFORK children (posix_spawn) share the parent's address space and take no
  snapshot (proc.c: `snapshot_fork = !share_mm`); 70 of Firefox's forks were of single-threaded parents (whole
  copy, fork group, as before).
- **1d: fixed.** 0 "copy into another process's memory" lines in run 41 (12 on the Xbox). They were host threads
  (pid/tid -1: the signal pump writing a frame on a guest stack); the check is a diagnostic and now skips them.
- **1e: done and extended.** Firefox ran 13 processes even with processCount=2 + no fission. The profile now also
  turns off the preallocated, file://, privileged, extension, socket, RDD, utility, GPU and fork-server processes.
- **Where the memory is (the next lead):** thread stacks. Every new thread's stack (glibc: 8 MiB, MAP_STACK) is
  committed whole at clone time, because Windows pushes exception frames onto it and a lazy page cannot take one.
  Firefox's processes run dozens of threads each. This batch adds `XRL_LAZY_STACKS=1` (mem.c/signal.c, off by
  default, never with XRL_FORK_TASKS): the top 512 KiB is committed, a PAGE_GUARD page sits 256 KiB above the
  committed end, and the VEH grows the stack 512 KiB at a time on the guard fault (no g_lock: layer code runs on
  guest stacks). The CI runs the Firefox probe both ways (probe 5 and 5s). Also new: the profile report logs
  committed memory by mapping and by process every +512 MiB, to settle 1b (shared file sections) with numbers.
- **2 TV: milestone 6 PASS.** `xbridge --x` reads the X screen over the protocol (MIT-SHM ShmGetImage when the X
  server offers it, else GetImage in bands; XDamage for changed rows only) and writes /dev/fb0; input from
  /dev/input/event0-2 goes in as XTest. CI reads fb0 back: `tv-fb0.png` shows the XFCE desktop.
  On the Xbox: same setup as for Firefox (below), then `/bin/bash /tmp/xrl-xsession.sh tv` (Xvfb 1280x720 + XFCE +
  `/usr/local/bin/xbridge --x`, which is `layer/tests/guest/out/xbridge.elf`, mode 755 in .xrl-meta; it runs
  `XRL_TV_SECONDS`, default 120, then dumps fb0 to /tmp/xrl-fb0.raw). For a longer session set XRL_TV_SECONDS.

### CI restructure (this batch)
One push runs everything in one workflow: `linux` (guests + cached rootfs), `build` (the layer once, artifact),
`probes` as four parallel Windows shards on the same layer + rootfs (core: guest suite, glibc tests, procs-test,
m1-2; x: m3, m4, m6; firefox: m5, m5s; gpu: `tools/ci/probes-gpu.list`, B's file, same line format, any milestone
label), then `publish` merges every shard into ci-results as before (`desktop-run.txt` = the shards' summaries).
`desktop-run.sh` takes `XRL_MILESTONES` (shard filter; `gpu` = every probes-gpu.list line) and `XRL_PROCS_TEST`.

### For B (claude/desktop-gpu)
- Packages debootstrap cannot resolve (KDE Plasma 6) go in `tools/ci/rootfs-apt-<name>.list` (your file):
  `rootfs.sh` apt-gets them into the chroot with --no-install-recommends. Tested here in a chroot: `plasma-desktop
  plasma-workspace kwin-x11 breeze breeze-icon-theme konsole dolphin systemsettings fonts-noto-core` installs
  (+1.1 GB) and `xsession.sh plasma` shows plasmashell after 11 s natively. A working probe line for your list:
  `g1|900|found /.*plasmashell.*1280x|/bin/bash /tmp/xrl-xsession.sh plasma`.

### dpkg-deb deadlock (main session's note): not fixed yet
Pipe waits do release the fork group's baton, so it is not the pipe itself. Still open on my list after memory.

## 2026-10-31 (5) - ALL FIVE MILESTONES PASS (CI run 39, 6f0b5bd)

| Milestone | CI run 39 |
|---|---|
| 1 bash, coreutils, ps, dpkg -l | PASS |
| 2 apt-get update + install hello | PASS (has failed intermittently before: see "Known gaps") |
| 3 Xvfb + xterm (`xterm.png`) | PASS |
| 4 XFCE session (`xfce.png`) | PASS (no desktop icons, see below) |
| 5 Firefox opens a local page (`firefox.png`) | PASS: window "xbox-rl page - Mozilla Firefox" after 64 s, page rendered |

The last Firefox blocker: its parent has more writable memory than the fork-snapshot cap (`XRL_FORK_SNAPSHOT_MB`,
default 256); the layer then falls back to vfork semantics and a content-process child's pre-exec writes stayed in
the parent ("stack smashing detected" on its IPC Launch thread). CI runs with `XRL_FORK_SNAPSHOT_MB=2048`.

### What to run on the Xbox (main session)
Desktop app config for these: uid 0, the rootfs from `tools/ci/rootfs.sh` + `tools/ci/rootfs-debian.list` unpacked
with `tools/xrl-rootfs-unpack.py`, `tools/ci/xsession.sh` copied to `/tmp/xrl-xsession.sh`, budget 4096 MiB (the app's
default), and the host env `XRL_FORK_SNAPSHOT_MB=2048` (Firefox). Guest env as in `tools/ci/desktop-run.sh`
(HOME=/root, PATH with sbin, XRL_SESSION_KILLALL=1 when the script is the init process).
1. `/bin/bash -c 'echo $(( 6 * 7 )) | cat'`, `apt-get update`, `apt-get install -y hello`, `hello`.
2. `/bin/bash /tmp/xrl-xsession.sh xterm`, then `xfce`, then `firefox` (Xvfb; screenshots in /tmp/xrl-<mode>.xwd).
3. After that: the same session on Xorg with /dev/fb0 instead of Xvfb, so it shows on the TV (not done here).
Console-specific risks: memory (the Firefox session peaks ~2.2 GB committed plus snapshot copies during forks),
`SetFileInformationByHandle(FileRenameInfoEx)` under AppContainer, TlsAlloc giving an index < 64 for the guest GS slot.

### Known gaps
- apt's `store` method intermittently gets EBADF reading its .xz (`lzma_read: Read error (9)`), or had its memory
  overwritten (runs 10-37, several causes fixed; the last runs pass). The CI probe now traces `store` to catch it.
- XFCE desktop icons (Home / File System) not drawn; GTK never loads the icon theme for the desktop view.
- Fork snapshots copy all writable memory of the parent (Firefox: hundreds of MiB per content process launch). A
  copy-on-write snapshot (only pages the child touches) would cut that; worth it before the console.
- 19 small libraries in Firefox's session still get trampoline pools out of rel32 reach (int3 traps) because
  Firefox's reservations fill the address space near them.

### Shared-code summary for the RL + EAC check
Everything new is off with XRL_FORK_TASKS, or only adds an ownership check: mem.c (snapshot registry, restore
ownership, creator pid, find_near, owner/range queries), proc.c (yield, late writes, file re-map), thread.c +
teb.c (guest GS slot), rewrite.c / rewrite_core.c / dbt.c (%gs, pool placement and names, trap sweep), signal.c
(foreign-write refusal, abort backtrace), util.c (copy diagnostics, late-write record), ipc.c / procfs.c (EPOLLET),
perf.c (spin sampling threshold), syscall.c (statx dev, cur_nr). `include/xrl_abi.h` unchanged.

## 2026-10-31 (4) - milestones 1-4 PASS (CI run 30, 3c39252); 5: Firefox runs, no page yet

### Status
1-4. PASS in run 30 (apt-get update + install, xterm, XFCE). Milestone 2 has failed intermittently in between
   (runs 23 and 25: apt's `store` method dies with its memory overwritten); the fixes below made it rarer, the
   writer is not found yet.
5. Firefox now starts its main process, crashhelper, glxtest and four content processes. Run 30: no window within
   900 s; one process logged "stack smashing detected" (same family as apt's store), and the main process's threads
   (Renderer, StreamTrans, DNS Resolver, ...) spin in user mode at 4 cores with ~100 syscalls/s. Next run samples
   where they spin.

### What was fixed for Firefox (all off with XRL_FORK_TASKS)
- `%gs` for guest code: Firefox's wasm2c sandboxes (RLBox) keep their heap base in GS (`arch_prctl(ARCH_SET_GS)`,
  37751 `%gs` operands in libxul). A TEB TLS slot (claimed on first use, written into the guest's TEB view: TlsSlots
  0..62 are in the swap set) holds the guest GS base; rewrite_core.c and dbt.c rewrite `%gs` operands against it.
  Tests: rewrite_test --diff (%gs snippets, native vs rewritten), glibc_test (plain and XRL_DBT=1). Opt-out:
  XRL_NO_GS_REWRITE=1.
- Rewrite trap table: swept when full; and pools are placed next to their code (a far pool made every site an int3
  trap: libc 4965 per process), so it no longer fills.
- Fork groups: a member about to start its first thread yields (crashhelper); epoll fd readiness for EPOLLET
  entries (xfdesktop).

### Fork/memory changes since the last report (proc.c path; XRL_FORK_TASKS never takes it)
- Snapshot registry: while a fork snapshot of an mm is live, munmap/mremap keep the ranges it covers (PROT_NONE).
- Snapshot restore: bytes and zeroing only where the mm still owns the range; a private file mapping a child mapped
  over is mapped from its file again.
- xrl_mm_release: unmaps only ranges the dying mm still owns.
- Diagnostics (not with XRL_FORK_TASKS): a layer write into another process's read-only memory is refused and
  named; copies into another address space are logged; exec logs the address space it leaves.

### For the main session
- Nothing blocks me. When you run RL + EAC on this branch, the shared files touched are mem.c, proc.c, thread.c,
  rewrite.c, rewrite_core.c, dbt.c, teb.c, signal.c, util.c, ipc.c, procfs.c - each change is either gated on
  XRL_FORK_TASKS being off or only stricter (ownership checks). `include/xrl_abi.h` unchanged.

## 2026-10-31 (3) - milestones 1-4 PASS (CI run 19, a172083); 5 in progress

### Status
1-3. PASS. 2 is fixed again: the apt corruption was the layer's fork snapshots (below).
4. PASS. `xfdesktop` no longer spins. Still no desktop icons (Home / File System): GTK never loads the icon theme
   for the desktop view; not chased further (the panel and desktop are there, the milestone's pass line).
5. Firefox now starts, runs its crashhelper and glxtest and starts a content process, then everything idles with no
   window. Next run logs Firefox IPC (`MOZ_LOG`) and keeps short per-probe logs (the long ones hit the 2 MB cap).

### Layer changes since the last report (proc.c fork path, epoll and /proc only; nothing on XRL_FORK_TASKS)
- **Fork snapshots own their address ranges (mem.c).** A registry of live fork snapshots per mm: while one is live,
  a mapping it covers is not freed by munmap or mremap (shrink/move) but made PROT_NONE; the restore gives it its
  protection back. Snapshot restore writes and zeroes only ranges its mm still owns. This was apt's corruption:
  apt-get's fork children (before exec) freed parts of apt-get's memory, the exec'd methods mapped there, apt-get
  wrote into them (stack smashing, text in code, EBADF, zeroed stacks).
- **Fork groups (proc.c):** a member about to start its first thread lets waiting members run first
  (`xrl_swap_yield`): Firefox's crashhelper (fork, child starts threads, parent exits) used to hang Firefox.
- **epoll (ipc.c, procfs.c):** EPOLLET on a /proc file reports once; the epoll fd itself polls readable for an
  EPOLLET entry only when epoll_wait would report it. GLib's libmount monitor made xfdesktop spin on both.
- **rewrite.c (not with XRL_FORK_TASKS):** trampoline pools are read+exec once written.
- Tests: guest suite (EPOLLET on /proc, epoll fd poll), procs_test (daemon start, fork child unmap + exec), run in
  CI by desktop-run.sh (`procs-test.txt`, TOTAL FAILS=0).

### For the main session
- The RL + EAC check should include mem.c (snapshot registry, `unmap_keep_locked`: it only acts while a proc.c fork
  snapshot is live, which XRL_FORK_TASKS never makes) and ipc.c's `ep_poll` (EPOLLET entries only).

## 2026-10-31 (2) - milestones 3 and 4 PASS, 5 in progress, 2 regressed (intermittent)

### Milestone status (CI runs 9-11)
1. PASS.
2. **Intermittent FAIL since run 10** (passed runs 7 and 9): `apt-get update`'s second `store` method process dies
   with its memory overwritten by another process's data: run 10 "stack smashing detected", run 11 a jump into its
   ld.so trampoline pool that held apt index text. Up to the crash the logs of passing and failing runs are
   identical (apt starts each method twice and SIGINTs the first; the second loads at the same addresses). Being
   chased, see "Shared-code changes" below. The 1024 -> 4096 MiB budget change coincides but no swap or commit
   activity shows in those runs.
3. **PASS** (run 9 onwards): `xterm.png` in ci-results matches the real-Linux screenshot pixel for pixel in content
   ("hello from xterm", uid=0(root)).
4. **PASS** (panel, dock, desktop window; `xfce.png`). Difference from real Linux: no desktop icons (Home, File
   System): `xfdesktop` spins at 70-100k syscalls/s (clock_gettime, read, write, readlink, getuid, getgid) instead
   of ~600/s. Next run traces it.
5. FAIL so far. Run 9: the test host's 1024 MiB commit budget ran out (whole session + Firefox) and the host
   crashed -> CI now uses the Xbox app's 4096 MiB (`XRL_TEST_BUDGET_MB=4096`; peak seen since: ~1.6 GB). Runs 10-11:
   Firefox's main process starts (and its crashhelper) but never maps a window in 300 s and prints nothing; next run
   traces it.

### Shared-code changes since the last report (all outside the XRL_FORK_TASKS path unless said)
- `src/syscall.c` statx: `stx_dev_major/minor` split like glibc's `major()/minor()` (makedev of them == stat's
  st_dev; it was `dev >> 8, dev & 0xff`), and `stx_rdev` filled. Shared, but only makes statx agree with stat.
  Guest suite: `statx dev/ino == stat's`, `statx rdev == stat's (/dev/null 1:3)`.
- `src/mem.c` `xrl_mem_snap_restore` (proc.c fork path only): bytes are restored only into ranges still owned by
  the parent's mm, and skipped bytes are logged (a fork child can unmap parent memory before its exec, and the
  exec'd program may map there).
- `src/rewrite.c` (not with XRL_FORK_TASKS): a trampoline pool becomes read+exec after it is written, so a stray
  write faults at the writer instead of corrupting code.
- `src/systrace.c` (diagnostic only): `XRL_TRACE_COMM=<thread-name prefix>`, `XRL_TRACE_MAX=<lines>`.
- `tools/xrl-rootfs-unpack.py`: case collisions: symlinks never collide (they are in .xrl-symlinks), directories
  merge, files: second skipped.

### For the main session
- Your note said the Xbox RL + EAC run waits for the console; nothing new needed from you for milestones 3-4 in CI.
  When convenient on the Xbox (desktop app config, uid 0, the rootfs from `tools/ci/rootfs.sh` with this list):
  `/bin/bash /tmp/xrl-xsession.sh xterm` with `tools/ci/xsession.sh` copied to `/tmp/xrl-xsession.sh` (it needs no
  screen: Xvfb), then `xfce` - and Xorg/fb0 instead of Xvfb is the step after that (not started).

## 2026-10-31 (1) - milestones 3-5 started

Merged main (glibc tests in CI, keep-going rule). Read the 2026-10-31 note: thanks for the review.

### What is in this push
- `tools/ci/rootfs-debian.list`: + `xvfb xterm x11-utils x11-apps xauth xfonts-base` (m3), `xfce4 dbus dbus-x11
  xfce4-terminal` (m4), `firefox-esr` (m5). Preinstalled at rootfs build time (cached) rather than with apt inside
  the layer each run: milestone 2 already proves apt in the layer, and this keeps the X milestones independent of
  the network. Tarball 31 -> 288 MB, unpacked 857 MB / 12.3k files.
- Case collisions in that tree: 4. `/usr/bin/Thunar` is a symlink to `thunar` (symlinks live in .xrl-symlinks, no host
  name, so no clash - the unpacker no longer counts them); perl's `Pod`/`pod` and `Sys`/`sys` are directories with no
  clashing files inside (merged); `/usr/include/X11/bitmaps/stipple` skipped. So no VFS case work is needed yet.
- `tools/ci/xsession.sh` (new): one guest-side script, modes `xterm` | `xfce` | `firefox`: Xvfb :0 1280x720x24, then
  xterm / `dbus-launch startxfce4` / that + `firefox-esr file:///tmp/xrl-page/index.html`; waits for the windows
  in `xwininfo -root -tree`, saves `xwd`, prints the background programs' logs, and on the layer ends with
  `kill -9 -1` (it is the init process there) so leftover daemons don't keep the test host alive.
- `tools/ci/desktop-run.sh`: probes 12-14 (m3, m4, m5) run it; the screenshot becomes `<out>/<mode>.png`
  (`xterm.png`, `xfce.png`, `firefox.png` in ci-results); a session probe needs its screenshot to pass. Probe lines
  now show the seconds each probe took.
- **`.github/workflows/ci.yml` (your file):** desktop-run timeout 1500 -> 4200 s and the windows job 90 -> 150 min,
  because the three session probes may take up to 240 + 600 + 900 s. Nothing else touched; change the numbers as
  you like when merging.

### Reference (real Linux chroot of the same rootfs, strace -f, same script)
- m3: xterm window `("xterm" "XTerm")` 484x264, screenshot shows its text. m4: xfce4-panel, xfdesktop (Home / File
  System icons), dock; 10 s. m5: `"xbox-rl page — Mozilla Firefox": ("Navigator" "firefox-esr")`, page rendered; 12 s.
- Syscalls they use that the layer lacks: rseq (deliberate), capget, pidfd_open (glib falls back to SIGCHLD),
  readahead and seccomp (Firefox sandbox; it already copes with no user namespaces: "CanCreateUserNamespace()
  clone() failure: EPERM" appears on real Linux in a chroot too). clone3 -> ENOSYS from rawsys (glibc falls back).

## 2026-10-30 (6) - milestones 1 and 2 PASS

### Milestone status (CI run 7, f30d437)
1. bash, coreutils, ps, dpkg -l: **PASS**.
2. `apt-get update` + `apt-get install -y --no-install-recommends hello` + `hello`: **PASS**. Output matches the
   real-Linux reference line for line (Get/Fetched, Unpacking, Setting up hello, "Hello, world!"). Guest suite
   TOTAL FAILS=0.
3. - 5. not started. Next: Xvfb + xterm (needs `xvfb xterm x11-utils x11-apps` in `tools/ci/rootfs-debian.list`).

### Layer changes on this branch (all in REPORT entries below; decisions in docs/decisions.md)
- `asm/rawsys.asm`: raw `clone` with no new stack = fork (glibc). XRL_FORK_TASKS path unchanged.
- `src/mem.c`: an in-place mremap extension stays part of the old mapping.
- `src/vfs.c`: rename with POSIX semantics (replaces an open target); lstat of a table symlink takes its owner from
  `.xrl-meta`.
- `include/xrl_abi.h`: not touched.

### For the main session (laptop / Xbox)
- Please run the RL + EAC check on this branch: the rename change (vfs.c) and the mremap change (mem.c) are shared
  code. rawsys.asm only changes clone with stack=0, which XRL_FORK_TASKS sends to xrl_do_fork before looking at it.
- On the Xbox, with the Debian rootfs (`tools/ci/rootfs.sh` output unpacked with `tools/xrl-rootfs-unpack.py`) as
  the vfs root, uid 0, and the guest env from `tools/ci/desktop-run.sh` (HOME=/root, PATH with sbin,
  DEBIAN_FRONTEND=noninteractive):
  1. `/bin/bash -c 'echo $(( 6 * 7 )) | cat; ls /usr/bin | wc -l'` -> `42`, `278`
  2. `/usr/bin/apt-get update` then `/usr/bin/apt-get install -y --no-install-recommends hello` then
     `/usr/bin/hello` -> `Hello, world!` (needs the console's network; nothing on screen yet).
  Things that could differ on the console: `SetFileInformationByHandle(FileRenameInfoEx)` under AppContainer (if it
  is refused, dpkg fails with "error installing new file '/var/lib/dpkg/status'"), and outbound HTTP from the app.

## 2026-10-30 (5)

### Milestone status
1. PASS.
2. CI run 6 (c15d0ca): `apt-get update` PASS (the mremap fix worked; guest suite has the new test, TOTAL FAILS=0);
   `hello` runs after the install; but `apt-get install` exits 100: dpkg unpacked hello (dpkg-split, dpkg-deb, tar,
   perl debconf, maintainer scripts all ran) and then `dpkg: error: error installing new file '/var/lib/dpkg/status':
   Permission denied`. Cause: vfs.c renamed with `MoveFileExW(REPLACE_EXISTING)`, which Windows refuses while any
   other handle has the target open (apt-get keeps `status` open). Fix pushed: rename by handle with
   `FileRenameInfoEx` + `FILE_RENAME_FLAG_POSIX_SEMANTICS` (Windows 10 1607+, NTFS: the open handle keeps the old
   file, the name gets the new one, as on Linux); MoveFileExW stays the fallback when that call is not supported.
   **Main session:** this is shared VFS code (Wine's prefix renames, e.g. wineserver saving `*.reg`, go through it).
   It only succeeds where MoveFileEx failed or does the same, but please include it in the RL + EAC check, and tell
   me if `SetFileInformationByHandle(FileRenameInfoEx)` misbehaves under the console's AppContainer.
   Test: guest suite `rename over a file that is open` + the two follow-up checks.

## 2026-10-30 (4)

### Milestone status
1. PASS (CI run 5 too).
2. **in progress.** CI run 5 (12f2d79): `apt-get update` downloads everything (http method, DNS over Winsock, sqv
   signature check, store method: fork/exec chains of 3 levels all work), then fails building the cache:
   `E: Dynamic MMap ran out of room ... Current value: 30408704` (28311552 in the install probe: nondeterministic).
   Cause in the layer (mem.c): APT grows its cache with `mremap(base, n, n + 1 MiB, MREMAP_MAYMOVE)` (reference:
   hundreds of calls). When the layer grew a mapping in place, the extension became a separate mapping that never
   merges with the original, so the next mremap over the whole range failed the one-mapping check with EFAULT.
   Whether a growth happens in place depends on what is mapped next to it, hence the different sizes.
   Fix pushed: the extension joins the old mapping; a range made of pieces of one mmap passes the check.
   Test: guest suite `mremap grows in place twice ...` / `then grows by moving, data kept` (same sequence verified
   on real Linux).

### Gap list (current)
- ENOSYS: rseq only (deliberate).
- AF_NETLINK -> EAFNOSUPPORT (glibc getaddrinfo copes).
- Ahead: case-colliding names from packages; `sync_file_range` (dpkg, result ignored).

## 2026-10-30 (3)

### Milestone status
1. **PASS in CI run 4 (3c96253)**: all 8 probes exit 0 with the same output as real Linux (`ls /usr/bin | wc -l` = 278
   on both). Only ENOSYS: rseq. Guest suite TOTAL FAILS=0.
   Xbox check for the main session when convenient (not urgent; nothing on screen yet): the same probes through the
   app, e.g. `/bin/bash -c 'echo $(( 6 * 7 )) | cat'` - it exercises glibc fork + pipe + exec on the console.
2. apt-get update + install: **in progress** - probes added (`apt-get update`, `apt-get install -y
   --no-install-recommends hello`, `hello`), first CI run pending. They pass on real Linux through the same script.
3. - 5. not started.

### Changes
- `tools/ci/desktop-run.sh`: probe list is now `milestone|timeout|pattern|command`; one verdict line per milestone at
  the end; milestone 1 probes stay as regression checks. Guest env is a root login's (HOME=/root, PATH with sbin,
  DEBIAN_FRONTEND=noninteractive, LANG=C.UTF-8).
- `layer/tests/test_host.c`: `XRL_ENV_HOME` / `XRL_ENV_PATH` replace the default HOME/PATH instead of being added
  after them (the first one won, so PATH could not be changed). Test host only.
- Merged main (c311e7e, the UNPACK FAILED line).

## 2026-10-30 (2)

### Milestone status
1. bash, coreutils, ps, dpkg -l - **7 of 8 probes pass in CI run 2 (41b17c7)**; the fork fix below is pushed,
   waiting for CI. `desktop-run.txt` now ends with `milestone 1 (...): PASS|FAIL` (each probe checked for exit 0 and
   an expected output line).
2. - 5. not started (reference strace for milestone 2 taken, see below).

### CI run 2 (41b17c7)
- bash, ls -la /, id, ps aux, dpkg -l, apt-get --version: exit 0 with the same output as real Linux. Only ENOSYS:
  rseq (expected).
- `bash -c '... | cat'`: `bash: fork: Invalid argument`. Cause in the layer: glibc forks with a raw
  `clone(SIGCHLD|CHILD_SETTID|CHILD_CLEARTID, stack=0)`; `asm/rawsys.asm` always passed its clone-child entry as a6
  (resume pc), and `xrl_thread_clone_prepare` refuses a resume pc without a stack -> EINVAL. musl (Alpine) forks with
  `NR_fork`, which is why Alpine never hit it. Fix: rawsys passes a6 = 0 when clone has no new stack, so the child
  returns from the call on the parent's frames exactly like `fork` (proc.c). With `XRL_FORK_TASKS=1` nothing changes:
  `xrl_do_clone` sends fork-like clones to `xrl_do_fork` before a6 is looked at.
  Test: `layer/tests/glibc/glibc_test.c` gained fork + waitpid and a raw clone with r9 != 0 (passes on real Linux;
  not run by CI because the linux job does not build `tests/glibc` - main session: worth adding).
- `ls -la /`: symlinks showed owner 1000:1000 (everything else root:root). lstat of a table symlink now takes its
  owner from `.xrl-meta` like files do (only when the tree has a `.xrl-meta`; the RL prefix has none).

### Milestone 2 reference (`apt-get update && apt-get install -y hello` in the chroot, strace -f, 20k lines)
- Not in the layer's table: `sync_file_range` (dpkg; ignores the result), `rseq`.
- Sockets: AF_INET stream/dgram (http method + DNS), AF_NETLINK (getaddrinfo AI_ADDRCONFIG; layer answers
  EAFNOSUPPORT, glibc then assumes v4+v6), AF_UNIX (nscd/userdb probes).
- exec chain: apt-get -> /usr/lib/apt/methods/{http,store,sqv} -> sqv; dpkg (15x), dpkg-deb, dpkg-split, tar, rm,
  dpkg-preconfigure (perl), apt-extracttemplates; apt drops to user `_apt` (setresuid/setgroups).

## 2026-10-30

### What the first CI run on main showed (run 1, a81e799)
- Build and guest suite: OK (TOTAL FAILS=0).
- Every Debian probe: exit 127, `elf: cannot open /bin/bash` / `cannot open /lib64/ld-linux-x86-64.so.2`.
  Cause: not the layer. `tools/xrl-rootfs-unpack.py` stopped at the first case collision
  (`/usr/share/man/man7/pam.7.gz` vs `PAM.7.gz`, the only one in the minbase tree) before writing `.xrl-symlinks`,
  so the usrmerge links (`/bin`, `/lib`, `/lib64`, `/sbin` -> `usr/...`) did not exist ("4 symlinks" in the vfs line =
  only the built-in /dev ones). The CI step ignored the unpacker's exit code.

### Changes on this branch
- `tools/xrl-rootfs-unpack.py`: a name that differs only in case from one already unpacked is skipped and listed
  instead of aborting the whole unpack. (Shared tool; no effect on trees without collisions, e.g. Alpine or the RL
  prefix.)
- `tools/ci/rootfs.sh`: the rootfs carries `/etc/dpkg/dpkg.cfg.d/01-xrl-nodoc` (no man/info/doc/locale, copyright
  files kept), and those trees are removed after debootstrap: 51 MB -> 31 MB, no collisions left, and later
  `apt install`s inside the layer will not unpack man pages either.

### Reference (real Linux, Debian trixie minbase chroot, `strace -f`)
All 7 probes exit 0. Syscalls used: openat close read mmap newfstatat fstat rt_sigaction mprotect rt_sigprocmask brk
write statx pread64 prctl access llistxattr ioctl set_robust_list munmap socket prlimit64 lseek connect execve
set_tid_address rseq getuid getrandom getgid geteuid getdents64 arch_prctl getpid getegid exit_group statfs readlink
wait4 getppid fcntl dup2 clone uname umask rt_sigreturn pipe2 getpgrp getpeername getgroups getcwd fadvise64 futex.
All are in the layer's table except rseq (deliberate ENOSYS; glibc falls back). Files: `ps` reads
/proc/N/{stat,status,cmdline,environ}, /proc/stat, /proc/meminfo, /proc/sys/kernel/pid_max, /proc/self/task;
`id` connects AF_UNIX to nscd/systemd-userdb sockets (ENOENT expected, falls back to /etc/passwd).

### Known gaps ahead (not yet hit)
- Case-insensitive host FS inside the layer: packages that ship names differing only in case (e.g. `ncurses-term`
  terminfo dirs, some perl/python trees) will collide when dpkg unpacks them in the layer. Will need a VFS-side
  answer before milestone 3/4 (escape the case of colliding names, like the U+F0xx scheme).

### Needed from the main session
- Nothing yet. Suggest: make the CI "desktop run" step fail loudly when the unpacker exits non-zero (I left ci.yml
  alone).

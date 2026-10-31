# Notes from the main session to the desktop session

Newest first.

## 2026-10-31 (third TV run) - the app died of WINDOWS commit with only 1.5 GiB guest committed

- Xbox, A+B build c3da1bc + the pointer bridge: XFCE + Application Finder + launching apps, at t=104 s the layer
  logged `310 MiB of Windows commit left: swapping toward 1374 MiB (budget 4096)` while guest committed was 1550 MiB,
  then the process was gone (no layer error: the OS ended it). So ~3.4 GiB of the 5 GiB partition was outside guest
  mappings. Just before: 143 forks so far; pid 2948 forked three times in 2 s with copy-on-write snapshots of
  72/111/128 MiB (44-67 MiB "written meanwhile put back" each).
- Suspects: host-side memory the guest numbers do not show: CoW snapshot save pages, rewrite pools per process,
  shared-section views, thread stacks of the host side, swap buffers. Please add the partition's commit (and "outside
  guest mappings") to the periodic memory line and CI's numbers, then find what grows. The CI peak numbers only count
  guest memory, so they looked fine.

## 2026-10-31 (second TV run) - pointer done on branch claude/xbridge-cursor: please merge

- Branch `claude/xbridge-cursor` (one commit on top of your d9dc7ce): xbridge draws the pointer into /dev/fb0
  (QueryPointer each frame, an arrow over the true picture, erased on move, redrawn under rewritten rows) and links at
  0x10000000 instead of 0x400000. Checked on the Xbox: the arrow shows on the TV, and cpp-14 (xrdb) now starts, so
  xfdesktop runs (desktop icons appear; no wallpaper set yet). Merge it before changing xbridge further.
- USB mouse: Xbox does not deliver hardware mouse input to UWP apps at all (0 CoreWindow pointer events, 0
  MouseDevice moves; Microsoft confirms only whitelisted titles get it). Keyboard works. Plan for a real mouse: a
  network input client in the Debian session (Input Leap / Barrier client from a PC, or KDE Connect's remote input
  from a phone). Add input-leap (client) and kdeconnect to the package lists and a way to start them in the session.
- Firefox prefs are now system-wide on the console by hand (/etc/firefox-esr/firefox-esr.js); YouTube loaded without
  running out of memory. Make that permanent in the rootfs.

## 2026-10-31 (first TV run on the Xbox, A + B build c3da1bc, old CI rootfs + new xbridge/xsession.sh)

The XFCE session shows on the TV (panel after 14 s, 386 MiB); the controller moves an invisible pointer. Issues, A:
1. **No mouse pointer** (top usability issue): draw the cursor into fb0 (XFixesGetCursorImage + CursorNotify), B's
   request 4.
2. **Black desktop background**: xfdesktop is not drawn. Possibly 3: `E layer: exec: /usr/bin/x86_64-linux-gnu-cpp-14
   is not position independent and needs [400000,521000), which address-space 12 (another live process) uses` -
   xrdb's cpp collides with the non-PIE xbridge at 0x400000. Build xbridge as PIE (or load it elsewhere).
3. **Firefox from the menu ran out of memory on YouTube**: 320 threads, committed 4096/4096 MiB, "cannot swap in",
   lazy commit refused, app gone. The memory prefs only apply to xsession.sh's own profile: put them system-wide
   (/etc/firefox-esr/ syspref.js or policies.json), and measure a YouTube page in CI (or another heavy page).
4. "Choose Preferred Application" on the dock's web browser: set Firefox as the default (xfce4 helpers.rc).
5. The USB mouse delivered no events at all (keyboard untested): host side, the main session is probing which input
   source the console uses.

## 2026-10-31 (for B) - video decode/encode, and the first Xbox TV test

- The user asks for hardware video decode and encode. The console's answers are already measured (the user's
  XboxProbe report, section on video): **D3D12 video decode works** in a Dev Mode UWP app: HEVC Main/Main10 to 8K,
  VP9 profile 0/2 to 2160p, H.264 to 1080p only, **no AV1**; 64 concurrent 1080p sessions. **No D3D12 video encode**
  (H.264/HEVC/AV1 all false); H.264 hardware encode exists only through Media Foundation (1080p60 at ~181 fps),
  the HEVC encoder MFT gives no output. Do not call Windows.Media.Core.CodecQuery (crashes) or unguarded
  MFTEnumEx(VIDEO_DECODER) (access violation) on the console.
  So: Mesa's d3d12 VA-API frontend for decode (-Dgallium-va=enabled, video-codecs h264dec,h265dec,vp9dec), tested in
  CI with vainfo + ffmpeg -hwaccel vaapi on WARP if WARP offers video (else a clean "no profiles"); Firefox with
  media.av1.enabled=false so YouTube picks VP9 (hardware) instead of AV1 (CPU). Encode later, through a host op that
  wraps the Media Foundation H.264 encoder (1080p), not D3D12.
- The main session runs A + B (branch claude/desktop-gpu at c3da1bc) on the Xbox TV now: XFCE via xbridge, controller
  + USB keyboard/mouse; Plasma with your overlay as an experiment (copied in by hand; no Mesa: CI rootfs lacks it).

## 2026-10-31 (plan from the user) - priorities

1. Memory first: copy-on-write fork snapshots (protect + save on first write); shared Windows sections for file mmaps
   (MapViewOfFile3 into placeholders, FILE_MAP_COPY for MAP_PRIVATE); no snapshot for vfork/posix_spawn children;
   fix "copy into another process's memory ... pid -1"; Firefox prefs dom.ipc.processCount=2, fission.autostart=false.
   Goal: the Firefox session under ~2 GiB committed and a clean teardown.
2. TV output: bridge over the X protocol (MIT-SHM ShmGetImage + XDamage, x11vnc as reference) to /dev/fb0 with XTest
   input from event0-2 (controller as mouse + keyboard); prove in CI by reading fb0 back.
3. Looks: KDE Plasma 6 (X11) on Mesa llvmpipe first, polished theme; then GPU: Mesa d3d12 gallium (the WSLg approach)
   built with STDMETHODCALLTYPE = __attribute__((ms_abi)), the ID3D12Device from the layer's host op table
   (XRL_SYS_GET_GFX_OPS), present via Mesa's xlib/MIT-SHM software winsys; later Dozen (Vulkan). WARP in CI.
4. Speed: cache rewritten library code across loads/runs; cheaper fork/exec.
5. Later: audio, persistent /home, on-screen keyboard, Steam/Proton.
Shared code must not change RL's behaviour (XRL_FORK_TASKS); I test shared changes on the Xbox before main.

## 2026-10-31 (Xbox result) - milestones 1-5 PASS on the console

- Branch `claude/xbox-desktop-test` (your branch + the RL work), app v1.5.441, your CI rootfs (run 41174414886) in
  LooseApps/xbox-rl/debian, budget 4096 MiB, XRL_FORK_SNAPSHOT_MB=2048. Times on the console: bash/ps/dpkg ~1 s each,
  `apt-get update` 8 s, install hello 7 s, xterm found 3 s after Xvfb, XFCE panel 19 s, desktop 23 s, Firefox page
  window 90 s, screenshot at 112 s (correct: green page, XFCE panel).
- Then the app died: memory. Committed reached ~4.07 GiB of the 4096 MiB budget, Windows commit left ~460-800 MiB,
  swap on, lazy-fault handler at 2-3.4 s per s; while the session was being torn down (SIGTERMs, a fork for `sleep`)
  a lazy commit could not be backed (`mem-fail[0] ... lazy=1 committed=0`) and the process went. Also 12 x
  `E layer: copy into another process's memory: ... by pid -1 tid -1 (address space 1) in ?` around Firefox's
  content processes (t=75 s). So the copy-on-write fork snapshot from your known gaps is now the top item (it is
  what fills the console), then the TV bridge. Ask me for log excerpts.

## 2026-10-31 (later) - all five milestones: great work. Next: the desktop on the TV

- The Xbox test of milestones 1-5 (headless, Xvfb, screenshots pulled back) is being prepared by me now, on a merge
  of your branch with the RL performance work (branch `claude/xbox-desktop-test`; mem.c kept both sides). Results go
  here.
- Your next task: show the session on the TV. The Xbox app presents `/dev/fb0` (gfx/src/fbpresent.cpp) and feeds
  `/dev/input/event0-2` from the controller/keyboard/mouse. `layer/tests/guest/xbridge.c` (docs/decisions.md,
  2026-10-26) bridges an X server to fb0 + evdev, but it reads Xvfb's `-fbdir` screen file; the layer's file mmaps
  are copies, so Xvfb's writes may not reach a reader. Make the bridge take the picture over the X protocol instead
  (MIT-SHM `ShmGetImage` if the layer's shm allows it, else `GetImage`, changed rows only), keep its XTest input,
  and add an `xsession.sh tv` mode (Xvfb 1280x720 or 1920x1080 + bridge + XFCE). Prove it in CI by reading
  `/dev/fb0` back (the test host's fb0) as a screenshot. Then I run it on the Xbox.
- Keep the RL path unchanged (XRL_FORK_TASKS) as before.

## 2026-10-31 (3)

- New workflow `ltp.yml`: the Linux Test Project's ~1400 syscall tests (LTP 20260930) run inside the layer on main
  (and by hand with a name regex). Results: `ci-results:ltp/latest/` (`summary.txt`, `ltp-results.txt` one line per
  test, `ltp-fails.txt` with the tail of each failure). Use it as a gap list alongside your strace references.
- Small change in your file `tools/ci/rootfs.sh`: `ROOTFS_LIST=<file>` picks another package list (LTP uses
  `tools/ci/rootfs-ltp.list`); the default is unchanged.

## 2026-10-31 (2)

- **Merged into main** (b146c45): your branch up to d3299cb together with the game branch. Xbox gate passed with a
  fresh login: EAC 301 + `bAntiCheatEnabled=(True)` in both runs, menu 60 fps, 76.7 fps with the 120 Hz pacer.
  Please merge main back into your branch.
- **Case collisions, branch `claude/desktop-case` (1a31ac1, on top of 49fe302):** `xrl_vfs_symlink` refused a link
  whose name differs only in case from an existing host file (EEXIST from the case-insensitive host). Now the
  table link is added (it is looked up first and case-sensitively). Needed when dpkg installs such a package inside
  the layer (Debian's 385 X/XFCE/Firefox packages have 2 such names). Guest suite test included, TOTAL FAILS=0 on the
  laptop. Merge it if you agree.
- **Bug for you (milestone 2 territory, not yet fixed): `apt-get install thunar` inside the layer hangs** in
  `dpkg-deb --fsys-tarfile`. Laptop repro in seconds with the CI rootfs:
  `bash -c 'dpkg-deb --fsys-tarfile /var/cache/apt/archives/libgtk-3-common_*.deb | wc -c'` (any .deb bigger than
  the 64 KiB pipe). dpkg-deb forks two children that do not exec (the data-member copier, then the decompressor) and
  they deadlock: hang dump shows the parent inside xrl_proc_clone waiting for the second child, the first child
  waiting with no syscall frame (swap baton?), the second blocked in a syscall. `hello` passes because it fits the
  pipe. I tried growing the pipe when the writer has `vfork_parent` set: it never triggered (the first child's parent
  had already been released), so the wait is elsewhere (likely the fork group's baton or the multi-threaded-parent
  "child runs to completion" rule in proc.c). Yours if you want it; tell me in REPORT.md if you'd rather I take it.

## 2026-10-31

- Milestones 1 + 2: well done. Reviewed your shared-code changes on a merge of your branch (f30d437) with main and
  the game branch: rawsys.asm is safe for XRL_FORK_TASKS (fork-like clones reach xrl_do_fork first, thread clones
  always pass a stack); guest suite TOTAL FAILS=0 on the laptop; Rocket League on the laptop runs (55 fps) with your
  mem.c + vfs.c. The Xbox RL + EAC run is still pending: the console sends remote launches to the background right
  now (idle console; needs the user back). I merge into main after that run passes.
- Your request is in: the CI builds `layer/tests/glibc` and runs glibc_test + cxx_test, plain and XRL_DBT=1
  (`glibc-tests.txt`). Merge main to get it.
- Go ahead with milestone 3.

## 2026-10-30 (CI change)

- Done as you suggested: the desktop-run step now writes `UNPACK FAILED (exit N)` at the top of `desktop-run.txt`
  when the unpacker fails. Good find on the case collision; your VFS case-escaping plan for dpkg-unpacked names is
  the right direction (keep the RL prefix and Alpine trees behaving exactly as now).

## 2026-10-30 (later)

- The CI works end to end (run 41136668683 on main): layer build, guest suite (exit 0), Debian rootfs, probes,
  results in `ci-results:main/latest/`. First evidence for milestone 1: every probe exits 127 right after the layer
  boots (see `probe-1.txt`). Debian is usr-merged (`/bin`, `/lib`, `/lib64`, `/sbin` are symlinks into `/usr`):
  check how the layer resolves a symlinked directory in the program path and the ELF interpreter path
  (`/lib64/ld-linux-x86-64.so.2`). The unpack also hit case collisions (`pam.7.gz` vs `PAM.7.gz`): NTFS is
  case-insensitive; harmless for man pages, but note any that matter.

## 2026-10-30

- You were started right after the CI went in; its first run on main was still going. If the CI is broken, it is mine to fix: say so in REPORT.md and keep doing the reference (strace) work meanwhile.

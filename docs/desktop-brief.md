# Desktop work: brief for the desktop session

You are the desktop developer of xbox-rl: a Linux compatibility layer that runs inside a UWP app on an Xbox Series S
in Developer Mode. Read `README.md`, then `docs/decisions.md` (newest entries first), `docs/syscall-abi.md` and
`layer/README.md` before changing anything.

## Goal

A real Debian 13 desktop (XFCE on X11) running on the layer, where the user can install programs with `apt`
(first targets: a terminal, a file manager, Firefox). Rocket League keeps working throughout.

## How the pieces fit

- `layer/` is the fake Linux kernel: Linux programs run natively on the CPU and their kernel calls are answered with
  Windows functions. It is a Windows program; on a PC it runs through the desktop test host
  (`layer/build/desktop/xrl_test_host.exe <rootfs> <program> [args]`).
- `host/` is the Xbox app, `gfx/` its graphics module. The desktop's screen is `/dev/fb0` (layer/src/fbdev.c ->
  gfx/src/fbpresent.cpp) and its input `/dev/input/event*` (layer/src/evdev.c), both already built.
- Already working (laptop): several Linux processes at once (fork/exec/wait, layer/src/proc.c), AF_UNIX sockets,
  SysV/POSIX shared memory, ptys, Unix modes/owners, and an Alpine Xvfb + twm + xterm session with a screenshot
  (`layer/tests/xsession-test.sh`). Alpine is musl; Debian is glibc: expect loader/startup differences.

## Your loop (evidence first, never guess)

1. **Reference:** run the program on real Linux in your own session, under `strace -f -o ref.txt`, and keep what it
   needs (syscalls, files under /proc, /sys, /dev, sockets).
2. **Push** your branch. CI (`.github/workflows/ci.yml`) builds the layer on a GitHub Windows machine, builds the
   Debian rootfs from `tools/ci/rootfs.sh` + `tools/ci/rootfs-debian.list` (cached), and runs the probes in
   `tools/ci/desktop-run.sh` on the layer.
3. **Read the result:** `git fetch origin ci-results && git show origin/ci-results:<branch>/latest/desktop-run.txt`
   (`/` in the branch name becomes `_`). Also there: `build.txt`, `guest-suite.txt`, one `probe-N.txt` per probe,
   `enosys.txt` (every syscall the layer answered "not supported", counted: your gap list), `RUN.txt` (which commit).
   CI takes a while: check `RUN.txt` names your commit before reading.
4. **Compare** the layer's log with the reference, fix the first difference that matters, add a test when the fix is
   in the layer (`layer/tests/`), push again.

To run one command instead of the probe list, use the workflow's manual trigger with `cmd` if you can reach the
GitHub API; otherwise edit the probe list.

## Milestones (each has a pass line in desktop-run.txt)

1. bash, coreutils, `ps`, `dpkg -l` run (the initial probe list).
2. `apt-get update && apt-get install -y <small package>` works inside the layer (network, dpkg scripts, fork/exec).
3. Xvfb (or Xorg on /dev/fb0) + xterm: a window exists (`xwininfo -root -tree`) and an `xwd` screenshot is saved to
   the out dir (`tools/xwd2png.py` converts it).
4. An XFCE session (`startxfce4` with dbus) on Xvfb: panel and desktop in the screenshot.
5. Firefox opens a local page in that session.

The Xbox runs (fb0 on the TV, controller/keyboard input) are done by the main session on the user's laptop: write in
`REPORT.md` what to run there when a milestone passes in CI.

## Staying in touch with the main session

The main session (on the user's laptop, with the Xbox) cannot message you directly. It talks through the repo:
- Each loop, `git fetch origin main` and read `docs/desktop-notes.md` there (create nothing in it; it is the main
  session's file). New notes are at the top, dated.
- You talk back through `REPORT.md` on your branch; the main session reads it with `git fetch`.
- The first CI run on `main` was still in progress when you were started; if `ci-results` has no `main/latest/` yet
  or the CI itself fails (not your code), note it in `REPORT.md` and continue with the reference work; the main
  session fixes the CI.

## Rules

- Work only on the branch `claude/desktop-debian` (from `main`). Never push to `main`; the main session merges after
  checking Rocket League + EAC on the laptop and the Xbox.
- Do not change the Rocket League/EAC process path: everything behind `XRL_FORK_TASKS=1` (layer/src/task.c, the exec
  hand-over, the memfd copy mapping) must behave exactly as now. New behaviour that could reach it goes behind its own
  switch or applies only when `XRL_FORK_TASKS` is off. Ask in `REPORT.md` if a change must touch shared code.
- Never commit game files, EAC files, logins, tokens, screenshots that show account names, or console system files.
  The repo is public.
- Keep `include/xrl_abi.h` compatible (host and layer are built separately); say so in `REPORT.md` if you bump it.
- Match the code around you: comment density, naming, C style of `layer/src`.
- Record decisions in `docs/decisions.md` (newest first) and progress in `REPORT.md` at the repo root of your branch:
  milestone status, the current gap list, what you need from the main session.
- No time estimates.
- **Keep going.** Work through the milestones in order without stopping between them: a milestone that passes in CI
  is the cue to start the next one, not to end your turn. Requests to the main session (an Xbox check, an RL + EAC
  check, a merge) never block you: write them in `REPORT.md` and continue on your branch. Stop only when every
  remaining step needs something only the main session or the user can do, and say exactly what in `REPORT.md`.

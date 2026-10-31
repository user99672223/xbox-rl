# Notes from the main session to the desktop session

Newest first.

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

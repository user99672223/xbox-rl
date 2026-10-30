# Notes from the main session to the desktop session

Newest first.

## 2026-10-30 (later)

- The CI works end to end (run 41136668683 on main): layer build, guest suite (exit 0), Debian rootfs, probes,
  results in `ci-results:main/latest/`. First evidence for milestone 1: every probe exits 127 right after the layer
  boots (see `probe-1.txt`). Debian is usr-merged (`/bin`, `/lib`, `/lib64`, `/sbin` are symlinks into `/usr`):
  check how the layer resolves a symlinked directory in the program path and the ELF interpreter path
  (`/lib64/ld-linux-x86-64.so.2`). The unpack also hit case collisions (`pam.7.gz` vs `PAM.7.gz`): NTFS is
  case-insensitive; harmless for man pages, but note any that matter.

## 2026-10-30

- You were started right after the CI went in; its first run on main was still going. If the CI is broken, it is mine to fix: say so in REPORT.md and keep doing the reference (strace) work meanwhile.

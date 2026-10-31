# Notes from the main session to the desktop session

Newest first.

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

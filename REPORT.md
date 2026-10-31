# Desktop session report (branch `claude/desktop-debian`)

Newest first. Read by the main session with `git fetch origin claude/desktop-debian`.

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

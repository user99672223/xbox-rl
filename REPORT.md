# Desktop session report (branch `claude/desktop-debian`)

Newest first. Read by the main session with `git fetch origin claude/desktop-debian`.

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

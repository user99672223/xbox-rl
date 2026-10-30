# Desktop session report (branch `claude/desktop-debian`)

Newest first. Read by the main session with `git fetch origin claude/desktop-debian`.

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

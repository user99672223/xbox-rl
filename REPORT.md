# Desktop session report (branch `claude/desktop-debian`)

Newest first. Read by the main session with `git fetch origin claude/desktop-debian`.

## 2026-10-30

### Milestone status
1. bash, coreutils, ps, dpkg -l - **in progress** (first fix pushed, waiting for CI).
2. - 5. not started.

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

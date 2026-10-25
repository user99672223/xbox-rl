# Syscall ABI (the B<->C contract in prose)

`xrl_syscall(nr, a1..a6)` in `include/xrl_abi.h` is the only path from the Wine
unix side into the layer. `nr` is a **Linux x86-64 syscall number**. Return
value follows the **Linux kernel convention**: `>= 0` (or a pointer) on success,
`-errno` in `[-4095, -1]` on failure. musl's patched `__syscall` returns this
verbatim and does its own errno split, so musl is unmodified above the shim.

## Why Linux numbers at all

We run Proton-GE's Wine *unix side* (the part that would normally talk to the
Linux kernel) built against musl. That code already speaks Linux syscalls. By
keeping the Linux ABI at the shim we reuse Wine+musl as-is; the layer is the
"kernel". The table is deliberately a Linux table so it can later grow into a
general Linux-program layer (brief: design for it, don't build it now).

## Implementation policy

Implement a syscall **only when Wine actually issues it** bringing RL up. Do not
pre-implement the Linux API. Each syscall starts as `ENOSYS` and the layer logs
every `ENOSYS` hit (number + name) so we see exactly what to add next. The loop
is: run -> read log -> implement the syscalls it asked for -> rerun.

## Expected first wave (what Wine's unix side hits on startup)

Grouped by the subsystem in the layer that owns them. Numbers are x86-64.

- **Threads/sync (futex is the hot path):** `futex`(202), `clone`(56)/
  `clone3`(435) [thread-only; no fork], `set_tid_address`(218),
  `set_robust_list`(273), `gettid`(186), `sched_yield`(24),
  `rt_sigprocmask`(14), `rt_sigaction`(13), `tgkill`(234), `exit`(60),
  `exit_group`(231).
- **Memory:** `mmap`(9), `munmap`(11), `mprotect`(10), `mremap`(25),
  `brk`(12), `madvise`(28). mmap arena lives inside `guest_mem_budget`; honor
  the console's VA hole (decisions.md).
- **Files/VFS (over LocalState):** `openat`(257), `open`(2), `close`(3),
  `read`(0), `write`(1), `pread64`(17), `pwrite64`(18), `lseek`(8),
  `stat`(4)/`fstat`(5)/`lstat`(6)/`newfstatat`(262), `access`(21)/
  `faccessat`(269), `getdents64`(217), `readlink`(89)/`readlinkat`(267),
  `fcntl`(72), `ioctl`(16) [TTY/winsize stubs], `dup`(32)/`dup2`(33)/
  `dup3`(292), `pipe`(22)/`pipe2`(293), `unlink`/`mkdir`/`rename` as the prefix
  needs. `/proc/self/*`, `/dev/null`, `/dev/urandom` (urandom seeded by host,
  since console has no RDRAND and firmware RNG is blocked).
- **Time/poll:** `clock_gettime`(228), `clock_nanosleep`(230),
  `nanosleep`(35), `gettimeofday`(96), `poll`(7)/`ppoll`(271),
  `epoll_create1`(291)/`epoll_ctl`(233)/`epoll_wait`(232) if Wine's server
  loop uses it, `eventfd2`(290).
- **Process/ident (mostly single-process stubs):** `getpid`(39),
  `getppid`(110), `getuid`/`geteuid`/`getgid`/`getegid`, `prctl`(157),
  `arch_prctl`(158) [FSBASE/GSBASE — real, needed for TLS],
  `uname`(63), `sysinfo`(99), `getrandom`(318).
- **Sockets:** only the Unix-domain pair Wine uses to talk to wineserver — and
  since wineserver is an in-process thread with an in-memory channel, this may
  reduce to `socketpair`(53) emulation or be bypassed entirely by the B patch.
  Decide at integration; log what Wine actually opens.

## wineserver channel

wineserver runs as a thread (B's patch). The request/reply channel is in-memory,
not a real socket. Where Wine's client would `send`/`recv` on the server fd, the
B patch redirects to the in-memory channel. If any Wine code path still expects
an fd, the layer provides a `socketpair`-backed emulation whose two ends are
serviced by the wineserver thread. This seam is owned jointly by B and C; record
the final shape here once it runs.

## arch_prctl / TLS

`arch_prctl(ARCH_SET_FS)` must actually set the thread's FS base (Wine + musl
TLS depend on it). On Windows the layer sets it via the host thread's GS/FS or a
context swap; this is a real implementation item, not a stub. Record the chosen
mechanism in decisions.md when C lands it.

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
  `clone3`(435) [threads, and processes: see "Guest processes"], `set_tid_address`(218),
  `set_robust_list`(273), `gettid`(186), `sched_yield`(24),
  `rt_sigprocmask`(14), `rt_sigaction`(13), `tgkill`(234), `exit`(60),
  `exit_group`(231).
- **execve**(59) (`exec.c`): replaces the process image inside the one OS process
  (other threads stopped, guest memory released, CLOEXEC fds closed, handlers reset,
  new ELF started); `ENOENT`/`ENOEXEC` come back before anything is destroyed. Layer-
  private `prctl` ops for Wine's in-process server: 0x58524c01 (this thread survives an
  exec) and 0x58524c02 (the next exec keeps the old image except the given ranges); see
  decisions.md "Exec handover". Layer-private pseudo syscall 0x10010 (`dbt.asm` asks for
  the translation of an address, `XRL_DBT`).
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
- **Process/ident (per guest process, see "Guest processes"):** `getpid`(39),
  `getppid`(110), `getuid`/`geteuid`/`getgid`/`getegid`, `prctl`(157),
  `arch_prctl`(158) [FSBASE/GSBASE — real, needed for TLS],
  `uname`(63), `sysinfo`(99), `getrandom`(318).
- **Sockets:** `socketpair`(53) AF_UNIX (in-memory, SCM_RIGHTS; ipc.c) and real
  **AF_INET / AF_INET6 TCP and UDP over Winsock** (net.c, added for Easy
  Anti-Cheat's module and any Linux program): `socket`(41), `connect`(42),
  `accept`(43)/`accept4`(288), `sendto`(44), `recvfrom`(45), `sendmsg`(46),
  `recvmsg`(47), `sendmmsg`(307), `recvmmsg`(299), `shutdown`(48), `bind`(49),
  `listen`(50), `getsockname`(51), `getpeername`(52), `setsockopt`(54),
  `getsockopt`(55); sockets are fds, so `read`/`write`/`readv`/`writev`/`poll`/
  `epoll`/`fcntl`/`ioctl(FIONBIO,FIONREAD)`/`dup`/`close` work on them. Linux
  constants and errnos are translated; blocking is done in the layer (interruptible,
  `SO_RCVTIMEO`/`SO_SNDTIMEO`), never inside Winsock. Not provided: AF_UNIX by
  path, raw/packet/netlink sockets (`-EAFNOSUPPORT`/`-EPERM`), ancillary data.

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

## Gateway contracts that are not 1:1 Linux (ABI v2, settled 2026-10-26)

These are what B's patched musl does and what the layer implements and tests
(`layer/tests/guest/guest.c`, section "abi v2 / wine contract").

- **`__xrl_gateway`.** libc.so (ld-musl) exports an 8-byte data symbol. The
  layer's ELF loader finds it through the image's PT_DYNAMIC (works on stripped
  images) and stores the address of the SysV `xrl_syscall` thunk there before
  any guest instruction runs, in the interpreter and in the main image. auxv
  `AT_XRL_SYSCALL` (0x5801) carries the same address for other guests.
- **7th argument.** `xrl_syscall` takes `nr` + `a1..a6`; `a6` is on the stack
  (SysV) and is always read. musl's stubs zero it when unused.
- **`clone` (56).** `xrl_syscall(56, flags, child_stack, ptid, ctid, tls,
  resume_pc)`. Threads (`CLONE_VM|CLONE_THREAD`) or, without `CLONE_THREAD`, a new
  process (see "Guest processes"). With `resume_pc != 0`
  the child starts at `resume_pc` with `rsp = child_stack`, `rax = 0`, FS base
  = `tls` under `CLONE_SETTLS`; other registers undefined; it never returns
  through the gateway. With `resume_pc == 0` the child "returns" from the call
  site instead (used by the layer's own test guest).
- **`exit` (60).** `xrl_syscall(60, status, map_base, map_size)`. The layer
  unmaps the region after leaving that stack and *before* the CLEARTID wake
  (Linux order). Because Linux ignores a2/a3 for exit, other callers leave
  garbage there: the region is honoured only if it is page-aligned, a page
  multiple, ≤ 256 MiB, contains the caller's stack pointer, and is mapped at
  both ends. (A permissive first version unmapped half the address space when
  a3 held a huge garbage value; see decisions.md.)
- **`rt_sigreturn` (15).** `xrl_syscall(15, ucontext_ptr)`. Not implemented yet
  (signal delivery is the next layer item).
- **`arch_prctl` FS/GS.** FS is real (wrfsbase + fault re-arm). GS: see the
  GS/TEB decision; `ARCH_SET_GS` is refused unless it names the host TEB.
- **No vDSO.** No `AT_SYSINFO_EHDR`; `clock_gettime` goes through 228.
- **Pseudo-syscalls** `0x10001`–`0x10004` (`XRL_SYS_GET_GFX_OPS`, `_AUDIO_OPS`,
  `_INPUT_OPS`, `_LOG_FN` in `xrl_abi.h`): return the guest-callable,
  SysV-stubbed copies of the host tables the layer builds at boot, or -ENOSYS.

## Guest processes (2026-10-26, `layer/src/proc.c`; design in decisions.md "Guest processes")

Many Linux processes live in the one Windows process and its one address space. `getpid()` of the process boot
starts is the Windows process id (as before); every other pid is the Windows thread id of the process's first thread
(unique for the layer's lifetime). `gettid()` is the Windows thread id. Everything below is Linux's behaviour unless a
line says otherwise.

- **`fork`(57), `vfork`(58), `clone` without `CLONE_THREAD`** create a process; the child returns from the call with 0
  (same mechanism as a thread clone: the gateway frame, or `resume_pc`). `vfork` / `CLONE_VM|CLONE_VFORK`: shared
  memory, the parent is suspended until the child execs or exits. `fork` / no `CLONE_VM`: the child runs on the
  parent's addresses and memory is kept apart by swapping (decisions.md, "fork groups"): the parent's memory is
  snapshotted at the fork, the parent runs again when the child execs, exits or blocks in the layer. `CLONE_FILES`
  shares the fd table, otherwise it is copied (CLOEXEC flags included); `CLONE_PARENT`, `CLONE_PARENT_SETTID`,
  `CLONE_CHILD_SETTID`, `CLONE_SETTLS` are honoured; `CLONE_NEW*` -> `-EPERM`. What a forked child does to its memory
  (writes, mmap, munmap) is not seen by the parent, as with a real fork (file mappings the child unmaps are not restored).
- **`execve`**: replaces the calling process's program. `FD_CLOEXEC` descriptors close in that process's table; handlers
  reset (`SIG_IGN` stays); other threads of the process stop; the old mm goes (a vfork/fork child leaves its parent's mm
  alone and takes a fresh one). A non-PIE image whose fixed range another live process uses fails with `-ENOMEM` (the
  layer logs the range). PIE and ld-musl load at a free base: one per process. comm = basename of the path given.
- **`wait4`(61), `waitid`(247)**: `pid` > 0, -1, 0, < -1; `WNOHANG`, `WUNTRACED`/`WSTOPPED`, `WCONTINUED`, `WNOWAIT`,
  `WEXITED`; blocking waits are interruptible and restart under `SA_RESTART`; `-ECHILD` when there are no children.
  Status is `exit << 8` or the fatal signal. `rusage` is zeros.
- **`exit`/`exit_group`/fatal signals**: the last thread or `exit_group` ends the process: its threads stop, its fds
  close (pipe readers see EOF), its mm goes, children are reparented (to the nearest `PR_SET_CHILD_SUBREAPER` ancestor,
  else the init process: the one boot started, whose exit ends the layer), `PR_SET_PDEATHSIG` is delivered, the parent
  gets SIGCHLD (the zombie is reaped at once if SIGCHLD is ignored or `SA_NOCLDWAIT`).
- **Signals**: dispositions are per process, masks and pending sets per thread. `kill(pid | 0 | -1 | -pgid)`,
  `tkill`, `tgkill` (the thread must belong to `tgid`). A default-action fatal signal ends the *target* process (before,
  the layer). SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU stop a process, SIGCONT continues it: the state and the parent's
  `wait4(WUNTRACED/WCONTINUED)` report are exact; the threads freeze at their next syscall return (a thread blocked
  in a long call stops when it returns, one running guest code is suspended at once). `rt_sigsuspend`(130) and
  `rt_sigtimedwait`(128) are implemented. A write to a pipe with no reader raises SIGPIPE (sockets: `-EPIPE` only).
  `rt_sigreturn` of a stock musl restorer (a bare `syscall`, after the rewriter) finds its frame through the guest rsp.
- **Ids, groups, sessions**: `getpgid`, `getpgrp`, `setpgid` (self or a child, same session), `getsid`, `setsid`
  (fails for a group leader), `setuid`/`setgid`/`setres[ug]id` store the ids (no privilege model), `getrlimit` /
  `setrlimit` / `prlimit64` are stored per process (inherited) and not enforced. `ioctl(TIOCGPGRP, TIOCSPGRP, TIOCSCTTY,
  TIOCNOTTY, TIOCGSID)` on `/dev/tty` give shells a foreground group (one terminal stand-in, the init process's group
  at first).
- **`/proc/<pid>/...`** (`cmdline`, `environ`, `comm`, `status`, `stat`, `statm`, `maps`, `fd/`, `task/`, `exe`, `cwd`,
  `root`, `limits`) for every live process; `/proc` lists them. `maps` shows only that process's mappings.
- **Not provided**: ptrace, namespaces, `unshare`/`setns`, pidfds, `process_vm_*`, `kcmp`, `sched_setaffinity` per
  process (accepted, ignored), a forked child that computes concurrently with its parent (see decisions.md).

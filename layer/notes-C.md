# Workstream C notes (layer/)

Everything the integrator should merge into docs/decisions.md and docs/syscall-abi.md
is in here. Status date: 2026-10-26.

## 1. Pins (layer/vendor.pins, fetched by layer/scripts/vendor.ps1)

| upstream | tag | commit |
|---|---|---|
| jart/blink | 1.1.0 | `7deaf5bf59766e6ac113997f5896bc9e77a2381d` |
| jart/cosmopolitan | 4.0.2 | `5907304049f37c9ed77593974d13202829443bea` |

`vendor.ps1` fetches `--depth 1` (Cosmopolitan additionally `--filter=blob:none` + sparse
checkout of libc/{nt,calls,intrin,thread,sysv,runtime,proc,sock,fmt,str,macros,log},
third_party/nsync), verifies the commit, applies `layer/patches/<name>/*.patch`, and
refuses to fetch with <1 GB free. On disk: blink ~11 MB, cosmopolitan ~58 MB (+49 MB .git).
`layer/vendor/*/` is git-ignored (layer/.gitignore); pins + patches are the source of truth.

### What is vendored and how it is *actually* used (honest)
Neither upstream is compiled in place; both are pinned provenance + reference:
* **Blink** `blink/loader.c`: its ELF loader is welded to Blink's emulated-machine page
  tables (ReserveVirtual/CopyToUser), so it cannot be linked. `layer/src/elf.c` is a
  clean-room port of its logic onto our mmap layer (per-PT_LOAD skew handling, bss,
  PT_INTERP + AT_BASE, PT_PHDR/AT_PHDR) and of Linux binfmt_elf's auxv/stack layout.
  We never use Blink's interpreter (guest runs native).
* **Cosmopolitan** `libc/{nt,intrin,calls}`: it needs Cosmo's own toolchain/headers (GCC,
  `__imp_` thunks, its own fd table), so it cannot be compiled by MSVC. We ported the
  *techniques*: Win32 error->errno table (`libc/intrin/dos2errno.c`, kDos2Errno),
  futex-on-WaitOnAddress, file/mmap-on-VirtualAlloc, and confirmed its Windows TLS
  scheme (`%gs:0x1480 + 8*__tls_index`, `libc/intrin/tls.c`). No Cosmo source is copied
  verbatim; ISC-licensed techniques credited in file headers.
* No vendor patches exist yet (`layer/patches/README.txt`).

## 2. Build / test

```
pwsh layer/scripts/vendor.ps1                      # fetch pins (idempotent)
pwsh layer/scripts/build.ps1 -Flavor both          # desktop + uwp; outputs layer/build/<flavor>/xrl_layer.lib
wsl -d Ubuntu -- sh -c "cd /mnt/c/Users/10harbad/Documents/xbox-rl/layer/tests/guest && sh build.sh"   # test guest ELF
layer/tests/run.sh                                  # (Git Bash) boots the guest under the desktop test host
```
* Toolchain: VS2022 cl 19.44 (C11), ml64 (MASM) for `asm/thunks.asm`, CMake+Ninja.
  `scripts/vsenv.ps1` imports vcvars64. MASM is invoked by an `add_custom_command`
  (CMake's ASM_MASM+Ninja emits a bad rule name on this CMake).
* **UWP flavor** = `-DXRL_UWP=ON` = `WINAPI_FAMILY=WINAPI_FAMILY_APP`; this is what host/
  links (`layer/build/uwp/xrl_layer.lib`, link `WindowsApp.lib bcrypt.lib synchronization.lib`).
  It compiles clean against the app-partition headers. `AddVectoredExceptionHandler` and
  `TerminateThread` are hidden by those headers but exported by WindowsApp.lib (verified
  with dumpbin); `xrl_internal.h` declares them for the UWP flavor.
* **Test status (desktop Win11, `tests/run.sh`): 70+ checks, 0 failures** (`TOTAL FAILS=0`):
  syscall dispatch, ENOSYS, mmap/mprotect/mremap/brk/madvise/budget, MAP_FIXED at
  0x140000000, file/dir/symlink/proc/dev VFS, pipe/eventfd/epoll/poll, socketpair +
  SCM_RIGHTS, 6 cloned threads with CLONE_SETTLS hammering `%fs:` across preemption
  (142 FS re-arm faults handled, 0 bad reads), futex ping-pong (~100k round trips/s),
  futex timeout/EAGAIN, host op-table downcalls incl. a 7-argument call.
  **Not run on the console** - the first console run must read the boot log lines
  `threads: FSGSBASE ...`, `rng: ...`, `mem: arena ...`.
* The test guest is a freestanding ELF that finds `xrl_syscall` through auxv exactly as
  B's patched musl must (see 3), and models B's `__clone` (`xclone` in guest.c).

## 3. The B<->C seam (the contract B builds against)

**3.1 Finding the syscall entry.** Three equivalent ways; use whichever fits:
1. auxv `AT_XRL_SYSCALL` (0x5801) = address of the SysV thunk `xrl_syscall`;
   `AT_XRL_CTX` (0x5802) = `struct xrl_guest_ctx*`; `AT_XRL_ABI` (0x5803) = ABI version.
   Required for the interpreter case (ld-musl relocates itself before anything else, so
   it cannot use symbol relocations): read it in `_dlstart_c`/`__init_libc` from `aux[]`
   and store it in a global before the first syscall.
2. For an interpreter-less image (static / static-PIE) the loader resolves undefined
   dynamic symbols named `xrl_syscall` (GLOB_DAT/JUMP_SLOT/64 relocs) at load time.
3. `xrl_guest_ctx.syscall`.
`AT_HWCAP2` is deliberately 0 (never advertise HWCAP2_FSGSBASE: Wine would try `wrgsbase`
on its TEB; see 5).

**3.2 Calling convention.** `xrl_syscall(nr,a1..a6)` is a **System V** call (the exported
symbol is a MASM thunk), `syscall` instruction replaced by `call *xrl_syscall`. The
thunk preserves rbx,rbp,r12-r15; everything else is caller-saved. It switches to the
thread's host stack, so the guest stack can be any mmap'd memory.

**3.3 clone (thread only).** Contract for B's `__clone` replacement: do
`call xrl_syscall(56, flags, child_stack, ptid, ctid, tls)`. The child "returns from
the call" with `rax=0`, `rsp=child_stack`, and rbx,rbp,r12-r15 **copied from the parent
at the call** (everything else undefined). Keep `fn` and `arg` in callee-saved regs
(r12/r13) across the call, like `tests/guest/guest.c: xclone`. Supported flags: needs
VM|THREAD; handles SETTLS, PARENT_SETTID, CHILD_SETTID, CHILD_CLEARTID (futex wake
on exit, so pthread_join works). fork/vfork/execve -> -ENOSYS with an error log.

**3.4 Thread exit / `__unmapself`.** `exit(60)` ends the calling thread (clears and
wakes ctid first). A `munmap` whose range contains the caller's own stack pointer is
*deferred to thread exit* and returns 0, so musl's `munmap; exit` sequence works with
`call`-based syscalls (the call itself needs the stack). `exit_group(231)`, or the last
thread exiting, makes `xrl_layer_boot` return the code.

**3.5 Host op tables.** `AT_XRL_CTX` points to `xrl_guest_ctx` (layout in
`layer/src/xrl_internal.h`; magic 'XRLG'). `gfx/audio/input` are *copies* of the host's
tables whose function pointers are generated SysV stubs (22-byte stub -> common
trampoline -> host stack -> Win64 call). Integer/pointer args only, <= 8 args
(verified with a 7-arg call). `log` is likewise stubbed.

**3.6 wineserver-as-thread hook.** If the main ELF still has its section headers and
exports the dynsym `xrl_wineserver_main` (SysV `int (*)(void)`), boot starts it on its own
guest thread (1 MiB stack, returns -> `exit`). Otherwise B starts the wineserver thread
itself with `clone`. Socket channel: `socketpair(AF_UNIX)` is real (in-memory, SCM_RIGHTS
works), so a socket-shaped channel is possible; B may bypass it with a direct in-memory
channel. Real AF_INET/AF_INET6 TCP/UDP sockets exist since net.c (Winsock; see
docs/syscall-abi.md "Sockets"); `socket(AF_UNIX)` other than `socketpair` is still
-EAFNOSUPPORT (logged once).

**3.7 Signals - NOT delivered yet.** `rt_sigaction/rt_sigprocmask/sigaltstack` record
state; `tgkill` marks pending and interrupts futex/poll waits but no handler runs;
hardware faults in guest code are caught by the VEH, **logged with a full register/VMA
crash report**, and the process exits with 128+signo. Wine needs SIGSEGV for write-watch,
stack guard and exceptions, plus SIGUSR1-style thread suspension; this is the next work
item. Design (not built): sync faults -> build `rt_sigframe` on guest/alt stack inside the
VEH, point CONTEXT at the handler; `rt_sigreturn` restores via a per-thread `jmp [rip+0]`
stub (no red-zone writes); async delivery on syscall return by retargeting the thunk's
return address, and into running guest code via SuspendThread/SetThreadContext.

**3.8 Auxv/stack** match Linux binfmt_elf: AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/UID...
/PLATFORM/HWCAP/CLKTCK/SECURE/RANDOM(16 bytes)/EXECFN/MINSIGSTKSZ + the three XRL ones.
No AT_SYSINFO_EHDR (no vDSO; musl falls back to the syscall). Initial stack 8 MiB.

## 4. FS base - mechanism and measurements (the important finding)

Windows x64 does not use FS, but **it also does not save/restore the FS base across context
switches**. Measured on Win11 (CR4.FSGSBASE is enabled, `rdfsbase/wrfsbase` work): a thread
that `wrfsbase`s a value reads 0 again after ~40-700 microseconds, always (4 threads, 4 cores).
So `arch_prctl(ARCH_SET_FS)` cannot be a one-shot `wrfsbase`. Chosen mechanism:

1. `thread->guest_tp` is authoritative (set by arch_prctl / CLONE_SETTLS).
2. `wrfsbase` on arch_prctl, on thread start, and on every return from `xrl_syscall` (asm).
3. A vectored exception handler (`signal.c`): a guest `%fs:` access with base 0 faults
   (`%fs:0..0xffff` is unmapped; negative offsets are non-canonical -> both reported as
   ACCESS_VIOLATION). If `rdfsbase != guest_tp` it rewrites the base and resumes the same
   instruction. Only faults whose RSP is outside the host stack and that happen outside
   layer code are considered.
Measured: 32 threads x 4 s, ~20 billion `%fs:0`/`%fs:-8` reads, 0 wrong, 3157 recoveries
(prototype); in the layer test 142 recoveries in a ~1 s run with 6 threads. Cost = one cheap
exception per scheduler preemption, not per access.

**Risk to verify on the console first:** FSGSBASE usable there? (Zen2 supports it; the OS
must have CR4.FSGSBASE set. Boot logs `FSGSBASE usable|NOT usable`.) If not usable,
`arch_prctl(SET_FS)` and `CLONE_SETTLS` fail with -ENOSYS (loud) and the fallback is
Tier 3: B patches musl `__get_tp` / compiles with `-mno-tls-direct-seg-refs` and reads
the thread pointer from `thread->guest_tp` via the host TEB (`gs:[0x30]` -> hash table
`xrl_ttab`), or decoder-based trap-and-emulate of `%fs:` accesses (Blink's x86 decoder is
vendored for that). Not built.

## 5. GS / TEB - unresolved architectural risk (needs integrator + B)

Wine on x86-64 Linux points `%gs` at its NT TEB (`arch_prctl(ARCH_SET_GS)` or `wrgsbase`);
every PE module (game, Wine's ntdll/kernelbase) reads `%gs:0x30/0x58/0x60/0x68...`.
On Windows `%gs` *is* the host TEB, re-installed by the kernel on every context switch, and
the host's own code (CRT, D3D, WinRT, our thunks' `gs:[30h]`) reads it. Unlike FS there is
no fault to hook: a `wrgsbase` would be silently undone. Therefore:
* Layer behaviour: `arch_prctl(ARCH_SET_GS, x)` succeeds only if x is the thread's own host
  TEB (or 0); otherwise -EPERM with a one-time ERROR explaining why. `arch_prctl(0x5801,&p)`
  returns the host TEB address; `0x5802` returns the FS mode.
* The only silent-failure-free design is **adoption**: Wine uses the host TEB as its TEB
  (no allocation of its own) and the layer swaps the host-owned fields on every
  guest<->host transition (candidates: StackBase/StackLimit/DeallocationStack, PEB ptr
  0x60, ClientId 0x40/0x48, ThreadLocalStoragePointer 0x58, TlsSlots 0x1480.., LastError).
  This needs a Wine patch (B) and a small swap list in the thunks (C) - ~20 ns per
  transition. Not implemented; the layer's own thread lookup deliberately uses only
  `gs:[30h]` (TEB Self), never TlsSlots, so it survives the guest scribbling on the TEB.
This is the largest correctness question in the project after FS base.

## 6. Futex

`futex.c`: a miniature of the kernel futex hash table (512 buckets, SRW lock each).
WAIT: lock bucket, re-read `*uaddr`, `!= val` -> -EAGAIN, enqueue, unlock, park on a
per-waiter word with `WaitOnAddress`. WAKE: dequeue up to N matching waiters, set their word,
`WakeByAddressSingle`. Exact wake counts, bitsets, no lost wakeups, no foreign spurious
wakes. Ops: WAIT, WAKE, WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE, WAKE_OP,
PRIVATE/CLOCK_REALTIME flags. PI futexes and futex_waitv -> -ENOSYS (Wine disables fsync
on that ENOSYS - desired). Timeouts: millisecond granularity (WaitOnAddress), rounded up.
Sleeps (`nanosleep`, `clock_nanosleep`) use a per-thread high-resolution waitable timer
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, falls back to a normal timer, then `Sleep`).

## 7. Memory (mem.c)

* 1 TiB (falling back to smaller) `MEM_RESERVE|MEM_TOP_DOWN` arena at boot; address-less
  mmaps are carved top-down by our own VMA list; low 1/16 (<=1 GiB) is the brk heap.
* MAP_FIXED / hinted mappings outside the arena (PE images at e.g. 0x140000000) reserve the
  covering 64 KiB granules individually and release them when unused. Fails -> -ENOMEM.
* PROT_NONE anonymous = reserve only; mprotect to accessible commits; mprotect(NONE) keeps
  pages committed (data preserved). File mappings are copy-in; shared writable file maps
  are written back at munmap/msync (not coherent).
* `guest_mem_budget` bounds *committed* bytes (Windows commit charge); over -> -ENOMEM.
* **Open risk:** Linux over-commits, Windows charges at commit. If Wine/UE3 commit large
  regions they never touch, the 5 GB console runs out earlier than Linux would. Planned
  mitigation: lazy commit (reserve RW anonymous, commit on first touch from the VEH).
  Not built; watch the `mem:` log and `/proc/self/status VmRSS`.
* madvise(DONTNEED/FREE) zeroes (decommit+recommit for >=64 KiB so memory is returned).

## 8. VFS

* Guest `/` = `<vfs_root>`; paths are `\\?\`-prefixed. Only LocalState is touched.
* `/proc`, `/sys` synthesized (maps from VMAs, cmdline, environ, status, stat, statm, comm,
  limits, mounts, cpuinfo (Zen2, 8 cpus), meminfo, stat, uptime, loadavg, version,
  `/proc/self/{exe,cwd,root,fd/N,task}`, sysctl bits, `/sys/devices/system/cpu/...`).
  Unknown paths -> -ENOENT (with `XRL_TRACE=1` the log names them).
* `/dev/null,zero,urandom,random,tty,console,full`; `/dev/shm`, `/tmp` are real dirs;
  stdin=/dev/null, stdout/stderr -> host log (`guest.out`/`guest.err`, line-buffered).
* **Symlinks** live in `<vfs_root>\.xrl-symlinks` (UTF-8 lines `GUEST_ABS_PATH<TAB>TARGET`,
  loaded at boot, appended to by `symlink(2)`). **The prefix baker must emit this file**
  for Wine's `dosdevices/c:` etc. They appear in getdents as DT_LNK and in lstat/readlink.
* **Name escaping**: `: * ? " < > | \` and control chars (and a trailing space/dot) are stored
  as U+F000+char; decoded in getdents. So `dosdevices/c:` is a legal NTFS name.
* Case-insensitive host; `st_ino` from NTFS FileId; `d_ino` is a path hash.
  Modes: dirs 0755; files 0644 (0755 for .exe/.dll/.so/.sh/...); READONLY attr clears w bits.
* File locks (`fcntl F_SETLK`, `flock`) are advisory no-ops; chmod/chown are no-ops.

## 9. Syscalls: real vs stub

Real (implemented; names are Linux x86-64):
* threads/sync: futex, clone, clone3, set_tid_address, set_robust_list (recorded only),
  get_robust_list, gettid, sched_yield, exit, exit_group, wait4 (-ECHILD), kill/tkill/tgkill
  (record+interrupt, no handler), pause
* memory: mmap, munmap, mprotect, mremap, brk, madvise, msync, mincore, mlock* (no-op)
* files: open, openat, creat, close, close_range, read, write, pread64, pwrite64, readv,
  writev, preadv, pwritev, lseek, stat, fstat, lstat, newfstatat, statx, access, faccessat,
  faccessat2, getdents64, readlink, readlinkat, fcntl, ioctl (FIONBIO/FIOCLEX/FIONREAD/
  tty->ENOTTY), dup, dup2, dup3, pipe, pipe2, eventfd, eventfd2, memfd_create, unlink,
  unlinkat, mkdir, mkdirat, rmdir, rename, renameat, renameat2, symlink, symlinkat, link,
  linkat, chdir, fchdir, getcwd, truncate, ftruncate, fsync, fdatasync, fallocate(mode 0),
  utimensat, utimes, statfs, fstatfs, umask
* poll/sockets: poll, ppoll, epoll_create/create1/ctl/wait/pwait (level + ET + ONESHOT),
  socketpair(AF_UNIX), sendmsg/recvmsg (iovec + SCM_RIGHTS), sendto/recvfrom (no address),
  shutdown, setsockopt/getsockopt/getsockname (stubs that succeed)
* time: clock_gettime, clock_getres, clock_nanosleep, nanosleep, gettimeofday, time, times,
  getrusage; clock_settime -> -EPERM
* process/ident: getpid, getppid(=1), get/set uid/gid family (1000), getpgid/getsid,
  prctl (NAME, no-op sets), arch_prctl (FS real; GS adoption only), uname, sysinfo, getrandom
  (ChaCha20, BCrypt+XRL_RNG_SEED seeded), getrlimit/setrlimit/prlimit64, sched_getaffinity
  (8 cpus), sched_*priority/param stubs, getcpu, personality
Recorded but with no effect: rt_sigaction, rt_sigprocmask, sigaltstack (see 3.7).
Stub/ENOSYS (logged as `ENOSYS <nr> <name>` - first 4 hits then power-of-two hits):
rt_sigreturn, rt_sigsuspend, rt_sigtimedwait, select/pselect6, getdents(78), sendfile,
copy_file_range, inotify*, timerfd*, signalfd*, real sockets (socket -> -EAFNOSUPPORT;
connect/bind/listen/accept/accept4 -> ENOSYS), fork/vfork/execve (by design), PI futexes,
futex_waitv, modify_ldt/set_thread_area, rseq, everything else.

## 10. Build gotchas worth keeping
* MSVC `long` is 32-bit: all Linux-width values use `slong`/`s64`.
* `__reserved` is a macro in windows.h (SAL); lx.h avoids it.
* CMake 4.x + Ninja + ASM_MASM generated an invalid rule name; MASM is a custom command.
* Git Bash rewrites `/bin/x` args to `C:/Program Files/Git/...`; `tests/run.sh` sets
  `MSYS_NO_PATHCONV=1`.

## 11. Open questions
1. Is FSGSBASE usable on the console? (first thing to read from the boot log)
2. GS/TEB adoption design (section 5) - owner B + integrator; C supplies swap hooks.
3. Signals (3.7): confirm Wine's usage in the first run (`ENOSYS`/crash report tells us).
4. Commit-charge vs 5 GB (section 7): lazy commit needed?
5. Does `BCryptGenRandom` work in the Xbox app container? (else host must pass XRL_RNG_SEED)
6. High-resolution waitable timers on the console OS? (falls back automatically)
7. `\\?\` paths + `CreateFile2` under LocalState on console: assumed OK, unverified.
8. Real network sockets: map AF_INET to Winsock if RL's offline mode touches them.
9. Thread records are leaked on exit (~500 B each); fix when thread churn is measured.

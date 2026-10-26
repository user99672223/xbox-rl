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
- **Process/ident (mostly single-process stubs):** `getpid`(39),
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
  resume_pc)`. Threads only (`CLONE_VM|CLONE_THREAD`). With `resume_pc != 0`
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

## ABI v3: keyboard/mouse and framebuffer (2026-10-26)

`XRL_ABI_VERSION` is 3. Everything is appended: existing members keep their offsets, an older host
(v1/v2 `struct_size`) simply lacks the new slots and the layer answers `-ENODEV` when a guest opens a device
that needs one; a v3 layer accepts hosts of any version.

### Host side (`include/xrl_abi.h`)

- **`xrl_input_event`** (16 bytes): `type` (u16), `code` (u16), `value` (s32), `time_ns` (u64, host monotonic).
  The vocabulary is Linux evdev's (numbers from `linux/input-event-codes.h`):
  - `XRL_IEV_KEY`: `code` = `KEY_*` (< 0x100) or `BTN_*` (`BTN_LEFT` 0x110 .. `BTN_EXTRA` 0x114); `value` 0 up, 1 down,
    2 autorepeat.
  - `XRL_IEV_REL`: `REL_X` 0, `REL_Y` 1, `REL_HWHEEL` 6, `REL_WHEEL` 8; counts, or notches for the wheels (+ up/right).
    X/Y only in `XRL_MOUSE_RELATIVE`; the wheels always.
  - `XRL_IEV_ABS`: `ABS_X` 0, `ABS_Y` 1, `0..XRL_ABS_MAX` (32767) over the picture the guest shows (see
    `set_content_size`), over the whole window before. Only in `XRL_MOUSE_ABSOLUTE`.
  - `XRL_IEV_TEXT`: `value` = Unicode code point, the OS layout's translation (SDL/Win32 text input).
  - `XRL_IEV_FOCUS`: 1 gained, 0 lost; held keys were released (as `KEY` value 0 events) just before a loss.
- **`xrl_input_ops`** gains `read_events(buf, max)` (non-blocking, returns the count), `wait_events(timeout_ms)`
  (`XRL_OK` when at least one event is queued, `XRL_ERR_NOTAVAIL` on timeout, `0xFFFFFFFF` = forever, any thread),
  `set_mouse_mode(XRL_MOUSE_ABSOLUTE | XRL_MOUSE_RELATIVE)` (the host shows/hides and captures the cursor; Left Ctrl +
  Left Alt releases a capture until the next click) and `set_content_size(w, h)`. The queue has **one consumer**:
  events are removed as they are read, so the layer's evdev devices and Wine's xrlhost must not both read it.
  The queue (`host/src/input_queue.cpp`) turns a press of a held key into a value-2 repeat, drops a release of a key
  that is not held, sums neighbouring `REL` events of the same axis, drops motion first when it is 3/4 full, and
  releases every held key on focus loss. The gamepad path (`get_pad_state`, `set_pad_vibration`) is unchanged.
- **`xrl_gfx_ops::present_framebuffer(pixels, width, height, stride, flags)`**: `pixels` is guest memory, 32 bits per
  pixel in B,G,R,X byte order, `stride` in bytes. The host draws it aspect-fitted (black bars) into the window's
  swapchain (`gfx/src/fbpresent.cpp`: native D3D11, dynamic texture, one draw, `Present(1)`), so it returns after
  vblank unless `XRL_PRESENT_NOWAIT`. The host owns the window's swapchain for this: do not mix with a guest swapchain.

### Guest side (layer VFS)

- **`/dev/input/event0` keyboard** (`KEY_ESC`..`KEY_MICMUTE`, `EV_REP`), **`event1` mouse** (`BTN_LEFT..BTN_EXTRA`,
  `REL_X/Y/HWHEEL/WHEEL`), **`event2` absolute pointer** (the same buttons, wheels, `ABS_X/ABS_Y` 0..32767, like a VM's
  USB tablet). Character devices, major 13, minor 64..66. `read` returns `struct input_event` (24 bytes on x86-64) in
  whole events, with a `SYN_REPORT` after every event except between the two axes of one motion; blocking, or
  `EAGAIN` under `O_NONBLOCK`; `poll`/`epoll` work. Each open file has its own 512-event ring filled while it is open
  (overflow gives `SYN_DROPPED`). ioctls: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGPROP`,
  `EVIOCGBIT(ev)`, `EVIOCGABS`, `EVIOCGKEY`, `EVIOCGREP`/`SREP`, `EVIOCGRAB`, `EVIOCREVOKE`, `EVIOCSCLOCKID`.
  Routing: motion goes to the device of its kind; buttons and wheels go to `event2` while any client has it open
  (and the host mouse mode is absolute), else to `event1`, which puts the host in relative mode while open.
  Not provided: `/dev/input` directory listing, `/sys/class/input`, `/proc/bus/input/devices` (udev-based
  hotplug stacks will not find the devices; open them by path, or use Xorg's `evdev` driver with explicit `Device`).
- **`/dev/fb0`**: 32 bpp, B,G,R,X (red offset 16, green 8, blue 0), default 1280x720 (`XRL_FB=WxH` before boot,
  `FBIOPUT_VSCREENINFO` after; virtual height up to 4x enables `FBIOPAN_DISPLAY` flipping). `FBIOGET_VSCREENINFO`,
  `FBIOPUT_VSCREENINFO` (`FB_ACTIVATE_TEST` honoured), `FBIOGET_FSCREENINFO` (truecolor, `smem_len`, `line_length`),
  `FBIOPAN_DISPLAY`, `FBIO_WAITFORVSYNC`, `FBIOBLANK`, `read`/`write`/`pread`/`pwrite`/`lseek`, and `mmap`
  (`MAP_SHARED`, `PROT_WRITE`, offset 0) as live memory: there is no scan-out hardware, so a presenter thread watches
  the visible rows and hands a snapshot to `present_framebuffer` whenever they changed (at most once per vblank,
  idle polling every 8 ms). One framebuffer and one live mapping; a second mapping replaces the first as the
  source. No palette (`FBIOGETCMAP` is `EINVAL`), no 16/24 bpp modes.
- New optional `xfile_ops` members `mapped(f, addr, len, off)` / `unmapped(f, addr, len)` (layer-private): called from
  `mmap`/`munmap` for `MAP_SHARED|PROT_WRITE` mappings of a file whose mapping is live memory (fbdev.c).

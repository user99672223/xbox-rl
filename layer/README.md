# layer/ - Workstream C (loader + syscall backend)

`xrl_layer_boot` / `xrl_layer_request_exit` (host-facing) and `xrl_syscall` (guest-facing,
a System V assembly thunk) implemented as a static library for Win32/UWP.

* ELF64 loader for Wine's unix side + musl (logic ported from Blink's loader; guest runs native).
* Linux x86-64 syscall backend on Win32 (futex, clone/threads, mmap family, VFS over
  LocalState, pipes/eventfd/epoll/socketpair, AF_INET/AF_INET6 TCP+UDP over Winsock, clocks,
  /proc + /dev). Every unimplemented number logs `ENOSYS <nr> <name>`.
* Load-time rewriting of foreign ELF images (`syscall`, `%fs`) so unmodified glibc/Alpine/closed
  binaries run: `src/rewrite*.c`, `src/x86dec.c`, `asm/rawsys.asm` (docs/decisions.md, "Easy Anti-Cheat").
* FS base via `wrfsbase` + fault re-arm (Windows drops it on context switches).

Start with **notes-C.md** (seam contract for B, pins, mechanisms, real-vs-stub table, open
questions) and **ABI-CHANGE-REQUESTS.md**. Build: `pwsh layer/scripts/build.ps1 -Flavor both`;
test: `layer/tests/run.sh`.

Layout: `src/` C sources, `asm/thunks.asm` (MASM), `tests/` (desktop host + freestanding
test guest), `scripts/` (vendor/build), `vendor.pins` + `vendor/` (git-ignored trees),
`patches/` (vendor patches; none yet).

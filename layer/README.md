# layer/ — Workstream C (loader + syscall backend)

Blink's ELF loader maps Wine's unix side + musl. Syscall backend derived from
Cosmopolitan Libc's Windows sources, implementing `xrl_syscall` (Linux x86-64
numbers, Linux return convention — see docs/syscall-abi.md). VFS over LocalState
with /proc + /dev stubs. threads/futex/mmap/arch_prctl(FSBASE). Implements
syscalls as Wine requests them; every ENOSYS is logged. Shares include/xrl_abi.h
with B. Designed to later grow into a general Linux-program layer; not built now.

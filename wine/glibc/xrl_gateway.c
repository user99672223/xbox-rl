/*
 * xrl_gateway.c - xrl_syscall() for a glibc build of Wine (wine/scripts/build-wine-glibc.sh).
 *
 * The Wine patches call xrl_syscall(nr, a1..a6) for the layer's syscalls and pseudo-syscalls
 * (patches 0002, 0007, 0010, 0023). In the musl build the patched musl exports it (the layer's
 * gateway). glibc has no such symbol, so this tiny library provides it as a raw `syscall`
 * instruction, which the layer's load-time rewriter turns into a call into the layer like every
 * other syscall site in a foreign binary. Like the kernel, it returns -errno (not glibc's syscall()).
 *
 * Build: gcc -O2 -fPIC -shared -nostdlib -Wl,-soname,libxrlgw.so -o libxrlgw.so xrl_gateway.c
 */
long xrl_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;
    register long r10 __asm__("r10") = a4, r8 __asm__("r8") = a5, r9 __asm__("r9") = a6;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/*
 * pp-guest.c - the Linux program the procprobe "layer" child runs (host/src/probe.cpp).
 * Freestanding (raw syscalls, no libc) so it needs nothing from the rootfs:
 *   pp-guest [MB] [segv]   touch MB MiB of anonymous memory, then print a heartbeat every second for 150 s;
 *                          "segv": write to address 0 after 5 s (no handler, so the process must end cleanly).
 * Build (WSL): gcc -static -nostdlib -ffreestanding -fno-stack-protector -O2 -o pp-guest pp-guest.c
 */
typedef unsigned long u64;

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

static void say(const char *s) { long n = 0; while (s[n]) n++; sys(1, 1, (long)s, n, 0, 0, 0); }

static void num(u64 v)
{
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    say(b + i);
}

__attribute__((used, noinline)) void run(long argc, char **argv)
{
    u64 mb = 0;
    if (argc > 1) for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) mb = mb * 10 + (u64)(*p - '0');
    if (mb) {
        u64 len = mb << 20;
        long p = sys(9, 0, (long)len, 3, 0x22, -1, 0);   /* mmap RW, MAP_PRIVATE|MAP_ANONYMOUS */
        if (p < 0 && p > -4096) { say("pp-guest: mmap failed\n"); }
        else for (u64 o = 0; o < len; o += 4096) ((volatile char *)p)[o] = 1;
        say("pp-guest: touched "); num(mb); say(" MiB\n");
    }
    struct { long s, ns; } ts = { 1, 0 };
    for (u64 n = 0; n < 150; n++) {
        say("pp-guest: alive "); num(n); say(" s\n");
        if (n == 5 && argc > 2 && argv[2][0] == 's') { say("pp-guest: SIGSEGV now\n"); *(volatile int *)0 = 1; }
        sys(35, (long)&ts, 0, 0, 0, 0, 0);   /* nanosleep */
    }
    sys(231, 0, 0, 0, 0, 0, 0);   /* exit_group */
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("mov (%rsp), %rdi\n lea 8(%rsp), %rsi\n and $-16, %rsp\n call run\n ud2");
}

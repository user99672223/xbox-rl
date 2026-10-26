/* bench.c - what the translator (XRL_DBT=1) costs: call/return heavy, indirect-call heavy, straight-line code,
 * and a syscall loop. Static glibc program, run by layer/tests/glibc/run.sh bench. Prints milliseconds. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static uint64_t hash_loop(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

int main(void)
{
    double t = now_ms();
    volatile int n32 = 35; int f = fib(n32);
    double t_fib = now_ms() - t;

    enum { N = 400000 };
    int *a = malloc(N * sizeof *a);
    unsigned s = 12345;
    for (int i = 0; i < N; i++) { s = s * 1103515245u + 12345u; a[i] = (int)(s >> 8); }
    t = now_ms();
    qsort(a, N, sizeof *a, cmp);
    double t_qs = now_ms() - t;

    uint8_t *buf = malloc(8 << 20);
    memset(buf, 7, 8 << 20);
    t = now_ms();
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) h ^= hash_loop(buf, 8 << 20);
    double t_hash = now_ms() - t;

    t = now_ms();
    long r = 0;
    for (int i = 0; i < 100000; i++) r += syscall(SYS_getpid);
    double t_sys = now_ms() - t;

    printf("bench: fib(35)=%d %.0f ms | qsort 400k ints %.0f ms | hash 64 MiB %.0f ms | 100k getpid %.0f ms  (%d %llx %ld)\n",
           f, t_fib, t_qs, t_hash, t_sys, a[N / 2] != 0, (unsigned long long)h, (int)(r != 0));
    return 0;
}

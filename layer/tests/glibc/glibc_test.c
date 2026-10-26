/*
 * glibc_test.c - an ordinary glibc program (static-PIE) run on the layer: nothing in it was
 * built for xbox-rl. It has `syscall` instructions all through libc and %fs everywhere
 * (errno, TLS, the stack-protector canary), which only work because the layer's loader
 * rewrites them (layer/src/rewrite.c). Exit code = number of failed checks.
 * Build (WSL): layer/tests/glibc/build.sh
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, name) do { if (cond) printf("  ok   %s\n", name); else { printf("  FAIL %s\n", name); fails++; } fflush(stdout); } while (0)

static __thread int tls_val = 7;
static __thread char tls_buf[300];

__attribute__((noinline)) static int canary_fn(const char *s)
{
    char buf[64];                         /* -fstack-protector-all: %fs:0x28 in prologue and epilogue */
    strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    return (int)strlen(buf);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready, sum;

static void *worker(void *arg)
{
    long id = (long)arg;
    tls_val += (int)id;
    snprintf(tls_buf, sizeof tls_buf, "thread-%ld", id);
    errno = 100 + (int)id;
    usleep(2000);
    char want[32];
    snprintf(want, sizeof want, "thread-%ld", id);
    int ok = tls_val == 7 + (int)id && errno == 100 + (int)id && !strcmp(tls_buf, want);
    pthread_mutex_lock(&mu);
    sum += ok ? 1 : 0;
    ready++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return (void *)(long)(ok ? 0 : 1);
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf jb;
static int depth(int n) { char pad[200]; memset(pad, n, sizeof pad); return n ? depth(n - 1) + pad[7] - n + 1 : 0; }

int main(int argc, char **argv)
{
    printf("glibc_test: glibc %s, argc=%d argv[0]=%s\n", gnu_get_libc_version(), argc, argv[0]);
    fflush(stdout);

    CHECK(tls_val == 7 && tls_buf[0] == 0, "main-thread TLS (__thread, local-exec)");
    int fd = open("/nonexistent/file", O_RDONLY);
    CHECK(fd < 0 && errno == ENOENT, "errno via %fs after a failing open");
    CHECK(canary_fn("hello") == 5, "stack protector canary (%fs:0x28)");

    char *big = malloc(8 << 20);
    CHECK(big != NULL, "malloc 8 MiB");
    if (big) { memset(big, 0xa5, 8 << 20); CHECK(((unsigned char *)big)[(8 << 20) - 1] == 0xa5, "write all of it"); free(big); }
    void *ptrs[256];
    for (int i = 0; i < 256; i++) { ptrs[i] = malloc(16 + i * 8); memset(ptrs[i], i, 16 + i * 8); }
    int heap_ok = 1;
    for (int i = 0; i < 256; i++) { if (((unsigned char *)ptrs[i])[15 + i * 8] != (unsigned char)i) heap_ok = 0; free(ptrs[i]); }
    CHECK(heap_ok, "many small allocations");

    long pid = syscall(SYS_getpid), tid = syscall(SYS_gettid);
    CHECK(pid > 0 && pid == getpid(), "raw syscall(SYS_getpid) == getpid()");
    CHECK(tid > 0, "raw syscall(SYS_gettid)");
    CHECK(syscall(SYS_write, 1, "  (raw write)\n", 14) == 14, "raw syscall(SYS_write)");

    pthread_t th[4];
    for (long i = 0; i < 4; i++) CHECK(pthread_create(&th[i], NULL, worker, (void *)(i + 1)) == 0, "pthread_create");
    pthread_mutex_lock(&mu);
    while (ready < 4) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    int jr = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); jr += (int)(long)r; }
    CHECK(sum == 4 && jr == 0, "4 threads: private TLS, errno and condvar hand-off");
    CHECK(tls_val == 7, "main thread's TLS untouched by the others");

    const char *path = "/tmp/glibc_test.dat";
    FILE *f = fopen(path, "w+");
    CHECK(f != NULL, "fopen /tmp");
    if (f) {
        for (int i = 0; i < 1000; i++) fprintf(f, "line %d\n", i);
        fflush(f); rewind(f);
        char line[64]; int n = 0, okl = 1;
        while (fgets(line, sizeof line, f)) { char want[32]; snprintf(want, sizeof want, "line %d\n", n); if (strcmp(line, want)) okl = 0; n++; }
        CHECK(n == 1000 && okl, "stdio write/read 1000 lines");
        fclose(f);
        struct stat st;
        CHECK(stat(path, &st) == 0 && st.st_size > 6000, "stat size");
        CHECK(unlink(path) == 0, "unlink");
    }

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 15 && ms < 2000, "nanosleep 20 ms");

    FILE *m = fopen("/proc/self/maps", "r");
    char ml[256]; int maps = 0;
    if (m) { while (fgets(ml, sizeof ml, m)) maps++; fclose(m); }
    CHECK(maps > 3, "/proc/self/maps readable");

    struct utsname un;
    CHECK(uname(&un) == 0 && !strcmp(un.sysname, "Linux"), "uname says Linux");
    CHECK(getenv("HOME") != NULL, "getenv HOME");

    int arr[2000];
    for (int i = 0; i < 2000; i++) arr[i] = (i * 7919) % 2003;
    qsort(arr, 2000, sizeof arr[0], cmp_int);
    int sorted = 1;
    for (int i = 1; i < 2000; i++) if (arr[i - 1] > arr[i]) sorted = 0;
    CHECK(sorted, "qsort");

    static char s1[5000], s2[5000];
    int strok = 1;
    for (int n = 1; n < 4096; n += 37) {
        memset(s1, 'x', n); s1[n] = 0;
        memcpy(s2, s1, n + 1);
        if (strlen(s2) != (size_t)n || memcmp(s1, s2, n + 1)) strok = 0;
    }
    CHECK(strok, "memset/memcpy/strlen/memcmp over many sizes (IFUNC-selected variants)");

    int v = setjmp(jb);
    if (!v) longjmp(jb, 5);
    CHECK(v == 5, "setjmp/longjmp");
    CHECK(depth(200) != 12345, "deep recursion on the stack");

    printf("glibc_test: %d failure(s)\n", fails);
    return fails;
}

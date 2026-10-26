/*
 * standin_eac.c - a stand-in for the closed easyanticheat_x64.so, built the way Valve builds
 * it: against glibc, default Ubuntu flags (FORTIFY, stack protector, PIC), no knowledge of
 * xbox-rl. Wine's musl process dlopen()s it. It does what an anti-cheat module does that
 * matters here: raw `syscall` instructions, %fs (TLS, canary), threads, libc calls through
 * glibc-only names (__printf_chk, __isoc23_sscanf, __ctype_b_loc, ...), reads its own
 * mapping and checksums its own code. Exported entries: standin_run() -> failures, and the two
 * factory functions of the real EAC client library (CreateGameClient, CreateClientAuth), which
 * is what Proton's bridge (easyanticheat_x64.so) dlsym()s: they run the checks, then return a
 * dummy object, so the whole PE bridge -> unix bridge -> dlopen -> module chain can be tried.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
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

/* a hand-rolled syscall, the kind a module uses to avoid libc */
static inline long raw3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static inline long raw0(long n) { return raw3(n, 0, 0, 0); }

static __thread int tls_gd = 11;                                   /* general-dynamic: __tls_get_addr */
static __thread char tls_arr[200];

__attribute__((noinline)) static int guarded(const char *s)
{
    char buf[48];
    snprintf(buf, sizeof buf, "<%s>", s);
    return (int)strlen(buf);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready, ok_threads;

static void *worker(void *arg)
{
    long id = (long)arg;
    tls_gd += (int)id;
    snprintf(tls_arr, sizeof tls_arr, "w%ld", id);
    long tid = raw0(SYS_gettid);
    int ok = tls_gd == 11 + (int)id && tls_arr[1] == '0' + (char)id && tid > 0;
    pthread_mutex_lock(&mu);
    ok_threads += ok;
    ready++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* checksum of this module's executable bytes: what a module does to notice tampering */
static uint64_t text_hash(const void *lo, const void *hi)
{
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t *p = lo; p < (const uint8_t *)hi; p++) { h ^= *p; h *= 1099511628211ull; }
    return h;
}

__attribute__((visibility("default"))) int standin_run(void)
{
    printf("standin_eac: module running (built against glibc)\n");
    fflush(stdout);

    long pid = raw0(SYS_getpid);
    CHECK(pid > 0 && pid == getpid(), "inline `syscall` getpid agrees with libc getpid");
    char msg[] = "  (inline syscall write)\n";
    CHECK(raw3(SYS_write, 1, (long)msg, (long)strlen(msg)) == (long)strlen(msg), "inline `syscall` write");
    struct timespec ts;
    CHECK(raw3(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts, 0) == 0 && (ts.tv_sec || ts.tv_nsec), "inline `syscall` clock_gettime");
    CHECK(raw3(SYS_open, (long)"/nonexistent/x", 0, 0) == -ENOENT, "inline `syscall` open fails with -ENOENT");

    CHECK(tls_gd == 11, "__thread variable in a dlopen'ed library");
    CHECK(guarded("abc") == 5, "stack-protected function (canary %fs:0x28)");

    pthread_t th[3];
    for (long i = 0; i < 3; i++) pthread_create(&th[i], NULL, worker, (void *)(i + 1));
    pthread_mutex_lock(&mu);
    while (ready < 3) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    CHECK(ok_threads == 3, "3 threads: private TLS and raw gettid");
    CHECK(tls_gd == 11, "creating thread's TLS untouched");

    char word[32] = "Hello, Module 42";
    int letters = 0, digits = 0;
    for (char *p = word; *p; p++) { if (isalpha((unsigned char)*p)) letters++; if (isdigit((unsigned char)*p)) digits++; }   /* __ctype_b_loc */
    CHECK(letters == 11 && digits == 2, "ctype macros (glibc table accessors)");
    int a = 0, b = 0;
    CHECK(sscanf("17 0x20", "%d %i", &a, &b) == 2 && a == 17 && b == 32, "sscanf (glibc C23 entry points)");
    long v = strtol("  -1234", NULL, 10);
    CHECK(v == -1234, "strtol");
    char big[100];
    memset(big, 'a', sizeof big); memcpy(big, "xyz", 3);
    CHECK(big[2] == 'z' && big[50] == 'a', "fortified memset/memcpy");

    struct stat st;
    CHECK(stat("/proc/self/maps", &st) == 0 || stat("/", &st) == 0, "stat");
    struct utsname un;
    CHECK(uname(&un) == 0 && !strcmp(un.sysname, "Linux"), "uname reports Linux");

    Dl_info di;
    int have = dladdr((void *)standin_run, &di);
    CHECK(have && di.dli_fname && strstr(di.dli_fname, ".so"), "dladdr finds this module");
    void *self = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    CHECK(self != NULL, "dlopen(self, RTLD_NOLOAD)");

    /* integrity check of our own code, as an anti-cheat would: compare the mapped .text with the file */
    FILE *mf = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long lo = 0, hi = 0, off = 0;
    int found = 0;
    while (mf && fgets(line, sizeof line, mf)) {
        unsigned long s, e, o; char perm[8];
        if (sscanf(line, "%lx-%lx %7s %lx", &s, &e, perm, &o) == 4 && perm[2] == 'x' && (unsigned long)standin_run >= s && (unsigned long)standin_run < e) { lo = s; hi = e; off = o; found = 1; }
    }
    if (mf) fclose(mf);
    CHECK(found, "found own executable mapping in /proc/self/maps");
    if (found && have) {
        FILE *f = fopen(di.dli_fname, "rb");
        size_t n = hi - lo;
        unsigned char *fileb = malloc(n);
        size_t got = 0;
        if (f) { fseek(f, (long)off, SEEK_SET); got = fread(fileb, 1, n, f); fclose(f); }
        size_t diff = 0;
        for (size_t i = 0; i < got && i < n; i++) if (fileb[i] != ((unsigned char *)lo)[i]) diff++;
        printf("  info: own text %zu bytes: mapped copy differs from the file in %zu byte(s) (hash mem %016llx file %016llx)\n", got, diff,
               (unsigned long long)text_hash((void *)lo, (void *)(lo + got)), (unsigned long long)text_hash(fileb, fileb + got));
        free(fileb);
    }
    printf("standin_eac: %d failure(s)\n", fails);
    return fails;
}

/* ---- what the EAC bridge looks up (names from the Proton runtime's easyanticheat_x64.so) ---- */
static long fake_method(void) { return 0; }       /* every slot of the dummy object: do nothing, return 0 */
static void *fake_vtable[32] = {
    (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method,
    (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method,
    (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method,
    (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method, (void *)fake_method };
static struct { void **vtable; } fake_object = { fake_vtable };
static int g_checked, g_failed;

__attribute__((visibility("default"))) void *CreateGameClient(const char *interface_version)
{
    printf("standin_eac: CreateGameClient(\"%s\") called through the bridge\n", interface_version ? interface_version : "(null)");
    if (!g_checked) { g_failed = standin_run(); g_checked = 1; }
    return g_failed ? NULL : &fake_object;
}

__attribute__((visibility("default"))) void *CreateClientAuth(const char *interface_version)
{
    printf("standin_eac: CreateClientAuth(\"%s\")\n", interface_version ? interface_version : "(null)");
    return &fake_object;
}

/*
 * glibc_compat.c - the glibc-ABI shim (libgcompat.so.0) over musl.
 *
 * Wine's unix side runs on musl. A native module built against glibc (Rocket
 * League's easyanticheat_x64.so from Valve's "Proton EasyAntiCheat Runtime")
 * that Wine dlopen()s lists libc.so.6 and uses symbols only glibc has. musl's
 * loader turns a DT_NEEDED of libc.so.6 (and libm/libdl/libpthread/librt/libutil)
 * into itself and ignores symbol versions, so `memcpy@GLIBC_2.14` already
 * resolves to musl's memcpy; this library, loaded RTLD_GLOBAL before the module
 * (Wine patch 0017), supplies what musl lacks: the fortify (_chk)
 * entry points, the C23 scanf/strtol names, the *64 and __xstat spellings, the
 * ctype table accessors, and a few GNU extras. It is the same idea as Alpine's
 * gcompat, kept small and driven by tools/glibc-compat-check.sh, which lists
 * every symbol of a given module that neither musl nor this shim provides.
 *
 * It is NOT glibc and does not pretend to be: gnu_get_libc_version() reports the
 * glibc ABI level this shim targets. Struct layouts shared with glibc on x86-64
 * (stat, timespec, pthread_mutex_t/cond_t sizes, FILE only through the API) agree
 * with musl's; things that differ (getcontext family, _IO_* internals, NSS,
 * iconv modules) are not provided: a module needing them fails dlopen with the
 * symbol's name.
 *
 * Built with the xbox-rl musl toolchain (wine/scripts/build-glibc-compat.sh):
 * no `syscall` instruction, thread pointer through the TEB slot.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <wchar.h>

#define EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------ identity */
EXPORT const char *gnu_get_libc_version(void) { return "2.31"; }   /* the glibc ABI level this shim targets */
EXPORT const char *gnu_get_libc_release(void) { return "stable"; }
EXPORT char __libc_single_threaded = 0;                            /* always assume threads */

/* ------------------------------------------------------------ fortify */
static void overflow(const char *fn)
{
    char m[96];
    int n = snprintf(m, sizeof m, "*** buffer overflow detected ***: %s terminated\n", fn);
    if (n > 0) (void)!write(2, m, (size_t)n);
    abort();
}
#define CHK(fn, cond) do { if (cond) overflow(fn); } while (0)

EXPORT void *__memcpy_chk(void *d, const void *s, size_t n, size_t dl) { CHK("memcpy", n > dl); return memcpy(d, s, n); }
EXPORT void *__memmove_chk(void *d, const void *s, size_t n, size_t dl) { CHK("memmove", n > dl); return memmove(d, s, n); }
EXPORT void *__mempcpy_chk(void *d, const void *s, size_t n, size_t dl) { CHK("mempcpy", n > dl); return mempcpy(d, s, n); }
EXPORT void *__memset_chk(void *d, int c, size_t n, size_t dl) { CHK("memset", n > dl); return memset(d, c, n); }
EXPORT void __explicit_bzero_chk(void *d, size_t n, size_t dl) { CHK("explicit_bzero", n > dl); explicit_bzero(d, n); }
EXPORT char *__strcpy_chk(char *d, const char *s, size_t dl) { CHK("strcpy", strlen(s) >= dl); return strcpy(d, s); }
EXPORT char *__stpcpy_chk(char *d, const char *s, size_t dl) { CHK("stpcpy", strlen(s) >= dl); return stpcpy(d, s); }
EXPORT char *__strncpy_chk(char *d, const char *s, size_t n, size_t dl) { CHK("strncpy", n > dl); return strncpy(d, s, n); }
EXPORT char *__stpncpy_chk(char *d, const char *s, size_t n, size_t dl) { CHK("stpncpy", n > dl); return stpncpy(d, s, n); }
EXPORT char *__strcat_chk(char *d, const char *s, size_t dl) { CHK("strcat", strlen(d) + strlen(s) >= dl); return strcat(d, s); }
EXPORT char *__strncat_chk(char *d, const char *s, size_t n, size_t dl) { size_t l = strnlen(s, n); CHK("strncat", strlen(d) + l >= dl); return strncat(d, s, n); }
EXPORT wchar_t *__wmemcpy_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { CHK("wmemcpy", n > dl); return wmemcpy(d, s, n); }
EXPORT wchar_t *__wmemset_chk(wchar_t *d, wchar_t c, size_t n, size_t dl) { CHK("wmemset", n > dl); return wmemset(d, c, n); }
EXPORT size_t __mbsrtowcs_chk(wchar_t *d, const char **s, size_t n, mbstate_t *ps, size_t dl) { CHK("mbsrtowcs", d && n > dl); return mbsrtowcs(d, s, n, ps); }
EXPORT ssize_t __read_chk(int fd, void *b, size_t n, size_t bl) { CHK("read", n > bl); return read(fd, b, n); }
EXPORT ssize_t __pread64_chk(int fd, void *b, size_t n, off_t o, size_t bl) { CHK("pread", n > bl); return pread(fd, b, n, o); }
EXPORT ssize_t __readlink_chk(const char *p, char *b, size_t n, size_t bl) { CHK("readlink", n > bl); return readlink(p, b, n); }
EXPORT char *__getcwd_chk(char *b, size_t n, size_t bl) { CHK("getcwd", n > bl); return getcwd(b, n); }
EXPORT int __gethostname_chk(char *b, size_t n, size_t bl) { CHK("gethostname", n > bl); return gethostname(b, n); }
EXPORT char *__realpath_chk(const char *p, char *r, size_t rl) { CHK("realpath", rl < PATH_MAX); return realpath(p, r); }
EXPORT char *__fgets_chk(char *b, size_t bl, int n, FILE *f) { CHK("fgets", (size_t)n > bl); return fgets(b, n, f); }
EXPORT long __fdelt_chk(long d) { if (d < 0 || d >= FD_SETSIZE) overflow("fd_set"); return d / (long)(8 * sizeof(long)); }
EXPORT void __longjmp_chk(jmp_buf e, int v) { longjmp(e, v); }

EXPORT int __printf_chk(int fl, const char *f, ...) { (void)fl; va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); return r; }
EXPORT int __fprintf_chk(FILE *s, int fl, const char *f, ...) { (void)fl; va_list a; va_start(a, f); int r = vfprintf(s, f, a); va_end(a); return r; }
EXPORT int __dprintf_chk(int fd, int fl, const char *f, ...) { (void)fl; va_list a; va_start(a, f); int r = vdprintf(fd, f, a); va_end(a); return r; }
EXPORT int __vprintf_chk(int fl, const char *f, va_list a) { (void)fl; return vprintf(f, a); }
EXPORT int __vfprintf_chk(FILE *s, int fl, const char *f, va_list a) { (void)fl; return vfprintf(s, f, a); }
EXPORT int __vdprintf_chk(int fd, int fl, const char *f, va_list a) { (void)fl; return vdprintf(fd, f, a); }
EXPORT int __sprintf_chk(char *d, int fl, size_t dl, const char *f, ...)
{
    (void)fl; va_list a; va_start(a, f); int r = vsnprintf(d, dl, f, a); va_end(a);
    CHK("sprintf", r >= 0 && (size_t)r >= dl); return r;
}
EXPORT int __vsprintf_chk(char *d, int fl, size_t dl, const char *f, va_list a)
{
    (void)fl; int r = vsnprintf(d, dl, f, a); CHK("vsprintf", r >= 0 && (size_t)r >= dl); return r;
}
EXPORT int __snprintf_chk(char *d, size_t n, int fl, size_t dl, const char *f, ...)
{
    (void)fl; CHK("snprintf", n > dl); va_list a; va_start(a, f); int r = vsnprintf(d, n, f, a); va_end(a); return r;
}
EXPORT int __vsnprintf_chk(char *d, size_t n, int fl, size_t dl, const char *f, va_list a)
{
    (void)fl; CHK("vsnprintf", n > dl); return vsnprintf(d, n, f, a);
}
EXPORT int __asprintf_chk(char **p, int fl, const char *f, ...) { (void)fl; va_list a; va_start(a, f); int r = vasprintf(p, f, a); va_end(a); return r; }
EXPORT int __vasprintf_chk(char **p, int fl, const char *f, va_list a) { (void)fl; return vasprintf(p, f, a); }
EXPORT void __syslog_chk(int pri, int fl, const char *f, ...) { (void)fl; va_list a; va_start(a, f); vsyslog(pri, f, a); va_end(a); }
EXPORT void __vsyslog_chk(int pri, int fl, const char *f, va_list a) { (void)fl; vsyslog(pri, f, a); }

EXPORT int __open_2(const char *p, int fl) { return open(p, fl); }
EXPORT int __open64_2(const char *p, int fl) { return open(p, fl); }
EXPORT int __openat_2(int d, const char *p, int fl) { return openat(d, p, fl); }
EXPORT int __openat64_2(int d, const char *p, int fl) { return openat(d, p, fl); }

/* ----------------------------------------------- C23 scanf/strtol names */
#define STRTO(name, type, fn) EXPORT type name(const char *s, char **e, int b) { return fn(s, e, b); }
STRTO(__isoc23_strtol, long, strtol)
STRTO(__isoc23_strtoll, long long, strtoll)
STRTO(__isoc23_strtoul, unsigned long, strtoul)
STRTO(__isoc23_strtoull, unsigned long long, strtoull)
STRTO(__isoc23_strtoimax, intmax_t, strtoimax)
STRTO(__isoc23_strtoumax, uintmax_t, strtoumax)
EXPORT long __isoc23_wcstol(const wchar_t *s, wchar_t **e, int b) { return wcstol(s, e, b); }
EXPORT unsigned long __isoc23_wcstoul(const wchar_t *s, wchar_t **e, int b) { return wcstoul(s, e, b); }
EXPORT int __isoc23_sscanf(const char *s, const char *f, ...) { va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }
EXPORT int __isoc23_fscanf(FILE *s, const char *f, ...) { va_list a; va_start(a, f); int r = vfscanf(s, f, a); va_end(a); return r; }
EXPORT int __isoc23_scanf(const char *f, ...) { va_list a; va_start(a, f); int r = vscanf(f, a); va_end(a); return r; }
EXPORT int __isoc23_vsscanf(const char *s, const char *f, va_list a) { return vsscanf(s, f, a); }
EXPORT int __isoc23_vfscanf(FILE *s, const char *f, va_list a) { return vfscanf(s, f, a); }
EXPORT int __isoc23_vscanf(const char *f, va_list a) { return vscanf(f, a); }

/* ------------------------------------------- stat family, *64 spellings */
EXPORT int __xstat(int v, const char *p, struct stat *b) { (void)v; return stat(p, b); }
EXPORT int __lxstat(int v, const char *p, struct stat *b) { (void)v; return lstat(p, b); }
EXPORT int __fxstat(int v, int fd, struct stat *b) { (void)v; return fstat(fd, b); }
EXPORT int __fxstatat(int v, int d, const char *p, struct stat *b, int fl) { (void)v; return fstatat(d, p, b, fl); }
EXPORT int __xstat64(int v, const char *p, struct stat *b) { (void)v; return stat(p, b); }
EXPORT int __lxstat64(int v, const char *p, struct stat *b) { (void)v; return lstat(p, b); }
EXPORT int __fxstat64(int v, int fd, struct stat *b) { (void)v; return fstat(fd, b); }
EXPORT int __fxstatat64(int v, int d, const char *p, struct stat *b, int fl) { (void)v; return fstatat(d, p, b, fl); }
EXPORT int stat64(const char *p, struct stat *b) { return stat(p, b); }
EXPORT int lstat64(const char *p, struct stat *b) { return lstat(p, b); }
EXPORT int fstat64(int fd, struct stat *b) { return fstat(fd, b); }
EXPORT int fstatat64(int d, const char *p, struct stat *b, int fl) { return fstatat(d, p, b, fl); }
EXPORT int statvfs64(const char *p, struct statvfs *b) { return statvfs(p, b); }
EXPORT int fstatvfs64(int fd, struct statvfs *b) { return fstatvfs(fd, b); }
EXPORT int open64(const char *p, int fl, ...)
{
    mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, fl); m = va_arg(a, mode_t); va_end(a); }
    return open(p, fl, m);
}
EXPORT int openat64(int d, const char *p, int fl, ...)
{
    mode_t m = 0;
    if (fl & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, fl); m = va_arg(a, mode_t); va_end(a); }
    return openat(d, p, fl, m);
}
EXPORT int fcntl64(int fd, int cmd, ...)
{
    va_list a; va_start(a, cmd); void *arg = va_arg(a, void *); va_end(a);
    return fcntl(fd, cmd, arg);
}
EXPORT void *mmap64(void *a, size_t l, int p, int f, int fd, off_t o) { return mmap(a, l, p, f, fd, o); }
EXPORT off_t lseek64(int fd, off_t o, int w) { return lseek(fd, o, w); }
EXPORT ssize_t pread64(int fd, void *b, size_t n, off_t o) { return pread(fd, b, n, o); }
EXPORT ssize_t pwrite64(int fd, const void *b, size_t n, off_t o) { return pwrite(fd, b, n, o); }
EXPORT ssize_t preadv64(int fd, const struct iovec *v, int c, off_t o) { return preadv(fd, v, c, o); }
EXPORT ssize_t pwritev64(int fd, const struct iovec *v, int c, off_t o) { return pwritev(fd, v, c, o); }
EXPORT int ftruncate64(int fd, off_t l) { return ftruncate(fd, l); }
EXPORT int truncate64(const char *p, off_t l) { return truncate(p, l); }
EXPORT int lockf64(int fd, int c, off_t l) { return lockf(fd, c, l); }
EXPORT int posix_fadvise64(int fd, off_t o, off_t l, int a) { return posix_fadvise(fd, o, l, a); }
EXPORT int posix_fallocate64(int fd, off_t o, off_t l) { return posix_fallocate(fd, o, l); }
EXPORT ssize_t sendfile64(int o, int i, off_t *off, size_t n) { return sendfile(o, i, off, n); }
EXPORT FILE *fopen64(const char *p, const char *m) { return fopen(p, m); }
EXPORT int fseeko64(FILE *f, off_t o, int w) { return fseeko(f, o, w); }
EXPORT off_t ftello64(FILE *f) { return ftello(f); }
EXPORT struct dirent *readdir64(DIR *d) { return readdir(d); }
EXPORT int getrlimit64(int r, struct rlimit *l) { return getrlimit(r, l); }
EXPORT int setrlimit64(int r, const struct rlimit *l) { return setrlimit(r, l); }

/* --------------------------------------------------------- ctype tables */
/* glibc's ctype macros index a 384-entry table through __ctype_b_loc() etc. (C locale). */
static unsigned short g_ctype_b[384];
static int g_ctype_lower[384], g_ctype_upper[384];
static const unsigned short *g_ctype_b_p = g_ctype_b + 128;
static const int *g_ctype_lower_p = g_ctype_lower + 128, *g_ctype_upper_p = g_ctype_upper + 128;

__attribute__((constructor)) static void ctype_init(void)
{
    for (int c = -128; c < 256; c++) {
        unsigned short m = 0;
        int ch = c < 0 ? c + 256 : c, a = ch < 128;
        if (a) {
            if (isupper(ch)) m |= 1 << 8;
            if (islower(ch)) m |= 2 << 8;
            if (isalpha(ch)) m |= 4 << 8;
            if (isdigit(ch)) m |= 8 << 8;
            if (isxdigit(ch)) m |= 16 << 8;
            if (isspace(ch)) m |= 32 << 8;
            if (isprint(ch)) m |= 64 << 8;
            if (isgraph(ch)) m |= 128 << 8;
            if (ch == ' ' || ch == '\t') m |= 1;
            if (iscntrl(ch)) m |= 2;
            if (ispunct(ch)) m |= 4;
            if (isalnum(ch)) m |= 8;
        }
        g_ctype_b[c + 128] = m;
        g_ctype_lower[c + 128] = a ? tolower(ch) : c;
        g_ctype_upper[c + 128] = a ? toupper(ch) : c;
    }
}
EXPORT const unsigned short **__ctype_b_loc(void) { return &g_ctype_b_p; }
EXPORT const int **__ctype_tolower_loc(void) { return &g_ctype_lower_p; }
EXPORT const int **__ctype_toupper_loc(void) { return &g_ctype_upper_p; }

/* ------------------------------------------------------------- GNU extras */
EXPORT int __sysconf(int n) { return (int)sysconf(n); }
EXPORT int __register_atfork(void (*p)(void), void (*a)(void), void (*c)(void), void *dso) { (void)dso; return pthread_atfork(p, a, c); }
EXPORT void *dlvsym(void *h, const char *n, const char *v) { (void)v; return dlsym(h, n); }
EXPORT int dladdr1(const void *a, Dl_info *i, void **e, int f) { (void)e; (void)f; return dladdr(a, i); }
EXPORT int _IO_getc(FILE *f) { return getc(f); }
EXPORT int _IO_putc(int c, FILE *f) { return putc(c, f); }
EXPORT int backtrace(void **b, int n) { (void)b; (void)n; return 0; }          /* no unwinder on the layer */
EXPORT char **backtrace_symbols(void *const *b, int n) { (void)b; (void)n; return NULL; }
EXPORT void backtrace_symbols_fd(void *const *b, int n, int fd) { (void)b; (void)n; (void)fd; }
EXPORT void arc4random_buf(void *b, size_t n) { extern int getentropy(void *, size_t); for (size_t i = 0; i < n; i += 256) getentropy((char *)b + i, n - i < 256 ? n - i : 256); }
EXPORT uint32_t arc4random(void) { uint32_t v; arc4random_buf(&v, sizeof v); return v; }
EXPORT int close_range(unsigned lo, unsigned hi, int fl)
{
    (void)fl;
    long max = sysconf(_SC_OPEN_MAX); if (max < 0 || max > 65536) max = 65536;
    for (unsigned fd = lo; fd <= hi && fd < (unsigned)max; fd++) close((int)fd);
    return 0;
}
EXPORT void closefrom(int lo) { close_range((unsigned)lo, ~0u, 0); }
EXPORT cpu_set_t *__sched_cpualloc(size_t n) { return (cpu_set_t *)calloc(1, CPU_ALLOC_SIZE(n)); }
EXPORT void __sched_cpufree(cpu_set_t *s) { free(s); }
EXPORT char *__strftime_l(char *s, size_t m, const char *f, const struct tm *t, locale_t l) { return (char *)(size_t)strftime_l(s, m, f, t, l); }

/* C++ thread_local destructors (libstdc++'s __cxa_thread_atexit): run at thread exit. */
struct tl_dtor { void (*fn)(void *); void *obj; struct tl_dtor *next; };
static __thread struct tl_dtor *tl_list;
static pthread_key_t tl_key;
static pthread_once_t tl_once = PTHREAD_ONCE_INIT;
static void tl_run(void *unused)
{
    (void)unused;
    while (tl_list) { struct tl_dtor *d = tl_list; tl_list = d->next; d->fn(d->obj); free(d); }
}
static void tl_make_key(void) { pthread_key_create(&tl_key, tl_run); }
EXPORT int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso)
{
    (void)dso;
    pthread_once(&tl_once, tl_make_key);
    struct tl_dtor *d = (struct tl_dtor *)malloc(sizeof *d);
    if (!d) return -1;
    d->fn = fn; d->obj = obj; d->next = tl_list; tl_list = d;
    pthread_setspecific(tl_key, d);
    return 0;
}

/* A stand-in for the module the EAC bootstrapper downloads and dlopen()s: a glibc-built shared object with
 * no libc dependency in its DT_NEEDED (built with the host compiler, -nostdlib). It
 *   - issues a raw `syscall` itself (getpid): a rewriter would patch these bytes, a byte-identical
 *     translator leaves them as the file has them (rawsys_eac.c compares the bytes in memory with the file),
 *   - calls a glibc-only symbol (__snprintf_chk) that musl lacks and the glibc-ABI shim (libgcompat) provides,
 *   - has a constructor, which dlopen must run.
 * Nothing here is Easy Anti-Cheat code. */
extern int __snprintf_chk(char *, unsigned long, int, unsigned long, const char *, ...);

static int g_ctor_ran;
__attribute__((constructor)) static void ctor(void) { g_ctor_ran = 7; }

__attribute__((visibility("default"))) int xrl_mod_ctor(void) { return g_ctor_ran; }
__attribute__((visibility("default"))) int xrl_mod_answer(int x) { return x * 2 + 1; }
__attribute__((visibility("default"))) long xrl_mod_getpid(void)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(39L) : "rcx", "r11", "memory");
    return r;
}
__attribute__((visibility("default"))) int xrl_mod_fmt(char *buf, unsigned n, int v)
{
    return __snprintf_chk(buf, n, 1, n, "v=%d", v);
}

/* coroutines with the ucontext family, which musl lacks (the shim provides it) */
#include <ucontext.h>
static ucontext_t main_ctx, co_ctx;
static char co_stack[64 * 1024] __attribute__((aligned(16)));
static volatile int co_counter;
static void co_fn(int a, int b)
{
    co_counter += a + b;                  /* 7 */
    swapcontext(&co_ctx, &main_ctx);
    co_counter += 100;                    /* resumed: 107, then the function returns into uc_link */
}
__attribute__((visibility("default"))) int xrl_mod_ctx(void)
{
    co_counter = 0;
    getcontext(&co_ctx);
    co_ctx.uc_stack.ss_sp = co_stack;
    co_ctx.uc_stack.ss_size = sizeof co_stack;
    co_ctx.uc_link = &main_ctx;
    makecontext(&co_ctx, (void (*)(void))co_fn, 2, 3, 4);
    swapcontext(&main_ctx, &co_ctx);
    int first = co_counter;               /* 7 after the first switch back */
    swapcontext(&main_ctx, &co_ctx);
    return first * 1000 + co_counter;     /* 7107 */
}

/* the unwinder: libgcc_s (built for the guest, build-libgcc-s.sh) walks this frame; it needs _dl_find_object */
typedef int (*unw_cb)(void *ctx, void *arg);
extern int _Unwind_Backtrace(unw_cb, void *);
static int count_frame(void *ctx, void *arg) { (void)ctx; (*(int *)arg)++; return 0; }
__attribute__((visibility("default"))) int xrl_mod_backtrace(void)
{
    int n = 0;
    _Unwind_Backtrace(count_frame, &n);
    return n;
}

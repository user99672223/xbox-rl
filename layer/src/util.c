/*
 * util.c - logging, user-memory access, errno translation, CSPRNG, clocks.
 *
 * Errno table adapted in spirit from Cosmopolitan's kDos2Errno
 * (libc/sysv/dos2errno.sh, pinned in vendor.pins); values here are the Linux
 * errnos, not Cosmopolitan's per-OS consts.
 */
#include "xrl_internal.h"
#include <bcrypt.h>

/* ============================================================== logging */
static SRWLOCK g_log_lock = SRWLOCK_INIT;

void xrl_logf(int level, const char *tag, const char *fmt, ...)
{
    char buf[1100];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    xrl_log_fn fn = G.ctx.log;
    if (fn) {
        /* Never block for long: a fault inside a log call (or a crash report from
         * a thread that already holds the lock) would otherwise hang the process
         * silently. After ~250 ms, log without the lock (lines may interleave). */
        int locked = 0;
        for (int i = 0; i < 250 && !(locked = TryAcquireSRWLockExclusive(&g_log_lock)); i++) Sleep(1);
        fn(level, tag, buf);
        if (locked) ReleaseSRWLockExclusive(&g_log_lock);
    } else {
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }
}

/* ======================================================== user memory */
int xrl_cfu(void *dst, u64 usrc, size_t n)
{
    if (!n) return 0;
    if (usrc < 0x10000) return -LX_EFAULT;
    __try { memcpy(dst, (const void *)usrc, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    return 0;
}

int xrl_ctu(u64 udst, const void *src, size_t n)
{
    if (!n) return 0;
    if (udst < 0x10000) return -LX_EFAULT;
    __try { memcpy((void *)udst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    return 0;
}

int xrl_zerou(u64 udst, size_t n)
{
    if (!n) return 0;
    if (udst < 0x10000) return -LX_EFAULT;
    __try { memset((void *)udst, 0, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    return 0;
}

int xrl_strfu(char *dst, u64 usrc, size_t cap)
{
    if (usrc < 0x10000) return -LX_EFAULT;
    size_t i = 0;
    __try {
        const char *s = (const char *)usrc;
        for (; i < cap; i++) {
            char c = s[i];
            dst[i] = c;
            if (!c) return 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -LX_EFAULT; }
    dst[cap - 1] = 0;
    return -LX_ENAMETOOLONG;
}

/* ================================================================ errno */
int xrl_win_to_errno(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return 0;
    case ERROR_INVALID_FUNCTION: return LX_EINVAL;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_NO_MORE_FILES:
    case ERROR_BAD_PATHNAME:
    case ERROR_BAD_NETPATH:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_UNIT: return LX_ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES: return LX_EMFILE;
    case ERROR_ACCESS_DENIED:
    case ERROR_LOCK_VIOLATION:
    case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_CANNOT_MAKE:
    case ERROR_FAIL_I24:
    case ERROR_DRIVE_LOCKED:
    case ERROR_SEEK_ON_DEVICE:
    case ERROR_NOT_LOCKED: return LX_EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_CURRENT_DIRECTORY:
    case ERROR_DELETE_PENDING: return LX_EBUSY;
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_TARGET_HANDLE:
    case ERROR_DIRECT_ACCESS_HANDLE: return LX_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
    case ERROR_NOT_ENOUGH_QUOTA:
    case ERROR_NO_SYSTEM_RESOURCES: return LX_ENOMEM;
    case ERROR_NOT_SAME_DEVICE: return LX_EXDEV;
    case ERROR_WRITE_PROTECT: return LX_EROFS;
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL: return LX_ENOSPC;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return LX_ENOSYS;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return LX_EEXIST;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_DATA:
    case ERROR_INVALID_ADDRESS:
    case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_FLAGS:
    case ERROR_NOT_A_REPARSE_POINT: return LX_EINVAL;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED: return LX_EPIPE;
    case ERROR_INSUFFICIENT_BUFFER: return LX_ERANGE;
    case ERROR_DIR_NOT_EMPTY: return LX_ENOTEMPTY;
    case ERROR_FILENAME_EXCED_RANGE: return LX_ENAMETOOLONG;
    case ERROR_DIRECTORY: return LX_ENOTDIR;
    case ERROR_NOACCESS: return LX_EFAULT;
    case ERROR_PRIVILEGE_NOT_HELD: return LX_EPERM;
    case ERROR_TOO_MANY_LINKS: return LX_EMLINK;
    case ERROR_SEM_TIMEOUT:
    case WAIT_TIMEOUT:
    case ERROR_TIMEOUT: return LX_ETIMEDOUT;
    case ERROR_OPERATION_ABORTED: return LX_EINTR;
    case ERROR_NOT_READY:
    case ERROR_CRC:
    case ERROR_READ_FAULT:
    case ERROR_WRITE_FAULT: return LX_EIO;
    case ERROR_NOT_OWNER: return LX_EPERM;
    case ERROR_FILE_TOO_LARGE: return LX_EFBIG;
    case ERROR_CANT_RESOLVE_FILENAME:
    case ERROR_STOPPED_ON_SYMLINK: return LX_ELOOP;
    default: return LX_EIO;
    }
}

slong xrl_lasterr(void)
{
    int e = xrl_win_to_errno(GetLastError());
    return -(slong)(e ? e : LX_EIO);
}

/* ================================================================= RNG */
/*
 * The console has no RDRAND and the firmware RNG is blocked (decisions.md).
 * Seed order: BCryptGenRandom(system RNG) [expected to work in the app
 * container], XRL_RNG_SEED=<hex> from the host's envp, then QPC/time/ASLR
 * jitter as a floor. All sources are mixed into a ChaCha20 key. If BCrypt
 * fails we log a WARN so the integrator knows the seed is weak.
 */
static SRWLOCK g_rng_lock = SRWLOCK_INIT;
static u32 g_rng_state[16];
static u8  g_rng_buf[64];
static int g_rng_pos = 64;
static int g_rng_ready;

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  c += d; b ^= c; b = ROTL32(b, 7)

static void chacha_block(const u32 in[16], u8 out[64])
{
    u32 x[16];
    memcpy(x, in, 64);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) { u32 v = x[i] + in[i]; memcpy(out + 4 * i, &v, 4); }
}

static u64 splitmix(u64 *s)
{
    u64 z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void xrl_rng_init(void)
{
    u8 key[32] = {0};
    u8 ent[32] = {0};
    int bc = 0;
    NTSTATUS st = BCryptGenRandom(NULL, ent, sizeof ent, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st == 0) bc = 1;
    for (int i = 0; i < 32; i++) key[i] ^= ent[i];

    int host_seed = 0;
    for (char **e = G.envp; e && *e; e++) {
        if (!strncmp(*e, "XRL_RNG_SEED=", 13)) {
            const char *h = *e + 13;
            for (int i = 0; i < 32 && h[2 * i] && h[2 * i + 1]; i++) {
                int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
                if (a < 0 || b < 0) break;
                key[i] ^= (u8)(a << 4 | b);
                host_seed = 1;
            }
        }
    }

    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    u64 s = (u64)q.QuadPart ^ ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime)
          ^ (u64)(uintptr_t)&q ^ ((u64)GetCurrentThreadId() << 40) ^ (u64)(uintptr_t)GetModuleHandleW(NULL);
    for (int i = 0; i < 4; i++) { u64 v = splitmix(&s); for (int j = 0; j < 8; j++) key[i * 8 + j] ^= (u8)(v >> (8 * j)); }

    memset(g_rng_state, 0, sizeof g_rng_state);
    g_rng_state[0] = 0x61707865; g_rng_state[1] = 0x3320646e;
    g_rng_state[2] = 0x79622d32; g_rng_state[3] = 0x6b206574;
    memcpy(&g_rng_state[4], key, 32);
    g_rng_pos = 64;
    g_rng_ready = 1;
    if (!bc)
        LOGW("rng: BCryptGenRandom failed (0x%08lx); seed is %s", (u64)st,
             host_seed ? "host-provided XRL_RNG_SEED only" : "WEAK (timing/ASLR only)");
    else
        LOGI("rng: seeded from system RNG%s", host_seed ? " + XRL_RNG_SEED" : "");
}

void xrl_rng_fill(void *buf, size_t n)
{
    u8 *p = (u8 *)buf;
    AcquireSRWLockExclusive(&g_rng_lock);
    if (!g_rng_ready) { /* called before init: best effort */ LARGE_INTEGER q; QueryPerformanceCounter(&q); u64 s = (u64)q.QuadPart; for (size_t i = 0; i < n; i++) p[i] = (u8)splitmix(&s); ReleaseSRWLockExclusive(&g_rng_lock); return; }
    while (n) {
        if (g_rng_pos >= 64) {
            chacha_block(g_rng_state, g_rng_buf);
            if (++g_rng_state[12] == 0) ++g_rng_state[13];
            g_rng_pos = 0;
            /* fast-key-erasure: every 1 MiB worth of blocks rekey from the stream itself */
            if ((g_rng_state[12] & 0x3fff) == 0) {
                u8 nk[64]; chacha_block(g_rng_state, nk); ++g_rng_state[12];
                memcpy(&g_rng_state[4], nk, 32);
            }
        }
        size_t take = 64 - g_rng_pos; if (take > n) take = n;
        memcpy(p, g_rng_buf + g_rng_pos, take);
        g_rng_pos += (int)take; p += take; n -= take;
    }
    ReleaseSRWLockExclusive(&g_rng_lock);
}

/* ================================================================ clocks */
static LARGE_INTEGER g_qpf;

void xrl_time_init(void)
{
    QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    G.qpc_freq = (u64)g_qpf.QuadPart;
    G.boot_qpc = (u64)q.QuadPart;
}

u64 xrl_mono_ns(void)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    u64 c = (u64)q.QuadPart, f = (u64)g_qpf.QuadPart;
    return (c / f) * 1000000000ull + ((c % f) * 1000000000ull) / f;
}

u64 xrl_real_ns(void)
{
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100ull;
}

u64 xrl_ts_to_ns(const lx_timespec *ts)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return ~0ull;
    if ((u64)ts->tv_sec > (~0ull / 1000000000ull) - 1) return ~0ull - 1;
    return (u64)ts->tv_sec * 1000000000ull + (u64)ts->tv_nsec;
}

int xrl_wait_addr(volatile void *addr, void *cmp, size_t sz, u64 ns)
{
    for (;;) {
        DWORD ms;
        u64 remain_ms = (ns == ~0ull) ? ~0ull : (ns + 999999ull) / 1000000ull;
        if (ns == ~0ull) ms = INFINITE;
        else if (remain_ms >= 0xFFFFFFFEull) ms = 0xFFFFFFFEu;
        else ms = (DWORD)remain_ms;
        if (WaitOnAddress(addr, cmp, sz, ms)) return 1;
        if (GetLastError() == ERROR_TIMEOUT) {
            if (ns != ~0ull && remain_ms > 0xFFFFFFFEull) {   /* long wait: continue */
                ns -= 0xFFFFFFFEull * 1000000ull;
                if (memcmp((const void *)addr, cmp, sz) != 0) return 1;
                continue;
            }
            return 0;
        }
        return 1;   /* any other failure: let caller recheck */
    }
}

slong xrl_sleep_ns(u64 ns, int *interrupted)
{
    xrl_thread *t = xrl_cur();
    if (interrupted) *interrupted = 0;
    if (!ns) { SwitchToThread(); return 0; }
    if (t) {
        if (!t->timer) {
            t->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!t->timer) t->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        if (t->timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ns + 99) / 100);
            if (SetWaitableTimer(t->timer, &due, 0, NULL, NULL, FALSE)) {
                if (!interrupted || !t->sig_ev) { WaitForSingleObjectEx(t->timer, INFINITE, FALSE); return 0; }
                HANDLE hs[2] = { t->timer, t->sig_ev };
                for (;;) {
                    if (xrl_sig_pending_for(t)) { CancelWaitableTimer(t->timer); *interrupted = 1; return 0; }
                    if (WaitForMultipleObjectsEx(2, hs, FALSE, INFINITE, FALSE) == WAIT_OBJECT_0) return 0;
                }
            }
        }
    }
    DWORD ms = (DWORD)((ns + 999999) / 1000000);
    Sleep(ms);
    return 0;
}

/* The caller's effective uid/gid. One identity for the whole layer for now: XRL_UID/XRL_GID in the host
 * environment, 1000 when unset (what getuid always answered). proc.c's per-process ids replace this. */
static u32 env_id(const char *name)
{
    char b[16];
    DWORD n = GetEnvironmentVariableA(name, b, sizeof b);
    return n && n < sizeof b ? (u32)strtoul(b, NULL, 10) : 1000;
}
u32 xrl_cur_uid(void) { static u32 v = ~0u; if (v == ~0u) v = env_id("XRL_UID"); return v; }
u32 xrl_cur_gid(void) { static u32 v = ~0u; if (v == ~0u) v = env_id("XRL_GID"); return v; }

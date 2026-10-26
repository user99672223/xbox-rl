/*
 * misc.c - uname, sysinfo, getrandom, rlimits, affinity, clocks, sleeps.
 */
#include "xrl_internal.h"
#include <ctype.h>

int xrl_nprocs(void)
{
    static int n;
    if (!n) {
        SYSTEM_INFO si; GetNativeSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors;
        if (n < 1) n = 1;
        if (n > 64) n = 64;
    }
    return n;
}

/* The machine's own name (what Linux's hostname would be), not a made-up one. */
const char *xrl_hostname(void)
{
    static char name[65];
    if (!name[0]) {
        char b[256]; DWORD n = sizeof b;
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
        /* no GetComputerNameEx for apps; the environment carries the name */
        n = GetEnvironmentVariableA("COMPUTERNAME", b, sizeof b);
        if (n && n < sizeof name) {
#else
        if (GetComputerNameExA(ComputerNameDnsHostname, b, &n) && n && n < sizeof name) {
#endif
            for (DWORD i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)b[i]);
            memcpy(name, b, n + 1);
        } else strcpy_s(name, sizeof name, "localhost");
    }
    return name;
}

slong xrl_sys_uname(u64 ubuf)
{
    lx_utsname u;
    memset(&u, 0, sizeof u);
    strcpy_s(u.sysname, sizeof u.sysname, "Linux");
    strcpy_s(u.nodename, sizeof u.nodename, xrl_hostname());
    strcpy_s(u.release, sizeof u.release, "6.1.0-xrl");
    strcpy_s(u.version, sizeof u.version, "#1 SMP xbox-rl layer " XRL_LAYER_VERSION);
    strcpy_s(u.machine, sizeof u.machine, "x86_64");
    strcpy_s(u.domainname, sizeof u.domainname, "(none)");
    return xrl_ctu(ubuf, &u, sizeof u);
}

slong xrl_sys_sysinfo(u64 ubuf)
{
    lx_sysinfo s; memset(&s, 0, sizeof s);
    u64 tot = G.mem_budget ? G.mem_budget : (3ull << 30);
    u64 used = xrl_mem_committed();
    s.uptime = (s64)(xrl_mono_ns() / 1000000000ull);
    s.totalram = tot; s.freeram = tot > used ? tot - used : 0;
    s.procs = (u16)G.nthreads; s.mem_unit = 1;
    return xrl_ctu(ubuf, &s, sizeof s);
}

slong xrl_sys_getrandom(u64 buf, u64 len, u32 flags)
{
    (void)flags;
    if (flags & ~(LX_GRND_NONBLOCK | LX_GRND_RANDOM | 4)) return -LX_EINVAL;
    if (len > 33554431) len = 33554431;
    u8 tmp[512];
    u64 done = 0;
    while (done < len) {
        size_t c = (size_t)((len - done) > sizeof tmp ? sizeof tmp : (len - done));
        xrl_rng_fill(tmp, c);
        if (xrl_ctu(buf + done, tmp, c)) return done ? (slong)done : -LX_EFAULT;
        done += c;
    }
    return (slong)len;
}

slong xrl_sys_prlimit(int pid, int res, u64 newp, u64 oldp)
{
    (void)newp;
    if (pid != 0 && pid != G.pid) return -LX_ESRCH;
    lx_rlimit r;
    switch (res) {
    case LX_RLIMIT_STACK: r.rlim_cur = 8u << 20; r.rlim_max = LX_RLIM_INFINITY; break;
    case LX_RLIMIT_NOFILE: r.rlim_cur = 65536; r.rlim_max = 65536; break;
    case LX_RLIMIT_CORE: r.rlim_cur = 0; r.rlim_max = LX_RLIM_INFINITY; break;
    case LX_RLIMIT_NPROC: r.rlim_cur = r.rlim_max = 32768; break;
    case LX_RLIMIT_MEMLOCK: r.rlim_cur = r.rlim_max = 64 << 10; break;
    default: r.rlim_cur = r.rlim_max = LX_RLIM_INFINITY; break;
    }
    if (res < 0 || res > 15) return -LX_EINVAL;
    if (oldp && xrl_ctu(oldp, &r, sizeof r)) return -LX_EFAULT;
    return 0;   /* set requests are accepted and ignored */
}

slong xrl_sys_sched_getaffinity(int pid, u64 size, u64 mask)
{
    (void)pid;
    if (size < 8) return -LX_EINVAL;
    u64 bits = 0;
    int n = xrl_nprocs();
    for (int i = 0; i < n; i++) bits |= 1ull << i;
    u64 outsz = size > 128 ? 128 : size;
    outsz &= ~7ull;
    u8 buf[128] = {0};
    memcpy(buf, &bits, 8);
    if (xrl_ctu(mask, buf, (size_t)outsz)) return -LX_EFAULT;
    return (slong)outsz;
}

/* ================================================================= time */
static void ns_to_ts(u64 ns, lx_timespec *ts) { ts->tv_sec = (s64)(ns / 1000000000ull); ts->tv_nsec = (s64)(ns % 1000000000ull); }

static u64 ft_ns(FILETIME ft) { return (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) * 100ull; }

static slong clock_ns(int clk, u64 *out)
{
    switch (clk) {
    case LX_CLOCK_REALTIME:
    case LX_CLOCK_REALTIME_COARSE: *out = xrl_real_ns(); return 0;
    case LX_CLOCK_MONOTONIC:
    case LX_CLOCK_MONOTONIC_RAW:
    case LX_CLOCK_MONOTONIC_COARSE:
    case LX_CLOCK_BOOTTIME: *out = xrl_mono_ns(); return 0;
    case LX_CLOCK_PROCESS_CPUTIME_ID: {
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return xrl_lasterr();
        *out = ft_ns(k) + ft_ns(u); return 0;
    }
    case LX_CLOCK_THREAD_CPUTIME_ID: {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return xrl_lasterr();
        *out = ft_ns(k) + ft_ns(u); return 0;
    }
    default: return -LX_EINVAL;
    }
}

slong xrl_sys_clock_gettime(int clk, u64 uts)
{
    u64 ns;
    slong r = clock_ns(clk, &ns);
    if (r) return r;
    lx_timespec ts; ns_to_ts(ns, &ts);
    return xrl_ctu(uts, &ts, sizeof ts);
}

/* The guest's vDSO-style clock (XRL_SYS_GET_VCLOCK), reached through xrl_vclock (thunks.asm) on
 * the guest's own stack with the guest TEB view live: only QueryPerformanceCounter and
 * GetSystemTimePreciseAsFileTime, which read the shared user data page and nothing per-thread.
 * The game's render thread reads the clock thousands of times a frame. */
slong xrl_vclock_c(int clk, lx_timespec *ts)
{
    u64 ns;
    switch (clk) {
    case LX_CLOCK_REALTIME:
    case LX_CLOCK_REALTIME_COARSE: ns = xrl_real_ns(); break;
    case LX_CLOCK_MONOTONIC:
    case LX_CLOCK_MONOTONIC_RAW:
    case LX_CLOCK_MONOTONIC_COARSE:
    case LX_CLOCK_BOOTTIME: ns = xrl_mono_ns(); break;
    default: return -LX_ENOSYS;   /* CPU-time clocks: the syscall */
    }
    ns_to_ts(ns, ts);
    return 0;
}

slong xrl_sys_clock_getres(int clk, u64 uts)
{
    if (clk < 0 || clk > LX_CLOCK_BOOTTIME || clk == 8 || clk == 9 || clk == 10) {}
    if (!uts) return 0;
    lx_timespec ts = { 0, (clk == LX_CLOCK_REALTIME_COARSE || clk == LX_CLOCK_MONOTONIC_COARSE) ? 1000000 : 1 };
    return xrl_ctu(uts, &ts, sizeof ts);
}

slong xrl_sys_gettimeofday(u64 utv, u64 utz)
{
    if (utv) {
        u64 ns = xrl_real_ns();
        lx_timeval tv = { (s64)(ns / 1000000000ull), (s64)((ns % 1000000000ull) / 1000) };
        if (xrl_ctu(utv, &tv, sizeof tv)) return -LX_EFAULT;
    }
    if (utz) { s32 tz[2] = { 0, 0 }; if (xrl_ctu(utz, tz, 8)) return -LX_EFAULT; }
    return 0;
}

slong xrl_sys_time(u64 ut)
{
    s64 t = (s64)(xrl_real_ns() / 1000000000ull);
    if (ut && xrl_ctu(ut, &t, 8)) return -LX_EFAULT;
    return (slong)t;
}

/* A signal ends the sleep: -EINTR with the time left in *urem (never restarted) */
static slong sleep_or_eintr(u64 ns, u64 urem)
{
    u64 t0 = xrl_mono_ns();
    int intr = 0;
    xrl_sleep_ns(ns, &intr);
    u64 el = xrl_mono_ns() - t0, left = intr && ns > el ? ns - el : 0;
    if (urem) {
        lx_timespec z = { (s64)(left / 1000000000ull), (s64)(left % 1000000000ull) };
        xrl_ctu(urem, &z, sizeof z);
    }
    return intr ? -LX_EINTR : 0;
}

slong xrl_sys_nanosleep(u64 ureq, u64 urem)
{
    lx_timespec ts;
    if (xrl_cfu(&ts, ureq, sizeof ts)) return -LX_EFAULT;
    u64 ns = xrl_ts_to_ns(&ts);
    if (ns == ~0ull) return -LX_EINVAL;
    return sleep_or_eintr(ns, urem);
}

slong xrl_sys_clock_nanosleep(int clk, int flags, u64 ureq, u64 urem)
{
    lx_timespec ts;
    if (xrl_cfu(&ts, ureq, sizeof ts)) return -LX_EFAULT;
    u64 ns = xrl_ts_to_ns(&ts);
    if (ns == ~0ull) return -LX_EINVAL;
    if (clk == LX_CLOCK_PROCESS_CPUTIME_ID) return -LX_EINVAL;
    if (flags & LX_TIMER_ABSTIME) {
        u64 now;
        slong r = clock_ns(clk, &now);
        if (r) return r;
        ns = ns > now ? ns - now : 0;
        return sleep_or_eintr(ns, 0);   /* absolute: nothing to report back */
    }
    return sleep_or_eintr(ns, urem);
}

slong xrl_sys_times(u64 ubuf)
{
    FILETIME c, e, k, u;
    lx_tms t; memset(&t, 0, sizeof t);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { t.tms_utime = (s64)(ft_ns(u) / 10000000ull); t.tms_stime = (s64)(ft_ns(k) / 10000000ull); }
    if (ubuf && xrl_ctu(ubuf, &t, sizeof t)) return -LX_EFAULT;
    return (slong)(xrl_mono_ns() / 10000000ull);
}

slong xrl_sys_getrusage(int who, u64 ubuf)
{
    (void)who;
    FILETIME c, e, k, u;
    lx_rusage r; memset(&r, 0, sizeof r);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        u64 un = ft_ns(u), kn = ft_ns(k);
        r.ru_utime.tv_sec = (s64)(un / 1000000000ull); r.ru_utime.tv_usec = (s64)((un % 1000000000ull) / 1000);
        r.ru_stime.tv_sec = (s64)(kn / 1000000000ull); r.ru_stime.tv_usec = (s64)((kn % 1000000000ull) / 1000);
    }
    r.ru_maxrss = (s64)(xrl_mem_committed() >> 10);
    return xrl_ctu(ubuf, &r, sizeof r);
}

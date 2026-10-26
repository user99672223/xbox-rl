/*
 * timers.c - timerfd, setitimer/getitimer/alarm, and inotify descriptors.
 *
 * One helper thread (not a guest thread) owns all the timers. It sleeps on a high-resolution waitable timer set
 * to the earliest expiry of any armed timerfd or of the real-time interval timer, and an event that says "the
 * set changed". At an expiry it books the expirations into the timerfd (count += 1 + overruns), re-arms an
 * interval timer or disarms a one-shot, and bumps the layer's I/O epoch (ipc.c) so read()/poll()/epoll_wait()
 * sleepers look again - the same wake-up path pipes and sockets use, so a timerfd needs no change in poll/epoll.
 * ITIMER_REAL (and alarm) is per process: one entry per pid, kept across exec, not inherited by a fork, signalled with
 * xrl_signal_pid (util.c; the pid comes from xrl_cur_pid()). ITIMER_VIRTUAL/ITIMER_PROF are accepted and never fire.
 *
 * inotify: a descriptor that never reports an event. inotify_add_watch validates the path and hands out watch
 * numbers; programs that watch files for changes (glib, Firefox) then simply see none. Not provided: signalfd.
 */
#include "xrl_internal.h"

#define LX_TFD_TIMER_ABSTIME 1
#define LX_TFD_NONBLOCK 04000
#define LX_TFD_CLOEXEC 02000000
#define LX_CLOCK_REALTIME_ 0
#define LX_CLOCK_MONOTONIC_ 1
#define LX_CLOCK_BOOTTIME_ 7
#define LX_CLOCK_REALTIME_ALARM_ 8
#define LX_CLOCK_BOOTTIME_ALARM_ 9

typedef struct tfd {
    struct tfd *next;
    int clock;
    int armed;
    u64 next_ns, interval_ns;     /* on the layer's monotonic clock */
    u64 expirations;
} tfd;

static SRWLOCK g_tm_lock = SRWLOCK_INIT;     /* guards every timer below */
static tfd *g_tfds;
typedef struct itimer { struct itimer *next; int pid; u64 next_ns, interval_ns; } itimer;
static itimer *g_its;   /* ITIMER_REAL timers, one per process; next_ns ~0 = disarmed (the thread drops it) */
static INIT_ONCE g_tm_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_tm_ev, g_tm_timer;

static void tm_wake(void) { if (g_tm_ev) SetEvent(g_tm_ev); }

static DWORD WINAPI tm_thread(LPVOID unused)
{
    (void)unused;
    HANDLE h[2] = { g_tm_ev, g_tm_timer };
    for (;;) {
        u64 now = xrl_mono_ns(), earliest = ~0ull;
        int notify = 0;
        AcquireSRWLockExclusive(&g_tm_lock);
        for (tfd *t = g_tfds; t; t = t->next) {
            if (!t->armed) continue;
            if (t->next_ns <= now) {
                if (t->interval_ns) { u64 k = 1 + (now - t->next_ns) / t->interval_ns; t->expirations += k; t->next_ns += k * t->interval_ns; }
                else { t->expirations++; t->armed = 0; }
                notify = 1;
            }
            if (t->armed && t->next_ns < earliest) earliest = t->next_ns;
        }
        int fire[16], nfire = 0;
        for (itimer *it = g_its; it; it = it->next) {
            if (it->next_ns <= now) {
                if (nfire < 16) fire[nfire++] = it->pid;
                if (it->interval_ns) { u64 k = 1 + (now - it->next_ns) / it->interval_ns; it->next_ns += k * it->interval_ns; } else it->next_ns = ~0ull;
            }
            if (it->next_ns != ~0ull && it->next_ns < earliest) earliest = it->next_ns;
        }
        for (itimer **pp = &g_its; *pp;) if ((*pp)->next_ns == ~0ull) { itimer *d = *pp; *pp = d->next; free(d); } else pp = &(*pp)->next;
        ReleaseSRWLockExclusive(&g_tm_lock);
        if (notify) xrl_io_notify();
        for (int i = 0; i < nfire; i++) xrl_signal_pid(fire[i], LX_SIGALRM);
        if (earliest == ~0ull) { CancelWaitableTimer(g_tm_timer); }
        else {
            now = xrl_mono_ns();
            LARGE_INTEGER due;
            due.QuadPart = earliest > now ? -(LONGLONG)((earliest - now) / 100 + 1) : -1;   /* relative, 100 ns units */
            SetWaitableTimer(g_tm_timer, &due, 0, NULL, NULL, FALSE);
        }
        WaitForMultipleObjects(2, h, FALSE, INFINITE);
    }
}

static BOOL CALLBACK tm_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    g_tm_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_tm_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_tm_timer) g_tm_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    HANDLE th = g_tm_ev && g_tm_timer ? CreateThread(NULL, 0, tm_thread, NULL, 0, NULL) : NULL;
    if (!th) { LOGE("timers: cannot start the timer thread: timerfd and setitimer will not fire"); g_tm_ev = NULL; return TRUE; }
    CloseHandle(th);
    return TRUE;
}
static int tm_up(void) { InitOnceExecuteOnce(&g_tm_once, tm_init, NULL, NULL); return g_tm_ev != NULL; }

static u64 ts_ns(const lx_timespec *t) { return (u64)t->tv_sec * 1000000000ull + (u64)t->tv_nsec; }
static void ns_ts(u64 ns, lx_timespec *t) { t->tv_sec = (s64)(ns / 1000000000ull); t->tv_nsec = (s64)(ns % 1000000000ull); }

/* ================================================================ timerfd */
static slong tfd_read(xfile *f, void *buf, u64 n, s64 off)
{
    (void)off;
    tfd *t = (tfd *)f->priv;
    if (n < 8) return -LX_EINVAL;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&g_tm_lock);
        u64 e = t->expirations;
        t->expirations = 0;
        ReleaseSRWLockExclusive(&g_tm_lock);
        if (e) return xrl_ctu((u64)buf, &e, 8) ? -LX_EFAULT : 8;
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
    }
}
static slong tfd_write(xfile *f, const void *b, u64 n, s64 o) { (void)f; (void)b; (void)n; (void)o; return -LX_EINVAL; }
static slong tfd_lseek(xfile *f, s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }
static slong tfd_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFREG | 0600, (u64)(uintptr_t)f, 0); return 0; }
static slong tfd_ioctl(xfile *f, u64 req, u64 arg)
{
    tfd *t = (tfd *)f->priv;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    if (req == LX_FIONREAD) { AcquireSRWLockShared(&g_tm_lock); int n = t->expirations ? 8 : 0; ReleaseSRWLockShared(&g_tm_lock); return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0; }
    return -LX_ENOTTY;
}
static u32 tfd_poll(xfile *f, u32 ev)
{
    tfd *t = (tfd *)f->priv;
    AcquireSRWLockShared(&g_tm_lock);
    u32 r = t->expirations ? (LX_POLLIN | LX_POLLRDNORM) : 0;
    ReleaseSRWLockShared(&g_tm_lock);
    return r & ev;
}
static void tfd_close(xfile *f)
{
    tfd *t = (tfd *)f->priv;
    AcquireSRWLockExclusive(&g_tm_lock);
    for (tfd **pp = &g_tfds; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    ReleaseSRWLockExclusive(&g_tm_lock);
    tm_wake();
    free(t);
}
static const xfile_ops g_tfd_ops = { "timerfd", tfd_read, tfd_write, tfd_lseek, tfd_fstat, NULL, tfd_ioctl, NULL, NULL, tfd_poll, tfd_close, NULL };

slong xrl_timerfd_create(int clock, int flags)
{
    if (clock != LX_CLOCK_REALTIME_ && clock != LX_CLOCK_MONOTONIC_ && clock != LX_CLOCK_BOOTTIME_ &&
        clock != LX_CLOCK_REALTIME_ALARM_ && clock != LX_CLOCK_BOOTTIME_ALARM_) return -LX_EINVAL;
    if (flags & ~(LX_TFD_NONBLOCK | LX_TFD_CLOEXEC)) return -LX_EINVAL;
    if (!tm_up()) return -LX_ENOSYS;
    tfd *t = (tfd *)calloc(1, sizeof *t);
    xfile *f = t ? xfile_new(&g_tfd_ops, LX_O_RDWR | (flags & LX_TFD_NONBLOCK)) : NULL;
    if (!f) { free(t); return -LX_ENOMEM; }
    t->clock = clock;
    f->priv = t;
    AcquireSRWLockExclusive(&g_tm_lock);
    t->next = g_tfds; g_tfds = t;
    ReleaseSRWLockExclusive(&g_tm_lock);
    int fd = xrl_fd_install(f, (flags & LX_TFD_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

static void tfd_fill(const tfd *t, u64 now, lx_timespec out[2])   /* {interval, value}; lock held */
{
    ns_ts(t->interval_ns, &out[0]);
    ns_ts(t->armed && t->next_ns > now ? t->next_ns - now : t->armed ? 1 : 0, &out[1]);
}

slong xrl_timerfd_settime(int fd, int flags, u64 unew, u64 uold)
{
    if (flags & ~LX_TFD_TIMER_ABSTIME) return -LX_EINVAL;
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->ops != &g_tfd_ops) { xfile_unref(f); return -LX_EINVAL; }
    tfd *t = (tfd *)f->priv;
    lx_timespec nv[2];   /* it_interval, it_value */
    slong r = 0;
    if (xrl_cfu(nv, unew, sizeof nv)) r = -LX_EFAULT;
    else if (nv[0].tv_sec < 0 || nv[0].tv_nsec < 0 || nv[0].tv_nsec >= 1000000000 || nv[1].tv_sec < 0 || nv[1].tv_nsec < 0 || nv[1].tv_nsec >= 1000000000) r = -LX_EINVAL;
    if (!r) {
        u64 now = xrl_mono_ns();
        lx_timespec old[2];
        AcquireSRWLockExclusive(&g_tm_lock);
        tfd_fill(t, now, old);
        t->expirations = 0;
        u64 val = ts_ns(&nv[1]);
        if (!val) { t->armed = 0; t->interval_ns = 0; }
        else {
            u64 due;
            if (flags & LX_TFD_TIMER_ABSTIME) {   /* a REALTIME clock's absolute time is converted to the monotonic base */
                u64 real = xrl_real_ns();
                int rt = t->clock == LX_CLOCK_REALTIME_ || t->clock == LX_CLOCK_REALTIME_ALARM_;
                due = rt ? (val > real ? now + (val - real) : now) : (val > now ? val : now);
            } else due = now + val;
            t->next_ns = due; t->interval_ns = ts_ns(&nv[0]); t->armed = 1;
        }
        ReleaseSRWLockExclusive(&g_tm_lock);
        tm_wake();
        xrl_io_notify();
        if (uold && xrl_ctu(uold, old, sizeof old)) r = -LX_EFAULT;
    }
    xfile_unref(f);
    return r;
}

slong xrl_timerfd_gettime(int fd, u64 ucur)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->ops != &g_tfd_ops) { xfile_unref(f); return -LX_EINVAL; }
    tfd *t = (tfd *)f->priv;
    lx_timespec cur[2];
    AcquireSRWLockShared(&g_tm_lock);
    tfd_fill(t, xrl_mono_ns(), cur);
    ReleaseSRWLockShared(&g_tm_lock);
    xfile_unref(f);
    return xrl_ctu(ucur, cur, sizeof cur) ? -LX_EFAULT : 0;
}

/* ============================================================== itimers */
static void tv_us(u64 ns, lx_timeval *t) { t->tv_sec = (s64)(ns / 1000000000ull); t->tv_usec = (s64)(ns % 1000000000ull) / 1000; }

static itimer *it_find(int pid) { for (itimer *it = g_its; it; it = it->next) if (it->pid == pid) return it; return NULL; }   /* lock held */
static itimer *it_get(int pid)   /* lock held */
{
    itimer *it = it_find(pid);
    if (!it) { it = (itimer *)calloc(1, sizeof *it); if (it) { it->pid = pid; it->next_ns = ~0ull; it->next = g_its; g_its = it; } }
    return it;
}

slong xrl_setitimer(int which, u64 unew, u64 uold)
{
    if (which < 0 || which > 2) return -LX_EINVAL;
    lx_timeval nv[2];   /* it_interval, it_value */
    if (unew && xrl_cfu(nv, unew, sizeof nv)) return -LX_EFAULT;
    if (unew && (nv[0].tv_sec < 0 || nv[0].tv_usec < 0 || nv[0].tv_usec >= 1000000 || nv[1].tv_sec < 0 || nv[1].tv_usec < 0 || nv[1].tv_usec >= 1000000)) return -LX_EINVAL;
    if (!tm_up()) return -LX_ENOSYS;
    lx_timeval old[2]; memset(old, 0, sizeof old);
    if (which == 0) {
        int pid = xrl_cur_pid();
        u64 now = xrl_mono_ns();
        AcquireSRWLockExclusive(&g_tm_lock);
        itimer *it = it_find(pid);
        if (it && it->next_ns != ~0ull) { tv_us(it->interval_ns, &old[0]); tv_us(it->next_ns > now ? it->next_ns - now : 0, &old[1]); }
        if (unew) {
            u64 val = (u64)nv[1].tv_sec * 1000000000ull + (u64)nv[1].tv_usec * 1000ull;
            if (!val) { if (it) { it->interval_ns = 0; it->next_ns = ~0ull; } }
            else if ((it = it_get(pid)) != NULL) { it->next_ns = now + val; it->interval_ns = (u64)nv[0].tv_sec * 1000000000ull + (u64)nv[0].tv_usec * 1000ull; }
        }
        ReleaseSRWLockExclusive(&g_tm_lock);
        if (unew) tm_wake();
    }
    return uold && xrl_ctu(uold, old, sizeof old) ? -LX_EFAULT : 0;
}

slong xrl_getitimer(int which, u64 ucur)
{
    if (which < 0 || which > 2) return -LX_EINVAL;
    lx_timeval cur[2]; memset(cur, 0, sizeof cur);
    if (which == 0) {
        u64 now = xrl_mono_ns();
        AcquireSRWLockShared(&g_tm_lock);
        itimer *it = it_find(xrl_cur_pid());
        if (it && it->next_ns != ~0ull) { tv_us(it->interval_ns, &cur[0]); tv_us(it->next_ns > now ? it->next_ns - now : 0, &cur[1]); }
        ReleaseSRWLockShared(&g_tm_lock);
    }
    return xrl_ctu(ucur, cur, sizeof cur) ? -LX_EFAULT : 0;
}

slong xrl_alarm(unsigned seconds)
{
    if (!tm_up()) return -LX_ENOSYS;
    int pid = xrl_cur_pid();
    u64 now = xrl_mono_ns();
    AcquireSRWLockExclusive(&g_tm_lock);
    itimer *it = it_find(pid);
    u64 left = it && it->next_ns != ~0ull && it->next_ns > now ? it->next_ns - now : 0;
    if (!seconds) { if (it) { it->interval_ns = 0; it->next_ns = ~0ull; } }
    else if ((it = it_get(pid)) != NULL) { it->next_ns = now + (u64)seconds * 1000000000ull; it->interval_ns = 0; }
    ReleaseSRWLockExclusive(&g_tm_lock);
    tm_wake();
    return (slong)((left + 999999999ull) / 1000000000ull);
}

/* ================================================================ inotify */
typedef struct ino { LONG next_wd; } ino;

static slong ino_read(xfile *f, void *b, u64 n, s64 o)
{
    (void)b; (void)o;
    if (n < 16) return -LX_EINVAL;   /* sizeof(struct inotify_event) */
    for (;;) {   /* no event ever arrives */
        LONG seen = xrl_io_epoch;
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
    }
}
static slong ino_write(xfile *f, const void *b, u64 n, s64 o) { (void)f; (void)b; (void)n; (void)o; return -LX_EINVAL; }
static slong ino_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFREG | 0600, (u64)(uintptr_t)f, 0); return 0; }
static slong ino_ioctl(xfile *f, u64 req, u64 arg)
{
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    if (req == LX_FIONREAD) { int n = 0; return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0; }
    return -LX_ENOTTY;
}
static u32 ino_poll(xfile *f, u32 ev) { (void)f; (void)ev; return 0; }
static void ino_close(xfile *f) { free(f->priv); }
static const xfile_ops g_ino_ops = { "inotify", ino_read, ino_write, tfd_lseek, ino_fstat, NULL, ino_ioctl, NULL, NULL, ino_poll, ino_close, NULL };

slong xrl_inotify_init(int flags)
{
    if (flags & ~(LX_TFD_NONBLOCK | LX_TFD_CLOEXEC)) return -LX_EINVAL;
    xfile *f = xfile_new(&g_ino_ops, LX_O_RDWR | (flags & LX_TFD_NONBLOCK));
    if (!f) return -LX_ENOMEM;
    f->priv = calloc(1, sizeof(ino));
    int fd = xrl_fd_install(f, (flags & LX_TFD_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

slong xrl_inotify_add_watch(int fd, const char *path, u32 mask)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r;
    if (f->ops != &g_ino_ops) r = -LX_EINVAL;
    else if (!(mask & 0xfff)) r = -LX_EINVAL;
    else {
        lx_stat st;
        r = xrl_vfs_stat(LX_AT_FDCWD, path, (mask & 0x2000000 /*IN_DONT_FOLLOW*/) ? LX_AT_SYMLINK_NOFOLLOW : 0, &st);
        if (!r) r = InterlockedIncrement(&((ino *)f->priv)->next_wd);
    }
    xfile_unref(f);
    return r;
}

slong xrl_inotify_rm_watch(int fd, int wd)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r = f->ops != &g_ino_ops || wd <= 0 ? -LX_EINVAL : 0;
    xfile_unref(f);
    return r;
}

/*
 * ntsync.c - /dev/ntsync: NT synchronization objects (Linux 6.14 uapi,
 * include/uapi/linux/ntsync.h), implemented in the layer.
 *
 * Why: without it Wine does every wait, event, mutex and semaphore operation
 * as a request to wineserver (here a thread, reached through our pipes), and
 * Rocket League's loading spent its time queued behind that one thread. With
 * /dev/ntsync, Wine's ntdll waits on these objects directly ("ntsync: up and
 * running.").
 *
 * Model, as in the kernel driver: each object is an fd holding its state (sem
 * count/max, mutex owner/recursion/abandoned, event manual/signaled). All
 * state is guarded by one lock. A waiting thread registers on every object it
 * waits for and parks on its own Win32 event; any state change re-evaluates
 * the waiters of that object and grants a wait atomically (wait-all acquires
 * all objects at once). Mutex owners are the caller-chosen u32 ids Wine passes
 * (its thread ids), not OS threads, so Win32 mutexes would not fit.
 */
#include "xrl_internal.h"

#define NTSYNC_WAIT_REALTIME 0x1
#define NTSYNC_MAX_WAIT_COUNT 64

/* _IOW/_IOR/_IOWR('N', nr, type) */
#define NIOC(dir, nr, sz) (((u64)(dir) << 30) | ((u64)(sz) << 16) | ((u64)'N' << 8) | (nr))
#define NW 1u
#define NR 2u
#define IOC_CREATE_SEM    NIOC(NW, 0x80, 8)
#define IOC_SEM_RELEASE   NIOC(NW | NR, 0x81, 4)
#define IOC_WAIT_ANY      NIOC(NW | NR, 0x82, 40)
#define IOC_WAIT_ALL      NIOC(NW | NR, 0x83, 40)
#define IOC_CREATE_MUTEX  NIOC(NW, 0x84, 8)
#define IOC_MUTEX_UNLOCK  NIOC(NW | NR, 0x85, 8)
#define IOC_MUTEX_KILL    NIOC(NW, 0x86, 4)
#define IOC_CREATE_EVENT  NIOC(NW, 0x87, 8)
#define IOC_EVENT_SET     NIOC(NR, 0x88, 4)
#define IOC_EVENT_RESET   NIOC(NR, 0x89, 4)
#define IOC_EVENT_PULSE   NIOC(NR, 0x8a, 4)
#define IOC_SEM_READ      NIOC(NR, 0x8b, 8)
#define IOC_MUTEX_READ    NIOC(NR, 0x8c, 8)
#define IOC_EVENT_READ    NIOC(NR, 0x8d, 8)

typedef struct wait_args { u64 timeout, objs; u32 count, index, flags, owner, alert, pad; } wait_args;

enum { T_SEM = 1, T_MUTEX, T_EVENT };

typedef struct waiter waiter;
typedef struct nobj {
    int type;
    u32 a, b;            /* sem: count, max | mutex: owner, count | event: manual, signaled */
    int abandoned;       /* mutex */
    waiter **w;          /* waiters registered on this object */
    int nw, capw;
} nobj;

struct waiter {
    nobj *objs[NTSYNC_MAX_WAIT_COUNT];
    nobj *alert;
    u32 count, owner;
    int all;
    int done;            /* granted */
    int index;           /* granted index (count = alert) */
    int abandoned;       /* a granted mutex was abandoned */
    HANDLE ev;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
/* perf.c: waits that ran to their timeout (asked for under 50 ms: a frame limiter's sleep) and how late they ended */
volatile LONG64 xrl_ns_timeouts, xrl_ns_late_ns, xrl_ns_late_max;
static __declspec(thread) HANDLE t_ev;

static const xfile_ops g_obj_ops;

/* ---------------------------------------------------------- state rules */
static int can_acquire(const nobj *o, u32 owner)
{
    switch (o->type) {
    case T_SEM: return o->a > 0;
    case T_MUTEX: return owner && (o->a == 0 || o->a == owner);   /* owner 0 never acquires */
    case T_EVENT: return o->b != 0;
    }
    return 0;
}

/* take one unit; returns 1 if it was an abandoned mutex */
static int acquire(nobj *o, u32 owner)
{
    int ab = 0;
    switch (o->type) {
    case T_SEM: o->a--; break;
    case T_MUTEX:
        if (o->abandoned) { ab = 1; o->abandoned = 0; }
        o->a = owner; o->b++; break;
    case T_EVENT: if (!o->a) o->b = 0; break;   /* auto-reset */
    }
    return ab;
}

static void unregister(waiter *w, nobj *o)
{
    for (int i = 0; i < o->nw; i++)
        if (o->w[i] == w) { o->w[i] = o->w[--o->nw]; return; }
}

static int register_on(waiter *w, nobj *o)
{
    if (o->nw == o->capw) {
        int nc = o->capw ? o->capw * 2 : 4;
        waiter **n = (waiter **)realloc(o->w, (size_t)nc * sizeof *n);
        if (!n) return -1;
        o->w = n; o->capw = nc;
    }
    o->w[o->nw++] = w;
    return 0;
}

/* Grant `w` if it can be satisfied now. Lock held. */
static int try_grant(waiter *w)
{
    if (w->done) return 1;
    if (w->all) {
        int ok = 1;
        for (u32 i = 0; i < w->count && ok; i++) {
            /* the same mutex listed twice counts once; anything else twice is a caller error */
            ok = can_acquire(w->objs[i], w->owner);
        }
        if (ok) {
            int ab = 0;
            for (u32 i = 0; i < w->count; i++) ab |= acquire(w->objs[i], w->owner);
            w->done = 1; w->index = 0; w->abandoned = ab;
            return 1;
        }
    } else {
        for (u32 i = 0; i < w->count; i++) {
            if (can_acquire(w->objs[i], w->owner)) {
                w->abandoned = acquire(w->objs[i], w->owner);
                w->done = 1; w->index = (int)i;
                return 1;
            }
        }
    }
    if (w->alert && can_acquire(w->alert, w->owner)) {
        acquire(w->alert, w->owner);
        w->done = 1; w->index = (int)w->count; w->abandoned = 0;
        return 1;
    }
    return 0;
}

/* `o` changed state: grant whoever waits on it. Lock held. */
static void wake(nobj *o)
{
    for (int i = 0; i < o->nw; i++) {
        waiter *w = o->w[i];
        if (!w->done && try_grant(w)) SetEvent(w->ev);
    }
}

/* ------------------------------------------------------------- objects */
static void obj_close(xfile *f)
{
    nobj *o = (nobj *)f->priv;
    if (!o) return;
    free(o->w);   /* no waiters: each holds a file ref while waiting */
    free(o);
}

static slong obj_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFREG | 0600, (u64)(uintptr_t)f, 0); return 0; }

static slong obj_ioctl(xfile *f, u64 req, u64 arg)
{
    nobj *o = (nobj *)f->priv;
    u32 v[2] = {0, 0};
    slong r = 0;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    switch (req) {
    case IOC_SEM_RELEASE:
        if (o->type != T_SEM) return -LX_EINVAL;
        if (xrl_cfu(v, arg, 4)) return -LX_EFAULT;
        AcquireSRWLockExclusive(&g_lock);
        { u32 prev = o->a;
          if ((u64)prev + v[0] > o->b) r = -LX_EOVERFLOW;
          else { o->a += v[0]; v[0] = prev; wake(o); } }
        ReleaseSRWLockExclusive(&g_lock);
        if (!r && xrl_ctu(arg, v, 4)) r = -LX_EFAULT;
        return r;
    case IOC_MUTEX_UNLOCK:
        if (o->type != T_MUTEX) return -LX_EINVAL;
        if (xrl_cfu(v, arg, 8)) return -LX_EFAULT;
        if (!v[0]) return -LX_EINVAL;
        AcquireSRWLockExclusive(&g_lock);
        if (o->a != v[0]) r = -LX_EPERM;
        else { v[1] = o->b; if (!--o->b) { o->a = 0; wake(o); } }
        ReleaseSRWLockExclusive(&g_lock);
        if (!r && xrl_ctu(arg, v, 8)) r = -LX_EFAULT;
        return r;
    case IOC_MUTEX_KILL:
        if (o->type != T_MUTEX) return -LX_EINVAL;
        if (xrl_cfu(v, arg, 4)) return -LX_EFAULT;
        if (!v[0]) return -LX_EINVAL;
        AcquireSRWLockExclusive(&g_lock);
        if (o->a != v[0]) r = -LX_EPERM;
        else { o->a = 0; o->b = 0; o->abandoned = 1; wake(o); }
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    case IOC_EVENT_SET: case IOC_EVENT_RESET: case IOC_EVENT_PULSE:
        if (o->type != T_EVENT) return -LX_EINVAL;
        AcquireSRWLockExclusive(&g_lock);
        v[0] = o->b;
        if (req == IOC_EVENT_RESET) o->b = 0;
        else { o->b = 1; wake(o); if (req == IOC_EVENT_PULSE) o->b = 0; }
        ReleaseSRWLockExclusive(&g_lock);
        return xrl_ctu(arg, v, 4) ? -LX_EFAULT : 0;
    case IOC_SEM_READ:
        if (o->type != T_SEM) return -LX_EINVAL;
        AcquireSRWLockShared(&g_lock); v[0] = o->a; v[1] = o->b; ReleaseSRWLockShared(&g_lock);
        return xrl_ctu(arg, v, 8) ? -LX_EFAULT : 0;
    case IOC_MUTEX_READ: {
        if (o->type != T_MUTEX) return -LX_EINVAL;
        int ab;
        AcquireSRWLockShared(&g_lock); v[0] = o->a; v[1] = o->b; ab = o->abandoned; ReleaseSRWLockShared(&g_lock);
        if (xrl_ctu(arg, v, 8)) return -LX_EFAULT;
        return ab ? -LX_EOWNERDEAD : 0;
    }
    case IOC_EVENT_READ:
        if (o->type != T_EVENT) return -LX_EINVAL;
        AcquireSRWLockShared(&g_lock); v[0] = o->a; v[1] = o->b; ReleaseSRWLockShared(&g_lock);
        return xrl_ctu(arg, v, 8) ? -LX_EFAULT : 0;
    }
    return -LX_ENOTTY;
}

static const xfile_ops g_obj_ops = { "ntsync-obj", NULL, NULL, NULL, obj_fstat, NULL, obj_ioctl, NULL, NULL, NULL, obj_close, NULL };

static slong create(int type, u64 arg)
{
    u32 v[2];
    if (xrl_cfu(v, arg, 8)) return -LX_EFAULT;
    if (type == T_SEM && v[0] > v[1]) return -LX_EINVAL;
    if (type == T_MUTEX && !v[0] != !v[1]) return -LX_EINVAL;   /* owner iff count */
    nobj *o = (nobj *)calloc(1, sizeof *o);
    if (!o) return -LX_ENOMEM;
    o->type = type; o->a = v[0]; o->b = v[1];
    if (type == T_EVENT) o->b = v[1] != 0;
    xfile *f = xfile_new(&g_obj_ops, LX_O_RDWR);
    if (!f) { free(o); return -LX_ENOMEM; }
    f->priv = o;
    int fd = xrl_fd_install(f, 1, 0);   /* the kernel returns O_CLOEXEC object fds */
    xfile_unref(f);
    return fd;
}

/* --------------------------------------------------------------- waits */
static slong do_wait(u64 arg, int all)
{
    wait_args a;
    if (xrl_cfu(&a, arg, sizeof a)) return -LX_EFAULT;
    if (a.count > NTSYNC_MAX_WAIT_COUNT || a.pad || (a.flags & ~NTSYNC_WAIT_REALTIME)) return -LX_EINVAL;
    u32 fds[NTSYNC_MAX_WAIT_COUNT];
    if (a.count && xrl_cfu(fds, a.objs, (size_t)a.count * 4)) return -LX_EFAULT;

    waiter w; memset(&w, 0, sizeof w);
    xfile *files[NTSYNC_MAX_WAIT_COUNT + 1];
    int nf = 0;
    slong r = 0;
    for (u32 i = 0; i < a.count; i++) {
        xfile *f = xrl_fd_get((int)fds[i]);
        if (!f || f->ops != &g_obj_ops) { if (f) xfile_unref(f); r = -LX_EINVAL; goto out; }
        files[nf++] = f;
        w.objs[i] = (nobj *)f->priv;
        if (all) for (u32 j = 0; j < i; j++) if (w.objs[j] == w.objs[i]) { r = -LX_EINVAL; goto out; }
    }
    if (a.alert) {
        xfile *f = xrl_fd_get((int)a.alert);
        if (!f || f->ops != &g_obj_ops || ((nobj *)f->priv)->type != T_EVENT) { if (f) xfile_unref(f); r = -LX_EINVAL; goto out; }
        files[nf++] = f;
        w.alert = (nobj *)f->priv;
    }
    w.count = a.count; w.owner = a.owner; w.all = all;
    /* the thread's signal event: a queued signal ends the wait with -EINTR,
     * like Linux's -ERESTARTSYS (rt_sigreturn restarts it for SA_RESTART) */
    xrl_thread *self = xrl_cur();
    if (!t_ev) t_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    w.ev = self && self->sig_ev ? self->sig_ev : t_ev;
    int intr = 0;

    AcquireSRWLockExclusive(&g_lock);
    if (!try_grant(&w)) {
        for (u32 i = 0; i < w.count; i++) register_on(&w, w.objs[i]);
        if (w.alert) register_on(&w, w.alert);
        ResetEvent(w.ev);
        u64 start = 0;
        for (;;) {
            u64 now = (a.flags & NTSYNC_WAIT_REALTIME) ? xrl_real_ns() : xrl_mono_ns();
            if (!start) start = now;
            if (a.timeout != ~0ull && now >= a.timeout) {
                if (a.timeout > start && a.timeout - start < 50000000ull) {
                    u64 late = now - a.timeout;
                    InterlockedIncrement64(&xrl_ns_timeouts); InterlockedAdd64(&xrl_ns_late_ns, (LONG64)late);
                    if ((LONG64)late > xrl_ns_late_max) xrl_ns_late_max = (LONG64)late;   /* (a statistic: racy max is fine) */
                }
                break;
            }
            if (self && xrl_sig_pending_for(self)) { intr = 1; break; }
            DWORD ms = INFINITE;
            if (a.timeout != ~0ull) {
                u64 left = (a.timeout - now + 999999) / 1000000;
                ms = left >= INFINITE ? INFINITE - 1 : (DWORD)left;
            }
            ReleaseSRWLockExclusive(&g_lock);
            WaitForSingleObject(w.ev, ms);
            AcquireSRWLockExclusive(&g_lock);
            if (w.done) break;
        }
        for (u32 i = 0; i < w.count; i++) unregister(&w, w.objs[i]);
        if (w.alert) unregister(&w, w.alert);
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (!w.done) r = intr ? -LX_EINTR : -LX_ETIMEDOUT;
    else {
        a.index = (u32)w.index;
        if (xrl_ctu(arg, &a, sizeof a)) r = -LX_EFAULT;
        else r = w.abandoned ? -LX_EOWNERDEAD : 0;
    }
out:
    for (int i = 0; i < nf; i++) xfile_unref(files[i]);
    return r;
}

/* -------------------------------------------------------------- device */
static slong dev_ioctl(xfile *f, u64 req, u64 arg)
{
    (void)f;
    switch (req) {
    case IOC_CREATE_SEM: return create(T_SEM, arg);
    case IOC_CREATE_MUTEX: return create(T_MUTEX, arg);
    case IOC_CREATE_EVENT: return create(T_EVENT, arg);
    case IOC_WAIT_ANY: return do_wait(arg, 0);
    case IOC_WAIT_ALL: return do_wait(arg, 1);
    case LX_FIOCLEX: case LX_FIONCLEX: return 0;
    }
    return -LX_ENOTTY;
}

static slong dev_fstat(xfile *f, lx_stat *st) { (void)f; xrl_stat_synth(st, LX_S_IFCHR | 0666, 0x6e7473, 0); return 0; }

static const xfile_ops g_dev_ops = { "ntsync", NULL, NULL, NULL, dev_fstat, NULL, dev_ioctl, NULL, NULL, NULL, NULL, NULL };

xfile *xrl_dev_ntsync(int flags) { return xfile_new(&g_dev_ops, flags); }

/*
 * ipc.c - in-process pipes, eventfd, AF_UNIX socketpair (with SCM_RIGHTS),
 * poll/ppoll and epoll.
 *
 * Everything is a user-space object guarded by SRW locks. Blocking operations
 * and poll() sleep on a single global "I/O epoch" word (WaitOnAddress): every
 * state change that could make some fd readable/writable bumps the epoch and
 * wakes all sleepers, who re-evaluate. Simple and correct; the thundering herd
 * is irrelevant at Wine's fd counts. A waiter reads the epoch BEFORE it
 * evaluates readiness, so a change between the check and the sleep is never
 * lost.
 *
 * Not implemented here: real network sockets (socket/connect/bind of
 * AF_UNIX-by-path or AF_INET). They return -EAFNOSUPPORT / -ENOSYS (logged).
 * The Wine<->wineserver channel is expected to be in-memory (B's patch);
 * socketpair is here for anything that still wants an fd pair.
 */
#include "xrl_internal.h"

volatile LONG xrl_io_epoch;
volatile LONG xrl_io_sleepers;                 /* threads asleep on the epoch now */
volatile LONG64 xrl_io_notifies, xrl_io_woken;   /* perf.c: notifies, and sleepers woken by them */

/* Keyed wake-ups. Waking every sleeper on every change cost ~100k wake-ups a second in RL's menu (each Wine server
 * request is a few pipe writes and reads, each woke all threads asleep in poll/read): much of the console's CPU went
 * to threads waking, finding nothing, and sleeping again. Now a sleeper says which objects it waits for (a pipe's
 * buffer, an eventfd, the network, an epoll set) and a change to one object wakes only its sleepers; a sleeper on
 * anything else (n < 0) and a change of unknown scope (xrl_io_notify) still go to all. The epoch stays: a sleeper
 * registers, then re-checks it, so a change between its readiness check and its sleep is never lost.
 * XRL_IO_KEYED=0: the old wake-all. */
typedef struct io_sleeper { volatile LONG word; const void *const *keys; int n; struct io_sleeper *next; } io_sleeper;
static SRWLOCK g_sl_lock = SRWLOCK_INIT;
static io_sleeper *g_sl;
static int g_keyed = -1;

static int keyed(void)
{
    if (g_keyed < 0) { char v[4]; g_keyed = !(GetEnvironmentVariableA("XRL_IO_KEYED", v, sizeof v) && v[0] == '0'); }
    return g_keyed;
}

void xrl_io_notify_key(const void *key)
{
    InterlockedIncrement(&xrl_io_epoch);
    InterlockedIncrement64(&xrl_io_notifies);
    if (!keyed()) {
        LONG s = xrl_io_sleepers;
        if (s > 0) InterlockedAdd64(&xrl_io_woken, s);
        WakeByAddressAll((PVOID)&xrl_io_epoch);
        return;
    }
    if (!xrl_io_sleepers) return;   /* one registering now re-checks the epoch after it counted itself */
    LONG64 woken = 0;
    AcquireSRWLockShared(&g_sl_lock);
    for (io_sleeper *s = g_sl; s; s = s->next) {
        int hit = !key || s->n < 0;
        for (int i = 0; !hit && i < s->n; i++) hit = s->keys[i] == key;
        if (hit && !InterlockedExchange(&s->word, 1)) { WakeByAddressSingle((PVOID)&s->word); woken++; }
    }
    ReleaseSRWLockShared(&g_sl_lock);
    if (woken) InterlockedAdd64(&xrl_io_woken, woken);
}

void xrl_io_notify(void) { xrl_io_notify_key(NULL); }

/* returns 1 woken, 0 timeout, -1 interrupted by a signal */
int xrl_io_wait_keys(LONG seen, const void *const *keys, int n, s64 timeout_ns)
{
    xrl_thread *t = xrl_cur();
    if (t && xrl_sig_pending_for(t)) return -1;
    if (xrl_io_epoch != seen) return 1;
    u64 ns = timeout_ns < 0 ? ~0ull : (u64)timeout_ns;
    int r;
    if (!keyed()) {
        InterlockedIncrement(&xrl_io_sleepers);
        r = xrl_wait_addr((volatile void *)&xrl_io_epoch, &seen, sizeof seen, ns);
        InterlockedDecrement(&xrl_io_sleepers);
    } else {
        io_sleeper s = { 0, keys, keys ? n : -1, NULL };
        AcquireSRWLockExclusive(&g_sl_lock);
        s.next = g_sl; g_sl = &s;
        ReleaseSRWLockExclusive(&g_sl_lock);
        InterlockedIncrement(&xrl_io_sleepers);
        r = 1;
        if (xrl_io_epoch == seen) { LONG z = 0; r = xrl_wait_addr(&s.word, &z, sizeof z, ns); }
        InterlockedDecrement(&xrl_io_sleepers);
        AcquireSRWLockExclusive(&g_sl_lock);
        for (io_sleeper **pp = &g_sl; *pp; pp = &(*pp)->next) if (*pp == &s) { *pp = s.next; break; }
        ReleaseSRWLockExclusive(&g_sl_lock);
    }
    if (t && xrl_sig_pending_for(t)) return -1;
    return r;
}

int xrl_io_wait(LONG seen, s64 timeout_ns) { return xrl_io_wait_keys(seen, NULL, -1, timeout_ns); }

/* ============================================================== byte pipe */
#define PIPE_CAP 65536u

typedef struct fdmsg { u64 pos; int n; xfile *files[8]; struct fdmsg *next; } fdmsg;

typedef struct pipebuf {
    SRWLOCK lock;
    volatile LONG refs;
    u8 *buf;
    size_t head, len;
    u64 wpos, rpos;         /* monotonically increasing byte counters */
    int readers, writers;
    fdmsg *msgs, *msgs_tail;
} pipebuf;

static pipebuf *pb_new(void)
{
    pipebuf *p = (pipebuf *)calloc(1, sizeof *p);
    p->buf = (u8 *)malloc(PIPE_CAP);
    p->refs = 1;
    InitializeSRWLock(&p->lock);
    return p;
}
static void pb_unref(pipebuf *p)
{
    if (InterlockedDecrement(&p->refs) == 0) {
        for (fdmsg *m = p->msgs; m;) { fdmsg *n = m->next; for (int i = 0; i < m->n; i++) xfile_unref(m->files[i]); free(m); m = n; }
        free(p->buf); free(p);
    }
}

typedef struct pipeend {
    pipebuf *rx;   /* we read from this  (NULL for write-only pipe end) */
    pipebuf *tx;   /* we write into this (NULL for read-only pipe end)  */
    int is_socket;
    int type;
} pipeend;

static int nonblock(xfile *f) { return (f->flags & LX_O_NONBLOCK) != 0; }

static slong pb_read(xfile *f, pipebuf *p, void *ubuf, u64 n, int *got_msgs, fdmsg **out_msg)
{
    (void)f;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&p->lock);
        if (p->len > 0) {
            size_t c = p->len < n ? p->len : (size_t)n;
            /* do not read across the start of the next fd-carrying message */
            if (p->msgs && p->msgs->pos > p->rpos && p->msgs->pos - p->rpos < c) c = (size_t)(p->msgs->pos - p->rpos);
            size_t first = PIPE_CAP - p->head; if (first > c) first = c;
            int fault = xrl_ctu((u64)ubuf, p->buf + p->head, first);
            if (!fault && c > first) fault = xrl_ctu((u64)ubuf + first, p->buf, c - first);
            if (fault) { ReleaseSRWLockExclusive(&p->lock); return -LX_EFAULT; }
            if (got_msgs && p->msgs && p->msgs->pos >= p->rpos && p->msgs->pos < p->rpos + c) {
                *out_msg = p->msgs; p->msgs = p->msgs->next; if (!p->msgs) p->msgs_tail = NULL;
            }
            p->head = (p->head + c) % PIPE_CAP; p->len -= c; p->rpos += c;
            ReleaseSRWLockExclusive(&p->lock);
            xrl_io_notify_key(p);
            return (slong)c;
        }
        int eof = p->writers == 0;
        ReleaseSRWLockExclusive(&p->lock);
        if (eof) return 0;
        if (nonblock(f)) return -LX_EAGAIN;
        const void *k = p;
        int w = xrl_io_wait_keys(seen, &k, 1, -1);
        if (w < 0) return -LX_EINTR;
    }
}

static slong pb_write(xfile *f, pipebuf *p, const void *ubuf, u64 n, fdmsg *attach)
{
    u64 done = 0;
    while (done < n || (n == 0 && attach)) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&p->lock);
        if (p->readers == 0) { ReleaseSRWLockExclusive(&p->lock); return done ? (slong)done : -LX_EPIPE; }
        size_t space = PIPE_CAP - p->len;
        u64 want = n - done;
        /* writes <= PIPE_BUF are atomic: wait for room for all of it */
        int atomic = n <= 4096;
        if (space > 0 && (!atomic || space >= want || done)) {
            size_t c = space < want ? space : (size_t)want;
            size_t tail = (p->head + p->len) % PIPE_CAP;
            size_t first = PIPE_CAP - tail; if (first > c) first = c;
            int fault = xrl_cfu(p->buf + tail, (u64)ubuf + done, first);
            if (!fault && c > first) fault = xrl_cfu(p->buf, (u64)ubuf + done + first, c - first);
            if (fault) { ReleaseSRWLockExclusive(&p->lock); return done ? (slong)done : -LX_EFAULT; }
            if (attach) { attach->pos = p->wpos; attach->next = NULL; if (p->msgs_tail) p->msgs_tail->next = attach; else p->msgs = attach; p->msgs_tail = attach; attach = NULL; }
            p->len += c; p->wpos += c; done += c;
            ReleaseSRWLockExclusive(&p->lock);
            xrl_io_notify_key(p);
            if (done >= n) break;
            continue;
        }
        if (n == 0 && attach) {   /* zero-byte send carrying fds */
            attach->pos = p->wpos; attach->next = NULL;
            if (p->msgs_tail) p->msgs_tail->next = attach; else p->msgs = attach; p->msgs_tail = attach; attach = NULL;
            ReleaseSRWLockExclusive(&p->lock);
            xrl_io_notify_key(p);
            break;
        }
        ReleaseSRWLockExclusive(&p->lock);
        if (nonblock(f)) return done ? (slong)done : -LX_EAGAIN;
        const void *k = p;
        if (xrl_io_wait_keys(seen, &k, 1, -1) < 0) return done ? (slong)done : -LX_EINTR;
    }
    return (slong)done;
}

static slong pipe_read(xfile *f, void *b, u64 n, s64 o)
{
    (void)o;
    pipeend *e = (pipeend *)f->priv;
    if (!e->rx) return -LX_EBADF;
    if (!n) return 0;
    return pb_read(f, e->rx, b, n, NULL, NULL);
}
static slong pipe_write(xfile *f, const void *b, u64 n, s64 o)
{
    (void)o;
    pipeend *e = (pipeend *)f->priv;
    if (!e->tx) return -LX_EBADF;
    if (!n) return 0;
    {   /* XRL_SRVREQ=1: count what looks like a Wine server request header (req, request_size, reply_size), by code */
        static int on = -1;
        if (on < 0) { char v[4]; on = GetEnvironmentVariableA("XRL_SRVREQ", v, sizeof v) && v[0] == '1'; }
        const u32 *h = (const u32 *)b;
        xrl_thread *t;
        if (on && n >= 64 && h[0] < XRL_SRVREQ_MAX && h[1] < (1u << 20) && h[2] < (1u << 20) && (t = xrl_cur())) {
            if (!t->srvreq) t->srvreq = (u32 *)calloc(XRL_SRVREQ_MAX, sizeof(u32));
            if (t->srvreq) t->srvreq[h[0]]++;
        }
    }
    slong r = pb_write(f, e->tx, b, n, NULL);
    if (r == -LX_EPIPE && !e->is_socket) {   /* a pipe with no reader: SIGPIPE too, like Linux (sockets: net.c / MSG_NOSIGNAL) */
        xrl_thread *t = xrl_cur();
        if (t) xrl_sys_tgkill(-1, t->tid, LX_SIGPIPE);
    }
    return r;
}
static slong pipe_lseek(xfile *f, s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }
static slong pipe_fstat(xfile *f, lx_stat *st)
{
    pipeend *e = (pipeend *)f->priv;
    xrl_stat_synth(st, (e->is_socket ? LX_S_IFSOCK : LX_S_IFIFO) | 0600, (u64)(uintptr_t)f, 0);
    return 0;
}
static slong pipe_ioctl(xfile *f, u64 req, u64 arg)
{
    pipeend *e = (pipeend *)f->priv;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    if (req == LX_FIONREAD && e->rx) { AcquireSRWLockShared(&e->rx->lock); int n = (int)e->rx->len; ReleaseSRWLockShared(&e->rx->lock); return xrl_ctu(arg, &n, 4); }
    return -LX_ENOTTY;
}
static u32 pipe_poll(xfile *f, u32 ev)
{
    pipeend *e = (pipeend *)f->priv;
    u32 r = 0;
    if (e->rx) {
        AcquireSRWLockShared(&e->rx->lock);
        if (e->rx->len > 0) r |= LX_POLLIN | LX_POLLRDNORM;
        else if (e->rx->writers == 0) r |= LX_POLLHUP | LX_POLLIN;
        ReleaseSRWLockShared(&e->rx->lock);
    }
    if (e->tx) {
        AcquireSRWLockShared(&e->tx->lock);
        if (e->tx->readers == 0) r |= LX_POLLERR | LX_POLLHUP;
        else if (PIPE_CAP - e->tx->len >= 4096) r |= LX_POLLOUT | LX_POLLWRNORM;
        ReleaseSRWLockShared(&e->tx->lock);
    }
    return r & (ev | LX_POLLERR | LX_POLLHUP | LX_POLLNVAL);
}
static void pipe_close(xfile *f)
{
    pipeend *e = (pipeend *)f->priv;
    if (e->rx) { AcquireSRWLockExclusive(&e->rx->lock); e->rx->readers--; ReleaseSRWLockExclusive(&e->rx->lock); }
    if (e->tx) { AcquireSRWLockExclusive(&e->tx->lock); e->tx->writers--; ReleaseSRWLockExclusive(&e->tx->lock); }
    if (e->rx) xrl_io_notify_key(e->rx);
    if (e->tx) xrl_io_notify_key(e->tx);
    if (e->rx) pb_unref(e->rx);
    if (e->tx) pb_unref(e->tx);
    free(e);
}
static u32 pipe_edge(xfile *f)   /* the bytes ever written to what this end reads, and writers going away */
{
    pipeend *e = (pipeend *)f->priv;
    return e->rx ? (u32)e->rx->wpos + (u32)e->rx->writers * 0x10000001u : 0;
}
static const xfile_ops g_pipe_ops = { "pipe", pipe_read, pipe_write, pipe_lseek, pipe_fstat, NULL, pipe_ioctl, NULL, NULL, pipe_poll, pipe_close, NULL, pipe_edge };
static xfile *mk_end(const xfile_ops *ops, pipebuf *rx, pipebuf *tx, int flags, int is_sock, int type)
{
    pipeend *e = (pipeend *)calloc(1, sizeof *e);
    e->rx = rx; e->tx = tx; e->is_socket = is_sock; e->type = type;
    if (rx) { InterlockedIncrement(&rx->refs); rx->readers++; }
    if (tx) { InterlockedIncrement(&tx->refs); tx->writers++; }
    xfile *f = xfile_new(ops, flags);
    f->priv = e;
    return f;
}

slong xrl_pipe_create(int flags, int fds[2])
{
    pipebuf *p = pb_new();
    xfile *r = mk_end(&g_pipe_ops, p, NULL, LX_O_RDONLY | (flags & LX_O_NONBLOCK), 0, 0);
    xfile *w = mk_end(&g_pipe_ops, NULL, p, LX_O_WRONLY | (flags & LX_O_NONBLOCK), 0, 0);
    pb_unref(p);
    int cl = (flags & LX_O_CLOEXEC) != 0;
    fds[0] = xrl_fd_install(r, cl, 0);
    fds[1] = fds[0] >= 0 ? xrl_fd_install(w, cl, 0) : fds[0];
    xfile_unref(r); xfile_unref(w);
    if (fds[0] < 0 || fds[1] < 0) { if (fds[0] >= 0) xrl_fd_close(fds[0]); return fds[0] < 0 ? fds[0] : fds[1]; }
    return 0;
}


/* ============================================================== eventfd */
typedef struct efd { SRWLOCK lock; u64 count; int sem; u32 gen; } efd;

static slong ev_read(xfile *f, void *b, u64 n, s64 o)
{
    (void)o;
    efd *e = (efd *)f->priv;
    if (n < 8) return -LX_EINVAL;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&e->lock);
        if (e->count) {
            u64 v = e->sem ? 1 : e->count;
            e->count -= v;
            ReleaseSRWLockExclusive(&e->lock);
            xrl_io_notify_key(e);
            return xrl_ctu((u64)b, &v, 8) ? -LX_EFAULT : 8;
        }
        ReleaseSRWLockExclusive(&e->lock);
        if (nonblock(f)) return -LX_EAGAIN;
        const void *k = e;
        if (xrl_io_wait_keys(seen, &k, 1, -1) < 0) return -LX_EINTR;
    }
}
static slong ev_write(xfile *f, const void *b, u64 n, s64 o)
{
    (void)o;
    efd *e = (efd *)f->priv;
    if (n < 8) return -LX_EINVAL;
    u64 v;
    if (xrl_cfu(&v, (u64)b, 8)) return -LX_EFAULT;
    if (v == ~0ull) return -LX_EINVAL;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&e->lock);
        if (e->count + v < e->count || e->count + v == ~0ull) {
            ReleaseSRWLockExclusive(&e->lock);
            if (nonblock(f)) return -LX_EAGAIN;
            const void *k = e;
            if (xrl_io_wait_keys(seen, &k, 1, -1) < 0) return -LX_EINTR;
            continue;
        }
        e->count += v; e->gen++;
        ReleaseSRWLockExclusive(&e->lock);
        xrl_io_notify_key(e);
        return 8;
    }
}
static u32 ev_poll(xfile *f, u32 ev)
{
    efd *e = (efd *)f->priv;
    u32 r = 0;
    AcquireSRWLockShared(&e->lock);
    if (e->count) r |= LX_POLLIN;
    if (e->count < ~0ull - 1) r |= LX_POLLOUT;
    ReleaseSRWLockShared(&e->lock);
    return r & (ev | LX_POLLERR);
}
static slong ev_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFREG | 0600, (u64)(uintptr_t)f, 0); return 0; }
static slong ev_ioctl(xfile *f, u64 req, u64 arg)
{
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    return -LX_ENOTTY;
}
static void ev_close(xfile *f) { free(f->priv); }
static u32 ev_edge(xfile *f) { return ((efd *)f->priv)->gen; }
static const xfile_ops g_ev_ops = { "eventfd", ev_read, ev_write, pipe_lseek, ev_fstat, NULL, ev_ioctl, NULL, NULL, ev_poll, ev_close, NULL, ev_edge };

slong xrl_eventfd_create(u32 initval, int flags)
{
    efd *e = (efd *)calloc(1, sizeof *e);
    InitializeSRWLock(&e->lock);
    e->count = initval;
    e->sem = (flags & LX_EFD_SEMAPHORE) != 0;
    xfile *f = xfile_new(&g_ev_ops, LX_O_RDWR | (flags & LX_EFD_NONBLOCK));
    f->priv = e;
    int fd = xrl_fd_install(f, (flags & LX_EFD_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

/* ================================================================== poll */
/* The keys a file's changes are notified with (up to 2 into out), or -1: unknown, wake on any change */
static int file_keys(xfile *f, const void **out)
{
    if (f->ops == &g_pipe_ops) {
        pipeend *e = (pipeend *)f->priv;
        int n = 0;
        if (e->rx) out[n++] = e->rx;
        if (e->tx) out[n++] = e->tx;
        return n;
    }
    if (f->ops == &g_ev_ops) { out[0] = f->priv; return 1; }
    if ((out[0] = xrl_net_key(f)) != NULL) return 1;
    return -1;
}

static u32 file_ready(xfile *f, u32 want)
{
    if (!f->ops->poll) return want & (LX_POLLIN | LX_POLLOUT);
    return f->ops->poll(f, want);
}

slong xrl_poll(u64 ufds, u64 nfds, s64 timeout_ns, u64 sigmask)
{
    (void)sigmask;
    if (nfds > 65536) return -LX_EINVAL;
    lx_pollfd *p = (lx_pollfd *)malloc((size_t)(nfds ? nfds : 1) * sizeof(lx_pollfd));
    if (!p) return -LX_ENOMEM;
    if (xrl_cfu(p, ufds, (size_t)(nfds * sizeof(lx_pollfd)))) { free(p); return -LX_EFAULT; }
    u64 deadline = timeout_ns < 0 ? ~0ull : xrl_mono_ns() + (u64)timeout_ns;
    const void **keys = (const void **)malloc((size_t)(nfds ? nfds : 1) * 2 * sizeof *keys);
    slong ret;
    for (;;) {
        LONG seen = xrl_io_epoch;
        int ready = 0, nk = keys ? 0 : -1;
        for (u64 i = 0; i < nfds; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            xfile *f = xrl_fd_get(p[i].fd);
            if (!f) { p[i].revents = LX_POLLNVAL; ready++; continue; }
            u32 r = file_ready(f, (u32)p[i].events | LX_POLLERR | LX_POLLHUP);
            if (nk >= 0) { int k = file_keys(f, keys + nk); nk = k < 0 ? -1 : nk + k; }
            xfile_unref(f);
            p[i].revents = (s16)(r & ((u32)p[i].events | LX_POLLERR | LX_POLLHUP | LX_POLLNVAL));
            if (p[i].revents) ready++;
        }
        if (ready) { ret = ready; break; }
        if (xrl_spinlog_on()) { static volatile LONG c; if ((InterlockedIncrement(&c) & 0x1ff) == 1) {
            char b[400]; int bn = 0;
            for (u64 i = 0; i < nfds && i < 6; i++) {
                if (p[i].fd < 0) continue;
                xfile *f = xrl_fd_get(p[i].fd);
                const char *nm = f ? (f->gpath ? f->gpath : (f->ops && f->ops->name ? f->ops->name : "?")) : "closed";
                bn += snprintf(b + bn, sizeof b - bn, " fd%d=%s(ev%x)", p[i].fd, nm, (unsigned)p[i].events);
                if (f) xfile_unref(f);
            }
            LOGW("spin-poll nfds=%llu:%s", (unsigned long long)nfds, b);
        } }
        s64 left = -1;
        if (deadline != ~0ull) {
            u64 now = xrl_mono_ns();
            if (now >= deadline) { ret = 0; break; }
            left = (s64)(deadline - now);
        }
        int w = xrl_io_wait_keys(seen, keys, nk, left);
        if (w < 0) { ret = -LX_EINTR; break; }
        /* non-fd waiters (files) are always ready, so only pipes/sockets/eventfds sleep here;
           wake-ups come from xrl_io_notify. A timeout wakes us to re-check the deadline. */
    }
    if (ret >= 0) { if (xrl_ctu(ufds, p, (size_t)(nfds * sizeof(lx_pollfd)))) ret = -LX_EFAULT; }
    free(keys);
    free(p);
    return ret;
}

/* ================================================================ epoll */
typedef struct epi { int fd; xfile *f; u32 events; u64 data; u32 last; int disabled; u32 gen; } epi;
typedef struct epoll { SRWLOCK lock; epi *e; size_t n, cap; } epoll;

static slong ep_nop_rw(xfile *f, void *b, u64 n, s64 o) { (void)f; (void)b; (void)n; (void)o; return -LX_EINVAL; }
static slong ep_wnop(xfile *f, const void *b, u64 n, s64 o) { (void)f; (void)b; (void)n; (void)o; return -LX_EINVAL; }
static slong ep_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFREG | 0600, (u64)(uintptr_t)f, 0); return 0; }
static void ep_close(xfile *f)
{
    epoll *p = (epoll *)f->priv;
    for (size_t i = 0; i < p->n; i++) xfile_unref(p->e[i].f);
    free(p->e); free(p);
}
static u32 ep_poll(xfile *f, u32 ev)
{
    epoll *p = (epoll *)f->priv;
    u32 r = 0;
    AcquireSRWLockShared(&p->lock);
    for (size_t i = 0; i < p->n; i++) {
        if (p->e[i].disabled) continue;
        if (file_ready(p->e[i].f, p->e[i].events | LX_POLLERR | LX_POLLHUP) & (p->e[i].events | LX_POLLERR | LX_POLLHUP)) { r |= LX_POLLIN; break; }
    }
    ReleaseSRWLockShared(&p->lock);
    return r & ev;
}
static const xfile_ops g_ep_ops = { "epoll", ep_nop_rw, ep_wnop, pipe_lseek, ep_fstat, NULL, ev_ioctl, NULL, NULL, ep_poll, ep_close, NULL };

slong xrl_epoll_create(int flags)
{
    epoll *p = (epoll *)calloc(1, sizeof *p);
    InitializeSRWLock(&p->lock);
    xfile *f = xfile_new(&g_ep_ops, LX_O_RDWR);
    f->priv = p;
    int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

slong xrl_epoll_ctl(int epfd, int op, int fd, u64 uev)
{
    xfile *ef = xrl_fd_get(epfd);
    if (!ef) return -LX_EBADF;
    if (ef->ops != &g_ep_ops) { xfile_unref(ef); return -LX_EINVAL; }
    xfile *tf = xrl_fd_get(fd);
    if (!tf) { xfile_unref(ef); return -LX_EBADF; }
    epoll *p = (epoll *)ef->priv;
    lx_epoll_event ev; memset(&ev, 0, sizeof ev);
    slong r = 0;
    if (op != LX_EPOLL_CTL_DEL && xrl_cfu(&ev, uev, sizeof ev)) { r = -LX_EFAULT; goto out; }
    AcquireSRWLockExclusive(&p->lock);
    size_t i;
    for (i = 0; i < p->n; i++) if (p->e[i].fd == fd && p->e[i].f == tf) break;
    if (op == LX_EPOLL_CTL_ADD) {
        if (i < p->n) r = -LX_EEXIST;
        else {
            if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 16; p->e = (epi *)realloc(p->e, p->cap * sizeof(epi)); }
            xfile_ref(tf);
            p->e[p->n++] = (epi){ fd, tf, ev.events, ev.data, 0, 0, 0 };
        }
    } else if (op == LX_EPOLL_CTL_MOD) {
        if (i >= p->n) r = -LX_ENOENT; else { p->e[i].events = ev.events; p->e[i].data = ev.data; p->e[i].disabled = 0; p->e[i].last = 0; p->e[i].gen = 0; }
    } else if (op == LX_EPOLL_CTL_DEL) {
        if (i >= p->n) r = -LX_ENOENT; else { xfile_unref(p->e[i].f); p->e[i] = p->e[--p->n]; }
    } else r = -LX_EINVAL;
    ReleaseSRWLockExclusive(&p->lock);
    xrl_io_notify_key(p);
out:
    xfile_unref(tf); xfile_unref(ef);
    return r;
}

slong xrl_epoll_wait(int epfd, u64 uevents, int maxevents, int timeout_ms, int *intr)
{
    (void)intr;
    if (maxevents <= 0) return -LX_EINVAL;
    xfile *ef = xrl_fd_get(epfd);
    if (!ef) return -LX_EBADF;
    if (ef->ops != &g_ep_ops) { xfile_unref(ef); return -LX_EINVAL; }
    epoll *p = (epoll *)ef->priv;
    lx_epoll_event *out = (lx_epoll_event *)malloc((size_t)maxevents * sizeof *out);
    u64 deadline = timeout_ms < 0 ? ~0ull : xrl_mono_ns() + (u64)timeout_ms * 1000000ull;
    const void **keys = NULL;
    size_t kcap = 0;
    slong ret;
    for (;;) {
        LONG seen = xrl_io_epoch;
        int n = 0, nk = 0;
        AcquireSRWLockExclusive(&p->lock);
        if (kcap < p->n * 2 + 1) { kcap = p->n * 2 + 1; free(keys); keys = (const void **)malloc(kcap * sizeof *keys); }
        if (!keys) { nk = -1; kcap = 0; }
        else keys[nk++] = p;   /* epoll_ctl on this set */
        for (size_t i = 0; i < p->n; i++) {   /* every entry's keys, also past maxevents */
            epi *e = &p->e[i];
            if (nk >= 0) { int k = file_keys(e->f, keys + nk); nk = k < 0 ? -1 : nk + k; }
            if (e->disabled || n >= maxevents) continue;
            u32 mask = (e->events & 0xffff) | LX_POLLERR | LX_POLLHUP;
            u32 r = file_ready(e->f, mask) & mask;
            u32 report = r;
            if (e->events & LX_EPOLLET) {
                /* An edge is new data, not only a change of the ready mask: a socket that was drained and refilled between
                 * two waits is ready both times. A file with an edge counter reports when it moved; any other one when
                 * something in the layer woke sleepers since (a spurious extra report is allowed, a lost one is not). */
                u32 g = e->f->ops->edge ? e->f->ops->edge(e->f) : (u32)xrl_io_epoch;
                report = (r & ~e->last) | (g != e->gen ? r : 0);
                e->gen = g;
            }
            e->last = r;
            if (report) {
                out[n].events = report; out[n].data = e->data; n++;
                if (e->events & LX_EPOLLONESHOT) e->disabled = 1;
            }
        }
        ReleaseSRWLockExclusive(&p->lock);
        if (n) { ret = n; break; }
        s64 left = -1;
        if (deadline != ~0ull) {
            u64 now = xrl_mono_ns();
            if (now >= deadline) { ret = 0; break; }
            left = (s64)(deadline - now);
        }
        if (xrl_io_wait_keys(seen, keys, nk, left) < 0) { ret = -LX_EINTR; break; }
    }
    if (ret > 0 && xrl_ctu(uevents, out, (size_t)ret * sizeof *out)) ret = -LX_EFAULT;
    free(keys);
    free(out);
    xfile_unref(ef);
    return ret;
}

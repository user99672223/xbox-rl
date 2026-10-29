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

void xrl_io_notify(void)
{
    InterlockedIncrement(&xrl_io_epoch);
    WakeByAddressAll((PVOID)&xrl_io_epoch);
}

/* returns 1 woken, 0 timeout, -1 interrupted by a signal */
int xrl_io_wait(LONG seen, s64 timeout_ns)
{
    xrl_thread *t = xrl_cur();
    if (t && xrl_sig_pending_for(t)) return -1;
    if (xrl_io_epoch != seen) return 1;
    u64 ns = timeout_ns < 0 ? ~0ull : (u64)timeout_ns;
    int r = xrl_wait_addr((volatile void *)&xrl_io_epoch, &seen, sizeof seen, ns);
    if (t && xrl_sig_pending_for(t)) return -1;
    return r;
}

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
            xrl_io_notify();
            return (slong)c;
        }
        int eof = p->writers == 0;
        ReleaseSRWLockExclusive(&p->lock);
        if (eof) return 0;
        if (nonblock(f)) return -LX_EAGAIN;
        int w = xrl_io_wait(seen, -1);
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
            xrl_io_notify();
            if (done >= n) break;
            continue;
        }
        if (n == 0 && attach) {   /* zero-byte send carrying fds */
            attach->pos = p->wpos; attach->next = NULL;
            if (p->msgs_tail) p->msgs_tail->next = attach; else p->msgs = attach; p->msgs_tail = attach; attach = NULL;
            ReleaseSRWLockExclusive(&p->lock);
            xrl_io_notify();
            break;
        }
        ReleaseSRWLockExclusive(&p->lock);
        if (nonblock(f)) return done ? (slong)done : -LX_EAGAIN;
        if (xrl_io_wait(seen, -1) < 0) return done ? (slong)done : -LX_EINTR;
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
    xrl_io_notify();
    if (e->rx) pb_unref(e->rx);
    if (e->tx) pb_unref(e->tx);
    free(e);
}
static const xfile_ops g_pipe_ops = { "pipe", pipe_read, pipe_write, pipe_lseek, pipe_fstat, NULL, pipe_ioctl, NULL, NULL, pipe_poll, pipe_close, NULL };
static const xfile_ops g_sock_ops = { "socket", pipe_read, pipe_write, pipe_lseek, pipe_fstat, NULL, pipe_ioctl, NULL, NULL, pipe_poll, pipe_close, NULL };

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

slong xrl_socketpair_create(int type, int flags, int fds[2])
{
    pipebuf *a = pb_new(), *b = pb_new();
    int fl = LX_O_RDWR | (flags & LX_O_NONBLOCK);
    xfile *x = mk_end(&g_sock_ops, a, b, fl, 1, type);
    xfile *y = mk_end(&g_sock_ops, b, a, fl, 1, type);
    pb_unref(a); pb_unref(b);
    int cl = (flags & LX_O_CLOEXEC) != 0;
    fds[0] = xrl_fd_install(x, cl, 0);
    fds[1] = fds[0] >= 0 ? xrl_fd_install(y, cl, 0) : fds[0];
    xfile_unref(x); xfile_unref(y);
    if (fds[0] < 0 || fds[1] < 0) { if (fds[0] >= 0) xrl_fd_close(fds[0]); return fds[0] < 0 ? fds[0] : fds[1]; }
    return 0;
}

slong xrl_shutdown(int fd, int how)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r = 0;
    if (f->ops != &g_sock_ops) r = -LX_ENOTSOCK;
    else {
        pipeend *e = (pipeend *)f->priv;
        if ((how == LX_SHUT_WR || how == LX_SHUT_RDWR) && e->tx) { AcquireSRWLockExclusive(&e->tx->lock); if (e->tx->writers > 0) e->tx->writers--; ReleaseSRWLockExclusive(&e->tx->lock); }
        if ((how == LX_SHUT_RD || how == LX_SHUT_RDWR) && e->rx) { AcquireSRWLockExclusive(&e->rx->lock); if (e->rx->readers > 0) e->rx->readers--; ReleaseSRWLockExclusive(&e->rx->lock); }
        xrl_io_notify();
    }
    xfile_unref(f);
    return r;
}

/* sendmsg/recvmsg with iovec + SCM_RIGHTS */
slong xrl_sendmsg(int fd, u64 umsg, int flags)
{
    (void)flags;
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->ops != &g_sock_ops) { xfile_unref(f); return -LX_ENOTSOCK; }
    pipeend *e = (pipeend *)f->priv;
    lx_msghdr m;
    if (xrl_cfu(&m, umsg, sizeof m)) { xfile_unref(f); return -LX_EFAULT; }
    fdmsg *att = NULL;
    if (m.msg_control && m.msg_controllen >= sizeof(lx_cmsghdr)) {
        u8 cbuf[512];
        u64 cl = m.msg_controllen < sizeof cbuf ? m.msg_controllen : sizeof cbuf;
        if (xrl_cfu(cbuf, m.msg_control, (size_t)cl)) { xfile_unref(f); return -LX_EFAULT; }
        u64 off = 0;
        while (off + sizeof(lx_cmsghdr) <= cl) {
            lx_cmsghdr *h = (lx_cmsghdr *)(cbuf + off);
            if (h->cmsg_len < sizeof(lx_cmsghdr) || off + h->cmsg_len > cl) break;
            if (h->cmsg_level == LX_SOL_SOCKET && h->cmsg_type == LX_SCM_RIGHTS) {
                int nf = (int)((h->cmsg_len - sizeof(lx_cmsghdr)) / 4);
                if (nf > 8) nf = 8;
                if (!att) { att = (fdmsg *)calloc(1, sizeof *att); }
                for (int i = 0; i < nf && att->n < 8; i++) {
                    int sfd; memcpy(&sfd, cbuf + off + sizeof(lx_cmsghdr) + 4 * i, 4);
                    xfile *sf = xrl_fd_get(sfd);
                    if (!sf) { for (int k = 0; k < att->n; k++) xfile_unref(att->files[k]); free(att); xfile_unref(f); return -LX_EBADF; }
                    att->files[att->n++] = sf;
                }
            }
            off += (h->cmsg_len + 7) & ~7ull;
        }
    }
    lx_iovec iov[64];
    u64 niov = m.msg_iovlen > 64 ? 64 : m.msg_iovlen;
    if (xrl_cfu(iov, m.msg_iov, (size_t)(niov * sizeof(lx_iovec)))) { xfile_unref(f); return -LX_EFAULT; }
    slong total = 0;
    int first = 1;
    for (u64 i = 0; i < niov; i++) {
        if (!iov[i].iov_len) continue;
        slong r = pb_write(f, e->tx, (const void *)iov[i].iov_base, iov[i].iov_len, first ? att : NULL);
        if (first) att = NULL;
        first = 0;
        if (r < 0) { total = total ? total : r; break; }
        total += r;
        if ((u64)r < iov[i].iov_len) break;
    }
    if (att) {   /* no data iovecs: attach to an empty send */
        slong r = pb_write(f, e->tx, NULL, 0, att);
        if (r < 0) total = r;
    }
    xfile_unref(f);
    return total;
}

slong xrl_recvmsg(int fd, u64 umsg, int flags)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->ops != &g_sock_ops) { xfile_unref(f); return -LX_ENOTSOCK; }
    pipeend *e = (pipeend *)f->priv;
    lx_msghdr m;
    if (xrl_cfu(&m, umsg, sizeof m)) { xfile_unref(f); return -LX_EFAULT; }
    lx_iovec iov[64];
    u64 niov = m.msg_iovlen > 64 ? 64 : m.msg_iovlen;
    if (xrl_cfu(iov, m.msg_iov, (size_t)(niov * sizeof(lx_iovec)))) { xfile_unref(f); return -LX_EFAULT; }
    int saved = f->flags;
    int want_msgs = 1;
    if (flags & LX_MSG_DONTWAIT) f->flags |= LX_O_NONBLOCK;
    slong total = 0;
    fdmsg *got = NULL;
    for (u64 i = 0; i < niov; i++) {
        if (!iov[i].iov_len) continue;
        if (total > 0) f->flags |= LX_O_NONBLOCK;     /* only block for the first byte */
        slong r = pb_read(f, e->rx, (void *)iov[i].iov_base, iov[i].iov_len, &want_msgs, &got);
        if (r < 0) { if (!total) total = (r == -LX_EAGAIN && total) ? total : r; break; }
        total += r;
        if (r == 0 || (u64)r < iov[i].iov_len || got) break;
    }
    f->flags = saved;
    u32 outflags = 0;
    u64 cused = 0;
    if (got) {
        u8 cbuf[512] = {0};
        lx_cmsghdr *h = (lx_cmsghdr *)cbuf;
        int installed[8], ni = 0;
        for (int i = 0; i < got->n; i++) {
            int nfd = xrl_fd_install(got->files[i], (flags & 0x40000000 /*MSG_CMSG_CLOEXEC*/) != 0, 0);
            xfile_unref(got->files[i]);
            if (nfd >= 0) installed[ni++] = nfd;
        }
        u64 need = sizeof(lx_cmsghdr) + 4ull * ni;
        if (m.msg_control && m.msg_controllen >= need && ni) {
            h->cmsg_len = need; h->cmsg_level = LX_SOL_SOCKET; h->cmsg_type = LX_SCM_RIGHTS;
            memcpy(cbuf + sizeof(lx_cmsghdr), installed, 4 * ni);
            xrl_ctu(m.msg_control, cbuf, (size_t)need);
            cused = (need + 7) & ~7ull;
        } else { for (int i = 0; i < ni; i++) xrl_fd_close(installed[i]); outflags |= LX_MSG_CTRUNC; }
        free(got);
    }
    m.msg_controllen = cused;
    m.msg_flags = outflags;
    m.msg_namelen = 0;
    xrl_ctu(umsg, &m, sizeof m);
    xfile_unref(f);
    return total;
}

/* ============================================================== eventfd */
typedef struct efd { SRWLOCK lock; u64 count; int sem; } efd;

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
            xrl_io_notify();
            return xrl_ctu((u64)b, &v, 8) ? -LX_EFAULT : 8;
        }
        ReleaseSRWLockExclusive(&e->lock);
        if (nonblock(f)) return -LX_EAGAIN;
        if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
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
            if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
            continue;
        }
        e->count += v;
        ReleaseSRWLockExclusive(&e->lock);
        xrl_io_notify();
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
static const xfile_ops g_ev_ops = { "eventfd", ev_read, ev_write, pipe_lseek, ev_fstat, NULL, ev_ioctl, NULL, NULL, ev_poll, ev_close, NULL };

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
    slong ret;
    for (;;) {
        LONG seen = xrl_io_epoch;
        int ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            xfile *f = xrl_fd_get(p[i].fd);
            if (!f) { p[i].revents = LX_POLLNVAL; ready++; continue; }
            u32 r = file_ready(f, (u32)p[i].events | LX_POLLERR | LX_POLLHUP);
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
        int w = xrl_io_wait(seen, left);
        if (w < 0) { ret = -LX_EINTR; break; }
        /* non-fd waiters (files) are always ready, so only pipes/sockets/eventfds sleep here;
           wake-ups come from xrl_io_notify. A timeout wakes us to re-check the deadline. */
    }
    if (ret >= 0) { if (xrl_ctu(ufds, p, (size_t)(nfds * sizeof(lx_pollfd)))) ret = -LX_EFAULT; }
    free(p);
    return ret;
}

/* ================================================================ epoll */
typedef struct epi { int fd; xfile *f; u32 events; u64 data; u32 last; int disabled; } epi;
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
            p->e[p->n++] = (epi){ fd, tf, ev.events, ev.data, 0, 0 };
        }
    } else if (op == LX_EPOLL_CTL_MOD) {
        if (i >= p->n) r = -LX_ENOENT; else { p->e[i].events = ev.events; p->e[i].data = ev.data; p->e[i].disabled = 0; p->e[i].last = 0; }
    } else if (op == LX_EPOLL_CTL_DEL) {
        if (i >= p->n) r = -LX_ENOENT; else { xfile_unref(p->e[i].f); p->e[i] = p->e[--p->n]; }
    } else r = -LX_EINVAL;
    ReleaseSRWLockExclusive(&p->lock);
    xrl_io_notify();
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
    slong ret;
    for (;;) {
        LONG seen = xrl_io_epoch;
        int n = 0;
        AcquireSRWLockExclusive(&p->lock);
        for (size_t i = 0; i < p->n && n < maxevents; i++) {
            epi *e = &p->e[i];
            if (e->disabled) continue;
            u32 mask = (e->events & 0xffff) | LX_POLLERR | LX_POLLHUP;
            u32 r = file_ready(e->f, mask) & mask;
            u32 report = (e->events & LX_EPOLLET) ? (r & ~e->last) : r;
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
        if (xrl_io_wait(seen, left) < 0) { ret = -LX_EINTR; break; }
    }
    if (ret > 0 && xrl_ctu(uevents, out, (size_t)ret * sizeof *out)) ret = -LX_EFAULT;
    free(out);
    xfile_unref(ef);
    return ret;
}

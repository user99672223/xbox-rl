/*
 * unix.c - AF_UNIX sockets: SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET, as in-memory objects.
 *
 * What an X11 desktop needs: a server that bind()s /tmp/.X11-unix/X0 and accept()s, clients that connect()
 * to it (or to an abstract name), poll/epoll readiness, SO_PEERCRED, and file descriptors passed with
 * SCM_RIGHTS (and credentials with SCM_CREDENTIALS).
 *
 * Object model (no per-process state in here, so it works between guest threads now and between guest
 * processes inside one Windows process once proc.c gives each its own fd table):
 *   - A `usock` is the socket itself, reference counted. An open file description (xfile, ops g_un_ops)
 *     holds one reference; so does each connected peer (a pair references each other until each side has
 *     closed), the accept queue holding a not yet accepted connection, the abstract-name table, and the
 *     VFS node of a bound path. Closing the description closes the socket; the object may outlive it
 *     (a peer still reads the queued data and then sees EOF; a path node stays until unlink, and connect()
 *     to it says ECONNREFUSED, as on Linux).
 *   - Data lives in the RECEIVER's queue of chunks. A chunk is one send() (stream: at most 64 KiB), one
 *     datagram or one seqpacket record, with the passed fds (xfile references: an fd in flight is a
 *     reference to the open file description, no fd number), the sender's credentials and, for datagrams,
 *     the sender's address. A stream read never merges data across a chunk that carries fds.
 *   - Pathname sockets are nodes in vfs.c's vnode table (they list as DT_SOCK, stat as S_IFSOCK, unlink
 *     removes them; they live in memory, never as host files, so nothing stale survives a restart).
 *     Abstract names ("\0name", per socket type) live in the table below.
 *   - One global lock guards every usock. The guest-memory copies happen under it (the lazy-commit fault
 *     path never calls back in here). Blocking operations sleep on the layer's I/O epoch like pipes do
 *     (ipc.c): every state change calls xrl_io_notify() and every waiter re-evaluates.
 *   - Credentials: cur_cred() is the one place that says who the calling process is.
 * Not provided: MSG_OOB, SO_PEERSEC/SO_PEERGROUPS, SIGPIPE (EPIPE only, as for pipes), autobind on send.
 */
#include "xrl_internal.h"

#define UN_RCVBUF 212992
#define UN_MAX_FDS 253                   /* SCM_MAX_FD */
#define UN_SEG 65536                     /* largest stream chunk */
#define UN_DGRAM_QLEN 512                /* queued datagrams per receiver (net.unix.max_dgram_qlen) */
#define UN_ADDR_MAX 112                  /* sockaddr_un is 110 bytes */
#define UN_IOV_MAX 1024

#define LX_SO_REUSEADDR 2
#define LX_SO_TYPE 3
#define LX_SO_ERROR 4
#define LX_SO_BROADCAST 6
#define LX_SO_SNDBUF 7
#define LX_SO_RCVBUF 8
#define LX_SO_KEEPALIVE 9
#define LX_SO_OOBINLINE 10
#define LX_SO_LINGER 13
#define LX_SO_PASSCRED 16
#define LX_SO_PEERCRED 17
#define LX_SO_RCVTIMEO 20
#define LX_SO_SNDTIMEO 21
#define LX_SO_ACCEPTCONN 30
#define LX_SO_PROTOCOL 38
#define LX_SO_DOMAIN 39
#define LX_MSG_OOB 0x1
#define LX_MSG_TRUNC 0x20
#define LX_MSG_WAITALL 0x100
#define LX_MSG_CMSG_CLOEXEC 0x40000000
#define LX_TIOCOUTQ 0x5411

typedef struct lx_ucred { s32 pid; u32 uid, gid; } lx_ucred;

typedef struct uchunk {
    struct uchunk *next;
    u8 *data;
    size_t len, off;
    xfile **files; int nfiles;
    lx_ucred cred;
    u8 from[UN_ADDR_MAX]; u32 fromlen;
} uchunk;

enum { US_NEW, US_LISTEN, US_CONN, US_CLOSED };

typedef struct usock usock;
struct usock {
    volatile LONG refs;
    int type, state;
    int shut_rd, shut_wr;          /* RCV_SHUTDOWN, SEND_SHUTDOWN */
    int err;                       /* pending errno (positive) for the next recv/SO_ERROR */
    usock *peer;                   /* connected peer, or a datagram socket's default destination (a reference) */
    u8 addr[UN_ADDR_MAX]; u32 alen;    /* as bind() was given it, NUL counted for a path; 0 = unbound */
    int abstract;
    uchunk *qh, *qt;
    size_t qbytes; int qmsgs;
    usock **acc; int nacc, accmax;     /* accepted-but-not-yet connections (references) */
    int backlog;
    int passcred, rcvbuf, sndbuf, rcvtimeo_ms, sndtimeo_ms, reuseaddr, keepalive;
    u32 gen;                       /* bumped on every arrival or state change a poller could care about (EPOLLET, ipc.c) */
    lx_ucred cred, peercred; int have_peercred;
};

static SRWLOCK g_ul = SRWLOCK_INIT;
static usock **g_abs; static size_t g_nabs, g_capabs;   /* abstract-name table (references) */
static const xfile_ops g_un_ops;

static usock *u_of(xfile *f) { return f && f->ops == &g_un_ops ? (usock *)f->priv : NULL; }
static int conn_oriented(const usock *u) { return u->type != LX_SOCK_DGRAM; }

static void cur_cred(lx_ucred *c) { c->pid = xrl_cur_pid(); c->uid = xrl_cur_uid(); c->gid = xrl_cur_gid(); }   /* getpid()/geteuid()/getegid() as syscall.c answers them */

static void un_ref(void *p) { InterlockedIncrement(&((usock *)p)->refs); }
static void un_unref(void *p)
{
    usock *u = (usock *)p;
    if (InterlockedDecrement(&u->refs) == 0) { free(u->acc); free(u); }   /* queues and peer link were released at close */
}

static usock *usock_new(int type)
{
    usock *u = (usock *)calloc(1, sizeof *u);
    if (!u) return NULL;
    u->refs = 1; u->type = type;
    u->rcvbuf = u->sndbuf = UN_RCVBUF;
    cur_cred(&u->cred);
    return u;
}

/* ----------------------------------------------------------------- lists */
typedef struct flist { xfile **v; size_t n, cap; } flist;   /* xfiles to unref once the lock is dropped (closing one re-enters here) */
static void fl_push(flist *l, xfile *f)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = (xfile **)realloc(l->v, l->cap * sizeof(xfile *)); }
    l->v[l->n++] = f;
}
static void fl_release(flist *l)
{
    for (size_t i = 0; i < l->n; i++) xfile_unref(l->v[i]);
    free(l->v); l->v = NULL; l->n = l->cap = 0;
}

static void chunk_free(uchunk *c, flist *fl)
{
    for (int i = 0; i < c->nfiles; i++) fl_push(fl, c->files[i]);
    free(c->files); free(c->data); free(c);
}

static void queue_push(usock *u, uchunk *c)
{
    c->next = NULL;
    if (u->qt) u->qt->next = c; else u->qh = c;
    u->qt = c;
    u->qbytes += c->len - c->off; u->qmsgs++;
    u->gen++;
}

static void queue_pop(usock *u)
{
    uchunk *c = u->qh;
    u->qh = c->next;
    if (!u->qh) u->qt = NULL;
    u->qbytes -= c->len - c->off; u->qmsgs--;
}

/* ------------------------------------------------------------- addresses */
static int abs_find(const u8 *addr, u32 alen, int type)   /* lock held; index or -1 */
{
    for (size_t i = 0; i < g_nabs; i++)
        if (g_abs[i]->type == type && g_abs[i]->alen == alen && !memcmp(g_abs[i]->addr, addr, alen)) return (int)i;
    return -1;
}

static void abs_remove(usock *u)   /* lock held */
{
    for (size_t i = 0; i < g_nabs; i++) if (g_abs[i] == u) { g_abs[i] = g_abs[--g_nabs]; un_unref(u); return; }
}

/* sockaddr_un from the guest -> addr/alen as stored (path NUL-terminated and counted). 0 or -errno. */
static slong parse_addr(u64 uaddr, u64 ulen, int for_bind, u8 *addr, u32 *alen, int *abstract)
{
    if (ulen > 110 || ulen < 2 || (ulen == 2 && !for_bind)) return -LX_EINVAL;
    memset(addr, 0, UN_ADDR_MAX);
    if (xrl_cfu(addr, uaddr, (size_t)ulen)) return -LX_EFAULT;
    u16 fam; memcpy(&fam, addr, 2);
    if (fam != LX_AF_UNIX) return -LX_EINVAL;
    *abstract = 0;
    if (ulen == 2) { *alen = 2; return 0; }    /* autobind */
    if (addr[2] == 0) { *abstract = 1; *alen = (u32)ulen; return 0; }
    size_t pl = strnlen((const char *)addr + 2, (size_t)ulen - 2);
    addr[2 + pl] = 0;
    *alen = (u32)(2 + pl + 1);
    return 0;
}

static slong put_addr(u64 uaddr, u64 ulenp, const u8 *a, u32 have)
{
    s32 cap;
    if (xrl_cfu(&cap, ulenp, 4)) return -LX_EFAULT;
    if (cap < 0) return -LX_EINVAL;
    u32 n = have < (u32)cap ? have : (u32)cap;
    if (n && xrl_ctu(uaddr, a, n)) return -LX_EFAULT;
    return xrl_ctu(ulenp, &have, 4) ? -LX_EFAULT : 0;
}

static u32 sock_addr_bytes(const usock *u, u8 *out)   /* what getsockname shows */
{
    if (u->alen) { memcpy(out, u->addr, u->alen); return u->alen; }
    u16 fam = LX_AF_UNIX; memcpy(out, &fam, 2);
    return 2;
}

/* The listening/receiving socket at a sockaddr_un, with a reference. */
static slong find_by_addr(const u8 *addr, u32 alen, int abstract, int type, usock **out)
{
    usock *u = NULL;
    if (abstract) {   /* the table is per socket type: another type's socket under the same name is not found */
        AcquireSRWLockExclusive(&g_ul);
        int i = abs_find(addr, alen, type);
        if (i >= 0) { u = g_abs[i]; un_ref(u); }
        ReleaseSRWLockExclusive(&g_ul);
        if (!u) return -LX_ECONNREFUSED;
    } else {
        u32 vt = 0; void *obj = NULL;
        slong r = xrl_vfs_vnode_lookup(LX_AT_FDCWD, (const char *)addr + 2, &vt, &obj);
        if (r) return r;
        if (vt != LX_S_IFSOCK) return -LX_ECONNREFUSED;
        u = (usock *)obj;
        if (u->type != type) { un_unref(u); return -LX_EPROTOTYPE; }
    }
    if (u->state == US_CLOSED) { un_unref(u); return -LX_ECONNREFUSED; }
    *out = u;
    return 0;
}

/* ------------------------------------------------------- waiting helper */
typedef struct unwait { u64 deadline; int nonblock; } unwait;
static void uw_init(unwait *w, int timeout_ms, int nonblock)
{
    w->deadline = timeout_ms > 0 ? xrl_mono_ns() + (u64)timeout_ms * 1000000ull : ~0ull;
    w->nonblock = nonblock;
}
/* After the state said "not yet": 0 = look again, else -errno (EAGAIN, EINTR). */
static slong uw_wait(const unwait *w, LONG seen)
{
    if (w->nonblock) return -LX_EAGAIN;
    s64 left = -1;
    if (w->deadline != ~0ull) {
        u64 now = xrl_mono_ns();
        if (now >= w->deadline) return -LX_EAGAIN;
        left = (s64)(w->deadline - now);
    }
    int r = xrl_io_wait(seen, left);
    if (r < 0) return -LX_EINTR;
    return 0;
}

/* ------------------------------------------------------------ creation */
static xfile *file_for(usock *u, int flags)   /* takes over the caller's reference on u */
{
    xfile *f = xfile_new(&g_un_ops, LX_O_RDWR | (flags & LX_O_NONBLOCK));
    if (!f) { un_unref(u); return NULL; }
    f->priv = u;
    f->mode = LX_S_IFSOCK;
    return f;
}

slong xrl_unix_socket(int domain, int type, int proto)
{
    if (domain != LX_AF_UNIX) return XRL_NOT_NET;
    int t = type & 0xf;
    if (type & ~(0xf | LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC)) return -LX_EINVAL;
    if (t != LX_SOCK_STREAM && t != LX_SOCK_DGRAM && t != LX_SOCK_SEQPACKET) return -LX_ESOCKTNOSUPPORT;
    if (proto) return -LX_EPROTONOSUPPORT;
    usock *u = usock_new(t);
    xfile *f = u ? file_for(u, type) : NULL;
    if (!f) return -LX_ENOMEM;
    int fd = xrl_fd_install(f, (type & LX_SOCK_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

slong xrl_unix_socketpair(int type, int flags, int fds[2])
{
    usock *a = usock_new(type), *b = usock_new(type);
    if (!a || !b) { if (a) un_unref(a); if (b) un_unref(b); return -LX_ENOMEM; }
    a->state = b->state = US_CONN;
    a->peer = b; b->peer = a; un_ref(a); un_ref(b);
    a->peercred = b->cred; b->peercred = a->cred; a->have_peercred = b->have_peercred = 1;
    xfile *x = file_for(a, flags), *y = file_for(b, flags);
    if (!x || !y) { if (x) xfile_unref(x); if (y) xfile_unref(y); return -LX_ENOMEM; }
    int cl = (flags & LX_O_CLOEXEC) != 0;
    fds[0] = xrl_fd_install(x, cl, 0);
    fds[1] = fds[0] >= 0 ? xrl_fd_install(y, cl, 0) : fds[0];
    xfile_unref(x); xfile_unref(y);
    if (fds[0] < 0 || fds[1] < 0) { if (fds[0] >= 0) xrl_fd_close(fds[0]); return fds[0] < 0 ? fds[0] : fds[1]; }
    return 0;
}

#define WITH_UN(fdv, fvar, uvar) \
    xfile *fvar = xrl_fd_get(fdv); \
    if (!fvar) return -LX_EBADF; \
    usock *uvar = u_of(fvar); \
    if (!uvar) { xfile_unref(fvar); return XRL_NOT_NET; }

/* ----------------------------------------------- bind / listen / connect */
slong xrl_unix_bind(int fd, u64 uaddr, u64 ulen)
{
    WITH_UN(fd, f, u);
    u8 addr[UN_ADDR_MAX]; u32 alen = 0; int abstract = 0;
    slong r = parse_addr(uaddr, ulen, 1, addr, &alen, &abstract);
    if (r) goto out;
    AcquireSRWLockShared(&g_ul);
    int bound = u->alen != 0;
    ReleaseSRWLockShared(&g_ul);
    if (bound) { r = -LX_EINVAL; goto out; }
    if (abstract || alen == 2) {   /* alen 2: autobind, an abstract name of five hex digits as the kernel picks one */
        static volatile LONG ctr = 0x2f3a1;
        AcquireSRWLockExclusive(&g_ul);
        if (u->alen) r = -LX_EINVAL;
        else {
            if (alen == 2) {
                int tries = 0;
                do {
                    u32 n = (u32)InterlockedIncrement(&ctr) & 0xfffff;
                    memset(addr, 0, UN_ADDR_MAX);
                    u16 fam = LX_AF_UNIX; memcpy(addr, &fam, 2);
                    snprintf((char *)addr + 3, 6, "%05x", n);
                    alen = 8;
                } while (abs_find(addr, alen, u->type) >= 0 && ++tries < 0x100000);
                if (tries >= 0x100000) r = -LX_ENOSPC;
            } else if (abs_find(addr, alen, u->type) >= 0) r = -LX_EADDRINUSE;
            if (!r) {
                if (g_nabs == g_capabs) { g_capabs = g_capabs ? g_capabs * 2 : 16; g_abs = (usock **)realloc(g_abs, g_capabs * sizeof(usock *)); }
                un_ref(u); g_abs[g_nabs++] = u;
                memcpy(u->addr, addr, alen); u->alen = alen; u->abstract = 1;
            }
        }
        ReleaseSRWLockExclusive(&g_ul);
    } else {
        u32 mode = 0777 & ~xrl_cur_umask();
        r = xrl_vfs_vnode_add(LX_AT_FDCWD, (const char *)addr + 2, LX_S_IFSOCK, mode, u, un_ref, un_unref);
        if (r == -LX_EEXIST) r = -LX_EADDRINUSE;
        if (!r) {
            AcquireSRWLockExclusive(&g_ul);
            memcpy(u->addr, addr, alen); u->alen = alen;
            ReleaseSRWLockExclusive(&g_ul);
        }
    }
out:
    xfile_unref(f);
    return r;
}

slong xrl_unix_listen(int fd, int backlog)
{
    WITH_UN(fd, f, u);
    slong r = 0;
    AcquireSRWLockExclusive(&g_ul);
    if (!conn_oriented(u)) r = -LX_EOPNOTSUPP;
    else if (!u->alen || u->state == US_CONN || u->state == US_CLOSED) r = -LX_EINVAL;
    else {
        u->state = US_LISTEN;
        u->backlog = backlog < 0 ? 0 : backlog > 4096 ? 4096 : backlog;
        cur_cred(&u->cred);
    }
    ReleaseSRWLockExclusive(&g_ul);
    xfile_unref(f);
    return r;
}

static slong connect_stream(xfile *f, usock *u, const u8 *addr, u32 alen, int abstract)
{
    usock *l;
    slong r = find_by_addr(addr, alen, abstract, u->type, &l);
    if (r) return r;
    unwait w;
    uw_init(&w, u->sndtimeo_ms, (f->flags & LX_O_NONBLOCK) != 0);
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&g_ul);
        if (u->state == US_CONN) r = -LX_EISCONN;
        else if (u->state != US_NEW) r = -LX_EINVAL;
        else if (l->state != US_LISTEN) r = -LX_ECONNREFUSED;
        else if (l->nacc > l->backlog) r = -LX_EAGAIN;     /* the accept queue is full: wait for an accept */
        else {
            usock *s = usock_new(u->type);
            if (!s) r = -LX_ENOMEM;
            else {
                s->state = US_CONN;
                memcpy(s->addr, l->addr, UN_ADDR_MAX); s->alen = l->alen; s->abstract = l->abstract;   /* the accepted socket carries the listener's name */
                cur_cred(&s->peercred); s->have_peercred = 1;
                u->peercred = l->cred; u->have_peercred = 1;
                s->peer = u; un_ref(u);
                u->peer = s; un_ref(s);                          /* the accept queue owns the creation reference */
                u->state = US_CONN;
                if (l->nacc == l->accmax) { l->accmax = l->accmax ? l->accmax * 2 : 8; l->acc = (usock **)realloc(l->acc, l->accmax * sizeof(usock *)); }
                l->acc[l->nacc++] = s;
                l->gen++;
                r = 0;
            }
        }
        ReleaseSRWLockExclusive(&g_ul);
        if (r != -LX_EAGAIN) break;
        slong wr = uw_wait(&w, seen);
        if (wr) { r = wr; break; }
    }
    if (!r) xrl_io_notify();
    un_unref(l);
    return r;
}

slong xrl_unix_connect(int fd, u64 uaddr, u64 ulen)
{
    WITH_UN(fd, f, u);
    slong r;
    u8 addr[UN_ADDR_MAX]; u32 alen = 0; int abstract = 0;
    if (u->type == LX_SOCK_DGRAM && ulen >= 2) {   /* AF_UNSPEC: dissolve the default destination */
        u16 fam;
        if (xrl_cfu(&fam, uaddr, 2)) { xfile_unref(f); return -LX_EFAULT; }
        if (fam == LX_AF_UNSPEC) {
            AcquireSRWLockExclusive(&g_ul);
            usock *old = u->peer; u->peer = NULL;
            ReleaseSRWLockExclusive(&g_ul);
            if (old) un_unref(old);
            xfile_unref(f);
            return 0;
        }
    }
    r = parse_addr(uaddr, ulen, 0, addr, &alen, &abstract);
    if (r) { xfile_unref(f); return r; }
    if (u->type == LX_SOCK_DGRAM) {
        usock *t;
        r = find_by_addr(addr, alen, abstract, LX_SOCK_DGRAM, &t);
        if (!r) {
            AcquireSRWLockExclusive(&g_ul);
            usock *old = u->peer; u->peer = t;
            ReleaseSRWLockExclusive(&g_ul);
            if (old) un_unref(old);
            u->peercred = t->cred; u->have_peercred = 1;
        }
    } else r = connect_stream(f, u, addr, alen, abstract);
    xfile_unref(f);
    return r;
}

slong xrl_unix_accept(int fd, u64 uaddr, u64 ulenp, int flags)
{
    WITH_UN(fd, f, l);
    slong ret;
    if (flags & ~(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC)) { xfile_unref(f); return -LX_EINVAL; }
    unwait w;
    uw_init(&w, l->rcvtimeo_ms, (f->flags & LX_O_NONBLOCK) != 0);
    usock *s = NULL;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&g_ul);
        if (!conn_oriented(l)) ret = -LX_EOPNOTSUPP;
        else if (l->state != US_LISTEN) ret = -LX_EINVAL;
        else if (l->nacc) {
            s = l->acc[0];
            memmove(l->acc, l->acc + 1, (size_t)--l->nacc * sizeof(usock *));
            ret = 0;
        } else ret = -LX_EAGAIN;
        ReleaseSRWLockExclusive(&g_ul);
        if (ret != -LX_EAGAIN) break;
        slong wr = uw_wait(&w, seen);
        if (wr) { ret = wr; break; }
    }
    if (!ret) {
        xrl_io_notify();   /* a connector may be waiting for backlog room */
        u8 pa[UN_ADDR_MAX]; u32 pl = 0;
        AcquireSRWLockShared(&g_ul);
        if (s->peer) pl = sock_addr_bytes(s->peer, pa);
        ReleaseSRWLockShared(&g_ul);
        xfile *nf = file_for(s, flags);   /* the queue's reference moves into the new description */
        if (!nf) ret = -LX_ENOMEM;
        else {
            ret = xrl_fd_install(nf, (flags & LX_SOCK_CLOEXEC) != 0, 0);
            xfile_unref(nf);
            if (ret >= 0 && uaddr && ulenp) { slong pr = put_addr(uaddr, ulenp, pa, pl); if (pr) { xrl_fd_close((int)ret); ret = pr; } }
        }
    }
    xfile_unref(f);
    return ret;
}

slong xrl_unix_getsockname(int fd, u64 uaddr, u64 ulenp)
{
    WITH_UN(fd, f, u);
    u8 a[UN_ADDR_MAX];
    AcquireSRWLockShared(&g_ul);
    u32 n = sock_addr_bytes(u, a);
    ReleaseSRWLockShared(&g_ul);
    slong r = put_addr(uaddr, ulenp, a, n);
    xfile_unref(f);
    return r;
}

slong xrl_unix_getpeername(int fd, u64 uaddr, u64 ulenp)
{
    WITH_UN(fd, f, u);
    u8 a[UN_ADDR_MAX];
    slong r;
    AcquireSRWLockShared(&g_ul);
    usock *p = u->peer;
    u32 n = p ? sock_addr_bytes(p, a) : 0;
    ReleaseSRWLockShared(&g_ul);
    r = p ? put_addr(uaddr, ulenp, a, n) : -LX_ENOTCONN;
    xfile_unref(f);
    return r;
}

slong xrl_unix_shutdown(int fd, int how)
{
    WITH_UN(fd, f, u);
    slong r = 0;
    if (how < LX_SHUT_RD || how > LX_SHUT_RDWR) r = -LX_EINVAL;
    else {
        AcquireSRWLockExclusive(&g_ul);
        usock *p = u->peer;
        if (how == LX_SHUT_RD || how == LX_SHUT_RDWR) { u->shut_rd = 1; u->gen++; if (p && conn_oriented(u)) { p->shut_wr = 1; p->gen++; } }
        if (how == LX_SHUT_WR || how == LX_SHUT_RDWR) { u->shut_wr = 1; u->gen++; if (p && conn_oriented(u)) { p->shut_rd = 1; p->gen++; } }
        ReleaseSRWLockExclusive(&g_ul);
        xrl_io_notify();
    }
    xfile_unref(f);
    return r;
}

/* ================================================================= send */
static slong lx_iov_total(const lx_iovec *iov, u64 niov, u64 *total)
{
    u64 t = 0;
    for (u64 i = 0; i < niov; i++) { t += iov[i].iov_len; if (iov[i].iov_len > 0x7fffffffull || t > 0x7fffffffull) return -LX_EINVAL; }
    *total = t;
    return 0;
}

/* iov[skip ...] -> buf[0..n) from guest memory */
static slong iov_gather(const lx_iovec *iov, u64 niov, u64 skip, u8 *buf, size_t n)
{
    size_t done = 0;
    for (u64 i = 0; i < niov && done < n; i++) {
        if (skip >= iov[i].iov_len) { skip -= iov[i].iov_len; continue; }
        size_t c = (size_t)(iov[i].iov_len - skip);
        if (c > n - done) c = n - done;
        if (xrl_cfu(buf + done, iov[i].iov_base + skip, c)) return -LX_EFAULT;
        done += c; skip = 0;
    }
    return 0;
}

/* buf[0..n) -> iov[skip ...] in guest memory */
static slong iov_scatter(const lx_iovec *iov, u64 niov, u64 skip, const u8 *buf, size_t n)
{
    size_t done = 0;
    for (u64 i = 0; i < niov && done < n; i++) {
        if (skip >= iov[i].iov_len) { skip -= iov[i].iov_len; continue; }
        size_t c = (size_t)(iov[i].iov_len - skip);
        if (c > n - done) c = n - done;
        if (xrl_ctu(iov[i].iov_base + skip, buf + done, c)) return -LX_EFAULT;
        done += c; skip = 0;
    }
    return 0;
}

/* Append a chunk to `dst`'s queue: 0, -EAGAIN (no room), or a hard error. Lock held. The chunk is the
 * caller's to free on any non-zero result. */
static slong enqueue_locked(const usock *src, usock *dst, uchunk *c)
{
    int conn = conn_oriented(src);
    if (dst->state == US_CLOSED) return conn ? -LX_EPIPE : -LX_ECONNREFUSED;
    if (dst->shut_rd) return -LX_EPIPE;
    size_t n = c->len;
    if (conn) { if (dst->qbytes && dst->qbytes + n > (size_t)dst->rcvbuf) return -LX_EAGAIN; }
    else if (dst->qmsgs >= UN_DGRAM_QLEN || (dst->qbytes && dst->qbytes + n > (size_t)dst->rcvbuf)) return -LX_EAGAIN;
    queue_push(dst, c);
    return 0;
}

static slong send_chunk(xfile *f, usock *u, usock *dst, uchunk *c, int flags)
{
    unwait w;
    uw_init(&w, u->sndtimeo_ms, (f->flags & LX_O_NONBLOCK) != 0 || (flags & LX_MSG_DONTWAIT) != 0);
    slong r;
    flist fl = {0};
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&g_ul);
        r = u->shut_wr ? -LX_EPIPE : enqueue_locked(u, dst, c);
        ReleaseSRWLockExclusive(&g_ul);
        if (r != -LX_EAGAIN) break;
        slong wr = uw_wait(&w, seen);
        if (wr) { r = wr; break; }
    }
    if (r) chunk_free(c, &fl);
    else xrl_io_notify();
    fl_release(&fl);
    return r;
}

static uchunk *chunk_make(const lx_iovec *iov, u64 niov, u64 skip, size_t n, slong *err)
{
    uchunk *c = (uchunk *)calloc(1, sizeof *c);
    if (c && n) c->data = (u8 *)malloc(n);
    if (!c || (n && !c->data)) { free(c); *err = -LX_ENOMEM; return NULL; }
    c->len = n;
    if (n && (*err = iov_gather(iov, niov, skip, c->data, n))) { free(c->data); free(c); return NULL; }
    return c;
}

/* One send: `files` (nfiles references taken by the caller) go with the first chunk and are consumed. */
static slong un_send(xfile *f, usock *u, const lx_iovec *iov, u64 niov, const u8 *uname_addr, u32 name_alen, int name_abs,
                     int have_name, xfile **files, int nfiles, const lx_ucred *cred, int flags)
{
    u64 total;
    slong r = lx_iov_total(iov, niov, &total);
    flist fl = {0};
    usock *dst = NULL;
    if (r) goto fail;
    if (u->type == LX_SOCK_DGRAM) {
        if (have_name) { r = find_by_addr(uname_addr, name_alen, name_abs, LX_SOCK_DGRAM, &dst); if (r) goto fail; }
        else {
            AcquireSRWLockShared(&g_ul);
            dst = u->peer;
            if (dst) un_ref(dst);
            ReleaseSRWLockShared(&g_ul);
            if (!dst) { r = -LX_ENOTCONN; goto fail; }
        }
        if (total > (u64)u->sndbuf - 32) { r = -LX_EMSGSIZE; goto fail; }
    } else {
        if (have_name) { r = u->state == US_CONN ? -LX_EISCONN : -LX_EOPNOTSUPP; goto fail; }
        AcquireSRWLockShared(&g_ul);
        dst = u->state == US_CONN ? u->peer : NULL;
        if (dst) un_ref(dst);
        ReleaseSRWLockShared(&g_ul);
        if (!dst) { r = -LX_ENOTCONN; goto fail; }
    }
    if (u->type != LX_SOCK_STREAM) {   /* message oriented: one record; a stream goes in segments */
        uchunk *c = chunk_make(iov, niov, 0, (size_t)total, &r);
        if (!c) goto fail;
        c->files = files; c->nfiles = nfiles; files = NULL; nfiles = 0;
        c->cred = *cred;
        AcquireSRWLockShared(&g_ul);
        if (u->type == LX_SOCK_DGRAM && u->alen) { memcpy(c->from, u->addr, u->alen); c->fromlen = u->alen; }
        ReleaseSRWLockShared(&g_ul);
        r = send_chunk(f, u, dst, c, flags);
        if (!r) r = (slong)total;
    } else {
        u64 sent = 0;
        r = 0;
        while (sent < total) {
            size_t seg = (size_t)(total - sent < UN_SEG ? total - sent : UN_SEG);
            slong e = 0;
            uchunk *c = chunk_make(iov, niov, sent, seg, &e);
            if (!c) { r = sent ? (slong)sent : e; goto done; }
            if (!sent) { c->files = files; c->nfiles = nfiles; files = NULL; nfiles = 0; }   /* a zero-length stream send passes nothing, as on Linux */
            c->cred = *cred;
            e = send_chunk(f, u, dst, c, flags);
            if (e) { r = sent ? (slong)sent : e; goto done; }
            sent += seg;
            r = (slong)sent;
        }
    }
done:
    if (dst) un_unref(dst);
    for (int i = 0; i < nfiles; i++) fl_push(&fl, files[i]);   /* never queued */
    free(files);
    fl_release(&fl);
    return r;
fail:
    goto done;
}

/* Parse the SCM_* control messages of a sendmsg: fds (references) and credentials. */
static slong parse_control(u64 ucontrol, u64 clen, xfile ***files, int *nfiles, lx_ucred *cred)
{
    *files = NULL; *nfiles = 0;
    if (!ucontrol || clen < sizeof(lx_cmsghdr)) return 0;
    if (clen > 65536) return -LX_ENOBUFS;
    u8 *cb = (u8 *)malloc((size_t)clen);
    if (!cb) return -LX_ENOMEM;
    slong r = 0;
    xfile **fv = NULL; int nf = 0;
    if (xrl_cfu(cb, ucontrol, (size_t)clen)) { free(cb); return -LX_EFAULT; }
    for (u64 off = 0; off + sizeof(lx_cmsghdr) <= clen;) {
        lx_cmsghdr h; memcpy(&h, cb + off, sizeof h);
        if (h.cmsg_len < sizeof h || off + h.cmsg_len > clen) { r = -LX_EINVAL; break; }
        size_t dl = (size_t)(h.cmsg_len - sizeof h);
        if (h.cmsg_level != LX_SOL_SOCKET) { r = -LX_EINVAL; break; }
        if (h.cmsg_type == LX_SCM_RIGHTS) {
            int n = (int)(dl / 4);
            if (nf + n > UN_MAX_FDS) { r = -LX_EINVAL; break; }
            fv = (xfile **)realloc(fv, (size_t)(nf + n + 1) * sizeof(xfile *));
            for (int i = 0; i < n; i++) {
                int sfd; memcpy(&sfd, cb + off + sizeof h + 4 * i, 4);
                xfile *sf = xrl_fd_get(sfd);
                if (!sf) { r = -LX_EBADF; break; }
                fv[nf++] = sf;
            }
            if (r) break;
        } else if (h.cmsg_type == LX_SCM_CREDENTIALS) {
            lx_ucred c;
            if (dl < sizeof c) { r = -LX_EINVAL; break; }
            memcpy(&c, cb + off + sizeof h, sizeof c);
            lx_ucred me; cur_cred(&me);
            if (c.pid != me.pid || c.uid != me.uid || c.gid != me.gid) { r = -LX_EPERM; break; }   /* only the sender's own */
            *cred = c;
        } else { r = -LX_EINVAL; break; }
        off += (h.cmsg_len + 7) & ~7ull;
    }
    free(cb);
    if (r) { for (int i = 0; i < nf; i++) xfile_unref(fv[i]); free(fv); return r; }
    *files = fv; *nfiles = nf;
    return 0;
}

static slong send_common(xfile *f, usock *u, u64 uname, u32 namelen, const lx_iovec *iov, u64 niov, u64 ucontrol, u64 clen, int flags)
{
    if (flags & LX_MSG_OOB) return -LX_EOPNOTSUPP;
    u8 na[UN_ADDR_MAX]; u32 nalen = 0; int nabs = 0, have_name = 0;
    if (uname && namelen) {
        slong r = parse_addr(uname, namelen, 0, na, &nalen, &nabs);
        if (r) return r;
        have_name = 1;
    }
    xfile **files; int nfiles;
    lx_ucred cred; cur_cred(&cred);
    slong r = parse_control(ucontrol, clen, &files, &nfiles, &cred);
    if (r) return r;
    return un_send(f, u, iov, niov, na, nalen, nabs, have_name, files, nfiles, &cred, flags);
}

static slong load_iov(u64 uiov, u64 cnt, lx_iovec *iov)
{
    if (cnt > UN_IOV_MAX) return -LX_EMSGSIZE;
    if (cnt && xrl_cfu(iov, uiov, (size_t)(cnt * sizeof(lx_iovec)))) return -LX_EFAULT;
    return 0;
}

slong xrl_unix_sendmsg(int fd, u64 umsg, int flags)
{
    WITH_UN(fd, f, u);
    lx_msghdr m;
    lx_iovec iov[UN_IOV_MAX];
    slong r = xrl_cfu(&m, umsg, sizeof m) ? -LX_EFAULT : load_iov(m.msg_iov, m.msg_iovlen, iov);
    if (!r) r = send_common(f, u, m.msg_name, m.msg_namelen, iov, m.msg_iovlen, m.msg_control, m.msg_controllen, flags);
    xfile_unref(f);
    return r;
}

slong xrl_unix_sendto(int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 ualen)
{
    WITH_UN(fd, f, u);
    lx_iovec iov = { ubuf, len };
    slong r = send_common(f, u, uaddr, (u32)ualen, &iov, 1, 0, 0, flags);
    xfile_unref(f);
    return r;
}

slong xrl_unix_sendmmsg(int fd, u64 uvec, u32 vlen, int flags)
{
    WITH_UN(fd, f, u);
    xfile_unref(f);
    if (vlen > 1024) vlen = 1024;
    u32 done = 0;
    slong r = 0;
    for (; done < vlen; done++) {
        u64 e = uvec + (u64)done * 64;   /* struct mmsghdr: msghdr (56) + msg_len (4) + pad */
        r = xrl_unix_sendmsg(fd, e, flags);
        if (r < 0) break;
        u32 n = (u32)r;
        if (xrl_ctu(e + 56, &n, 4)) { r = -LX_EFAULT; break; }
    }
    return done ? (slong)done : r;
}

/* ================================================================= recv */
typedef struct rxout {
    xfile **files; int nfiles;
    lx_ucred cred; int have_cred;
    u8 from[UN_ADDR_MAX]; u32 fromlen;
    u32 msgflags;
} rxout;

/* Take what is queued (lock held). >=0 bytes (an empty datagram is 0 with *msg set), RX_EOF, RX_WAIT or -errno. */
#define RX_EOF (-1000)
#define RX_WAIT (-1001)
static slong recv_locked(usock *u, const lx_iovec *iov, u64 niov, u64 got, u64 want, int flags, rxout *o, int *msg)
{
    int peek = (flags & LX_MSG_PEEK) != 0;
    uchunk *c = u->qh;
    *msg = 0;
    if (u->state == US_LISTEN) return -LX_EINVAL;
    if (!c) {
        if (u->err) { int e = u->err; u->err = 0; return -e; }
        if (conn_oriented(u) && u->state == US_NEW) return -LX_ENOTCONN;
        if (u->shut_rd || (conn_oriented(u) && u->state == US_CLOSED)) return RX_EOF;
        return RX_WAIT;
    }
    if (u->type == LX_SOCK_STREAM) {
        u64 took = 0;
        while (c && got + took < want) {
            if (took && c->nfiles) break;                     /* never merge across a chunk that carries fds */
            size_t avail = c->len - c->off;
            size_t take = (size_t)(want - got - took < avail ? want - got - took : avail);
            if (take && iov_scatter(iov, niov, got + took, c->data + c->off, take)) { if (took) break; return -LX_EFAULT; }   /* un_recv returns what it already has */
            if (!took) { o->cred = c->cred; o->have_cred = 1; }
            took += take;
            if (c->nfiles && !o->nfiles) {
                if (peek) { o->files = (xfile **)malloc((size_t)c->nfiles * sizeof(xfile *)); for (int i = 0; i < c->nfiles; i++) { o->files[i] = c->files[i]; xfile_ref(c->files[i]); } o->nfiles = c->nfiles; }
                else { o->files = c->files; o->nfiles = c->nfiles; c->files = NULL; c->nfiles = 0; }
            }
            if (!peek) {
                if (take == avail) { uchunk *next = c->next; queue_pop(u); free(c->data); free(c->files); free(c); c = next; }
                else { c->off += take; u->qbytes -= take; break; }
            } else { if (take < avail) break; c = c->next; }
            if (o->nfiles) break;                             /* fds were delivered: this read ends here */
        }
        return (slong)took;
    }
    /* datagram / seqpacket: one record per call, the rest of it is discarded */
    size_t avail = c->len - c->off;
    size_t take = avail < want - got ? avail : (size_t)(want - got);
    if (take && iov_scatter(iov, niov, got, c->data + c->off, take)) return -LX_EFAULT;
    if (take < avail) o->msgflags |= LX_MSG_TRUNC;
    o->cred = c->cred; o->have_cred = 1;
    memcpy(o->from, c->from, UN_ADDR_MAX); o->fromlen = c->fromlen;
    if (c->nfiles) {
        if (peek) { o->files = (xfile **)malloc((size_t)c->nfiles * sizeof(xfile *)); for (int i = 0; i < c->nfiles; i++) { o->files[i] = c->files[i]; xfile_ref(c->files[i]); } o->nfiles = c->nfiles; }
        else { o->files = c->files; o->nfiles = c->nfiles; c->files = NULL; c->nfiles = 0; }
    }
    *msg = 1;
    if (!peek) { queue_pop(u); free(c->data); free(c->files); free(c); }
    return (flags & LX_MSG_TRUNC) ? (slong)avail : (slong)take;
}

static slong un_recv(xfile *f, usock *u, const lx_iovec *iov, u64 niov, int flags, rxout *o)
{
    u64 want;
    memset(o, 0, sizeof *o);
    slong r = lx_iov_total(iov, niov, &want);
    if (r) return r;
    unwait w;
    uw_init(&w, u->rcvtimeo_ms, (f->flags & LX_O_NONBLOCK) != 0 || (flags & LX_MSG_DONTWAIT) != 0);
    int waitall = u->type == LX_SOCK_STREAM && (flags & LX_MSG_WAITALL) && !(flags & LX_MSG_PEEK);
    u64 got = 0;
    for (;;) {
        LONG seen = xrl_io_epoch;
        int msg;
        AcquireSRWLockExclusive(&g_ul);
        r = recv_locked(u, iov, niov, got, want, flags, o, &msg);
        ReleaseSRWLockExclusive(&g_ul);
        if (r >= 0) {
            got += (u64)r;
            xrl_io_notify();   /* room for a blocked sender */
            if (msg || !waitall || got >= want || o->nfiles) return (slong)got;
            continue;
        }
        if (r == RX_EOF) return (slong)got;
        if (r != RX_WAIT) return got ? (slong)got : r;
        if (got && w.nonblock) return (slong)got;
        slong wr = uw_wait(&w, seen);
        if (wr) return got ? (slong)got : wr;
    }
}

/* Build the control buffer for what arrived: credentials (if the socket asked), then the fds, which are
 * installed here. Returns the used length and sets MSG_CTRUNC when it did not all fit. */
static u64 build_control(usock *u, rxout *o, u64 ucontrol, u64 clen, int flags, u32 *msgflags)
{
    u8 *cb = NULL;
    u64 used = 0, cap = ucontrol ? clen : 0;
    if (cap > 65536) cap = 65536;
    if (cap) cb = (u8 *)calloc(1, (size_t)cap);
    if (u->passcred && o->have_cred) {
        u64 need = sizeof(lx_cmsghdr) + sizeof(lx_ucred);
        if (cb && used + need <= cap) {
            lx_cmsghdr h = { need, LX_SOL_SOCKET, LX_SCM_CREDENTIALS };
            memcpy(cb + used, &h, sizeof h); memcpy(cb + used + sizeof h, &o->cred, sizeof o->cred);
            used += ((need + 7) & ~7ull) < cap - used ? ((need + 7) & ~7ull) : cap - used;   /* CMSG_SPACE, but never past the buffer */
        } else *msgflags |= LX_MSG_CTRUNC;
    }
    if (o->nfiles) {
        int fit = 0;
        if (cb && cap > used + sizeof(lx_cmsghdr)) { fit = (int)((cap - used - sizeof(lx_cmsghdr)) / 4); if (fit > o->nfiles) fit = o->nfiles; }
        if (fit < o->nfiles) *msgflags |= LX_MSG_CTRUNC;
        int *fds = (int *)malloc((size_t)(fit ? fit : 1) * sizeof(int));
        int ni = 0;
        for (int i = 0; i < o->nfiles; i++) {
            if (i < fit) { int nfd = xrl_fd_install(o->files[i], (flags & LX_MSG_CMSG_CLOEXEC) != 0, 0); if (nfd >= 0) fds[ni++] = nfd; else *msgflags |= LX_MSG_CTRUNC; }
            xfile_unref(o->files[i]);
        }
        if (ni) {
            u64 need = sizeof(lx_cmsghdr) + 4ull * ni;
            lx_cmsghdr h = { need, LX_SOL_SOCKET, LX_SCM_RIGHTS };
            memcpy(cb + used, &h, sizeof h); memcpy(cb + used + sizeof h, fds, 4 * (size_t)ni);
            used += ((need + 7) & ~7ull) < cap - used ? ((need + 7) & ~7ull) : cap - used;
        }
        free(fds); free(o->files); o->files = NULL; o->nfiles = 0;
    }
    if (used && xrl_ctu(ucontrol, cb, (size_t)used)) used = 0;
    free(cb);
    return used;
}

slong xrl_unix_recvmsg(int fd, u64 umsg, int flags)
{
    WITH_UN(fd, f, u);
    lx_msghdr m;
    lx_iovec iov[UN_IOV_MAX];
    rxout o;
    slong r = xrl_cfu(&m, umsg, sizeof m) ? -LX_EFAULT : load_iov(m.msg_iov, m.msg_iovlen, iov);
    if (r) { xfile_unref(f); return r; }
    r = un_recv(f, u, iov, m.msg_iovlen, flags, &o);
    if (r < 0 && !(o.nfiles)) { xfile_unref(f); return r; }
    u32 mf = o.msgflags;
    u64 cused = build_control(u, &o, m.msg_control, m.msg_controllen, flags, &mf);
    u32 nl = 0;
    if (m.msg_name && o.fromlen) { u32 n = o.fromlen < m.msg_namelen ? o.fromlen : m.msg_namelen; if (n && xrl_ctu(m.msg_name, o.from, n)) r = -LX_EFAULT; nl = o.fromlen; }
    m.msg_controllen = cused; m.msg_flags = mf; m.msg_namelen = nl;
    if (xrl_ctu(umsg, &m, sizeof m)) r = -LX_EFAULT;
    xfile_unref(f);
    return r;
}

slong xrl_unix_recvfrom(int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 ulenp)
{
    WITH_UN(fd, f, u);
    lx_iovec iov = { ubuf, len };
    rxout o;
    slong r = un_recv(f, u, &iov, 1, flags, &o);
    if (r >= 0 || o.nfiles) { u32 mf = 0; build_control(u, &o, 0, 0, flags, &mf); }   /* fds in a plain recv are dropped, as on Linux */
    if (r >= 0 && uaddr && ulenp) { slong pr = put_addr(uaddr, ulenp, o.from, o.fromlen); if (pr) r = pr; }
    xfile_unref(f);
    return r;
}

slong xrl_unix_recvmmsg(int fd, u64 uvec, u32 vlen, int flags)
{
    WITH_UN(fd, f, u);
    xfile_unref(f);
    if (vlen > 1024) vlen = 1024;
    u32 done = 0;
    slong r = 0;
    for (; done < vlen; done++) {
        u64 e = uvec + (u64)done * 64;
        r = xrl_unix_recvmsg(fd, e, done ? flags | LX_MSG_DONTWAIT : flags);   /* block for the first, then take what is there */
        if (r < 0) break;
        u32 n = (u32)r;
        if (xrl_ctu(e + 56, &n, 4)) { r = -LX_EFAULT; break; }
    }
    return done ? (slong)done : r;
}

/* readv/writev on a datagram or seqpacket socket are one message, not one per iovec */
slong xrl_unix_iov(int fd, u64 uiov, u64 cnt, int is_write)
{
    xfile *f = xrl_fd_get(fd);
    usock *u = u_of(f);
    if (!u || u->type == LX_SOCK_STREAM) { if (f) xfile_unref(f); return XRL_NOT_NET; }
    lx_iovec iov[UN_IOV_MAX];
    slong r = load_iov(uiov, cnt, iov);
    if (!r) {
        if (is_write) r = send_common(f, u, 0, 0, iov, cnt, 0, 0, 0);
        else { rxout o; r = un_recv(f, u, iov, cnt, 0, &o); if (o.nfiles) { u32 mf = 0; build_control(u, &o, 0, 0, 0, &mf); } }
    }
    xfile_unref(f);
    return r;
}

/* ================================================================ xfile */
static slong un_read(xfile *f, void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    usock *u = (usock *)f->priv;
    if (!n && u->type == LX_SOCK_STREAM) return 0;
    lx_iovec iov = { (u64)(uintptr_t)buf, n };
    rxout o;
    slong r = un_recv(f, u, &iov, 1, 0, &o);
    if (o.nfiles) { u32 mf = 0; build_control(u, &o, 0, 0, 0, &mf); }
    return r;
}

static slong un_write(xfile *f, const void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    usock *u = (usock *)f->priv;
    lx_iovec iov = { (u64)(uintptr_t)buf, n };
    lx_ucred cred; cur_cred(&cred);
    return un_send(f, u, &iov, 1, NULL, 0, 0, 0, NULL, 0, &cred, 0);
}

static slong un_lseek(xfile *f, s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }

static slong un_fstat(xfile *f, lx_stat *st)
{
    xrl_stat_synth(st, LX_S_IFSOCK | 0777, (u64)(uintptr_t)f->priv, 0);
    return 0;
}

static slong un_ioctl(xfile *f, u64 req, u64 arg)
{
    usock *u = (usock *)f->priv;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    if (req == LX_FIONREAD || req == LX_TIOCOUTQ) {
        int n = 0;
        if (req == LX_FIONREAD) {
            AcquireSRWLockShared(&g_ul);
            if (u->state == US_LISTEN) n = -1;
            else n = u->type == LX_SOCK_STREAM ? (int)u->qbytes : u->qh ? (int)(u->qh->len - u->qh->off) : 0;
            ReleaseSRWLockShared(&g_ul);
            if (n < 0) return -LX_EINVAL;
        }
        return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0;
    }
    return -LX_ENOTTY;
}

static u32 un_poll(xfile *f, u32 ev)
{
    usock *u = (usock *)f->priv;
    u32 r = 0;
    AcquireSRWLockShared(&g_ul);
    if (u->state == US_LISTEN) { if (u->nacc) r |= LX_POLLIN | LX_POLLRDNORM; }
    else {
        if (u->qh) r |= LX_POLLIN | LX_POLLRDNORM;
        if (u->shut_rd) r |= LX_POLLIN | LX_POLLRDNORM | LX_POLLRDHUP;
        if (u->shut_rd && u->shut_wr) r |= LX_POLLHUP;
        if (conn_oriented(u) && (u->state == US_NEW || u->state == US_CLOSED)) r |= LX_POLLHUP;   /* never connected / done: as TCP_CLOSE */
        if (u->err) r |= LX_POLLERR;
        usock *p = u->peer;
        int writable = 1;   /* a send that fails at once does not block either */
        if (!u->shut_wr && p && p->state != US_CLOSED && !p->shut_rd) {
            if (conn_oriented(u)) writable = !p->qbytes || p->qbytes + 4096 <= (size_t)p->rcvbuf;
            else writable = p->qmsgs < UN_DGRAM_QLEN && (!p->qbytes || p->qbytes + 4096 <= (size_t)p->rcvbuf);
        }
        if (writable) r |= LX_POLLOUT | LX_POLLWRNORM;
    }
    ReleaseSRWLockShared(&g_ul);
    return r & (ev | LX_POLLERR | LX_POLLHUP | LX_POLLNVAL);
}

static void un_close(xfile *f)
{
    usock *u = (usock *)f->priv;
    flist fl = {0};
    AcquireSRWLockExclusive(&g_ul);
    u->state = US_CLOSED;
    abs_remove(u);
    for (int i = 0; i < u->nacc; i++) {   /* connections nobody accepted: their connectors see a reset */
        usock *s = u->acc[i];
        s->state = US_CLOSED;
        usock *cp = s->peer;
        if (cp) { cp->shut_rd = cp->shut_wr = 1; cp->gen++; cp->err = LX_ECONNRESET; s->peer = NULL; un_unref(cp); }
        while (s->qh) { uchunk *c = s->qh; queue_pop(s); chunk_free(c, &fl); }
        un_unref(s);
    }
    u->nacc = 0;
    int unread = u->qh != NULL;
    while (u->qh) { uchunk *c = u->qh; queue_pop(u); chunk_free(c, &fl); }
    usock *p = u->peer;
    u->peer = NULL;
    if (p) {
        if (conn_oriented(u)) { p->shut_rd = p->shut_wr = 1; p->gen++; if (unread) p->err = LX_ECONNRESET; }   /* p->peer keeps u until p closes */
        un_unref(p);
    }
    ReleaseSRWLockExclusive(&g_ul);
    xrl_io_notify();
    fl_release(&fl);
    un_unref(u);
}

static u32 un_edge(xfile *f) { return ((usock *)f->priv)->gen; }

static const xfile_ops g_un_ops = { "socket", un_read, un_write, un_lseek, un_fstat, NULL, un_ioctl, NULL, NULL, un_poll, un_close, NULL, un_edge };

/* ============================================================ sockopts */
slong xrl_unix_setsockopt(int fd, int level, int opt, u64 uval, u64 len)
{
    WITH_UN(fd, f, u);
    slong r = 0;
    int v = 0;
    if (level != LX_SOL_SOCKET) { xfile_unref(f); return -LX_ENOPROTOOPT; }
    if (opt == LX_SO_RCVTIMEO || opt == LX_SO_SNDTIMEO) {
        lx_timeval tv;
        if (len < sizeof tv) r = -LX_EINVAL;
        else if (xrl_cfu(&tv, uval, sizeof tv)) r = -LX_EFAULT;
        else if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) r = -LX_EDOM;
        else { s64 ms = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000; if (ms > 0x7fffffff) ms = 0x7fffffff; if (opt == LX_SO_RCVTIMEO) u->rcvtimeo_ms = (int)ms; else u->sndtimeo_ms = (int)ms; }
        xfile_unref(f);
        return r;
    }
    if (opt == LX_SO_LINGER) { xfile_unref(f); return len >= 8 ? 0 : -LX_EINVAL; }
    if (len < 4) { xfile_unref(f); return -LX_EINVAL; }
    if (xrl_cfu(&v, uval, 4)) { xfile_unref(f); return -LX_EFAULT; }
    switch (opt) {
    case LX_SO_PASSCRED: u->passcred = v != 0; break;
    case LX_SO_REUSEADDR: u->reuseaddr = v != 0; break;
    case LX_SO_KEEPALIVE: u->keepalive = v != 0; break;
    case LX_SO_SNDBUF: case LX_SO_RCVBUF: {   /* the kernel doubles the request (bookkeeping overhead) within [2304, rmem_max] */
        s64 b = v < 2304 ? 2304 : v > UN_RCVBUF ? UN_RCVBUF : v;
        if (opt == LX_SO_SNDBUF) u->sndbuf = (int)(b * 2); else u->rcvbuf = (int)(b * 2);
        break;
    }
    case LX_SO_BROADCAST: case LX_SO_OOBINLINE: case 34 /*SO_PASSSEC*/: case 29 /*SO_TIMESTAMP*/: case 35 /*SO_TIMESTAMPNS*/:
    case 15 /*SO_REUSEPORT*/: case 12 /*SO_PRIORITY*/: break;
    default: r = -LX_ENOPROTOOPT;
    }
    xfile_unref(f);
    return r;
}

slong xrl_unix_getsockopt(int fd, int level, int opt, u64 uval, u64 ulenp)
{
    WITH_UN(fd, f, u);
    slong r = 0;
    s32 cap;
    u8 out[16] = {0};
    u32 n = 4;
    if (level != LX_SOL_SOCKET) { xfile_unref(f); return -LX_ENOPROTOOPT; }
    if (xrl_cfu(&cap, ulenp, 4)) { xfile_unref(f); return -LX_EFAULT; }
    if (cap < 0) { xfile_unref(f); return -LX_EINVAL; }
    AcquireSRWLockExclusive(&g_ul);
    int v = 0;
    switch (opt) {
    case LX_SO_TYPE: v = u->type; break;
    case LX_SO_ERROR: v = u->err; u->err = 0; break;
    case LX_SO_ACCEPTCONN: v = u->state == US_LISTEN; break;
    case LX_SO_DOMAIN: v = LX_AF_UNIX; break;
    case LX_SO_PROTOCOL: v = 0; break;
    case LX_SO_PASSCRED: v = u->passcred; break;
    case LX_SO_REUSEADDR: v = u->reuseaddr; break;
    case LX_SO_KEEPALIVE: v = u->keepalive; break;
    case LX_SO_SNDBUF: v = u->sndbuf; break;
    case LX_SO_RCVBUF: v = u->rcvbuf; break;
    case LX_SO_BROADCAST: case LX_SO_OOBINLINE: case 34: case 29: case 35: case 15: case 12: v = 0; break;
    case LX_SO_PEERCRED: {
        lx_ucred c = { 0, 0xffffffffu, 0xffffffffu };
        if (u->have_peercred) c = u->peercred;
        memcpy(out, &c, sizeof c); n = sizeof c;
        break;
    }
    case LX_SO_RCVTIMEO: case LX_SO_SNDTIMEO: {
        int ms = opt == LX_SO_RCVTIMEO ? u->rcvtimeo_ms : u->sndtimeo_ms;
        lx_timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        memcpy(out, &tv, sizeof tv); n = sizeof tv;
        break;
    }
    case LX_SO_LINGER: n = 8; break;
    default: r = -LX_ENOPROTOOPT;
    }
    ReleaseSRWLockExclusive(&g_ul);
    if (!r) {
        if (n == 4) memcpy(out, &v, 4);
        u32 w = n < (u32)cap ? n : (u32)cap;
        if ((w && xrl_ctu(uval, out, w)) || xrl_ctu(ulenp, &w, 4)) r = -LX_EFAULT;
    }
    xfile_unref(f);
    return r;
}

/*
 * fd.c - the guest file-descriptor table, refcounted file objects, and the
 * synthetic device files (/dev/null, /dev/zero, /dev/urandom, stdout/stderr
 * log sinks, tty).
 */
#include "xrl_internal.h"

/* =============================================================== xfile */
xfile *xfile_new(const xfile_ops *ops, int flags)
{
    xfile *f = (xfile *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->refs = 1;
    f->ops = ops;
    f->flags = flags;
    f->h = INVALID_HANDLE_VALUE;
    InitializeSRWLock(&f->lock);
    return f;
}

void xfile_ref(xfile *f) { InterlockedIncrement(&f->refs); }

void xfile_unref(xfile *f)
{
    if (!f) return;
    if (InterlockedDecrement(&f->refs) == 0) {
        if (f->ops && f->ops->close) f->ops->close(f);
        if (f->h && f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
        free(f->gpath);
        free(f);
    }
}

/* ============================================================ fd table */
typedef struct fdent { xfile *f; int cloexec; } fdent;
static fdent *g_fd;
static int g_fdcap;
static SRWLOCK g_fdlock = SRWLOCK_INIT;
#define FD_MAX (1 << 16)

static int fd_grow(int need)
{
    if (need < g_fdcap) return 0;
    int nc = g_fdcap ? g_fdcap : 64;
    while (nc <= need) nc *= 2;
    if (nc > FD_MAX) nc = FD_MAX;
    if (need >= nc) return -1;
    fdent *n = (fdent *)realloc(g_fd, nc * sizeof(fdent));
    if (!n) return -1;
    memset(n + g_fdcap, 0, (nc - g_fdcap) * sizeof(fdent));
    g_fd = n; g_fdcap = nc;
    return 0;
}

int xrl_fd_install(xfile *f, int cloexec, int minfd)
{
    int fd = -LX_EMFILE;
    AcquireSRWLockExclusive(&g_fdlock);
    for (int i = minfd < 0 ? 0 : minfd; i < FD_MAX; i++) {
        if (i >= g_fdcap && fd_grow(i)) break;
        if (!g_fd[i].f) {
            xfile_ref(f);
            g_fd[i].f = f; g_fd[i].cloexec = cloexec;
            fd = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_fdlock);
    return fd;
}

xfile *xrl_fd_get(int fd)
{
    xfile *f = NULL;
    if (fd < 0) return NULL;
    AcquireSRWLockShared(&g_fdlock);
    if (fd < g_fdcap && g_fd[fd].f) { f = g_fd[fd].f; xfile_ref(f); }
    ReleaseSRWLockShared(&g_fdlock);
    return f;
}

slong xrl_fd_close(int fd)
{
    xfile *f = NULL;
    if (fd < 0) return -LX_EBADF;
    AcquireSRWLockExclusive(&g_fdlock);
    if (fd < g_fdcap && g_fd[fd].f) { f = g_fd[fd].f; g_fd[fd].f = NULL; g_fd[fd].cloexec = 0; }
    ReleaseSRWLockExclusive(&g_fdlock);
    if (!f) return -LX_EBADF;
    xfile_unref(f);
    return 0;
}

slong xrl_fd_dup(int oldfd, int newfd, int flags, int use_min)
{
    xfile *f = xrl_fd_get(oldfd);
    if (!f) return -LX_EBADF;
    slong r;
    int cloexec = (flags & LX_O_CLOEXEC) != 0;
    if (use_min) {
        r = xrl_fd_install(f, cloexec, newfd);
    } else {
        if (newfd < 0 || newfd >= FD_MAX) { xfile_unref(f); return -LX_EBADF; }
        xfile *old = NULL;
        AcquireSRWLockExclusive(&g_fdlock);
        if (fd_grow(newfd)) { ReleaseSRWLockExclusive(&g_fdlock); xfile_unref(f); return -LX_EMFILE; }
        old = g_fd[newfd].f;
        xfile_ref(f);
        g_fd[newfd].f = f; g_fd[newfd].cloexec = cloexec;
        ReleaseSRWLockExclusive(&g_fdlock);
        if (old) xfile_unref(old);
        r = newfd;
    }
    xfile_unref(f);
    return r;
}

int xrl_fd_getfd(int fd)
{
    int r = -LX_EBADF;
    AcquireSRWLockShared(&g_fdlock);
    if (fd >= 0 && fd < g_fdcap && g_fd[fd].f) r = g_fd[fd].cloexec ? LX_FD_CLOEXEC : 0;
    ReleaseSRWLockShared(&g_fdlock);
    return r;
}

slong xrl_fd_setfd(int fd, int cloexec)
{
    slong r = -LX_EBADF;
    AcquireSRWLockExclusive(&g_fdlock);
    if (fd >= 0 && fd < g_fdcap && g_fd[fd].f) { g_fd[fd].cloexec = cloexec; r = 0; }
    ReleaseSRWLockExclusive(&g_fdlock);
    return r;
}

/* /proc/self/fd listing support */
int xrl_fd_list(int *out, int cap)
{
    int n = 0;
    AcquireSRWLockShared(&g_fdlock);
    for (int i = 0; i < g_fdcap && n < cap; i++) if (g_fd[i].f) out[n++] = i;
    ReleaseSRWLockShared(&g_fdlock);
    return n;
}

slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags)
{
    (void)flags;
    if (first > last) return -LX_EINVAL;
    for (unsigned i = first; i <= last && i < (unsigned)g_fdcap; i++) xrl_fd_close((int)i);
    return 0;
}

/* ======================================================= synthetic stat */
void xrl_stat_synth(lx_stat *st, u32 mode, u64 ino, s64 size)
{
    memset(st, 0, sizeof *st);
    st->st_dev = 0x2;
    st->st_ino = ino;
    st->st_nlink = 1;
    st->st_mode = mode;
    st->st_uid = 1000; st->st_gid = 1000;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
    u64 now = xrl_real_ns();
    st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = (s64)(now / 1000000000ull);
    st->st_atim.tv_nsec = st->st_mtim.tv_nsec = st->st_ctim.tv_nsec = (s64)(now % 1000000000ull);
}

/* ============================================================== devices */
static slong null_read(xfile *f, void *b, u64 n, s64 o) { (void)f; (void)b; (void)n; (void)o; return 0; }
static slong null_write(xfile *f, const void *b, u64 n, s64 o) { (void)f; (void)b; (void)o; return (slong)n; }
static slong dev_lseek(xfile *f, s64 off, int wh) { (void)f; (void)off; (void)wh; return 0; }
static slong dev_ioctl(xfile *f, u64 req, u64 arg)
{
    (void)f; (void)arg;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX || req == LX_FIONBIO) return 0;
    return -LX_ENOTTY;
}
static u32 dev_poll(xfile *f, u32 ev) { (void)f; return ev & (LX_POLLIN | LX_POLLOUT); }

#define DEVSTAT(name, mode_, rdev_) \
    static slong name##_fstat(xfile *f, lx_stat *st) { (void)f; xrl_stat_synth(st, mode_, rdev_, 0); st->st_rdev = rdev_; return 0; }
DEVSTAT(null, LX_S_IFCHR | 0666, (1u << 8) | 3)
DEVSTAT(zero, LX_S_IFCHR | 0666, (1u << 8) | 5)
DEVSTAT(urandom, LX_S_IFCHR | 0666, (1u << 8) | 9)
DEVSTAT(tty, LX_S_IFCHR | 0666, (5u << 8) | 0)
DEVSTAT(logsink, LX_S_IFCHR | 0620, (136u << 8) | 0)

static const xfile_ops g_null_ops = { "null", null_read, null_write, dev_lseek, null_fstat, NULL, dev_ioctl, NULL, NULL, dev_poll, NULL, NULL };

static slong zero_read(xfile *f, void *b, u64 n, s64 o)
{
    (void)f; (void)o;
    if (xrl_zerou((u64)b, (size_t)n)) return -LX_EFAULT;
    return (slong)n;
}
static const xfile_ops g_zero_ops = { "zero", zero_read, null_write, dev_lseek, zero_fstat, NULL, dev_ioctl, NULL, NULL, dev_poll, NULL, NULL };

static slong urandom_read(xfile *f, void *b, u64 n, s64 o)
{
    (void)f; (void)o;
    u8 tmp[512];
    u64 done = 0;
    while (done < n) {
        size_t c = (size_t)((n - done) > sizeof tmp ? sizeof tmp : (n - done));
        xrl_rng_fill(tmp, c);
        if (xrl_ctu((u64)b + done, tmp, c)) return done ? (slong)done : -LX_EFAULT;
        done += c;
    }
    return (slong)n;
}
static const xfile_ops g_urandom_ops = { "urandom", urandom_read, null_write, dev_lseek, urandom_fstat, NULL, dev_ioctl, NULL, NULL, dev_poll, NULL, NULL };

static const xfile_ops g_tty_ops = { "tty", null_read, null_write, dev_lseek, tty_fstat, NULL, dev_ioctl, NULL, NULL, dev_poll, NULL, NULL };

xfile *xrl_dev_null(int flags) { return xfile_new(&g_null_ops, flags); }
xfile *xrl_dev_zero(int flags) { return xfile_new(&g_zero_ops, flags); }
xfile *xrl_dev_urandom(int flags) { return xfile_new(&g_urandom_ops, flags); }
xfile *xrl_dev_tty(int flags) { return xfile_new(&g_tty_ops, flags); }

/* stdout/stderr -> host log, line buffered */
typedef struct logsink { int level; char tag[24]; char buf[1000]; size_t len; SRWLOCK lock; } logsink;

static void logsink_flush(logsink *s)
{
    if (!s->len) return;
    s->buf[s->len] = 0;
    xrl_logf(s->level, s->tag, "%s", s->buf);
    s->len = 0;
}

static slong logsink_write(xfile *f, const void *b, u64 n, s64 o)
{
    (void)o;
    logsink *s = (logsink *)f->priv;
    char tmp[1024];
    u64 left = n, off = 0;
    while (left) {
        size_t c = (size_t)(left > sizeof tmp ? sizeof tmp : left);
        if (xrl_cfu(tmp, (u64)b + off, c)) return off ? (slong)off : -LX_EFAULT;
        AcquireSRWLockExclusive(&s->lock);
        for (size_t i = 0; i < c; i++) {
            char ch = tmp[i];
            if (ch == '\n' || s->len >= sizeof s->buf - 1) { logsink_flush(s); if (ch == '\n') continue; }
            if (ch == '\r') continue;
            s->buf[s->len++] = ch;
        }
        ReleaseSRWLockExclusive(&s->lock);
        off += c; left -= c;
    }
    return (slong)n;
}
static void logsink_close(xfile *f) { logsink *s = (logsink *)f->priv; if (s) { logsink_flush(s); free(s); } }
static const xfile_ops g_log_ops = { "log", null_read, logsink_write, dev_lseek, logsink_fstat, NULL, dev_ioctl, NULL, NULL, dev_poll, logsink_close, NULL };

xfile *xrl_dev_log(int level, const char *tag)
{
    xfile *f = xfile_new(&g_log_ops, LX_O_WRONLY);
    if (!f) return NULL;
    logsink *s = (logsink *)calloc(1, sizeof *s);
    if (!s) { xfile_unref(f); return NULL; }
    s->level = level;
    strncpy_s(s->tag, sizeof s->tag, tag, _TRUNCATE);
    InitializeSRWLock(&s->lock);
    f->priv = s;
    return f;
}

void xrl_fd_init(void)
{
    xfile *in = xrl_dev_null(LX_O_RDONLY);
    xfile *out = xrl_dev_log(1, "guest.out");
    xfile *err = xrl_dev_log(2, "guest.err");
    xrl_fd_install(in, 0, 0);
    xrl_fd_install(out, 0, 1);
    xrl_fd_install(err, 0, 2);
    xfile_unref(in); xfile_unref(out); xfile_unref(err);
}

/* execve: every descriptor opened with O_CLOEXEC / FD_CLOEXEC is closed */
void xrl_fd_close_on_exec(void)
{
    for (int i = 0;; i++) {
        int c;
        AcquireSRWLockShared(&g_fdlock);
        if (i >= g_fdcap) { ReleaseSRWLockShared(&g_fdlock); break; }
        c = g_fd[i].f && g_fd[i].cloexec;
        ReleaseSRWLockShared(&g_fdlock);
        if (c) xrl_fd_close(i);
    }
}

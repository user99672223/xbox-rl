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
typedef struct fdtab { fdent *e; int cap; SRWLOCK lock; } fdtab;
static fdtab g_main = { NULL, 0, SRWLOCK_INIT };
/* a fork()ed task (task.c) has a table of its own, a copy of its parent's; everything else shares g_main */
static fdtab *cur_tab(void) { xrl_thread *t = xrl_cur(); return t && t->fdt ? (fdtab *)t->fdt : &g_main; }
#define FD_MAX (1 << 16)

static int fd_grow(fdtab *T, int need)
{
    if (need < T->cap) return 0;
    int nc = T->cap ? T->cap : 64;
    while (nc <= need) nc *= 2;
    if (nc > FD_MAX) nc = FD_MAX;
    if (need >= nc) return -1;
    fdent *n = (fdent *)realloc(T->e, nc * sizeof(fdent));
    if (!n) return -1;
    memset(n + T->cap, 0, (nc - T->cap) * sizeof(fdent));
    T->e = n; T->cap = nc;
    return 0;
}

int xrl_fd_install(xfile *f, int cloexec, int minfd)
{
    fdtab *T = cur_tab();
    int fd = -LX_EMFILE;
    AcquireSRWLockExclusive(&T->lock);
    for (int i = minfd < 0 ? 0 : minfd; i < FD_MAX; i++) {
        if (i >= T->cap && fd_grow(T, i)) break;
        if (!T->e[i].f) {
            xfile_ref(f);
            T->e[i].f = f; T->e[i].cloexec = cloexec;
            fd = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&T->lock);
    return fd;
}

xfile *xrl_fd_get(int fd)
{
    fdtab *T = cur_tab();
    xfile *f = NULL;
    if (fd < 0) return NULL;
    AcquireSRWLockShared(&T->lock);
    if (fd < T->cap && T->e[fd].f) { f = T->e[fd].f; xfile_ref(f); }
    ReleaseSRWLockShared(&T->lock);
    return f;
}

slong xrl_fd_close(int fd)
{
    fdtab *T = cur_tab();
    xfile *f = NULL;
    if (fd < 0) return -LX_EBADF;
    AcquireSRWLockExclusive(&T->lock);
    if (fd < T->cap && T->e[fd].f) { f = T->e[fd].f; T->e[fd].f = NULL; T->e[fd].cloexec = 0; }
    ReleaseSRWLockExclusive(&T->lock);
    if (!f) return -LX_EBADF;
    xfile_unref(f);
    return 0;
}

slong xrl_fd_dup(int oldfd, int newfd, int flags, int use_min)
{
    fdtab *T = cur_tab();
    xfile *f = xrl_fd_get(oldfd);
    if (!f) return -LX_EBADF;
    slong r;
    int cloexec = (flags & LX_O_CLOEXEC) != 0;
    if (use_min) {
        r = xrl_fd_install(f, cloexec, newfd);
    } else {
        if (newfd < 0 || newfd >= FD_MAX) { xfile_unref(f); return -LX_EBADF; }
        xfile *old = NULL;
        AcquireSRWLockExclusive(&T->lock);
        if (fd_grow(T, newfd)) { ReleaseSRWLockExclusive(&T->lock); xfile_unref(f); return -LX_EMFILE; }
        old = T->e[newfd].f;
        xfile_ref(f);
        T->e[newfd].f = f; T->e[newfd].cloexec = cloexec;
        ReleaseSRWLockExclusive(&T->lock);
        if (old) xfile_unref(old);
        r = newfd;
    }
    xfile_unref(f);
    return r;
}

int xrl_fd_getfd(int fd)
{
    fdtab *T = cur_tab();
    int r = -LX_EBADF;
    AcquireSRWLockShared(&T->lock);
    if (fd >= 0 && fd < T->cap && T->e[fd].f) r = T->e[fd].cloexec ? LX_FD_CLOEXEC : 0;
    ReleaseSRWLockShared(&T->lock);
    return r;
}

slong xrl_fd_setfd(int fd, int cloexec)
{
    fdtab *T = cur_tab();
    slong r = -LX_EBADF;
    AcquireSRWLockExclusive(&T->lock);
    if (fd >= 0 && fd < T->cap && T->e[fd].f) { T->e[fd].cloexec = cloexec; r = 0; }
    ReleaseSRWLockExclusive(&T->lock);
    return r;
}

/* /proc/self/fd listing support */
int xrl_fd_list(int *out, int cap)
{
    fdtab *T = cur_tab();
    int n = 0;
    AcquireSRWLockShared(&T->lock);
    for (int i = 0; i < T->cap && n < cap; i++) if (T->e[i].f) out[n++] = i;
    ReleaseSRWLockShared(&T->lock);
    return n;
}

slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags)
{
    fdtab *T = cur_tab();
    (void)flags;
    if (first > last) return -LX_EINVAL;
    for (unsigned i = first; i <= last && i < (unsigned)T->cap; i++) xrl_fd_close((int)i);
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
    fdtab *T = cur_tab();
    for (int i = 0;; i++) {
        int c;
        AcquireSRWLockShared(&T->lock);
        if (i >= T->cap) { ReleaseSRWLockShared(&T->lock); break; }
        c = T->e[i].f && T->e[i].cloexec;
        ReleaseSRWLockShared(&T->lock);
        if (c) xrl_fd_close(i);
    }
}

/* ---- the table of a fork()ed task: a copy of the caller's, sharing the open files ---- */
void *xrl_fd_table_clone(void)
{
    fdtab *S = cur_tab(), *n = (fdtab *)calloc(1, sizeof *n);
    InitializeSRWLock(&n->lock);
    AcquireSRWLockShared(&S->lock);
    n->cap = S->cap;
    n->e = (fdent *)calloc((size_t)(S->cap ? S->cap : 1), sizeof(fdent));
    for (int i = 0; i < S->cap; i++) if (S->e[i].f) { n->e[i] = S->e[i]; xfile_ref(S->e[i].f); }
    ReleaseSRWLockShared(&S->lock);
    return n;
}
void xrl_fd_table_free(void *tab)
{
    fdtab *n = (fdtab *)tab;
    if (!n) return;
    for (int i = 0; i < n->cap; i++) if (n->e[i].f) xfile_unref(n->e[i].f);
    free(n->e); free(n);
}

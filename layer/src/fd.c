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
/* One table per guest process (proc.c): the calling thread's process, or the init process's for threads the
 * layer did not start (boot). fork copies the entries (the xfile objects are shared and refcounted, so a pipe end
 * stays open while any process holds it), CLONE_FILES shares the table. */
typedef struct fdent { xfile *f; int cloexec; } fdent;
struct xrl_files { volatile LONG refs; SRWLOCK lock; fdent *fd; int cap; };
#define FD_MAX (1 << 16)

xrl_files *xrl_files_new(void)
{
    xrl_files *t = (xrl_files *)calloc(1, sizeof *t);
    if (t) { t->refs = 1; InitializeSRWLock(&t->lock); }
    return t;
}

void xrl_files_ref(xrl_files *t) { InterlockedIncrement(&t->refs); }

xrl_files *xrl_files_clone(xrl_files *src)
{
    xrl_files *t = xrl_files_new();
    if (!t) return NULL;
    AcquireSRWLockShared(&src->lock);
    if (src->cap) {
        t->fd = (fdent *)calloc((size_t)src->cap, sizeof(fdent));
        if (!t->fd) { ReleaseSRWLockShared(&src->lock); free(t); return NULL; }
        t->cap = src->cap;
        for (int i = 0; i < src->cap; i++)
            if (src->fd[i].f) { xfile_ref(src->fd[i].f); t->fd[i] = src->fd[i]; }
    }
    ReleaseSRWLockShared(&src->lock);
    return t;
}

void xrl_files_put(xrl_files *t)
{
    if (!t || InterlockedDecrement(&t->refs) != 0) return;
    for (int i = 0; i < t->cap; i++) if (t->fd[i].f) xfile_unref(t->fd[i].f);
    free(t->fd);
    free(t);
}

/* the table of the calling thread's process; an exited process has none (an empty one stands in). A fork()ed
 * task (task.c, XRL_FORK_TASKS) has a table of its own, a copy of its parent's, on its thread. */
static xrl_files *cur_files(void)
{
    static xrl_files none;
    xrl_thread *ct = xrl_cur();
    if (ct && ct->fdt) return (xrl_files *)ct->fdt;
    xrl_proc *p = xrl_curproc();
    xrl_files *t = p ? p->files : NULL;
    if (!t) { InitializeSRWLock(&none.lock); return &none; }
    return t;
}

static int fd_grow(xrl_files *t, int need)
{
    if (need < t->cap) return 0;
    int nc = t->cap ? t->cap : 64;
    while (nc <= need) nc *= 2;
    if (nc > FD_MAX) nc = FD_MAX;
    if (need >= nc) return -1;
    fdent *n = (fdent *)realloc(t->fd, nc * sizeof(fdent));
    if (!n) return -1;
    memset(n + t->cap, 0, (nc - t->cap) * sizeof(fdent));
    t->fd = n; t->cap = nc;
    return 0;
}

int xrl_fd_install(xfile *f, int cloexec, int minfd)
{
    xrl_files *t = cur_files();
    int fd = -LX_EMFILE;
    AcquireSRWLockExclusive(&t->lock);
    for (int i = minfd < 0 ? 0 : minfd; i < FD_MAX; i++) {
        if (i >= t->cap && fd_grow(t, i)) break;
        if (!t->fd[i].f) {
            xfile_ref(f);
            t->fd[i].f = f; t->fd[i].cloexec = cloexec;
            fd = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&t->lock);
    return fd;
}

xfile *xrl_files_get(xrl_files *t, int fd)
{
    xfile *f = NULL;
    if (fd < 0 || !t) return NULL;
    AcquireSRWLockShared(&t->lock);
    if (fd < t->cap && t->fd[fd].f) { f = t->fd[fd].f; xfile_ref(f); }
    ReleaseSRWLockShared(&t->lock);
    return f;
}

xfile *xrl_fd_get(int fd) { return xrl_files_get(cur_files(), fd); }

slong xrl_fd_close(int fd)
{
    xrl_files *t = cur_files();
    xfile *f = NULL;
    if (fd < 0) return -LX_EBADF;
    AcquireSRWLockExclusive(&t->lock);
    if (fd < t->cap && t->fd[fd].f) { f = t->fd[fd].f; t->fd[fd].f = NULL; t->fd[fd].cloexec = 0; }
    ReleaseSRWLockExclusive(&t->lock);
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
        xrl_files *t = cur_files();
        if (newfd < 0 || newfd >= FD_MAX) { xfile_unref(f); return -LX_EBADF; }
        xfile *old = NULL;
        AcquireSRWLockExclusive(&t->lock);
        if (fd_grow(t, newfd)) { ReleaseSRWLockExclusive(&t->lock); xfile_unref(f); return -LX_EMFILE; }
        old = t->fd[newfd].f;
        xfile_ref(f);
        t->fd[newfd].f = f; t->fd[newfd].cloexec = cloexec;
        ReleaseSRWLockExclusive(&t->lock);
        if (old) xfile_unref(old);
        r = newfd;
    }
    xfile_unref(f);
    return r;
}

int xrl_fd_getfd(int fd)
{
    xrl_files *t = cur_files();
    int r = -LX_EBADF;
    AcquireSRWLockShared(&t->lock);
    if (fd >= 0 && fd < t->cap && t->fd[fd].f) r = t->fd[fd].cloexec ? LX_FD_CLOEXEC : 0;
    ReleaseSRWLockShared(&t->lock);
    return r;
}

slong xrl_fd_setfd(int fd, int cloexec)
{
    xrl_files *t = cur_files();
    slong r = -LX_EBADF;
    AcquireSRWLockExclusive(&t->lock);
    if (fd >= 0 && fd < t->cap && t->fd[fd].f) { t->fd[fd].cloexec = cloexec; r = 0; }
    ReleaseSRWLockExclusive(&t->lock);
    return r;
}

/* /proc/<pid>/fd listing support */
int xrl_files_list(xrl_files *t, int *out, int cap)
{
    int n = 0;
    if (!t) return 0;
    AcquireSRWLockShared(&t->lock);
    for (int i = 0; i < t->cap && n < cap; i++) if (t->fd[i].f) out[n++] = i;
    ReleaseSRWLockShared(&t->lock);
    return n;
}
int xrl_fd_list(int *out, int cap) { return xrl_files_list(cur_files(), out, cap); }

slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags)
{
    xrl_files *t = cur_files();
    if (first > last) return -LX_EINVAL;
    if (flags & 4) {   /* CLOSE_RANGE_CLOEXEC */
        AcquireSRWLockExclusive(&t->lock);
        for (unsigned i = first; i <= last && i < (unsigned)t->cap; i++) if (t->fd[i].f) t->fd[i].cloexec = 1;
        ReleaseSRWLockExclusive(&t->lock);
        return 0;
    }
    for (unsigned i = first; i <= last && i < (unsigned)t->cap; i++) xrl_fd_close((int)i);
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

/* the terminal stand-in: a foreground process group for shells (proc.c) */
static slong tty_ioctl(xfile *f, u64 req, u64 arg)
{
    int v;
    switch (req) {
    case LX_TIOCGPGRP: v = xrl_tty_pgrp(); return xrl_ctu(arg, &v, 4) ? -LX_EFAULT : 0;
    case LX_TIOCSPGRP: if (xrl_cfu(&v, arg, 4)) return -LX_EFAULT; return xrl_tty_set_pgrp(v);
    case LX_TIOCGSID: v = xrl_curproc()->sid; return xrl_ctu(arg, &v, 4) ? -LX_EFAULT : 0;
    case LX_TIOCSCTTY: case LX_TIOCNOTTY: return 0;
    }
    return dev_ioctl(f, req, arg);
}
static const xfile_ops g_tty_ops = { "tty", null_read, null_write, dev_lseek, tty_fstat, NULL, tty_ioctl, NULL, NULL, dev_poll, NULL, NULL };

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

/* execve: every descriptor opened with O_CLOEXEC / FD_CLOEXEC is closed (in the calling process's table) */
void xrl_fd_close_on_exec(void)
{
    xrl_files *t = cur_files();
    for (int i = 0;; i++) {
        int c;
        AcquireSRWLockShared(&t->lock);
        if (i >= t->cap) { ReleaseSRWLockShared(&t->lock); break; }
        c = t->fd[i].f && t->fd[i].cloexec;
        ReleaseSRWLockShared(&t->lock);
        if (c) xrl_fd_close(i);
    }
}

/* ---- the table of a fork()ed task (task.c): a copy of the caller's, sharing the open files ---- */
void *xrl_fd_table_clone(void) { return xrl_files_clone(cur_files()); }
void xrl_fd_table_free(void *tab) { if (tab) xrl_files_put((xrl_files *)tab); }

/* the memfds of the calling task/process that survive exec, as "fd|key|name;" (vfs.c reopens them in the new process) */
size_t xrl_fd_inherit_list(char *out, size_t cap)
{
    xrl_files *T = cur_files();
    size_t n = 0;
    out[0] = 0;
    AcquireSRWLockShared(&T->lock);
    for (int i = 3; i < T->cap; i++) {
        xfile *f = T->fd[i].f;
        if (!f || T->fd[i].cloexec || !f->gpath || strncmp(f->gpath, "/memfd:", 7)) continue;
        char name[260]; snprintf(name, sizeof name, "%s", f->gpath + 7);
        char *d = strstr(name, " (deleted)"); if (d) *d = 0;
        int w = snprintf(out + n, cap - n, "%d|%llu|%s;", i, (unsigned long long)(uintptr_t)f->priv, name);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
    ReleaseSRWLockShared(&T->lock);
    return n;
}

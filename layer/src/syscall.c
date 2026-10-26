/*
 * syscall.c - the Linux x86-64 syscall dispatch table.
 *
 * Every number in docs/syscall-abi.md has an entry. Implemented ones call into
 * the subsystem modules; everything else falls through to enosys() which logs
 * "ENOSYS <nr> <name>" so the integrator sees exactly what to add next.
 *
 * Handlers run on the thread's HOST stack with t->in_sys > 0.
 */
#include "xrl_internal.h"

#define SYS(name) static slong sys_##name(slong a1, slong a2, slong a3, slong a4, slong a5, slong a6)
#define UNUSED6 (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6
#define PATH_MAX_ 4096

/* from misc.c */
slong xrl_sys_clock_gettime(int clk, u64 uts);
slong xrl_sys_clock_getres(int clk, u64 uts);
slong xrl_sys_gettimeofday(u64 utv, u64 utz);
slong xrl_sys_time(u64 ut);
slong xrl_sys_nanosleep(u64 ureq, u64 urem);
slong xrl_sys_clock_nanosleep(int clk, int flags, u64 ureq, u64 urem);
slong xrl_sys_times(u64 ubuf);
slong xrl_sys_getrusage(int who, u64 ubuf);
slong xrl_memfd_create(const char *name, unsigned flags);
slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags);

static slong getpath(char *dst, u64 usrc)
{
    int r = xrl_strfu(dst, usrc, PATH_MAX_);
    return r;
}

/* =========================================================== file I/O */
static slong do_read(int fd, u64 buf, u64 n, s64 off)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r;
    if (!f->ops->read) r = -LX_EBADF;
    else {
        if (n > 0x7ffff000ull) n = 0x7ffff000ull;
        r = f->ops->read(f, (void *)buf, n, off);
    }
    xfile_unref(f);
    return r;
}
static slong do_write(int fd, u64 buf, u64 n, s64 off)
{
    xfile *f = xrl_fd_get(fd);
    if (!f) return -LX_EBADF;
    slong r;
    if (!f->ops->write) r = -LX_EBADF;
    else {
        if (n > 0x7ffff000ull) n = 0x7ffff000ull;
        r = f->ops->write(f, (const void *)buf, n, off);
    }
    xfile_unref(f);
    return r;
}

SYS(read) { UNUSED6; return do_read((int)a1, (u64)a2, (u64)a3, -1); }
SYS(write) { UNUSED6; return do_write((int)a1, (u64)a2, (u64)a3, -1); }
SYS(pread64) { UNUSED6; if (a4 < 0) return -LX_EINVAL; return do_read((int)a1, (u64)a2, (u64)a3, a4); }
SYS(pwrite64) { UNUSED6; if (a4 < 0) return -LX_EINVAL; return do_write((int)a1, (u64)a2, (u64)a3, a4); }

static slong do_iov(int fd, u64 uiov, u64 cnt, s64 off, int is_write)
{
    if (cnt > 1024) return -LX_EINVAL;
    lx_iovec iov[1024];
    if (xrl_cfu(iov, uiov, (size_t)(cnt * sizeof(lx_iovec)))) return -LX_EFAULT;
    slong total = 0;
    for (u64 i = 0; i < cnt; i++) {
        if (!iov[i].iov_len) continue;
        slong r = is_write ? do_write(fd, iov[i].iov_base, iov[i].iov_len, off < 0 ? -1 : off + total)
                          : do_read(fd, iov[i].iov_base, iov[i].iov_len, off < 0 ? -1 : off + total);
        if (r < 0) return total ? total : r;
        total += r;
        if ((u64)r < iov[i].iov_len) break;
    }
    return total;
}
SYS(readv) { UNUSED6; return do_iov((int)a1, (u64)a2, (u64)a3, -1, 0); }
SYS(writev) { UNUSED6; return do_iov((int)a1, (u64)a2, (u64)a3, -1, 1); }
SYS(preadv) { UNUSED6; return do_iov((int)a1, (u64)a2, (u64)a3, a4, 0); }
SYS(pwritev) { UNUSED6; return do_iov((int)a1, (u64)a2, (u64)a3, a4, 1); }

SYS(lseek)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    slong r = f->ops->lseek ? f->ops->lseek(f, a2, (int)a3) : -LX_ESPIPE;
    xfile_unref(f);
    return r;
}

SYS(openat)
{
    UNUSED6;
    char p[PATH_MAX_];
    slong r = getpath(p, (u64)a2);
    if (r) return r;
    return xrl_vfs_open((int)a1, p, (int)a3, (int)a4);
}
SYS(open)
{
    UNUSED6;
    char p[PATH_MAX_];
    slong r = getpath(p, (u64)a1);
    if (r) return r;
    return xrl_vfs_open(LX_AT_FDCWD, p, (int)a2, (int)a3);
}
SYS(creat)
{
    UNUSED6;
    char p[PATH_MAX_];
    slong r = getpath(p, (u64)a1);
    if (r) return r;
    return xrl_vfs_open(LX_AT_FDCWD, p, LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, (int)a2);
}
SYS(close) { UNUSED6; return xrl_fd_close((int)a1); }
SYS(close_range) { UNUSED6; return xrl_fd_close_range((unsigned)a1, (unsigned)a2, (unsigned)a3); }
SYS(dup) { UNUSED6; return xrl_fd_dup((int)a1, 0, 0, 1); }
SYS(dup2)
{
    UNUSED6;
    if (a1 == a2) { xfile *f = xrl_fd_get((int)a1); if (!f) return -LX_EBADF; xfile_unref(f); return a2; }
    return xrl_fd_dup((int)a1, (int)a2, 0, 0);
}
SYS(dup3)
{
    UNUSED6;
    if (a1 == a2) return -LX_EINVAL;
    return xrl_fd_dup((int)a1, (int)a2, (int)a3, 0);
}

SYS(fcntl)
{
    UNUSED6;
    int fd = (int)a1;
    xfile *f;
    slong r = 0;
    switch ((int)a2) {
    case LX_F_DUPFD: return xrl_fd_dup(fd, (int)a3, 0, 1);
    case LX_F_DUPFD_CLOEXEC: return xrl_fd_dup(fd, (int)a3, LX_O_CLOEXEC, 1);
    case LX_F_GETFD: return xrl_fd_getfd(fd);
    case LX_F_SETFD: return xrl_fd_setfd(fd, (a3 & LX_FD_CLOEXEC) != 0);
    case LX_F_GETFL: f = xrl_fd_get(fd); if (!f) return -LX_EBADF; r = f->flags; xfile_unref(f); return r;
    case LX_F_SETFL:
        f = xrl_fd_get(fd); if (!f) return -LX_EBADF;
        f->flags = (f->flags & ~(LX_O_NONBLOCK | LX_O_APPEND)) | ((int)a3 & (LX_O_NONBLOCK | LX_O_APPEND));
        xfile_unref(f); return 0;
    case LX_F_GETLK: {
        f = xrl_fd_get(fd); if (!f) return -LX_EBADF; xfile_unref(f);
        u16 t = LX_F_UNLCK; return xrl_ctu((u64)a3, &t, 2);
    }
    case LX_F_SETLK: case LX_F_SETLKW:
        f = xrl_fd_get(fd); if (!f) return -LX_EBADF; xfile_unref(f); return 0;   /* advisory locks: no-ops */
    case LX_F_SETOWN: case LX_F_ADD_SEALS: return 0;
    case LX_F_GETOWN: case LX_F_GET_SEALS: return 0;
    case 1031: return 65536;      /* F_SETPIPE_SZ */
    case 1032: return 65536;      /* F_GETPIPE_SZ */
    default: return -LX_EINVAL;
    }
}

SYS(ioctl)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    slong r = f->ops->ioctl ? f->ops->ioctl(f, (u64)a2, (u64)a3) : -LX_ENOTTY;
    xfile_unref(f);
    return r;
}

SYS(fsync) { UNUSED6; xfile *f = xrl_fd_get((int)a1); if (!f) return -LX_EBADF; slong r = f->ops->fsync ? f->ops->fsync(f) : 0; xfile_unref(f); return r; }
SYS(ftruncate) { UNUSED6; xfile *f = xrl_fd_get((int)a1); if (!f) return -LX_EBADF; slong r = f->ops->ftruncate ? f->ops->ftruncate(f, a2) : -LX_EINVAL; xfile_unref(f); return r; }
SYS(fallocate)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    slong r = 0;
    if (a2 == 0 && f->ops->fstat && f->ops->ftruncate) {
        lx_stat st;
        r = f->ops->fstat(f, &st);
        if (!r && st.st_size < a3 + a4) r = f->ops->ftruncate(f, a3 + a4);
    } else if (a2 != 0) r = -LX_EOPNOTSUPP;
    xfile_unref(f);
    return r;
}
SYS(truncate) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_truncate(p, a2); }

SYS(fstat)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    lx_stat st;
    slong r = f->ops->fstat ? f->ops->fstat(f, &st) : -LX_EBADF;
    xfile_unref(f);
    if (r) return r;
    return xrl_ctu((u64)a2, &st, sizeof st);
}
static slong stat_common(int dirfd, u64 upath, u64 ubuf, int flags)
{
    char p[PATH_MAX_];
    lx_stat st;
    slong r = getpath(p, upath);
    if (r) return r;
    r = xrl_vfs_stat(dirfd, p, flags, &st);
    if (r) return r;
    return xrl_ctu(ubuf, &st, sizeof st);
}
SYS(stat) { UNUSED6; return stat_common(LX_AT_FDCWD, (u64)a1, (u64)a2, 0); }
SYS(lstat) { UNUSED6; return stat_common(LX_AT_FDCWD, (u64)a1, (u64)a2, LX_AT_SYMLINK_NOFOLLOW); }
SYS(newfstatat) { UNUSED6; return stat_common((int)a1, (u64)a2, (u64)a3, (int)a4); }

SYS(statx)
{
    UNUSED6;
    /* statx(dirfd, path, flags, mask, buf) */
    char p[PATH_MAX_];
    lx_stat st;
    slong r = getpath(p, (u64)a2);
    if (r) return r;
    int fl = (int)a3;
    if (!p[0] && !(fl & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
    r = xrl_vfs_stat((int)a1, p, fl, &st);
    if (r) return r;
    lx_statx x; memset(&x, 0, sizeof x);
    x.stx_mask = 0x7ff;
    x.stx_blksize = (u32)st.st_blksize; x.stx_nlink = (u32)st.st_nlink; x.stx_uid = st.st_uid; x.stx_gid = st.st_gid;
    x.stx_mode = (u16)st.st_mode; x.stx_ino = st.st_ino; x.stx_size = (u64)st.st_size; x.stx_blocks = (u64)st.st_blocks;
    x.stx_atime.tv_sec = st.st_atim.tv_sec; x.stx_atime.tv_nsec = (u32)st.st_atim.tv_nsec;
    x.stx_mtime.tv_sec = st.st_mtim.tv_sec; x.stx_mtime.tv_nsec = (u32)st.st_mtim.tv_nsec;
    x.stx_ctime.tv_sec = st.st_ctim.tv_sec; x.stx_ctime.tv_nsec = (u32)st.st_ctim.tv_nsec;
    x.stx_btime = x.stx_ctime;
    x.stx_dev_major = (u32)(st.st_dev >> 8); x.stx_dev_minor = (u32)(st.st_dev & 0xff);
    return xrl_ctu((u64)a5, &x, sizeof x);
}

SYS(access) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_access(LX_AT_FDCWD, p, (int)a2); }
SYS(faccessat) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a2); if (r) return r; return xrl_vfs_access((int)a1, p, (int)a3); }

SYS(getdents64)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    slong r = f->ops->getdents ? f->ops->getdents(f, (void *)a2, (u64)a3) : -LX_ENOTDIR;
    xfile_unref(f);
    return r;
}

static slong readlink_common(int dirfd, u64 upath, u64 ubuf, u64 cap)
{
    char p[PATH_MAX_], out[PATH_MAX_];
    if ((slong)cap <= 0) return -LX_EINVAL;
    slong r = getpath(p, upath);
    if (r) return r;
    if (cap > sizeof out) cap = sizeof out;
    r = xrl_vfs_readlink(dirfd, p, out, cap);
    if (r < 0) return r;
    return xrl_ctu(ubuf, out, (size_t)r) ? -LX_EFAULT : r;
}
SYS(readlink) { UNUSED6; return readlink_common(LX_AT_FDCWD, (u64)a1, (u64)a2, (u64)a3); }
SYS(readlinkat) { UNUSED6; return readlink_common((int)a1, (u64)a2, (u64)a3, (u64)a4); }

SYS(getcwd)
{
    UNUSED6;
    char b[PATH_MAX_];
    slong r = xrl_vfs_getcwd(b, (u64)a2 < sizeof b ? (u64)a2 : sizeof b);
    if (r < 0) return r;
    return xrl_ctu((u64)a1, b, (size_t)r) ? -LX_EFAULT : r;
}
SYS(chdir) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_chdir(p); }
SYS(fchdir)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    slong r = f->gpath ? xrl_vfs_chdir(f->gpath) : -LX_ENOTDIR;
    xfile_unref(f);
    return r;
}
SYS(mkdir) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_mkdir(LX_AT_FDCWD, p, (int)a2); }
SYS(mkdirat) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a2); if (r) return r; return xrl_vfs_mkdir((int)a1, p, (int)a3); }
SYS(rmdir) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_unlink(LX_AT_FDCWD, p, LX_AT_REMOVEDIR); }
SYS(unlink) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a1); if (r) return r; return xrl_vfs_unlink(LX_AT_FDCWD, p, 0); }
SYS(unlinkat) { UNUSED6; char p[PATH_MAX_]; slong r = getpath(p, (u64)a2); if (r) return r; return xrl_vfs_unlink((int)a1, p, (int)a3); }
SYS(rename)
{
    UNUSED6; char o[PATH_MAX_], n[PATH_MAX_];
    slong r = getpath(o, (u64)a1); if (r) return r;
    r = getpath(n, (u64)a2); if (r) return r;
    return xrl_vfs_rename(LX_AT_FDCWD, o, LX_AT_FDCWD, n, 0);
}
SYS(renameat)
{
    UNUSED6; char o[PATH_MAX_], n[PATH_MAX_];
    slong r = getpath(o, (u64)a2); if (r) return r;
    r = getpath(n, (u64)a4); if (r) return r;
    return xrl_vfs_rename((int)a1, o, (int)a3, n, 0);
}
SYS(renameat2)
{
    UNUSED6; char o[PATH_MAX_], n[PATH_MAX_];
    slong r = getpath(o, (u64)a2); if (r) return r;
    r = getpath(n, (u64)a4); if (r) return r;
    if (a5 & ~1) return -LX_EINVAL;   /* RENAME_EXCHANGE/WHITEOUT unsupported */
    return xrl_vfs_rename((int)a1, o, (int)a3, n, (int)a5);
}
SYS(symlink)
{
    UNUSED6; char t[PATH_MAX_], l[PATH_MAX_];
    slong r = getpath(t, (u64)a1); if (r) return r;
    r = getpath(l, (u64)a2); if (r) return r;
    return xrl_vfs_symlink(t, LX_AT_FDCWD, l);
}
SYS(symlinkat)
{
    UNUSED6; char t[PATH_MAX_], l[PATH_MAX_];
    slong r = getpath(t, (u64)a1); if (r) return r;
    r = getpath(l, (u64)a3); if (r) return r;
    return xrl_vfs_symlink(t, (int)a2, l);
}
SYS(link)
{
    UNUSED6; char o[PATH_MAX_], n[PATH_MAX_];
    slong r = getpath(o, (u64)a1); if (r) return r;
    r = getpath(n, (u64)a2); if (r) return r;
    return xrl_vfs_link(LX_AT_FDCWD, o, LX_AT_FDCWD, n);
}
SYS(linkat)
{
    UNUSED6; char o[PATH_MAX_], n[PATH_MAX_];
    slong r = getpath(o, (u64)a2); if (r) return r;
    r = getpath(n, (u64)a4); if (r) return r;
    return xrl_vfs_link((int)a1, o, (int)a3, n);
}
SYS(utimensat)
{
    UNUSED6;
    lx_timespec ts[2]; char p[PATH_MAX_];
    slong r = a2 ? getpath(p, (u64)a2) : 0;
    if (r) return r;
    if (a3 && xrl_cfu(ts, (u64)a3, sizeof ts)) return -LX_EFAULT;
    if (!a2) { xfile *f = xrl_fd_get((int)a1); if (!f || !f->gpath) { if (f) xfile_unref(f); return -LX_EBADF; } strcpy_s(p, sizeof p, f->gpath); xfile_unref(f); }
    return xrl_vfs_utimens(a2 ? (int)a1 : LX_AT_FDCWD, p, a3 ? ts : NULL, (int)a4);
}
SYS(utimes)
{
    UNUSED6;
    lx_timeval tv[2]; lx_timespec ts[2]; char p[PATH_MAX_];
    slong r = getpath(p, (u64)a1); if (r) return r;
    if (a2) { if (xrl_cfu(tv, (u64)a2, sizeof tv)) return -LX_EFAULT; for (int i = 0; i < 2; i++) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = tv[i].tv_usec * 1000; } }
    return xrl_vfs_utimens(LX_AT_FDCWD, p, a2 ? ts : NULL, 0);
}
SYS(statfs) { UNUSED6; char p[PATH_MAX_]; lx_statfs s; slong r = getpath(p, (u64)a1); if (r) return r; r = xrl_vfs_statfs(p, &s); return r ? r : xrl_ctu((u64)a2, &s, sizeof s); }
SYS(fstatfs) { UNUSED6; lx_statfs s; xfile *f = xrl_fd_get((int)a1); if (!f) return -LX_EBADF; xfile_unref(f); slong r = xrl_vfs_statfs("/", &s); return r ? r : xrl_ctu((u64)a2, &s, sizeof s); }
SYS(zero_ok) { UNUSED6; return 0; }
SYS(umask) { UNUSED6; u32 old = G.umask; G.umask = (u32)a1 & 0777; return old; }

SYS(pipe) { UNUSED6; int fds[2]; slong r = xrl_pipe_create(0, fds); if (r) return r; return xrl_ctu((u64)a1, fds, 8); }
SYS(pipe2) { UNUSED6; int fds[2]; slong r = xrl_pipe_create((int)a2, fds); if (r) return r; if (xrl_ctu((u64)a1, fds, 8)) { xrl_fd_close(fds[0]); xrl_fd_close(fds[1]); return -LX_EFAULT; } return 0; }
SYS(eventfd2) { UNUSED6; return xrl_eventfd_create((u32)a1, (int)a2); }
SYS(eventfd) { UNUSED6; return xrl_eventfd_create((u32)a1, 0); }
SYS(memfd_create) { UNUSED6; char n[256]; slong r = xrl_strfu(n, (u64)a1, sizeof n); if (r) return r; return xrl_memfd_create(n, (unsigned)a2); }

/* ---- sockets: only AF_UNIX socketpair is real ------------------------ */
SYS(socketpair)
{
    UNUSED6;
    if (a1 != LX_AF_UNIX) return -LX_EAFNOSUPPORT;
    int type = (int)a2 & 0xf;
    if (type != LX_SOCK_STREAM && type != LX_SOCK_SEQPACKET && type != LX_SOCK_DGRAM) return -LX_EINVAL;
    int fds[2];
    slong r = xrl_socketpair_create(type, (int)a2 & (LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC), fds);
    if (r) return r;
    if (xrl_ctu((u64)a4, fds, 8)) { xrl_fd_close(fds[0]); xrl_fd_close(fds[1]); return -LX_EFAULT; }
    return 0;
}
SYS(sendmsg) { UNUSED6; return xrl_sendmsg((int)a1, (u64)a2, (int)a3); }
SYS(recvmsg) { UNUSED6; return xrl_recvmsg((int)a1, (u64)a2, (int)a3); }
SYS(shutdown) { UNUSED6; return xrl_shutdown((int)a1, (int)a2); }
SYS(sendto) { UNUSED6; return do_write((int)a1, (u64)a2, (u64)a3, -1); }
SYS(recvfrom)
{
    UNUSED6;
    slong r = do_read((int)a1, (u64)a2, (u64)a3, -1);
    if (r >= 0 && a6) { u32 z = 0; xrl_ctu((u64)a6, &z, 4); }
    return r;
}
SYS(socket)
{
    UNUSED6;
    static volatile LONG once;
    if (!InterlockedExchange(&once, 1)) LOGW("socket(domain=%lld type=%lld): real sockets not implemented (-EAFNOSUPPORT)", a1, a2);
    return -LX_EAFNOSUPPORT;
}
SYS(setsockopt) { UNUSED6; xfile *f = xrl_fd_get((int)a1); if (!f) return -LX_EBADF; xfile_unref(f); return 0; }
SYS(getsockopt)
{
    UNUSED6;
    xfile *f = xrl_fd_get((int)a1);
    if (!f) return -LX_EBADF;
    xfile_unref(f);
    if (a2 == LX_SOL_SOCKET && a5) {
        int v = 0, len = 4;
        if (a3 == 3 /*SO_TYPE*/) v = LX_SOCK_STREAM;
        else if (a3 == 7 || a3 == 8) v = 65536;
        xrl_ctu((u64)a4, &v, 4); xrl_ctu((u64)a5, &len, 4);
    }
    return 0;
}
SYS(getsockname) { UNUSED6; if (a3) { u32 z = 2; xrl_ctu((u64)a3, &z, 4); } if (a2) { u16 fam = LX_AF_UNIX; xrl_ctu((u64)a2, &fam, 2); } return 0; }

/* ---- polling ---------------------------------------------------------- */
SYS(poll) { UNUSED6; return xrl_poll((u64)a1, (u64)a2, (int)a3 < 0 ? -1 : (s64)(int)a3 * 1000000ll, 0); }
SYS(ppoll)
{
    UNUSED6;
    s64 to = -1;
    if (a3) {
        lx_timespec ts;
        if (xrl_cfu(&ts, (u64)a3, sizeof ts)) return -LX_EFAULT;
        u64 ns = xrl_ts_to_ns(&ts);
        if (ns == ~0ull) return -LX_EINVAL;
        to = (s64)ns;
    }
    return xrl_poll((u64)a1, (u64)a2, to, (u64)a4);
}
SYS(epoll_create1) { UNUSED6; return xrl_epoll_create((int)a1); }
SYS(epoll_create) { UNUSED6; if (a1 <= 0) return -LX_EINVAL; return xrl_epoll_create(0); }
SYS(epoll_ctl) { UNUSED6; return xrl_epoll_ctl((int)a1, (int)a2, (int)a3, (u64)a4); }
SYS(epoll_wait) { UNUSED6; return xrl_epoll_wait((int)a1, (u64)a2, (int)a3, (int)a4, NULL); }
SYS(epoll_pwait) { UNUSED6; return xrl_epoll_wait((int)a1, (u64)a2, (int)a3, (int)a4, NULL); }

/* ============================================================== memory */
SYS(mmap) { UNUSED6; return xrl_mem_mmap((u64)a1, (u64)a2, (int)a3, (int)a4, (int)a5, a6); }
SYS(munmap)
{
    UNUSED6;
    xrl_thread *t = xrl_cur();
    u64 addr = (u64)a1, len = (u64)a2;
    /* A thread unmapping the stack it is running on (musl __unmapself) is
     * deferred to thread exit; the caller's return address lives on it. */
    if (t && t->frame && len) {
        u64 sp = t->frame + 56;
        u64 end = addr + ((len + 4095) & ~4095ull);
        if (sp >= addr && sp < end) { t->defer_unmap_addr = addr; t->defer_unmap_len = len; return 0; }
    }
    return xrl_mem_munmap(addr, len);
}
SYS(mprotect) { UNUSED6; return xrl_mem_mprotect((u64)a1, (u64)a2, (int)a3); }
SYS(mremap) { UNUSED6; return xrl_mem_mremap((u64)a1, (u64)a2, (u64)a3, (int)a4, (u64)a5); }
SYS(brk) { UNUSED6; return xrl_mem_brk((u64)a1); }
SYS(madvise) { UNUSED6; return xrl_mem_madvise((u64)a1, (u64)a2, (int)a3); }
SYS(msync) { UNUSED6; return xrl_mem_msync((u64)a1, (u64)a2, (int)a3); }
SYS(mincore) { UNUSED6; return xrl_mem_mincore((u64)a1, (u64)a2, (u64)a3); }

/* ============================================================== threads */
SYS(clone) { UNUSED6; return xrl_do_clone((u64)a1, (u64)a2, (u64)a3, (u64)a4, (u64)a5, (u64)a6); }
SYS(clone3)
{
    UNUSED6;
    struct { u64 flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; } ca;
    if ((u64)a2 < 64) return -LX_EINVAL;
    memset(&ca, 0, sizeof ca);
    if (xrl_cfu(&ca, (u64)a1, 64)) return -LX_EFAULT;
    u64 sp = ca.stack ? ca.stack + ca.stack_size : 0;
    return xrl_do_clone(ca.flags, sp, ca.parent_tid, ca.child_tid, ca.tls, 0);
}
SYS(fork)
{
    UNUSED6;
    LOGE("fork/vfork called: the layer is single-process; use posix_spawn-free paths (-ENOSYS)");
    return -LX_ENOSYS;
}
SYS(execve) { UNUSED6; LOGE("execve called: not supported (in-process exec is handled by Wine, not the kernel)"); return -LX_ENOSYS; }
/* exit(status, base, size): B's musl __unmapself passes the dying thread's own
 * stack; unmapped in xrl_do_exit once we are off it (on the host stack). */
SYS(exit)
{
    UNUSED6;
    xrl_thread *t = xrl_cur();
    /* Linux ignores a2/a3 for exit, so other callers leave garbage there. Only
     * honour the region when it really is this thread's own stack (the
     * __unmapself case): it must contain the guest sp at the call. */
    if (t && t->frame && a2 && a3) {
        u64 sp = t->frame + 56, lo = (u64)a2, len = (u64)a3, hi = lo + len;
        /* musl's map_base/map_size: page-aligned, page-multiple, bounded, and a
         * live layer mapping at both ends. Garbage (e.g. a huge size) fails. */
        int plausible = !(lo & (LX_PAGE - 1)) && !(len & (LX_PAGE - 1)) && len <= (256ull << 20) &&
                        hi > lo && sp >= lo && sp < hi &&
                        xrl_mem_is_mapped(lo) && xrl_mem_is_mapped(hi - LX_PAGE);
        if (plausible) { t->defer_unmap_addr = lo; t->defer_unmap_len = len; }
    }
    xrl_do_exit((int)a1, 0);
    return 0;
}
SYS(exit_group) { UNUSED6; xrl_do_exit((int)a1, 1); return 0; }
SYS(gettid) { UNUSED6; xrl_thread *t = xrl_cur(); return t ? t->tid : G.pid; }
SYS(getpid) { UNUSED6; return G.pid; }
SYS(getppid) { UNUSED6; return 1; }
SYS(uid) { UNUSED6; return 1000; }
SYS(set_tid_address) { UNUSED6; xrl_thread *t = xrl_cur(); if (t) t->clear_child_tid = (u64)a1; return t ? t->tid : G.pid; }
SYS(set_robust_list) { UNUSED6; xrl_thread *t = xrl_cur(); if (a2 != 24) return -LX_EINVAL; if (t) { t->robust_list = (u64)a1; t->robust_len = (u64)a2; } return 0; }
SYS(get_robust_list)
{
    UNUSED6;
    xrl_thread *t = (a1 == 0) ? xrl_cur() : xrl_thread_by_tid((int)a1);
    if (!t) return -LX_ESRCH;
    if (xrl_ctu((u64)a2, &t->robust_list, 8) || xrl_ctu((u64)a3, &t->robust_len, 8)) return -LX_EFAULT;
    return 0;
}
SYS(futex) { UNUSED6; return xrl_sys_futex((u64)a1, (int)a2, (u32)a3, (u64)a4, (u64)a5, (u32)a6); }
SYS(sched_yield) { UNUSED6; if (!SwitchToThread()) Sleep(0); return 0; }
SYS(sched_getaffinity) { UNUSED6; return xrl_sys_sched_getaffinity((int)a1, (u64)a2, (u64)a3); }
SYS(sched_prio) { UNUSED6; return (a1 == 1 || a1 == 2) ? 99 : 0; }
SYS(sched_prio_min) { UNUSED6; return (a1 == 1 || a1 == 2) ? 1 : 0; }
SYS(sched_getscheduler) { UNUSED6; return 0; }
SYS(sched_getparam) { UNUSED6; int z = 0; return a2 ? xrl_ctu((u64)a2, &z, 4) : -LX_EINVAL; }
SYS(getcpu) { UNUSED6; u32 c = (u32)(GetCurrentProcessorNumber() % xrl_nprocs()); u32 z = 0; if (a1 && xrl_ctu((u64)a1, &c, 4)) return -LX_EFAULT; if (a2 && xrl_ctu((u64)a2, &z, 4)) return -LX_EFAULT; return 0; }
SYS(kill)
{
    UNUSED6;
    int pid = (int)a1, sig = (int)a2;
    if (pid != G.pid && pid != 0 && pid != -1) return -LX_ESRCH;
    if (sig == 0) return 0;
    return xrl_sys_tgkill(G.pid, G.pid, sig);
}
SYS(tkill) { UNUSED6; return xrl_sys_tgkill(G.pid, (int)a1, (int)a2); }
SYS(tgkill) { UNUSED6; return xrl_sys_tgkill((int)a1, (int)a2, (int)a3); }
SYS(wait4) { UNUSED6; return -LX_ECHILD; }
SYS(rt_sigaction) { UNUSED6; return xrl_sys_rt_sigaction((int)a1, (u64)a2, (u64)a3, (u64)a4); }
SYS(rt_sigprocmask) { UNUSED6; return xrl_sys_rt_sigprocmask((int)a1, (u64)a2, (u64)a3, (u64)a4); }
SYS(sigaltstack) { UNUSED6; return xrl_sys_sigaltstack((u64)a1, (u64)a2); }
SYS(rt_sigreturn) { UNUSED6; return xrl_sys_rt_sigreturn(); }
SYS(rt_sigsuspend) { UNUSED6; return xrl_sys_rt_sigsuspend((u64)a1, (u64)a2); }
SYS(rt_sigtimedwait) { UNUSED6; return xrl_sys_rt_sigtimedwait((u64)a1, (u64)a2, (u64)a3, (u64)a4); }
SYS(rt_sigpending) { UNUSED6; xrl_thread *t = xrl_cur(); u64 p = t ? (t->sigpending & t->sigmask) : 0; return xrl_ctu((u64)a1, &p, 8); }
SYS(pause)
{
    UNUSED6;
    for (;;) { LONG s = xrl_io_epoch; if (xrl_io_wait(s, -1) < 0) return -LX_EINTR; }
}

/* ================================================================ misc */
SYS(arch_prctl) { UNUSED6; return xrl_sys_arch_prctl((int)a1, (u64)a2); }
SYS(prctl) { UNUSED6; return xrl_sys_prctl((int)a1, (u64)a2, (u64)a3, (u64)a4, (u64)a5); }
SYS(uname) { UNUSED6; return xrl_sys_uname((u64)a1); }
SYS(sysinfo) { UNUSED6; return xrl_sys_sysinfo((u64)a1); }
SYS(getrandom) { UNUSED6; return xrl_sys_getrandom((u64)a1, (u64)a2, (u32)a3); }
SYS(prlimit64) { UNUSED6; return xrl_sys_prlimit((int)a1, (int)a2, (u64)a3, (u64)a4); }
SYS(getrlimit) { UNUSED6; return xrl_sys_prlimit(0, (int)a1, 0, (u64)a2); }
SYS(setrlimit) { UNUSED6; return 0; }
SYS(clock_gettime) { UNUSED6; return xrl_sys_clock_gettime((int)a1, (u64)a2); }
SYS(clock_getres) { UNUSED6; return xrl_sys_clock_getres((int)a1, (u64)a2); }
SYS(clock_settime) { UNUSED6; return -LX_EPERM; }
SYS(clock_nanosleep) { UNUSED6; return xrl_sys_clock_nanosleep((int)a1, (int)a2, (u64)a3, (u64)a4); }
SYS(nanosleep) { UNUSED6; return xrl_sys_nanosleep((u64)a1, (u64)a2); }
SYS(gettimeofday) { UNUSED6; return xrl_sys_gettimeofday((u64)a1, (u64)a2); }
SYS(time) { UNUSED6; return xrl_sys_time((u64)a1); }
SYS(times) { UNUSED6; return xrl_sys_times((u64)a1); }
SYS(getrusage) { UNUSED6; return xrl_sys_getrusage((int)a1, (u64)a2); }
SYS(getpgid) { UNUSED6; return G.pid; }
SYS(getgroups) { UNUSED6; return 0; }
SYS(getresuid) { UNUSED6; u32 id = 1000; if (xrl_ctu((u64)a1, &id, 4) || xrl_ctu((u64)a2, &id, 4) || xrl_ctu((u64)a3, &id, 4)) return -LX_EFAULT; return 0; }
SYS(personality) { UNUSED6; return 0; }
SYS(restart_syscall) { UNUSED6; return -LX_EINTR; }

/* ============================================================== table */

typedef slong (*sysfn)(slong, slong, slong, slong, slong, slong);

#define T(nr, fn) [nr] = sys_##fn
static const sysfn g_tab[XRL_NR_MAX] = {
    /* threads / sync */
    T(NR_futex, futex), T(NR_clone, clone), T(NR_clone3, clone3), T(NR_set_tid_address, set_tid_address),
    T(NR_set_robust_list, set_robust_list), T(NR_get_robust_list, get_robust_list), T(NR_gettid, gettid),
    T(NR_sched_yield, sched_yield), T(NR_rt_sigprocmask, rt_sigprocmask), T(NR_rt_sigaction, rt_sigaction),
    T(NR_rt_sigreturn, rt_sigreturn), T(NR_rt_sigsuspend, rt_sigsuspend), T(NR_rt_sigtimedwait, rt_sigtimedwait),
    T(NR_rt_sigpending, rt_sigpending), T(NR_sigaltstack, sigaltstack), T(NR_tgkill, tgkill), T(NR_tkill, tkill),
    T(NR_kill, kill), T(NR_exit, exit), T(NR_exit_group, exit_group), T(NR_pause, pause),
    T(NR_fork, fork), T(NR_vfork, fork), T(NR_execve, execve), T(NR_wait4, wait4),
    /* memory */
    T(NR_mmap, mmap), T(NR_munmap, munmap), T(NR_mprotect, mprotect), T(NR_mremap, mremap), T(NR_brk, brk),
    T(NR_madvise, madvise), T(NR_msync, msync), T(NR_mincore, mincore),
    T(NR_mlock, zero_ok), T(NR_munlock, zero_ok), T(NR_mlockall, zero_ok), T(NR_munlockall, zero_ok),
    /* files */
    T(NR_read, read), T(NR_write, write), T(NR_pread64, pread64), T(NR_pwrite64, pwrite64),
    T(NR_readv, readv), T(NR_writev, writev), T(NR_preadv, preadv), T(NR_pwritev, pwritev),
    T(NR_open, open), T(NR_openat, openat), T(NR_creat, creat), T(NR_close, close), T(NR_close_range, close_range),
    T(NR_lseek, lseek), T(NR_stat, stat), T(NR_fstat, fstat), T(NR_lstat, lstat), T(NR_newfstatat, newfstatat),
    T(NR_statx, statx), T(NR_access, access), T(NR_faccessat, faccessat), T(NR_faccessat2, faccessat),
    T(NR_getdents64, getdents64), T(NR_readlink, readlink), T(NR_readlinkat, readlinkat),
    T(NR_fcntl, fcntl), T(NR_ioctl, ioctl), T(NR_dup, dup), T(NR_dup2, dup2), T(NR_dup3, dup3),
    T(NR_pipe, pipe), T(NR_pipe2, pipe2), T(NR_eventfd, eventfd), T(NR_eventfd2, eventfd2), T(NR_memfd_create, memfd_create),
    T(NR_unlink, unlink), T(NR_unlinkat, unlinkat), T(NR_mkdir, mkdir), T(NR_mkdirat, mkdirat), T(NR_rmdir, rmdir),
    T(NR_rename, rename), T(NR_renameat, renameat), T(NR_renameat2, renameat2),
    T(NR_symlink, symlink), T(NR_symlinkat, symlinkat), T(NR_link, link), T(NR_linkat, linkat),
    T(NR_chdir, chdir), T(NR_fchdir, fchdir), T(NR_getcwd, getcwd), T(NR_truncate, truncate), T(NR_ftruncate, ftruncate),
    T(NR_fsync, fsync), T(NR_fdatasync, fsync), T(NR_fallocate, fallocate), T(NR_fadvise64, zero_ok),
    T(NR_flock, zero_ok), T(NR_chmod, zero_ok), T(NR_fchmod, zero_ok), T(NR_fchmodat, zero_ok),
    T(NR_chown, zero_ok), T(NR_fchown, zero_ok), T(NR_lchown, zero_ok), T(NR_umask, umask),
    T(NR_utimensat, utimensat), T(NR_utimes, utimes), T(NR_statfs, statfs), T(NR_fstatfs, fstatfs),
    /* poll / sockets */
    T(NR_poll, poll), T(NR_ppoll, ppoll), T(NR_epoll_create, epoll_create), T(NR_epoll_create1, epoll_create1),
    T(NR_epoll_ctl, epoll_ctl), T(NR_epoll_wait, epoll_wait), T(NR_epoll_pwait, epoll_pwait),
    T(NR_socketpair, socketpair), T(NR_sendmsg, sendmsg), T(NR_recvmsg, recvmsg), T(NR_shutdown, shutdown),
    T(NR_sendto, sendto), T(NR_recvfrom, recvfrom), T(NR_socket, socket),
    T(NR_setsockopt, setsockopt), T(NR_getsockopt, getsockopt), T(NR_getsockname, getsockname),
    /* time */
    T(NR_clock_gettime, clock_gettime), T(NR_clock_getres, clock_getres), T(NR_clock_settime, clock_settime),
    T(NR_clock_nanosleep, clock_nanosleep), T(NR_nanosleep, nanosleep), T(NR_gettimeofday, gettimeofday),
    T(NR_time, time), T(NR_times, times), T(NR_getrusage, getrusage),
    /* ident / misc */
    T(NR_getpid, getpid), T(NR_getppid, getppid), T(NR_getuid, uid), T(NR_geteuid, uid), T(NR_getgid, uid), T(NR_getegid, uid),
    T(NR_getpgid, getpgid), T(NR_getpgrp, getpgid), T(NR_getsid, getpgid), T(NR_setsid, getpgid), T(NR_setpgid, zero_ok),
    T(NR_setuid, zero_ok), T(NR_setgid, zero_ok), T(NR_setresuid, zero_ok), T(NR_setresgid, zero_ok),
    T(NR_getgroups, getgroups), T(NR_getresuid, getresuid), T(NR_getresgid, getresuid),
    T(NR_prctl, prctl), T(NR_arch_prctl, arch_prctl), T(NR_uname, uname), T(NR_sysinfo, sysinfo), T(NR_getrandom, getrandom),
    T(NR_getrlimit, getrlimit), T(NR_setrlimit, setrlimit), T(NR_prlimit64, prlimit64), T(NR_personality, personality),
    T(NR_sched_getaffinity, sched_getaffinity), T(NR_sched_setaffinity, zero_ok), T(NR_getcpu, getcpu),
    T(NR_sched_setparam, zero_ok), T(NR_sched_getparam, sched_getparam), T(NR_sched_setscheduler, zero_ok),
    T(NR_sched_getscheduler, sched_getscheduler), T(NR_sched_get_priority_max, sched_prio),
    T(NR_sched_get_priority_min, sched_prio_min), T(NR_getpriority, zero_ok), T(NR_setpriority, zero_ok),
    T(NR_restart_syscall, restart_syscall),
};
#undef T

/* Syscalls the layer knows about but intentionally does not implement (still
 * ENOSYS, but logged once with the reason so they aren't chased by mistake). */
static const char *stub_reason(slong nr)
{
    switch (nr) {
    case NR_rseq: return "probe by newer libcs; harmless";
    case NR_futex_waitv: return "Proton fsync probe; ENOSYS makes Wine disable fsync (desired)";
    default: return NULL;
    }
}

static volatile LONG g_enosys_hits[XRL_NR_MAX];
static slong enosys(slong nr, slong a1, slong a2, slong a3)
{
    slong idx = (nr >= 0 && nr < XRL_NR_MAX) ? nr : 0;
    LONG n = InterlockedIncrement(&g_enosys_hits[idx]);
    if (n <= 4 || (n & (n - 1)) == 0) {
        const char *why = stub_reason(nr);
        LOGW("ENOSYS %lld %s (a1=%llx a2=%llx a3=%llx) hit #%lld%s%s", nr, xrl_sysname(nr), a1, a2, a3, (slong)n, why ? " - " : "", why ? why : "");
    }
    return -LX_ENOSYS;
}

/* XRL pseudo-syscalls (ABI v2): guest-callable copies of the host op tables. */
static slong xrl_pseudo(slong nr)
{
    const xrl_guest_ctx *g = (const xrl_guest_ctx *)G.guest_ctx;
    const void *p = NULL;
    if (g) {
        switch (nr) {
        case XRL_SYS_GET_GFX_OPS:   p = g->gfx; break;
        case XRL_SYS_GET_AUDIO_OPS: p = g->audio; break;
        case XRL_SYS_GET_INPUT_OPS: p = g->input; break;
        case XRL_SYS_GET_LOG_FN:    p = (const void *)g->log; break;
        }
    }
    return p ? (slong)(uintptr_t)p : -LX_ENOSYS;
}

slong xrl_syscall_dispatch(slong nr, slong a1, slong a2, slong a3, slong a4, slong a5, slong a6, void *frame)
{
    (void)frame;
    xrl_thread *t = xrl_cur();
    slong r;
    int was_guest = xrl_teb_enter_host(t);
    if (t) t->in_sys++;
    if ((u64)nr < XRL_NR_MAX && g_tab[nr]) r = g_tab[nr](a1, a2, a3, a4, a5, a6);
    else if (nr >= XRL_SYS_GET_GFX_OPS && nr <= XRL_SYS_GET_LOG_FN) r = xrl_pseudo(nr);
    else r = enosys(nr, a1, a2, a3);
    if (G.trace_syscalls) LOGT("[%d] %s(%llx, %llx, %llx, %llx, %llx, %llx) = %lld", t ? t->tid : -1, xrl_sysname(nr), a1, a2, a3, a4, a5, a6, r);
    if (t) t->in_sys--;
    xrl_teb_restore(t, was_guest);
    return r;
}

void xrl_syscall_table_init(void) {}

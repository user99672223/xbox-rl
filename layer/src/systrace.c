/*
 * systrace.c - decoded system-call trace (XRL_TRACE=2).
 *
 * XRL_TRACE=1 logs every call as raw numbers (syscall.c). XRL_TRACE=2 decodes them: path strings, the
 * bytes a read returned (whole text for /proc, /etc and /sys files, a prefix for the rest), socket
 * addresses, uname, limits, stat results, the errno by name. It starts at the first memfd_create
 * (what the EAC bootstrapper's module mapping issues first) so a Wine start-up's millions of calls
 * stay out of the log; XRL_TRACE_FROM=all starts at once, XRL_TRACE_FROM=<syscall name> at that call.
 * Used to record what a program asks of the system and what the layer answers (docs/decisions.md).
 */
#include "xrl_internal.h"

enum { N_READ = 0, N_WRITE = 1, N_READV = 19, N_WRITEV = 20, N_OPEN = 2, N_CLOSE = 3, N_STAT = 4, N_FSTAT = 5, N_LSTAT = 6, N_PREAD = 17,
       N_ACCESS = 21, N_DUP = 32, N_DUP2 = 33, N_SOCKET = 41, N_CONNECT = 42, N_SENDTO = 44, N_RECVFROM = 45,
       N_BIND = 49, N_GETSOCKNAME = 51, N_GETPEERNAME = 52, N_SETSOCKOPT = 54, N_GETSOCKOPT = 55, N_EXECVE = 59,
       N_UNAME = 63, N_FCNTL = 72, N_READLINK = 89, N_GETRLIMIT = 97, N_SYSINFO = 99, N_GETDENTS64 = 217,
       N_OPENAT = 257, N_NEWFSTATAT = 262, N_READLINKAT = 267, N_FACCESSAT = 269, N_DUP3 = 292,
       N_PRLIMIT64 = 302, N_GETRANDOM = 318, N_MEMFD_CREATE = 319, N_STATX = 332, N_FACCESSAT2 = 439,
       N_SENDMSG = 46, N_RECVMSG = 47 };

static volatile LONG g_armed;
static long g_from_nr = -2;   /* -2 not read yet, -1 all, else the syscall number that starts the trace */
static char g_fdpath[4096][96];   /* path behind a small fd number, for deciding how much of a read to show */

static const char *errname(int e)
{
    switch (e) {
    case 1: return "EPERM"; case 2: return "ENOENT"; case 3: return "ESRCH"; case 4: return "EINTR"; case 5: return "EIO";
    case 9: return "EBADF"; case 11: return "EAGAIN"; case 12: return "ENOMEM"; case 13: return "EACCES";
    case 14: return "EFAULT"; case 17: return "EEXIST"; case 20: return "ENOTDIR"; case 21: return "EISDIR";
    case 22: return "EINVAL"; case 24: return "EMFILE"; case 25: return "ENOTTY"; case 29: return "ESPIPE";
    case 32: return "EPIPE"; case 34: return "ERANGE"; case 38: return "ENOSYS"; case 61: return "ENODATA";
    case 88: return "ENOTSOCK"; case 95: return "EOPNOTSUPP"; case 97: return "EAFNOSUPPORT"; case 98: return "EADDRINUSE";
    case 101: return "ENETUNREACH"; case 110: return "ETIMEDOUT"; case 111: return "ECONNREFUSED"; case 115: return "EINPROGRESS";
    default: return NULL;
    }
}

/* printable copy of up to cap-1 bytes of n at user address u: text kept, control bytes escaped */
static void fmt_bytes(char *o, size_t cap, u64 u, size_t n, size_t maxn)
{
    unsigned char b[2048];
    size_t shown = n < maxn ? n : maxn;
    if (shown > sizeof b) shown = sizeof b;
    size_t w = 0;
    if (xrl_cfu(b, u, shown)) { snprintf(o, cap, "<unreadable>"); return; }
    for (size_t i = 0; i < shown && w + 6 < cap; i++) {
        unsigned char c = b[i];
        if (c == '\n') { o[w++] = '\\'; o[w++] = 'n'; }
        else if (c == '"') { o[w++] = '\\'; o[w++] = '"'; }
        else if (c == '\\') { o[w++] = '\\'; o[w++] = '\\'; }
        else if (c >= 32 && c < 127) o[w++] = (char)c;
        else w += (size_t)snprintf(o + w, cap - w, "\\x%02x", c);
    }
    if (shown < n && w + 8 < cap) w += (size_t)snprintf(o + w, cap - w, "...(+%zu)", n - shown);
    o[w] = 0;
}

static void fmt_path(char *o, size_t cap, u64 u)
{
    char p[512];
    if (!u) { snprintf(o, cap, "NULL"); return; }
    int e = xrl_strfu(p, u, sizeof p);
    if (e) snprintf(o, cap, "<bad %llx>", u); else snprintf(o, cap, "\"%s\"", p);
}

static void fmt_sockaddr(char *o, size_t cap, u64 u, u64 len)
{
    unsigned char sa[128];
    if (!u || len < 2) { snprintf(o, cap, "NULL"); return; }
    if (len > sizeof sa) len = sizeof sa;
    if (xrl_cfu(sa, u, (size_t)len)) { snprintf(o, cap, "<bad %llx>", u); return; }
    unsigned fam = sa[0] | (sa[1] << 8);
    if (fam == 2 && len >= 8) snprintf(o, cap, "{AF_INET %u.%u.%u.%u:%u}", sa[4], sa[5], sa[6], sa[7], (sa[2] << 8) | sa[3]);
    else if (fam == 10 && len >= 24) {
        char h[64]; size_t w = 0;
        for (int i = 0; i < 16; i += 2) w += (size_t)snprintf(h + w, sizeof h - w, "%s%x", i ? ":" : "", (sa[8 + i] << 8) | sa[9 + i]);
        snprintf(o, cap, "{AF_INET6 [%s]:%u}", h, (sa[2] << 8) | sa[3]);
    } else if (fam == 1) snprintf(o, cap, "{AF_UNIX \"%.*s\"}", (int)(len - 2), (const char *)sa + 2);
    else snprintf(o, cap, "{family %u len %llu}", fam, len);
}

/* how much of what a read returned is worth showing */
static size_t show_limit(int fd)
{
    if (fd < 0 || fd >= 4096) return 32;
    const char *p = g_fdpath[fd];
    if (!strncmp(p, "/proc", 5) || !strncmp(p, "/etc", 4) || !strncmp(p, "/sys", 4) || !strncmp(p, "/tmp", 4) || !strncmp(p, "/run", 4)) return 2000;
    return 32;
}

static void note_open(int fd, const char *path)
{
    if (fd >= 0 && fd < 4096) { strncpy(g_fdpath[fd], path, sizeof g_fdpath[fd] - 1); g_fdpath[fd][sizeof g_fdpath[fd] - 1] = 0; }
}

static const char *sockopt_name(int level, int opt)
{
    if (level == 1 && opt == 2) return "SO_REUSEADDR";
    if (level == 1 && opt == 4) return "SO_ERROR";
    if (level == 1 && opt == 9) return "SO_KEEPALIVE";
    if (level == 6 && opt == 1) return "TCP_NODELAY";
    return NULL;
}

void xrl_systrace(slong nr, const slong *a, slong r, int tid)
{
    char l[1800], s1[640], s2[256];
    if (g_from_nr == -2) {
        char v[32];
        g_from_nr = (long)N_MEMFD_CREATE;
        if (GetEnvironmentVariableA("XRL_TRACE_FROM", v, sizeof v)) {
            if (!strcmp(v, "all")) g_from_nr = -1;
            else for (int i = 0; i < 400; i++) { const char *nm = xrl_sysname(i); if (nm && !strcmp(nm, v)) { g_from_nr = i; break; } }
        }
    }
    if (!g_armed) {
        if (g_from_nr != -1 && nr != g_from_nr) { /* keep fd names current before the start */ goto bookkeeping; }
        InterlockedExchange(&g_armed, 1);
    }

    int n = 0;
    /* timing and locking noise (Wine's idle loops) is left out; XRL_TRACE_NOISE=1 keeps it */
    switch (nr) {
    case 202: case 228: case 96: case 230: case 35: case 24: case 14: case 98: case 201: case 7: case 271: case 232: case 281:
        { static int noise = -1; if (noise < 0) { char v[4]; noise = GetEnvironmentVariableA("XRL_TRACE_NOISE", v, sizeof v) && v[0] == '1'; }
          if (!noise) return; }
    }
    const char *nm = xrl_sysname((int)nr);
    n += snprintf(l + n, sizeof l - n, "[%d] %s(", tid, nm ? nm : "?");
    switch (nr) {
    case N_OPEN: fmt_path(s1, sizeof s1, (u64)a[0]); n += snprintf(l + n, sizeof l - n, "%s, 0x%llx", s1, (u64)a[1]); break;
    case N_OPENAT: case N_NEWFSTATAT: case N_FACCESSAT: case N_FACCESSAT2: case N_STATX:
        fmt_path(s1, sizeof s1, (u64)a[1]); n += snprintf(l + n, sizeof l - n, "%d, %s, 0x%llx", (int)a[0], s1, (u64)a[2]); break;
    case N_STAT: case N_LSTAT: case N_ACCESS: case N_READLINK: case N_EXECVE:
        fmt_path(s1, sizeof s1, (u64)a[0]); n += snprintf(l + n, sizeof l - n, "%s", s1); break;
    case N_READLINKAT: fmt_path(s1, sizeof s1, (u64)a[1]); n += snprintf(l + n, sizeof l - n, "%d, %s", (int)a[0], s1); break;
    case N_MEMFD_CREATE: fmt_path(s1, sizeof s1, (u64)a[0]); n += snprintf(l + n, sizeof l - n, "%s, 0x%llx", s1, (u64)a[1]); break;
    case N_CONNECT: case N_BIND: fmt_sockaddr(s1, sizeof s1, (u64)a[1], (u64)a[2]); n += snprintf(l + n, sizeof l - n, "%d, %s", (int)a[0], s1); break;
    case N_SENDTO: fmt_sockaddr(s2, sizeof s2, (u64)a[4], (u64)a[5]);
        n += snprintf(l + n, sizeof l - n, "%d, len %llu, 0x%llx, %s", (int)a[0], (u64)a[2], (u64)a[3], s2); break;
    case N_SOCKET: n += snprintf(l + n, sizeof l - n, "domain %lld, type 0x%llx, proto %lld", a[0], (u64)a[1], a[2]); break;
    case N_SETSOCKOPT: case N_GETSOCKOPT: {
        const char *on = sockopt_name((int)a[1], (int)a[2]);
        n += snprintf(l + n, sizeof l - n, "%d, level %lld, opt %lld%s%s%s", (int)a[0], a[1], a[2], on ? " (" : "", on ? on : "", on ? ")" : ""); break; }
    case N_READ: case N_PREAD: case N_WRITE: case N_RECVFROM:
        n += snprintf(l + n, sizeof l - n, "%d (%s), len %llu", (int)a[0], (a[0] >= 0 && a[0] < 4096 && g_fdpath[a[0]][0]) ? g_fdpath[a[0]] : "", (u64)a[2]); break;
    case N_FCNTL: n += snprintf(l + n, sizeof l - n, "%d, cmd %lld, %llx", (int)a[0], a[1], (u64)a[2]); break;
    case N_PRLIMIT64: n += snprintf(l + n, sizeof l - n, "pid %lld, resource %lld", a[0], a[1]); break;
    case N_GETRLIMIT: n += snprintf(l + n, sizeof l - n, "resource %lld", a[0]); break;
    default: n += snprintf(l + n, sizeof l - n, "%llx, %llx, %llx", (u64)a[0], (u64)a[1], (u64)a[2]); break;
    }
    n += snprintf(l + n, sizeof l - n, ") = %lld", (long long)r);
    if (r < 0 && r > -4096) { const char *en = errname((int)-r); if (en) n += snprintf(l + n, sizeof l - n, " %s", en); }

    if (r >= 0) switch (nr) {
    case N_READ: case N_PREAD: case N_RECVFROM:
        if (r > 0) { fmt_bytes(s1, sizeof s1, (u64)a[1], (size_t)r, show_limit((int)a[0]) ); n += snprintf(l + n, sizeof l - n, " \"%s\"", s1); }
        break;
    case N_WRITE: case N_SENDTO: if (r > 0) { fmt_bytes(s1, sizeof s1, (u64)a[1], (size_t)r, 48); n += snprintf(l + n, sizeof l - n, " \"%s\"", s1); } break;
    case N_READLINK: if (r > 0) { fmt_bytes(s1, sizeof s1, (u64)a[1], (size_t)r, 300); n += snprintf(l + n, sizeof l - n, " -> \"%s\"", s1); } break;
    case N_READLINKAT: if (r > 0) { fmt_bytes(s1, sizeof s1, (u64)a[2], (size_t)r, 300); n += snprintf(l + n, sizeof l - n, " -> \"%s\"", s1); } break;
    case N_UNAME: {
        char u[390];
        if (!xrl_cfu(u, (u64)a[0], sizeof u))
            n += snprintf(l + n, sizeof l - n, " {sysname=%.65s nodename=%.65s release=%.65s version=%.65s machine=%.65s domain=%.65s}",
                          u, u + 65, u + 130, u + 195, u + 260, u + 325);
        break; }
    case N_FSTAT: case N_STAT: case N_LSTAT: case N_NEWFSTATAT: {
        u64 buf = nr == N_NEWFSTATAT ? (u64)a[2] : (u64)a[1];
        unsigned char st[144];
        if (!xrl_cfu(st, buf, sizeof st)) {
            u32 mode; s64 size; u64 ino, dev, nlink; u32 uid, gid;
            memcpy(&dev, st, 8); memcpy(&ino, st + 8, 8); memcpy(&nlink, st + 16, 8); memcpy(&mode, st + 24, 4);
            memcpy(&uid, st + 28, 4); memcpy(&gid, st + 32, 4); memcpy(&size, st + 48, 8);
            n += snprintf(l + n, sizeof l - n, " {dev=%llx ino=%llu mode=%o nlink=%llu uid=%u gid=%u size=%lld}", dev, ino, mode, nlink, uid, gid, (long long)size);
        }
        break; }
    case N_STATX: {
        unsigned char st[256];
        if (!xrl_cfu(st, (u64)a[4], sizeof st)) {
            u16 mode; u64 size; u32 mask; memcpy(&mask, st, 4); memcpy(&mode, st + 28, 2); memcpy(&size, st + 40, 8);
            n += snprintf(l + n, sizeof l - n, " {mask=%x mode=%o size=%llu}", mask, mode, size);
        }
        break; }
    case N_GETSOCKNAME: case N_GETPEERNAME: {
        u32 len = 0; if (!xrl_cfu(&len, (u64)a[2], 4)) { fmt_sockaddr(s1, sizeof s1, (u64)a[1], len); n += snprintf(l + n, sizeof l - n, " %s", s1); }
        break; }
    case N_PRLIMIT64: case N_GETRLIMIT: {
        u64 rl[2]; u64 p = nr == N_PRLIMIT64 ? (u64)a[3] : (u64)a[1];
        if (p && !xrl_cfu(rl, p, 16)) n += snprintf(l + n, sizeof l - n, " {cur=%lld max=%lld}", (long long)rl[0], (long long)rl[1]);
        break; }
    case N_SYSINFO: {
        u64 si[14];
        if (!xrl_cfu(si, (u64)a[0], sizeof si)) n += snprintf(l + n, sizeof l - n, " {uptime=%llu totalram=%llu freeram=%llu}", si[0], si[4], si[5]);
        break; }
    case N_READV: {   /* the bytes of the vectors, in order, as far as r reaches */
        u64 iov[2 * 8]; u64 cnt = (u64)a[2] > 8 ? 8 : (u64)a[2], left = (u64)r; size_t lim = show_limit((int)a[0]);
        if (!xrl_cfu(iov, (u64)a[1], (size_t)cnt * 16))
            for (u64 i = 0; i < cnt && left && lim; i++) {
                u64 c = iov[2 * i + 1] < left ? iov[2 * i + 1] : left;
                if (c) { fmt_bytes(s1, sizeof s1, iov[2 * i], (size_t)c, lim); n += snprintf(l + n, sizeof l - n, " \"%s\"", s1); }
                left -= c; lim = lim > c ? lim - (size_t)c : 0;
            }
        break; }
    case N_GETRANDOM: break;
    case N_GETDENTS64: if (r > 0) { fmt_bytes(s1, sizeof s1, (u64)a[1], (size_t)r, 400); n += snprintf(l + n, sizeof l - n, " \"%s\"", s1); } break;
    }
    LOGT("%s", l);

bookkeeping:
    switch (nr) {
    case N_OPEN: if (r >= 0 && !xrl_strfu(s1, (u64)a[0], sizeof s1)) note_open((int)r, s1); break;
    case N_OPENAT: if (r >= 0 && !xrl_strfu(s1, (u64)a[1], sizeof s1)) note_open((int)r, s1); break;
    case N_MEMFD_CREATE: if (r >= 0) note_open((int)r, "/memfd"); break;
    case N_CLOSE: if (a[0] >= 0 && a[0] < 4096) g_fdpath[a[0]][0] = 0; break;
    case N_DUP: case N_DUP2: case N_DUP3: if (r >= 0 && r < 4096 && a[0] >= 0 && a[0] < 4096) memcpy(g_fdpath[r], g_fdpath[a[0]], sizeof g_fdpath[0]); break;
    }
}

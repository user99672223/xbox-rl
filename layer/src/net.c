/*
 * net.c - AF_INET / AF_INET6 TCP and UDP sockets over Winsock.
 *
 * The layer used to have only in-process pipes and socketpairs (ipc.c). A guest
 * socket is now a real Winsock SOCKET behind an xfile, so everything that takes an
 * fd works on it: read/write/readv/writev, poll/ppoll/select/epoll, fcntl(O_NONBLOCK),
 * ioctl(FIONBIO/FIONREAD), dup, close, fstat (S_IFSOCK).
 *
 * Design
 *  - Every Winsock socket is non-blocking and registered with WSAEventSelect on ONE
 *    shared event. A small helper thread (not a guest thread) waits on it and bumps
 *    the layer's I/O epoch (ipc.c: xrl_io_notify), which is exactly what poll/epoll
 *    and the blocking pipe/eventfd paths already sleep on. So poll()/epoll_wait()
 *    needed no change: net_poll() below reports readiness (one WSAPoll with a zero
 *    timeout) and a network event wakes the sleepers to ask again.
 *  - Blocking semantics are ours, not Winsock's: a blocking socket call tries the
 *    operation and, on WSAEWOULDBLOCK, sleeps on the epoch (interruptible by
 *    signals like every other layer wait, honouring SO_RCVTIMEO/SO_SNDTIMEO).
 *    Because nothing ever blocks inside the Windows kernel, exit_group, tgkill and
 *    thread teardown always win.
 *  - Guest buffers are never handed to Winsock: data moves through a bounded bounce
 *    buffer and xrl_cfu/xrl_ctu, so lazily committed guest memory just works.
 *  - Linux and Windows differ in constants (SOL_SOCKET, SO_*, TCP_*, IP_*, MSG_*,
 *    AF_INET6, errno values) and a few behaviours; each is mapped below. Notable
 *    Linux behaviours kept: connect() to 0.0.0.0 / :: means loopback, AF_INET6
 *    sockets are dual-stack by default, SO_REUSEADDR on a TCP socket is accepted
 *    without Windows' port-stealing semantics, send on a closed peer is EPIPE
 *    (there is no SIGPIPE).
 *
 * Not provided: AF_UNIX sockets other than socketpair (-EAFNOSUPPORT), raw sockets
 * and AF_NETLINK/AF_PACKET (-EAFNOSUPPORT / -EPERM), ancillary data (SCM_*, IP_PKTINFO),
 * out-of-band data beyond MSG_OOB pass-through, SIOCGIF* ioctls.
 */
#include "xrl_internal.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
/* SIO_TCP_INITIAL_RTO_PARAMETERS: mstcpip.h gates it (and the struct) behind a Windows 8 NTDDI the layer does not set */
#define XRL_SIO_TCP_INITIAL_RTO_PARAMETERS _WSAIOW(IOC_VENDOR, 17)
typedef struct { USHORT Rtt; UCHAR MaxSynRetransmissions; } xrl_rto_params;
#define XRL_RTO_UNSPECIFIED_RTT 0xffff
#define XRL_RTO_NO_SYN_RETRANSMISSIONS 0xfe

/* ----------------------------------------------------------------- errno */
#define NETERR(name, v) enum { name##_VAL = v }
#define LXE(name, v) NETERR(name, v)
LXE(NE_EACCES, 13); LXE(NE_EFAULT, 14); LXE(NE_EMFILE, 24); LXE(NE_EPIPE, 32); LXE(NE_ENAMETOOLONG, 36); LXE(NE_ELOOP, 40);
LXE(NE_EDESTADDRREQ, 89); LXE(NE_EMSGSIZE, 90); LXE(NE_EPROTOTYPE, 91); LXE(NE_ENOPROTOOPT, 92); LXE(NE_EPROTONOSUPPORT, 93);
LXE(NE_ESOCKTNOSUPPORT, 94); LXE(NE_EOPNOTSUPP, 95); LXE(NE_EPFNOSUPPORT, 96); LXE(NE_EAFNOSUPPORT, 97); LXE(NE_EADDRINUSE, 98);
LXE(NE_EADDRNOTAVAIL, 99); LXE(NE_ENETDOWN, 100); LXE(NE_ENETUNREACH, 101); LXE(NE_ENETRESET, 102); LXE(NE_ECONNABORTED, 103);
LXE(NE_ECONNRESET, 104); LXE(NE_ENOBUFS, 105); LXE(NE_EISCONN, 106); LXE(NE_ENOTCONN, 107); LXE(NE_ETIMEDOUT, 110);
LXE(NE_ECONNREFUSED, 111); LXE(NE_EHOSTDOWN, 112); LXE(NE_EHOSTUNREACH, 113); LXE(NE_EALREADY, 114); LXE(NE_EINPROGRESS, 115);
LXE(NE_EINTR, 4); LXE(NE_EAGAIN, 11); LXE(NE_EBADF, 9); LXE(NE_EINVAL, 22); LXE(NE_ENOMEM, 12); LXE(NE_ENOTSOCK, 88);
LXE(NE_ENOSYS, 38); LXE(NE_EPERM, 1);
#define NE(n) (NE_##n##_VAL)

/* Winsock error -> positive Linux errno */
static int wsa_errno(int e)
{
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return NE(EINTR);
    case WSAEBADF: return NE(EBADF);
    case WSAEACCES: return NE(EACCES);
    case WSAEFAULT: return NE(EFAULT);
    case WSAEINVAL: return NE(EINVAL);
    case WSAEMFILE: return NE(EMFILE);
    case WSAEWOULDBLOCK: return NE(EAGAIN);
    case WSAEINPROGRESS: return NE(EINPROGRESS);
    case WSAEALREADY: return NE(EALREADY);
    case WSAENOTSOCK: return NE(ENOTSOCK);
    case WSAEDESTADDRREQ: return NE(EDESTADDRREQ);
    case WSAEMSGSIZE: return NE(EMSGSIZE);
    case WSAEPROTOTYPE: return NE(EPROTOTYPE);
    case WSAENOPROTOOPT: return NE(ENOPROTOOPT);
    case WSAEPROTONOSUPPORT: return NE(EPROTONOSUPPORT);
    case WSAESOCKTNOSUPPORT: return NE(ESOCKTNOSUPPORT);
    case WSAEOPNOTSUPP: return NE(EOPNOTSUPP);
    case WSAEPFNOSUPPORT: return NE(EPFNOSUPPORT);
    case WSAEAFNOSUPPORT: return NE(EAFNOSUPPORT);
    case WSAEADDRINUSE: return NE(EADDRINUSE);
    case WSAEADDRNOTAVAIL: return NE(EADDRNOTAVAIL);
    case WSAENETDOWN: case WSANOTINITIALISED: case WSASYSNOTREADY: return NE(ENETDOWN);
    case WSAENETUNREACH: return NE(ENETUNREACH);
    case WSAENETRESET: return NE(ENETRESET);
    case WSAECONNABORTED: return NE(ECONNABORTED);
    case WSAECONNRESET: return NE(ECONNRESET);
    case WSAENOBUFS: return NE(ENOBUFS);
    case WSAEISCONN: return NE(EISCONN);
    case WSAENOTCONN: return NE(ENOTCONN);
    case WSAESHUTDOWN: return NE(EPIPE);
    case WSAETIMEDOUT: return NE(ETIMEDOUT);
    case WSAECONNREFUSED: return NE(ECONNREFUSED);
    case WSAELOOP: return NE(ELOOP);
    case WSAENAMETOOLONG: return NE(ENAMETOOLONG);
    case WSAEHOSTDOWN: return NE(EHOSTDOWN);
    case WSAEHOSTUNREACH: return NE(EHOSTUNREACH);
    default: return NE(EINVAL);
    }
}
static slong werr(void) { return -(slong)wsa_errno(WSAGetLastError()); }

/* ---------------------------------------------------------- Linux values */
#define L_AF_UNSPEC 0
#define L_AF_INET 2
#define L_AF_INET6 10
#define L_SOCK_STREAM 1
#define L_SOCK_DGRAM 2
#define L_SOCK_RAW 3
#define L_SOCK_NONBLOCK 04000
#define L_SOCK_CLOEXEC 02000000
#define L_IPPROTO_IP 0
#define L_IPPROTO_ICMP 1
#define L_IPPROTO_TCP 6
#define L_IPPROTO_UDP 17
#define L_IPPROTO_IPV6 41
#define L_SOL_SOCKET 1
#define L_MSG_OOB 0x1
#define L_MSG_PEEK 0x2
#define L_MSG_TRUNC 0x20
#define L_MSG_DONTWAIT 0x40
#define L_MSG_WAITALL 0x100
#define L_MSG_NOSIGNAL 0x4000

typedef struct net_iovec { u64 base, len; } net_iovec;

/* ---------------------------------------------------------------- state */
typedef struct netsock {
    SOCKET s;
    int domain, type, proto;      /* Linux values */
    int listening, connecting, connected, bound;
    int soerr;                    /* pending errno for SO_ERROR (positive), set by a failed non-blocking connect */
    int rcvtimeo_ms, sndtimeo_ms; /* 0 = none */
    int reuseaddr, reuseport, keepalive_set, v6only;
} netsock;

static INIT_ONCE g_net_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_net_ev;
static int g_net_ok;

static DWORD WINAPI net_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_net_ev, INFINITE);
        WSAResetEvent(g_net_ev);
        xrl_io_notify_key(&g_net_ev);   /* one key for all sockets: the one shared event does not tell them apart */
    }
}

static BOOL CALLBACK net_init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    WSADATA wd;
    int e = WSAStartup(MAKEWORD(2, 2), &wd);
    if (e) { LOGE("net: WSAStartup failed (%d): sockets unavailable", e); return TRUE; }
    g_net_ev = WSACreateEvent();
    HANDLE th = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!th || g_net_ev == WSA_INVALID_EVENT) { LOGE("net: cannot start the network event thread"); return TRUE; }
    CloseHandle(th);
    g_net_ok = 1;
    LOGI("net: Winsock %d.%d up (TCP/UDP over IPv4/IPv6)", LOBYTE(wd.wVersion), HIBYTE(wd.wVersion));
    return TRUE;
}
static int net_up(void)
{
    InitOnceExecuteOnce(&g_net_once, net_init_once, NULL, NULL);
    return g_net_ok;
}

static const xfile_ops g_net_ops;

static netsock *ns_of(xfile *f) { return f && f->ops == &g_net_ops ? (netsock *)f->priv : NULL; }
const void *xrl_net_key(xfile *f) { return ns_of(f) ? (const void *)&g_net_ev : NULL; }
int xrl_net_is_key(const void *k) { return k == (const void *)&g_net_ev; }

/* The sentinel the syscall layer uses to try the pipe-based socketpair code next. */
#define NOT_NET XRL_NOT_NET

/* ------------------------------------------------------------ addresses */
/* Linux sockaddr -> Winsock. Returns 0 or -errno. */
static slong sa_to_win(netsock *n, const void *lx, u32 len, SOCKADDR_STORAGE *out, int *outlen, int for_connect)
{
    const u8 *b = (const u8 *)lx;
    u16 fam;
    memset(out, 0, sizeof *out);
    if (len < 2) return -NE(EINVAL);
    memcpy(&fam, b, 2);
    if (fam == L_AF_INET) {
        if (len < 16 || n->domain != L_AF_INET) return n->domain == L_AF_INET ? -NE(EINVAL) : -NE(EAFNOSUPPORT);
        SOCKADDR_IN *s = (SOCKADDR_IN *)out;
        s->sin_family = AF_INET;
        memcpy(&s->sin_port, b + 2, 2);
        memcpy(&s->sin_addr, b + 4, 4);
        if (for_connect && s->sin_addr.s_addr == 0) s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* Linux: connect to 0.0.0.0 reaches this host */
        *outlen = sizeof *s;
        return 0;
    }
    if (fam == L_AF_INET6) {
        if (len < 28 || n->domain != L_AF_INET6) return n->domain == L_AF_INET6 ? -NE(EINVAL) : -NE(EAFNOSUPPORT);
        SOCKADDR_IN6 *s = (SOCKADDR_IN6 *)out;
        s->sin6_family = AF_INET6;
        memcpy(&s->sin6_port, b + 2, 2);
        memcpy(&s->sin6_flowinfo, b + 4, 4);
        memcpy(&s->sin6_addr, b + 8, 16);
        memcpy(&s->sin6_scope_id, b + 24, 4);
        static const u8 any6[16] = { 0 }, loop6[16] = { [15] = 1 };   /* ::1 (in6addr_loopback is not in the UWP link set) */
        if (for_connect && !memcmp(&s->sin6_addr, any6, 16)) memcpy(&s->sin6_addr, loop6, 16);
        *outlen = sizeof *s;
        return 0;
    }
    if (fam == L_AF_UNSPEC && for_connect && n->type == L_SOCK_DGRAM) {     /* UDP: dissolve the association */
        if (n->domain == L_AF_INET) { SOCKADDR_IN *s = (SOCKADDR_IN *)out; s->sin_family = AF_INET; *outlen = sizeof *s; }
        else { SOCKADDR_IN6 *s = (SOCKADDR_IN6 *)out; s->sin6_family = AF_INET6; *outlen = sizeof *s; }
        return 0;
    }
    return -NE(EAFNOSUPPORT);
}

/* Winsock sockaddr -> Linux bytes (sockaddr_in 16 / sockaddr_in6 28). */
static u32 sa_to_lx(const SOCKADDR *w, int wlen, u8 out[32])
{
    memset(out, 0, 32);
    if (w->sa_family == AF_INET && wlen >= (int)sizeof(SOCKADDR_IN)) {
        const SOCKADDR_IN *s = (const SOCKADDR_IN *)w;
        u16 fam = L_AF_INET;
        memcpy(out, &fam, 2); memcpy(out + 2, &s->sin_port, 2); memcpy(out + 4, &s->sin_addr, 4);
        return 16;
    }
    if (w->sa_family == AF_INET6 && wlen >= (int)sizeof(SOCKADDR_IN6)) {
        const SOCKADDR_IN6 *s = (const SOCKADDR_IN6 *)w;
        u16 fam = L_AF_INET6;
        memcpy(out, &fam, 2); memcpy(out + 2, &s->sin6_port, 2); memcpy(out + 4, &s->sin6_flowinfo, 4);
        memcpy(out + 8, &s->sin6_addr, 16); memcpy(out + 24, &s->sin6_scope_id, 4);
        return 28;
    }
    return 0;
}

/* Copy a Linux sockaddr of `have` bytes to the user buffer (*ulen in/out, Linux truncation rules). */
static slong put_addr(u64 uaddr, u64 ulen, const u8 *lx, u32 have)
{
    if (!uaddr || !ulen) return 0;
    u32 cap;
    if (xrl_cfu(&cap, ulen, 4)) return -NE(EFAULT);
    u32 n = have < cap ? have : cap;
    if (n && xrl_ctu(uaddr, lx, n)) return -NE(EFAULT);
    if (xrl_ctu(ulen, &have, 4)) return -NE(EFAULT);
    return 0;
}

/* ------------------------------------------------------------ socket()  */
static slong new_netsock(SOCKET s, int domain, int type, int proto, int sockflags, int nonblock_inherit)
{
    netsock *n = (netsock *)calloc(1, sizeof *n);
    if (!n) { closesocket(s); return -NE(ENOMEM); }
    n->s = s; n->domain = domain; n->type = type; n->proto = proto;
    if (WSAEventSelect(s, g_net_ev, FD_READ | FD_WRITE | FD_ACCEPT | FD_CONNECT | FD_CLOSE | FD_OOB) == SOCKET_ERROR) {
        slong e = werr(); closesocket(s); free(n); return e;
    }
    xfile *f = xfile_new(&g_net_ops, LX_O_RDWR | (((sockflags & L_SOCK_NONBLOCK) || nonblock_inherit) ? LX_O_NONBLOCK : 0));
    if (!f) { closesocket(s); free(n); return -NE(ENOMEM); }
    f->priv = n;
    f->mode = LX_S_IFSOCK;
    int fd = xrl_fd_install(f, (sockflags & L_SOCK_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

slong xrl_net_socket(int domain, int type, int proto)
{
    if (domain != L_AF_INET && domain != L_AF_INET6) return NOT_NET;
    int t = type & 0xf, flags = type & (L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
    if (type & ~(0xf | L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)) return -NE(EINVAL);
    if (t == L_SOCK_RAW) return -NE(EPERM);
    if (t != L_SOCK_STREAM && t != L_SOCK_DGRAM) return -NE(ESOCKTNOSUPPORT);
    if (!net_up()) return -NE(ENETDOWN);
    int wproto = proto;
    if (!wproto) wproto = t == L_SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    if ((t == L_SOCK_STREAM && wproto != IPPROTO_TCP) || (t == L_SOCK_DGRAM && wproto != IPPROTO_UDP)) return -NE(EPROTONOSUPPORT);
    SOCKET s = WSASocketW(domain == L_AF_INET ? AF_INET : AF_INET6, t == L_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, wproto, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return werr();
    if (domain == L_AF_INET6) { DWORD off = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off); }   /* Linux default: dual-stack */
    return new_netsock(s, domain, t, t == L_SOCK_STREAM ? L_IPPROTO_TCP : L_IPPROTO_UDP, flags, 0);
}

/* --------------------------------------------------------------- waiting */
typedef struct waiter { u64 deadline; int nonblock; } waiter;      /* deadline ~0 = none */

static void wt_init(waiter *w, int timeout_ms, int nonblock)
{
    w->deadline = timeout_ms > 0 ? xrl_mono_ns() + (u64)timeout_ms * 1000000ull : ~0ull;
    w->nonblock = nonblock;
}

/* After a WSAEWOULDBLOCK: sleep until the network thread bumps the epoch. 0 = retry, else -errno.
 * Sleeps in slices so a wake-up Winsock never delivers (a re-arm corner case) only costs latency. */
static slong wt_wait(waiter *w, LONG seen)
{
    s64 left = -1;
    if (w->deadline != ~0ull) {
        u64 now = xrl_mono_ns();
        if (now >= w->deadline) return -NE(EAGAIN);
        left = (s64)(w->deadline - now);
    }
    if (left < 0 || left > 250000000ll) left = 250000000ll;
    if (xrl_io_wait(seen, left) < 0) return -NE(EINTR);   /* any change, as before keyed wake-ups (ipc.c file_keys) */
    return 0;
}

#define CHUNK 65536u

/* ------------------------------------------------------------------ recv */
static int net_diag(void) { static int v = -1; if (v < 0) { char b[4]; v = GetEnvironmentVariableA("XRL_NETDIAG", b, sizeof b) && b[0] == '1'; } return v; }

static slong net_recv(xfile *f, netsock *n, u64 ubuf, u64 len, int flags, SOCKADDR_STORAGE *from, int *fromlen, int *trunc)
{
    int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & L_MSG_DONTWAIT);
    int wflags = ((flags & L_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & L_MSG_OOB) ? MSG_OOB : 0);
    if (trunc) *trunc = 0;
    if (!len) return 0;
    u64 want = len;
    u64 done = 0;
    u32 cap = (u32)(len < CHUNK ? len : CHUNK);
    char *buf = (char *)malloc(cap);
    if (!buf) return -NE(ENOMEM);
    waiter w;
    wt_init(&w, n->rcvtimeo_ms, nonblock);
    slong ret = 0;
    for (;;) {
        LONG seen = xrl_io_epoch;
        u32 ask = (u32)((want - done) < CHUNK ? (want - done) : CHUNK);
        int r, tr = 0;
        if (from) { *fromlen = (int)sizeof(SOCKADDR_STORAGE); r = recvfrom(n->s, buf, (int)ask, wflags, (SOCKADDR *)from, fromlen); }
        else r = recv(n->s, buf, (int)ask, wflags);
        if (net_diag() && n->type == L_SOCK_STREAM) {   /* DIAG (XRL_NETDIAG=1); keeps the last error for the code below */
            int le = WSAGetLastError();
            if (r == 0 || (r == SOCKET_ERROR && le != WSAEWOULDBLOCK))
                LOGW("netdiag: recv sock=%llu ask=%u -> %d err=%d", (unsigned long long)n->s, ask, r, r == SOCKET_ERROR ? le : 0);
            WSASetLastError(le);
        }
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEMSGSIZE) { r = (int)ask; tr = 1; }     /* UDP datagram longer than the buffer: truncated copy */
            else if (e == WSAEWOULDBLOCK) {
                if (done) { ret = (slong)done; break; }
                if (nonblock) { ret = -NE(EAGAIN); break; }
                slong s = wt_wait(&w, seen);
                if (s) { ret = s; break; }
                continue;
            } else { ret = done ? (slong)done : -(slong)wsa_errno(e); break; }
        }
        if (r > 0 && xrl_ctu(ubuf + done, buf, (size_t)r)) { ret = done ? (slong)done : -NE(EFAULT); break; }
        done += (u64)r;
        if (tr && trunc) *trunc = 1;
        /* stream sockets return what is there; MSG_WAITALL keeps going until `len` or EOF */
        if (n->type == L_SOCK_STREAM && (flags & L_MSG_WAITALL) && r > 0 && done < want && !(flags & L_MSG_PEEK)) {
            wt_init(&w, n->rcvtimeo_ms, nonblock);
            continue;
        }
        ret = (slong)done;
        break;
    }
    free(buf);
    return ret;
}

/* ------------------------------------------------------------------ send */
static slong net_send(xfile *f, netsock *n, u64 ubuf, u64 len, int flags, const SOCKADDR *to, int tolen)
{
    int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & L_MSG_DONTWAIT);
    int wflags = (flags & L_MSG_OOB) ? MSG_OOB : 0;
    if (n->type == L_SOCK_DGRAM && len > 65507) return -NE(EMSGSIZE);
    u32 cap = (u32)(len < CHUNK ? len : CHUNK);
    char *buf = (char *)malloc(cap ? cap : 1);
    if (!buf) return -NE(ENOMEM);
    u64 done = 0;
    slong ret = 0;
    waiter w;
    wt_init(&w, n->sndtimeo_ms, nonblock);
    int first = 1;
    while (done < len || (first && !len)) {
        first = 0;
        u32 c = (u32)((len - done) < CHUNK ? (len - done) : CHUNK);
        if (c && xrl_cfu(buf, ubuf + done, c)) { ret = done ? (slong)done : -NE(EFAULT); break; }
        int sent = 0, stop = 0;
        for (;;) {
            LONG seen = xrl_io_epoch;
            int r = to ? sendto(n->s, buf, (int)c, wflags, to, tolen) : send(n->s, buf, (int)c, wflags);
            if (r != SOCKET_ERROR) { sent = r; break; }
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                if (nonblock) { ret = done ? (slong)done : -NE(EAGAIN); stop = 1; break; }
                slong s = wt_wait(&w, seen);
                if (s) { ret = done ? (slong)done : s; stop = 1; break; }
                continue;
            }
            int le = wsa_errno(e);
            if (le == NE(ECONNRESET) || le == NE(ECONNABORTED)) le = NE(EPIPE);     /* no SIGPIPE here: the write error is all there is */
            ret = done ? (slong)done : -(slong)le;
            stop = 1;
            break;
        }
        if (stop) { free(buf); return ret; }
        done += (u64)sent;
        if (n->type == L_SOCK_DGRAM) break;
        wt_init(&w, n->sndtimeo_ms, nonblock);
    }
    free(buf);
    return (slong)done;
}

/* ------------------------------------------------------- xfile operations */
static slong net_read(xfile *f, void *b, u64 n, s64 o) { (void)o; return net_recv(f, (netsock *)f->priv, (u64)(uintptr_t)b, n, 0, NULL, NULL, NULL); }
static slong net_write(xfile *f, const void *b, u64 n, s64 o) { (void)o; return net_send(f, (netsock *)f->priv, (u64)(uintptr_t)b, n, 0, NULL, 0); }
static slong net_lseek(xfile *f, s64 o, int wh) { (void)f; (void)o; (void)wh; return -LX_ESPIPE; }
static slong net_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFSOCK | 0777, (u64)(uintptr_t)f, 0); return 0; }

/* nonblocking connect progress: 1 connected, 0 pending, -errno failed */
static int conn_state(netsock *n)
{
    WSAPOLLFD p; p.fd = n->s; p.events = POLLWRNORM; p.revents = 0;
    int pr = WSAPoll(&p, 1, 0);
    if (pr != SOCKET_ERROR && (p.revents & (POLLERR | POLLHUP))) {
        int e = 0, l = sizeof e;
        getsockopt(n->s, SOL_SOCKET, SO_ERROR, (char *)&e, &l);
        n->connecting = 0;
        n->soerr = e ? wsa_errno(e) : NE(ECONNREFUSED);
        int r = n->soerr; n->soerr = 0;                    /* reported through connect() itself */
        return -r;
    }
    if (pr != SOCKET_ERROR && (p.revents & POLLWRNORM)) { n->connecting = 0; n->connected = 1; return 1; }
    /* WSAPoll(POLLWRNORM) is an unreliable connect-completion signal on a socket that is
     * registered with WSAEventSelect (the case here) — most visibly for loopback self-IPC,
     * which the game uses and which otherwise never reports "connected", so it busy-loops on
     * getsockopt(SO_ERROR). getpeername is authoritative: it succeeds once the connect
     * completes and returns WSAENOTCONN while still pending (SO_ERROR then carries a failure). */
    { SOCKADDR_STORAGE sa; int sl = sizeof sa;
      if (getpeername(n->s, (SOCKADDR *)&sa, &sl) == 0) { n->connecting = 0; n->connected = 1; return 1; }
      int ge = WSAGetLastError();
      if (ge == WSAENOTCONN) {
          int e = 0, l = sizeof e;
          getsockopt(n->s, SOL_SOCKET, SO_ERROR, (char *)&e, &l);
          if (e) { n->connecting = 0; n->soerr = wsa_errno(e); int r = n->soerr; n->soerr = 0; return -r; }
          return 0;   /* genuinely still pending */
      }
      if (pr == SOCKET_ERROR) { n->connecting = 0; n->soerr = wsa_errno(ge); int r = n->soerr; n->soerr = 0; return -r; } }
    return 0;
}

static u32 net_poll(xfile *f, u32 ev)
{
    netsock *n = (netsock *)f->priv;
    u32 r = 0;
    if (n->connecting) {
        int st = conn_state(n);
        if (st < 0) { n->soerr = -st; return LX_POLLERR | LX_POLLHUP | LX_POLLOUT; }
    }
    WSAPOLLFD p; p.fd = n->s; p.events = 0; p.revents = 0;
    if (ev & (LX_POLLIN | LX_POLLRDNORM)) p.events |= POLLRDNORM;
    if (ev & (LX_POLLOUT | LX_POLLWRNORM)) p.events |= POLLWRNORM;
    if (ev & LX_POLLPRI) p.events |= POLLRDBAND;
    if (n->connecting && !(p.events & POLLWRNORM)) { /* nothing is ready until the connect completes */ }
    int wpr = WSAPoll(&p, 1, 0), wpe = wpr == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (net_diag() && (wpr == SOCKET_ERROR || (p.revents & (POLLERR | POLLNVAL)) || n->soerr))   /* DIAG (XRL_NETDIAG=1) */
        LOGW("netdiag: poll sock=%llu ev=%x -> wsapoll=%d err=%d revents=%x soerr=%d connecting=%d connected=%d",
             (unsigned long long)n->s, ev, wpr, wpe, p.revents, n->soerr, n->connecting, n->connected);
    if (wpr != SOCKET_ERROR) {
        if (p.revents & POLLRDNORM) r |= LX_POLLIN | LX_POLLRDNORM;
        if (p.revents & POLLWRNORM) r |= LX_POLLOUT | LX_POLLWRNORM;
        if (p.revents & POLLRDBAND) r |= LX_POLLPRI;
        if (p.revents & POLLHUP) r |= LX_POLLHUP | LX_POLLIN;
        if (p.revents & POLLERR) r |= LX_POLLERR;
        if (p.revents & POLLNVAL) r |= LX_POLLNVAL;
    } else r |= LX_POLLERR;
    if (n->soerr) r |= LX_POLLERR;
    return r;
}

static slong net_ioctl(xfile *f, u64 req, u64 arg)
{
    netsock *n = (netsock *)f->priv;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) {
        int on = 0;
        if (xrl_cfu(&on, arg, 4)) return -NE(EFAULT);
        if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK;
        return 0;
    }
    if (req == LX_FIONREAD) {
        u_long v = 0;
        if (ioctlsocket(n->s, FIONREAD, &v)) return werr();
        int i = (int)v;
        return xrl_ctu(arg, &i, 4) ? -NE(EFAULT) : 0;
    }
    if (req == 0x5411 /*TIOCOUTQ = SIOCOUTQ*/) { int z = 0; return xrl_ctu(arg, &z, 4) ? -NE(EFAULT) : 0; }
    if (req == 0x8905 /*SIOCATMARK*/) {
        u_long v = 0;
        if (ioctlsocket(n->s, SIOCATMARK, &v)) return werr();
        int i = (int)v;
        return xrl_ctu(arg, &i, 4) ? -NE(EFAULT) : 0;
    }
    return -LX_ENOTTY;
}

static void net_close(xfile *f)
{
    netsock *n = (netsock *)f->priv;
    if (n && net_diag()) { xrl_thread *t = xrl_cur();   /* DIAG: who really closes a socket */
        LOGW("netdiag: closesocket sock=%llu by tid %d task=%d connected=%d", (unsigned long long)n->s, t ? t->tid : -1, t && t->task ? 1 : 0, n->connected); }
    if (n) { closesocket(n->s); free(n); }
}

static const xfile_ops g_net_ops = { "socket", net_read, net_write, net_lseek, net_fstat, NULL, net_ioctl, NULL, NULL, net_poll, net_close, NULL };

/* ------------------------------------------------- bind / listen / accept */
#define WITH_NET(fdv, fvar, nvar) \
    xfile *fvar = xrl_fd_get(fdv); \
    if (!fvar) return -NE(EBADF); \
    netsock *nvar = ns_of(fvar); \
    if (!nvar) { xfile_unref(fvar); return NOT_NET; }

static slong user_sockaddr(netsock *n, u64 uaddr, u64 ulen, SOCKADDR_STORAGE *sa, int *salen, int for_connect)
{
    u8 raw[128];
    if (ulen > sizeof raw) return -NE(EINVAL);
    if (!uaddr) return -NE(EFAULT);
    if (xrl_cfu(raw, uaddr, (size_t)ulen)) return -NE(EFAULT);
    return sa_to_win(n, raw, (u32)ulen, sa, salen, for_connect);
}

slong xrl_net_bind(int fd, u64 uaddr, u64 ulen)
{
    WITH_NET(fd, f, n);
    SOCKADDR_STORAGE sa; int sl = 0;
    slong r = user_sockaddr(n, uaddr, ulen, &sa, &sl, 0);
    if (!r && bind(n->s, (SOCKADDR *)&sa, sl) == SOCKET_ERROR) r = werr();
    if (!r) n->bound = 1;
    xfile_unref(f);
    return r;
}

slong xrl_net_listen(int fd, int backlog)
{
    WITH_NET(fd, f, n);
    slong r = 0;
    if (n->type != L_SOCK_STREAM) r = -NE(EOPNOTSUPP);
    else if (listen(n->s, backlog <= 0 ? 1 : backlog > 4096 ? SOMAXCONN : backlog) == SOCKET_ERROR) r = werr();
    else n->listening = 1;
    xfile_unref(f);
    return r;
}

slong xrl_net_accept(int fd, u64 uaddr, u64 ulen, int flags)
{
    WITH_NET(fd, f, n);
    slong ret;
    if (flags & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)) { xfile_unref(f); return -NE(EINVAL); }
    if (!n->listening) { xfile_unref(f); return -NE(EINVAL); }
    waiter w;
    wt_init(&w, 0, (f->flags & LX_O_NONBLOCK) != 0);
    for (;;) {
        LONG seen = xrl_io_epoch;
        SOCKADDR_STORAGE sa; int sl = (int)sizeof sa;
        SOCKET c = accept(n->s, (SOCKADDR *)&sa, &sl);
        if (c != INVALID_SOCKET) {
            u8 lx[32];
            u32 have = sa_to_lx((SOCKADDR *)&sa, sl, lx);
            ret = new_netsock(c, n->domain, n->type, n->proto, flags, 0);
            if (ret >= 0 && uaddr) { slong pr = put_addr(uaddr, ulen, lx, have); if (pr) { xrl_fd_close((int)ret); ret = pr; } }
            break;
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) { ret = -(slong)wsa_errno(e); break; }
        if (w.nonblock) { ret = -NE(EAGAIN); break; }
        slong s = wt_wait(&w, seen);
        if (s) { ret = s; break; }
    }
    xfile_unref(f);
    return ret;
}

slong xrl_net_connect(int fd, u64 uaddr, u64 ulen)
{
    WITH_NET(fd, f, n);
    slong ret;
    int nonblock = (f->flags & LX_O_NONBLOCK) != 0;
    waiter w;
    wt_init(&w, 0, nonblock);
    if (!n->connecting) {
        SOCKADDR_STORAGE sa; int sl = 0;
        if (n->listening) { xfile_unref(f); return -NE(EISCONN); }
        ret = user_sockaddr(n, uaddr, ulen, &sa, &sl, 1);
        if (ret) { xfile_unref(f); return ret; }
        /* Windows retransmits the SYN to a refusing loopback port (about 2 s before ECONNREFUSED); Linux answers
         * at once. Loopback never loses a packet, so retransmission is switched off for loopback peers only. */
        if (n->type == L_SOCK_STREAM && ((sa.ss_family == AF_INET && (ntohl(((SOCKADDR_IN *)&sa)->sin_addr.s_addr) >> 24) == 127) ||
                                         (sa.ss_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((SOCKADDR_IN6 *)&sa)->sin6_addr)))) {
            xrl_rto_params rto;
            DWORD got;
            rto.Rtt = XRL_RTO_UNSPECIFIED_RTT;
            rto.MaxSynRetransmissions = XRL_RTO_NO_SYN_RETRANSMISSIONS;
            WSAIoctl(n->s, XRL_SIO_TCP_INITIAL_RTO_PARAMETERS, &rto, sizeof rto, NULL, 0, &got, NULL, NULL);
        }
        if (connect(n->s, (SOCKADDR *)&sa, sl) == 0) { n->connected = n->type == L_SOCK_STREAM; xfile_unref(f); return 0; }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) { xfile_unref(f); return -(slong)wsa_errno(e); }
        n->connecting = 1;
        if (nonblock) { xfile_unref(f); return -NE(EINPROGRESS); }
    } else {
        int st = conn_state(n);
        if (st == 1) { xfile_unref(f); return -NE(EISCONN); }
        if (st < 0) { xfile_unref(f); return st; }
        if (nonblock) { xfile_unref(f); return -NE(EALREADY); }
    }
    for (;;) {                                  /* blocking: wait for the connect to finish */
        LONG seen = xrl_io_epoch;
        int st = conn_state(n);
        if (st == 1) { ret = 0; break; }
        if (st < 0) { ret = st; break; }
        slong s = wt_wait(&w, seen);
        if (s) { ret = s == -NE(EAGAIN) ? -NE(EINPROGRESS) : s; break; }
    }
    xfile_unref(f);
    return ret;
}

static slong net_name(int fd, u64 uaddr, u64 ulen, int peer)
{
    WITH_NET(fd, f, n);
    SOCKADDR_STORAGE sa; int sl = (int)sizeof sa;
    slong r = 0;
    u8 lx[32];
    u32 have = 0;
    if ((peer ? getpeername(n->s, (SOCKADDR *)&sa, &sl) : getsockname(n->s, (SOCKADDR *)&sa, &sl)) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (peer || e != WSAEINVAL) r = -(slong)wsa_errno(e);
        /* getsockname on an unbound socket: Winsock refuses, Linux returns the family with a zero address */
    } else have = sa_to_lx((SOCKADDR *)&sa, sl, lx);
    if (!r) {
        if (!have) {
            u16 fam = (u16)n->domain;
            memset(lx, 0, sizeof lx);
            memcpy(lx, &fam, 2);
            have = n->domain == L_AF_INET ? 16 : 28;
        }
        r = put_addr(uaddr, ulen, lx, have);
    }
    xfile_unref(f);
    return r;
}
slong xrl_net_getsockname(int fd, u64 uaddr, u64 ulen) { return net_name(fd, uaddr, ulen, 0); }
slong xrl_net_getpeername(int fd, u64 uaddr, u64 ulen) { return net_name(fd, uaddr, ulen, 1); }

slong xrl_net_shutdown(int fd, int how)
{
    WITH_NET(fd, f, n);
    slong r = 0;
    if (how < 0 || how > 2) r = -NE(EINVAL);
    else if (shutdown(n->s, how) == SOCKET_ERROR) r = werr();
    xfile_unref(f);
    return r;
}

/* ----------------------------------------------- sendto / recvfrom / msg */
slong xrl_net_sendto(int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 ulen)
{
    WITH_NET(fd, f, n);
    SOCKADDR_STORAGE sa; int sl = 0;
    slong r = 0;
    if (uaddr) r = user_sockaddr(n, uaddr, ulen, &sa, &sl, 1);
    if (!r) r = net_send(f, n, ubuf, len, flags, uaddr ? (SOCKADDR *)&sa : NULL, sl);
    xfile_unref(f);
    return r;
}

slong xrl_net_recvfrom(int fd, u64 ubuf, u64 len, int flags, u64 uaddr, u64 ulen)
{
    WITH_NET(fd, f, n);
    SOCKADDR_STORAGE sa; int sl = 0;
    slong r = net_recv(f, n, ubuf, len, flags, uaddr ? &sa : NULL, &sl, NULL);
    if (r >= 0 && uaddr) {
        u8 lx[32];
        u32 have = sa_to_lx((SOCKADDR *)&sa, sl, lx);
        slong pr = put_addr(uaddr, ulen, lx, have);
        if (pr) r = pr;
    }
    xfile_unref(f);
    return r;
}

static slong msg_send(xfile *f, netsock *n, u64 umsg, int flags)
{
    lx_msghdr m;
    if (xrl_cfu(&m, umsg, sizeof m)) return -NE(EFAULT);
    if (m.msg_iovlen > 1024) return -NE(EMSGSIZE);
    net_iovec iov[64];
    u64 cnt = m.msg_iovlen > 64 ? 64 : m.msg_iovlen;
    if (cnt && xrl_cfu(iov, m.msg_iov, (size_t)(cnt * sizeof iov[0]))) return -NE(EFAULT);
    SOCKADDR_STORAGE sa; int sl = 0;
    if (m.msg_name && m.msg_namelen) {
        slong r = user_sockaddr(n, m.msg_name, m.msg_namelen, &sa, &sl, 1);
        if (r) return r;
    }
    const SOCKADDR *to = (m.msg_name && m.msg_namelen) ? (const SOCKADDR *)&sa : NULL;
    if (n->type == L_SOCK_DGRAM) {              /* one datagram: gather the iovecs */
        u64 total = 0;
        for (u64 i = 0; i < cnt; i++) total += iov[i].len;
        if (total > 65507) return -NE(EMSGSIZE);
        char *g = (char *)malloc(total ? (size_t)total : 1);
        if (!g) return -NE(ENOMEM);
        u64 off = 0;
        for (u64 i = 0; i < cnt; i++) {
            if (iov[i].len && xrl_cfu(g + off, iov[i].base, (size_t)iov[i].len)) { free(g); return -NE(EFAULT); }
            off += iov[i].len;
        }
        /* the data now lives in host memory: send it through the same path with a temporary user mapping */
        int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & L_MSG_DONTWAIT);
        waiter w; wt_init(&w, n->sndtimeo_ms, nonblock);
        slong ret;
        for (;;) {
            LONG seen = xrl_io_epoch;
            int r = to ? sendto(n->s, g, (int)total, (flags & L_MSG_OOB) ? MSG_OOB : 0, to, sl) : send(n->s, g, (int)total, (flags & L_MSG_OOB) ? MSG_OOB : 0);
            if (r != SOCKET_ERROR) { ret = r; break; }
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) { ret = -(slong)wsa_errno(e); break; }
            if (nonblock) { ret = -NE(EAGAIN); break; }
            slong s = wt_wait(&w, seen);
            if (s) { ret = s; break; }
        }
        free(g);
        return ret;
    }
    slong total = 0;
    for (u64 i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        slong r = net_send(f, n, iov[i].base, iov[i].len, flags, NULL, 0);
        if (r < 0) return total ? total : r;
        total += r;
        if ((u64)r < iov[i].len) break;
    }
    return total;
}

static slong msg_recv(xfile *f, netsock *n, u64 umsg, int flags)
{
    lx_msghdr m;
    if (xrl_cfu(&m, umsg, sizeof m)) return -NE(EFAULT);
    if (m.msg_iovlen > 1024) return -NE(EMSGSIZE);
    net_iovec iov[64];
    u64 cnt = m.msg_iovlen > 64 ? 64 : m.msg_iovlen;
    if (cnt && xrl_cfu(iov, m.msg_iov, (size_t)(cnt * sizeof iov[0]))) return -NE(EFAULT);
    SOCKADDR_STORAGE sa; int sl = 0, trunc = 0;
    int want_name = m.msg_name && m.msg_namelen;
    slong total = 0;
    if (n->type == L_SOCK_DGRAM) {              /* one datagram scattered over the iovecs */
        u64 cap = 0;
        for (u64 i = 0; i < cnt; i++) cap += iov[i].len;
        /* receive into the first iovec only when it is big enough; otherwise bounce through the iov chain below */
        if (cnt == 1 || cap == 0) {
            total = net_recv(f, n, cnt ? iov[0].base : 0, cnt ? iov[0].len : 0, flags, want_name ? &sa : NULL, &sl, &trunc);
        } else {
            char *bb = (char *)malloc((size_t)(cap > CHUNK ? CHUNK : cap));
            if (!bb) return -NE(ENOMEM);
            int nonblock = (f->flags & LX_O_NONBLOCK) || (flags & L_MSG_DONTWAIT);
            waiter w; wt_init(&w, n->rcvtimeo_ms, nonblock);
            u32 ask = (u32)(cap > CHUNK ? CHUNK : cap);
            for (;;) {
                LONG seen = xrl_io_epoch;
                int fl = (flags & L_MSG_PEEK) ? MSG_PEEK : 0, r;
                sl = (int)sizeof sa;
                r = recvfrom(n->s, bb, (int)ask, fl, (SOCKADDR *)&sa, &sl);
                if (r == SOCKET_ERROR) {
                    int e = WSAGetLastError();
                    if (e == WSAEMSGSIZE) { r = (int)ask; trunc = 1; }
                    else if (e == WSAEWOULDBLOCK) {
                        if (nonblock) { total = -NE(EAGAIN); break; }
                        slong s = wt_wait(&w, seen);
                        if (s) { total = s; break; }
                        continue;
                    } else { total = -(slong)wsa_errno(e); break; }
                }
                u64 off = 0;
                for (u64 i = 0; i < cnt && off < (u64)r; i++) {
                    u64 c = iov[i].len < (u64)r - off ? iov[i].len : (u64)r - off;
                    if (c && xrl_ctu(iov[i].base, bb + off, (size_t)c)) { total = -NE(EFAULT); break; }
                    off += c;
                }
                if (total == 0) total = r;
                break;
            }
            free(bb);
        }
    } else {
        for (u64 i = 0; i < cnt; i++) {
            if (!iov[i].len) continue;
            slong r = net_recv(f, n, iov[i].base, iov[i].len, i ? (flags | L_MSG_DONTWAIT) : flags, NULL, NULL, NULL);
            if (r < 0) { if (!total) total = r; break; }
            total += r;
            if ((u64)r < iov[i].len) break;
        }
    }
    if (total >= 0) {
        u32 outlen = 0;
        if (want_name && total >= 0 && n->type == L_SOCK_DGRAM) {
            u8 lx[32];
            u32 have = sa_to_lx((SOCKADDR *)&sa, sl, lx);
            u32 c = have < m.msg_namelen ? have : m.msg_namelen;
            if (c && xrl_ctu(m.msg_name, lx, c)) return -NE(EFAULT);
            outlen = have;
        }
        m.msg_namelen = outlen;
        m.msg_controllen = 0;
        m.msg_flags = trunc ? L_MSG_TRUNC : 0;
        if (xrl_ctu(umsg, &m, sizeof m)) return -NE(EFAULT);
    }
    return total;
}

slong xrl_net_sendmsg(int fd, u64 umsg, int flags) { WITH_NET(fd, f, n); slong r = msg_send(f, n, umsg, flags); xfile_unref(f); return r; }
slong xrl_net_recvmsg(int fd, u64 umsg, int flags) { WITH_NET(fd, f, n); slong r = msg_recv(f, n, umsg, flags); xfile_unref(f); return r; }

/* sendmmsg / recvmmsg: struct mmsghdr = msghdr (56 bytes) + u32 msg_len + 4 pad */
slong xrl_net_sendmmsg(int fd, u64 uvec, u32 vlen, int flags)
{
    WITH_NET(fd, f, n);
    slong done = 0, r = 0;
    if (vlen > 1024) vlen = 1024;
    for (u32 i = 0; i < vlen; i++) {
        r = msg_send(f, n, uvec + (u64)i * 64, flags);
        if (r < 0) break;
        u32 l = (u32)r;
        if (xrl_ctu(uvec + (u64)i * 64 + 56, &l, 4)) { r = -NE(EFAULT); break; }
        done++;
    }
    xfile_unref(f);
    return done ? done : r;
}

slong xrl_net_recvmmsg(int fd, u64 uvec, u32 vlen, int flags)
{
    WITH_NET(fd, f, n);
    slong done = 0, r = 0;
    if (vlen > 1024) vlen = 1024;
    for (u32 i = 0; i < vlen; i++) {
        r = msg_recv(f, n, uvec + (u64)i * 64, i ? (flags | L_MSG_DONTWAIT) : flags);
        if (r < 0) break;
        u32 l = (u32)r;
        if (xrl_ctu(uvec + (u64)i * 64 + 56, &l, 4)) { r = -NE(EFAULT); break; }
        done++;
    }
    xfile_unref(f);
    return done ? done : (r == -NE(EAGAIN) && done ? done : r);
}

/* -------------------------------------------------------- socket options */
/* Linux option numbers (level SOL_SOCKET) */
enum { SO_DEBUG_L = 1, SO_REUSEADDR_L = 2, SO_TYPE_L = 3, SO_ERROR_L = 4, SO_DONTROUTE_L = 5, SO_BROADCAST_L = 6, SO_SNDBUF_L = 7,
       SO_RCVBUF_L = 8, SO_KEEPALIVE_L = 9, SO_OOBINLINE_L = 10, SO_NO_CHECK_L = 11, SO_PRIORITY_L = 12, SO_LINGER_L = 13,
       SO_REUSEPORT_L = 15, SO_PASSCRED_L = 16, SO_RCVLOWAT_L = 18, SO_RCVTIMEO_L = 20, SO_SNDTIMEO_L = 21, SO_BINDTODEVICE_L = 25,
       SO_ATTACH_FILTER_L = 26, SO_TIMESTAMP_L = 29, SO_ACCEPTCONN_L = 30, SO_SNDBUFFORCE_L = 32, SO_RCVBUFFORCE_L = 33,
       SO_MARK_L = 36, SO_PROTOCOL_L = 38, SO_DOMAIN_L = 39, SO_BUSY_POLL_L = 46, SO_INCOMING_CPU_L = 49,
       SO_RCVTIMEO_NEW_L = 66, SO_SNDTIMEO_NEW_L = 67 };

typedef struct optmap { int lx, win_level, win; } optmap;

static int map_opt(int level, int opt, optmap *m)
{
    static const optmap sol[] = {
        { SO_BROADCAST_L, SOL_SOCKET, SO_BROADCAST }, { SO_KEEPALIVE_L, SOL_SOCKET, SO_KEEPALIVE }, { SO_OOBINLINE_L, SOL_SOCKET, SO_OOBINLINE },
        { SO_DONTROUTE_L, SOL_SOCKET, SO_DONTROUTE }, { SO_SNDBUF_L, SOL_SOCKET, SO_SNDBUF }, { SO_RCVBUF_L, SOL_SOCKET, SO_RCVBUF },
        { SO_SNDBUFFORCE_L, SOL_SOCKET, SO_SNDBUF }, { SO_RCVBUFFORCE_L, SOL_SOCKET, SO_RCVBUF },
    };
    static const optmap tcp[] = {
        { 1, IPPROTO_TCP, 1 /*TCP_NODELAY*/ }, { 4, IPPROTO_TCP, 3 /*TCP_KEEPIDLE*/ }, { 5, IPPROTO_TCP, 17 /*TCP_KEEPINTVL*/ },
        { 6, IPPROTO_TCP, 16 /*TCP_KEEPCNT*/ }, { 2, IPPROTO_TCP, 4 /*TCP_MAXSEG*/ },
    };
    static const optmap ip4[] = {
        { 1, IPPROTO_IP, 3 /*IP_TOS*/ }, { 2, IPPROTO_IP, 4 /*IP_TTL*/ }, { 32, IPPROTO_IP, 9 /*IP_MULTICAST_IF*/ },
        { 33, IPPROTO_IP, 10 /*IP_MULTICAST_TTL*/ }, { 34, IPPROTO_IP, 11 /*IP_MULTICAST_LOOP*/ },
        { 35, IPPROTO_IP, 12 /*IP_ADD_MEMBERSHIP*/ }, { 36, IPPROTO_IP, 13 /*IP_DROP_MEMBERSHIP*/ }, { 8, IPPROTO_IP, 19 /*IP_PKTINFO*/ },
    };
    static const optmap ip6[] = {
        { 26, IPPROTO_IPV6, 27 /*IPV6_V6ONLY*/ }, { 16, IPPROTO_IPV6, 4 /*IPV6_UNICAST_HOPS*/ }, { 17, IPPROTO_IPV6, 9 /*IPV6_MULTICAST_IF*/ },
        { 18, IPPROTO_IPV6, 10 /*IPV6_MULTICAST_HOPS*/ }, { 19, IPPROTO_IPV6, 11 /*IPV6_MULTICAST_LOOP*/ },
        { 20, IPPROTO_IPV6, 12 /*IPV6_JOIN_GROUP*/ }, { 21, IPPROTO_IPV6, 13 /*IPV6_LEAVE_GROUP*/ }, { 49, IPPROTO_IPV6, 49 /*IPV6_RECVPKTINFO*/ },
        { 67, IPPROTO_IPV6, 39 /*IPV6_TCLASS*/ },
    };
    const optmap *t; size_t cnt;
    switch (level) {
    case L_SOL_SOCKET: t = sol; cnt = sizeof sol / sizeof sol[0]; break;
    case L_IPPROTO_TCP: t = tcp; cnt = sizeof tcp / sizeof tcp[0]; break;
    case L_IPPROTO_IP: t = ip4; cnt = sizeof ip4 / sizeof ip4[0]; break;
    case L_IPPROTO_IPV6: t = ip6; cnt = sizeof ip6 / sizeof ip6[0]; break;
    default: return 0;
    }
    for (size_t i = 0; i < cnt; i++) if (t[i].lx == opt) { *m = t[i]; return 1; }
    return 0;
}

/* options Linux accepts (and mostly ignores) that have no Winsock counterpart */
static int ignorable_opt(int level, int opt)
{
    if (level == L_SOL_SOCKET)
        return opt == SO_DEBUG_L || opt == SO_NO_CHECK_L || opt == SO_PRIORITY_L || opt == SO_PASSCRED_L || opt == SO_RCVLOWAT_L ||
               opt == SO_BINDTODEVICE_L || opt == SO_ATTACH_FILTER_L || opt == SO_TIMESTAMP_L || opt == SO_MARK_L || opt == SO_BUSY_POLL_L ||
               opt == SO_INCOMING_CPU_L || opt == 35 /*SO_TIMESTAMPNS*/ || opt == 37 /*SO_TIMESTAMPING*/ || opt == 28 /*SO_RXQ_OVFL*/;
    if (level == L_IPPROTO_TCP)
        return opt == 3 /*CORK*/ || opt == 7 /*SYNCNT*/ || opt == 9 /*DEFER_ACCEPT*/ || opt == 12 /*QUICKACK*/ || opt == 13 /*CONGESTION*/ || opt == 18 /*USER_TIMEOUT*/ ||
               opt == 23 /*FASTOPEN*/ || opt == 19 /*THIN_LINEAR_TIMEOUTS*/ || opt == 20 /*THIN_DUPACK*/;
    if (level == L_IPPROTO_IP)
        return opt == 10 /*MTU_DISCOVER*/ || opt == 11 /*RECVERR*/ || opt == 12 /*RECVTTL*/ || opt == 13 /*RECVTOS*/ || opt == 15 /*FREEBIND*/ ||
               opt == 19 /*TRANSPARENT*/ || opt == 14 /*MTU*/ || opt == 6 /*RECVOPTS*/;
    if (level == L_IPPROTO_IPV6)
        return opt == 23 /*RECVERR*/ || opt == 66 /*RECVTCLASS*/ || opt == 62 /*RECVHOPLIMIT*/ || opt == 23 || opt == 24 /*MTU_DISCOVER*/;
    return 0;
}

static int tv_to_ms(const u8 *tv)
{
    s64 sec, usec;
    memcpy(&sec, tv, 8); memcpy(&usec, tv + 8, 8);
    if (sec < 0 || usec < 0) return -1;
    s64 ms = sec * 1000 + (usec + 999) / 1000;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    return (int)ms;
}

slong xrl_net_setsockopt(int fd, int level, int opt, u64 uval, u64 ulen)
{
    WITH_NET(fd, f, n);
    u8 v[64];
    slong r = 0;
    if (ulen > sizeof v) ulen = sizeof v;
    if (uval && ulen && xrl_cfu(v, uval, (size_t)ulen)) { xfile_unref(f); return -NE(EFAULT); }
    int ival = 0;
    if (ulen >= 4) memcpy(&ival, v, 4);
    else if (ulen >= 1) ival = v[0];
    optmap m;
    if (level == L_SOL_SOCKET) {
        switch (opt) {
        case SO_REUSEADDR_L: n->reuseaddr = ival != 0; goto reuse;
        case SO_REUSEPORT_L: n->reuseport = ival != 0;
        reuse:
            /* UDP: several sockets sharing a port needs Winsock's SO_REUSEADDR. TCP: Linux' REUSEADDR only
             * lets a listener re-bind over TIME_WAIT, which Winsock allows anyway; its REUSEADDR would let
             * another process steal the port, so it is not forwarded. */
            if (n->type == L_SOCK_DGRAM) { DWORD d = (n->reuseaddr || n->reuseport) ? 1 : 0; setsockopt(n->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&d, sizeof d); }
            goto done;
        case SO_LINGER_L: {
            if (ulen < 8) { r = -NE(EINVAL); goto done; }
            LINGER l; int a, b; memcpy(&a, v, 4); memcpy(&b, v + 4, 4);
            l.l_onoff = (u_short)(a != 0); l.l_linger = (u_short)b;
            if (setsockopt(n->s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l)) r = werr();
            goto done;
        }
        case SO_RCVTIMEO_L: case SO_RCVTIMEO_NEW_L: case SO_SNDTIMEO_L: case SO_SNDTIMEO_NEW_L: {
            if (ulen < 16) { r = -NE(EINVAL); goto done; }
            int ms = tv_to_ms(v);
            if (ms < 0) { r = -NE(EINVAL); goto done; }
            if (opt == SO_RCVTIMEO_L || opt == SO_RCVTIMEO_NEW_L) n->rcvtimeo_ms = ms; else n->sndtimeo_ms = ms;
            goto done;
        }
        case SO_SNDBUF_L: case SO_RCVBUF_L: case SO_SNDBUFFORCE_L: case SO_RCVBUFFORCE_L: break;
        case SO_TYPE_L: case SO_ERROR_L: case SO_ACCEPTCONN_L: case SO_PROTOCOL_L: case SO_DOMAIN_L: r = -NE(ENOPROTOOPT); goto done;
        default: break;
        }
    }
    if (map_opt(level, opt, &m)) {
        if (level == L_IPPROTO_IPV6 && opt == 26) n->v6only = ival != 0;
        /* Linux takes IP_MULTICAST_TTL/LOOP as int or a byte; Winsock wants a DWORD */
        DWORD d = (DWORD)ival;
        const char *p = (const char *)v; int pl = (int)ulen;
        if (ulen < 4 && ulen >= 1 && (level == L_IPPROTO_IP || level == L_IPPROTO_IPV6)) { p = (const char *)&d; pl = 4; d = v[0]; }
        if (setsockopt(n->s, m.win_level, m.win, p, pl) == SOCKET_ERROR) r = werr();
        goto done;
    }
    if (ignorable_opt(level, opt)) goto done;
    r = -NE(ENOPROTOOPT);
done:
    xfile_unref(f);
    return r;
}

static slong put_opt(u64 uval, u64 ulen, const void *src, u32 have)
{
    u32 cap;
    if (xrl_cfu(&cap, ulen, 4)) return -NE(EFAULT);
    u32 c = have < cap ? have : cap;
    if (c && xrl_ctu(uval, src, c)) return -NE(EFAULT);
    if (xrl_ctu(ulen, &c, 4)) return -NE(EFAULT);
    return 0;
}

slong xrl_net_getsockopt(int fd, int level, int opt, u64 uval, u64 ulen)
{
    WITH_NET(fd, f, n);
    slong r = 0;
    int iv = 0;
    optmap m;
    if (!uval || !ulen) { xfile_unref(f); return -NE(EFAULT); }
    if (level == L_SOL_SOCKET) {
        switch (opt) {
        case SO_TYPE_L: iv = n->type; goto put_int;
        case SO_DOMAIN_L: iv = n->domain; goto put_int;
        case SO_PROTOCOL_L: iv = n->proto; goto put_int;
        case SO_ACCEPTCONN_L: iv = n->listening; goto put_int;
        case SO_REUSEADDR_L: iv = n->reuseaddr; goto put_int;
        case SO_REUSEPORT_L: iv = n->reuseport; goto put_int;
        case SO_ERROR_L: {
            if (n->connecting) { int st = conn_state(n); if (st < 0) n->soerr = -st; }
            iv = n->soerr; n->soerr = 0;
            if (!iv) { int e = 0, l = sizeof e; if (!getsockopt(n->s, SOL_SOCKET, SO_ERROR, (char *)&e, &l)) iv = wsa_errno(e); }
            goto put_int;
        }
        case SO_LINGER_L: {
            LINGER l; int l_len = sizeof l;
            int out[2] = { 0, 0 };
            if (!getsockopt(n->s, SOL_SOCKET, SO_LINGER, (char *)&l, &l_len)) { out[0] = l.l_onoff; out[1] = l.l_linger; }
            r = put_opt(uval, ulen, out, 8);
            goto done;
        }
        case SO_RCVTIMEO_L: case SO_RCVTIMEO_NEW_L: case SO_SNDTIMEO_L: case SO_SNDTIMEO_NEW_L: {
            int ms = (opt == SO_RCVTIMEO_L || opt == SO_RCVTIMEO_NEW_L) ? n->rcvtimeo_ms : n->sndtimeo_ms;
            s64 tv[2] = { ms / 1000, (ms % 1000) * 1000 };
            r = put_opt(uval, ulen, tv, 16);
            goto done;
        }
        default: break;
        }
    }
    if (level == L_IPPROTO_IPV6 && opt == 26) { iv = n->v6only; goto put_int; }
    if (map_opt(level, opt, &m)) {
        DWORD d = 0; int dl = sizeof d;
        if (getsockopt(n->s, m.win_level, m.win, (char *)&d, &dl) == SOCKET_ERROR) { r = werr(); goto done; }
        iv = (int)d;
        goto put_int;
    }
    if (ignorable_opt(level, opt)) { iv = 0; goto put_int; }
    r = -NE(ENOPROTOOPT);
    goto done;
put_int:
    r = put_opt(uval, ulen, &iv, 4);
done:
    xfile_unref(f);
    return r;
}

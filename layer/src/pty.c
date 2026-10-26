/*
 * pty.c - pseudo-terminals: /dev/ptmx (master) and /dev/pts/N (slave), with a Linux-style line discipline.
 *
 * What a terminal emulator (xterm) and a shell need: open /dev/ptmx, TIOCGPTN/TIOCSPTLCK (musl's ptsname/unlockpt),
 * open the slave, termios (canonical or raw, echo, ICRNL, ONLCR), the window size, ^C/^Z/^\ turned into signals
 * for the foreground process group, EOF on ^D, EIO on the master when the slave is gone, HUP on the slave when the
 * master is. Everything is an in-memory object (no per-process state, like unix.c), so the master and the slave
 * can sit in different guest processes' fd tables. The default stdin/stdout/stderr of the layer's guest stay the
 * logging tty of fd.c; this file is only for real pty pairs.
 *
 * Data flow: the master WRITES what the user typed. It passes the input discipline (ICRNL/INLCR/IGNCR, ISIG,
 * canonical line editing with ECHO) into the slave's input queue, echo goes to the master's output queue. The slave
 * WRITES program output through the output discipline (OPOST/ONLCR) into the master's output queue, which the master
 * READS. Not provided: packet mode (TIOCPKT), IXON/IXOFF flow control (^S/^Q are swallowed when IXON is set),
 * parity/break/baud emulation (the speeds are stored), VTIME timing (VMIN>0 && VTIME>0 returns after the first byte),
 * and the controlling-terminal/session bookkeeping, which is proc.c's: `xrl_tty_signal` and TIOCSCTTY/TIOCSPGRP
 * below are the hooks it replaces.
 */
#include "xrl_internal.h"

#define PTY_MAX 4096
#define PTY_BUF 65536
#define PTY_LINE 4096

/* termios */
#define T_ICRNL 0000400
#define T_INLCR 0000100
#define T_IGNCR 0000200
#define T_IXON 0002000
#define T_OPOST 0000001
#define T_ONLCR 0000004
#define T_OCRNL 0000010
#define T_ISIG 0000001
#define T_ICANON 0000002
#define T_ECHO 0000010
#define T_ECHOE 0000020
#define T_ECHOK 0000040
#define T_ECHONL 0000100
#define T_NOFLSH 0000200
#define T_ECHOCTL 0001000
#define T_IEXTEN 0100000
#define V_INTR 0
#define V_QUIT 1
#define V_ERASE 2
#define V_KILL 3
#define V_EOF 4
#define V_TIME 5
#define V_MIN 6
#define V_START 8
#define V_STOP 9
#define V_SUSP 10
#define V_EOL 11
#define V_WERASE 14
#define V_EOL2 16

#define IOC_TCGETS 0x5401
#define IOC_TCSETS 0x5402
#define IOC_TCSETSW 0x5403
#define IOC_TCSETSF 0x5404
#define IOC_TCSBRK 0x5409
#define IOC_TIOCSCTTY 0x540E
#define IOC_TIOCGPGRP 0x540F
#define IOC_TIOCSPGRP 0x5410
#define IOC_TCFLSH 0x540B
#define IOC_TIOCOUTQ 0x5411
#define IOC_TIOCGWINSZ 0x5413
#define IOC_TIOCSWINSZ 0x5414
#define IOC_TIOCNOTTY 0x5422
#define IOC_TIOCGSID 0x5429
#define IOC_TCGETS2 0x802c542a
#define IOC_TCSETS2 0x402c542b
#define IOC_TCSETSW2 0x402c542c
#define IOC_TCSETSF2 0x402c542d
#define IOC_TIOCGPTN 0x80045430
#define IOC_TIOCSPTLCK 0x40045431
#define IOC_TIOCGPTLCK 0x80045439
#define IOC_TIOCGPTPEER 0x5441
#define IOC_FIONREAD 0x541B
#define IOC_FIONBIO 0x5421

typedef struct ktermios { u32 iflag, oflag, cflag, lflag; u8 line; u8 cc[19]; u32 ispeed, ospeed; } ktermios;   /* the kernel's termios + termios2 speeds */
typedef struct winsz { u16 row, col, xpix, ypix; } winsz;

typedef struct ring { u8 *b; size_t head, len; } ring;

typedef struct pty {
    volatile LONG refs;
    int index;
    SRWLOCK lock;
    ring in, out;               /* in: bytes the slave reads; out: bytes the master reads */
    size_t canon;               /* bytes at the front of `in` that are complete lines (canonical mode) */
    int eofs;                   /* ^D typed on an empty line: that many zero-length slave reads */
    u8 line[PTY_LINE]; int linelen;    /* the line being edited */
    ktermios t;
    winsz ws;
    int locked, master_open, slave_opens, slave_ever;
    int pgrp, sid;
    u32 owner_uid;
} pty;

static SRWLOCK g_pty_lock = SRWLOCK_INIT;   /* the table */
static pty *g_pty[PTY_MAX];

/* proc.c replaces this: signal a process group (the line discipline's ^C/^Z/^\ and SIGHUP). */
int xrl_tty_signal_default(int pgrp, int sig)
{
    static volatile LONG once;
    if (!InterlockedExchange(&once, 1)) LOGW("pty: signal %d for process group %d dropped (no process model: xrl_tty_signal is proc.c's)", sig, pgrp);
    return 0;
}
int xrl_tty_signal(int pgrp, int sig);
#pragma comment(linker, "/alternatename:xrl_tty_signal=xrl_tty_signal_default")

static const xfile_ops g_pm_ops, g_ps_ops;

/* ------------------------------------------------------------------ ring */
static int ring_put(ring *r, u8 c)   /* 0 = full */
{
    if (r->len == PTY_BUF) return 0;
    r->b[(r->head + r->len) % PTY_BUF] = c;
    r->len++;
    return 1;
}
static size_t ring_take(ring *r, u8 *dst, size_t n)
{
    size_t k = r->len < n ? r->len : n;
    for (size_t i = 0; i < k; i++) dst[i] = r->b[(r->head + i) % PTY_BUF];
    r->head = (r->head + k) % PTY_BUF; r->len -= k;
    return k;
}

static pty *pty_new(int index)
{
    pty *p = (pty *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->in.b = (u8 *)malloc(PTY_BUF); p->out.b = (u8 *)malloc(PTY_BUF);
    if (!p->in.b || !p->out.b) { free(p->in.b); free(p->out.b); free(p); return NULL; }
    p->refs = 1; p->index = index;
    InitializeSRWLock(&p->lock);
    p->t = (ktermios){ T_ICRNL | T_IXON, T_OPOST | T_ONLCR, 0000060 | 0000200 | 0002000 | 0000017 /* CS8|CREAD|HUPCL|B38400 */,
                       0x8a3b /* ISIG|ICANON|ECHO|ECHOE|ECHOK|ECHOCTL|ECHOKE|IEXTEN */, 0, { 3, 034, 0177, 025, 04, 0, 1, 0, 021, 023, 032, 0, 022, 017, 027, 026, 0 }, 38400, 38400 };
    p->ws = (winsz){ 24, 80, 0, 0 };
    p->locked = 1;
    p->owner_uid = xrl_cur_uid();
    return p;
}

static void pty_ref(pty *p) { InterlockedIncrement(&p->refs); }
static void pty_unref(pty *p)
{
    if (InterlockedDecrement(&p->refs) == 0) { free(p->in.b); free(p->out.b); free(p); }
}

static void pty_release_index(pty *p)   /* both sides closed */
{
    AcquireSRWLockExclusive(&g_pty_lock);
    if (g_pty[p->index] == p) g_pty[p->index] = NULL;
    ReleaseSRWLockExclusive(&g_pty_lock);
    pty_unref(p);   /* the table's reference */
}

/* ------------------------------------------------------ line discipline */
static void out_char(pty *p, u8 c)   /* slave -> master with the output discipline; lock held; drops on a full queue */
{
    if ((p->t.oflag & T_OPOST) && c == '\n' && (p->t.oflag & T_ONLCR)) ring_put(&p->out, '\r');
    else if ((p->t.oflag & T_OPOST) && c == '\r' && (p->t.oflag & T_OCRNL)) c = '\n';
    ring_put(&p->out, c);
}

static void echo_char(pty *p, u8 c)
{
    if (!(p->t.lflag & T_ECHO)) { if (c == '\n' && (p->t.lflag & T_ECHONL) && (p->t.lflag & T_ICANON)) out_char(p, '\n'); return; }
    if (c == '\n') { out_char(p, '\n'); return; }
    if (c < 32 && c != '\t' && (p->t.lflag & T_ECHOCTL)) { ring_put(&p->out, '^'); ring_put(&p->out, (u8)(c + 64)); return; }
    if (c == 127 && (p->t.lflag & T_ECHOCTL)) { ring_put(&p->out, '^'); ring_put(&p->out, '?'); return; }
    ring_put(&p->out, c);
}

static void flush_input(pty *p) { p->in.len = 0; p->in.head = 0; p->canon = 0; p->linelen = 0; p->eofs = 0; }

/* One byte typed at the master. Returns the signal to raise (0 = none); lock held. */
static int in_char(pty *p, u8 c)
{
    u32 ifl = p->t.iflag, lfl = p->t.lflag;
    const u8 *cc = p->t.cc;
    if (c == '\r') { if (ifl & T_IGNCR) return 0; if (ifl & T_ICRNL) c = '\n'; }
    else if (c == '\n' && (ifl & T_INLCR)) c = '\r';
    if (lfl & T_ISIG) {
        int sig = 0;
        if (c == cc[V_INTR] && c) sig = LX_SIGINT; else if (c == cc[V_QUIT] && c) sig = LX_SIGQUIT; else if (c == cc[V_SUSP] && c) sig = 20 /*SIGTSTP*/;
        if (sig) {
            if (!(lfl & T_NOFLSH)) flush_input(p);
            if (lfl & T_ECHO) { if (c < 32 && (lfl & T_ECHOCTL)) { ring_put(&p->out, '^'); ring_put(&p->out, (u8)(c + 64)); } }
            return sig;
        }
    }
    if ((ifl & T_IXON) && (c == cc[V_START] || c == cc[V_STOP]) && c) return 0;   /* flow control: swallowed */
    if (!(lfl & T_ICANON)) { echo_char(p, c); ring_put(&p->in, c); return 0; }
    /* canonical: edit the line until a newline / EOL / EOF completes it */
    if (c == cc[V_EOF] && c) {
        if (p->linelen) { for (int i = 0; i < p->linelen; i++) ring_put(&p->in, p->line[i]); p->canon += (size_t)p->linelen; p->linelen = 0; }
        else p->eofs++;
        return 0;
    }
    if ((c == cc[V_ERASE] || c == 8) && c) {
        if (p->linelen) { p->linelen--; if (lfl & T_ECHOE) { ring_put(&p->out, '\b'); ring_put(&p->out, ' '); ring_put(&p->out, '\b'); } }
        return 0;
    }
    if (c == cc[V_KILL] && c) {
        if (lfl & T_ECHOK) for (int i = 0; i < p->linelen; i++) { ring_put(&p->out, '\b'); ring_put(&p->out, ' '); ring_put(&p->out, '\b'); }
        p->linelen = 0;
        return 0;
    }
    if (c == cc[V_WERASE] && c && (lfl & T_IEXTEN)) {
        while (p->linelen && p->line[p->linelen - 1] == ' ') { p->linelen--; if (lfl & T_ECHOE) { ring_put(&p->out, '\b'); ring_put(&p->out, ' '); ring_put(&p->out, '\b'); } }
        while (p->linelen && p->line[p->linelen - 1] != ' ') { p->linelen--; if (lfl & T_ECHOE) { ring_put(&p->out, '\b'); ring_put(&p->out, ' '); ring_put(&p->out, '\b'); } }
        return 0;
    }
    if (p->linelen >= PTY_LINE - 1 && c != '\n' && c != cc[V_EOL]) return 0;   /* a full line: more is dropped */
    echo_char(p, c);
    p->line[p->linelen++] = c;
    if (c == '\n' || (c == cc[V_EOL] && c) || (c == cc[V_EOL2] && c)) {
        for (int i = 0; i < p->linelen; i++) ring_put(&p->in, p->line[i]);
        p->canon += (size_t)p->linelen; p->linelen = 0;
    }
    return 0;
}

/* Re-run the canonical/raw decision after TCSETS changed ICANON: a half-typed line becomes readable bytes */
static void termios_changed(pty *p, u32 old_lflag)
{
    if ((old_lflag & T_ICANON) && !(p->t.lflag & T_ICANON)) {
        for (int i = 0; i < p->linelen; i++) ring_put(&p->in, p->line[i]);
        p->linelen = 0; p->canon = 0;
    } else if (!(old_lflag & T_ICANON) && (p->t.lflag & T_ICANON)) p->canon = 0;
}

/* ------------------------------------------------------------------ I/O */
static int nonblock(const xfile *f) { return (f->flags & LX_O_NONBLOCK) != 0; }

static slong master_read(xfile *f, void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    pty *p = (pty *)f->priv;
    if (!n) return 0;
    u8 *tmp = (u8 *)malloc(n < 4096 ? (size_t)n : 4096);
    if (!tmp) return -LX_ENOMEM;
    slong r;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&p->lock);
        if (p->out.len) {
            size_t k = ring_take(&p->out, tmp, n < 4096 ? (size_t)n : 4096);
            ReleaseSRWLockExclusive(&p->lock);
            r = xrl_ctu((u64)buf, tmp, k) ? -LX_EFAULT : (slong)k;
            xrl_io_notify();
            break;
        }
        int gone = p->slave_ever && p->slave_opens == 0;
        ReleaseSRWLockExclusive(&p->lock);
        if (gone) { r = -LX_EIO; break; }
        if (nonblock(f)) { r = -LX_EAGAIN; break; }
        if (xrl_io_wait(seen, -1) < 0) { r = -LX_EINTR; break; }
    }
    free(tmp);
    return r;
}

static slong master_write(xfile *f, const void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    pty *p = (pty *)f->priv;
    u8 tmp[1024];
    u64 done = 0;
    while (done < n) {
        size_t k = n - done < sizeof tmp ? (size_t)(n - done) : sizeof tmp;
        if (xrl_cfu(tmp, (u64)buf + done, k)) return done ? (slong)done : -LX_EFAULT;
        for (size_t i = 0; i < k;) {
            LONG seen = xrl_io_epoch;
            int sig = 0;
            AcquireSRWLockExclusive(&p->lock);
            /* a byte may need room in both queues (echo, data): wait until there is some */
            int room = PTY_BUF - (int)p->in.len > 8 && PTY_BUF - (int)p->out.len > 8;
            if (room) { sig = in_char(p, tmp[i]); i++; }
            int pg = p->pgrp;
            ReleaseSRWLockExclusive(&p->lock);
            if (room) { xrl_io_notify(); if (sig) xrl_tty_signal(pg, sig); continue; }
            if (nonblock(f)) return done + i ? (slong)(done + i) : -LX_EAGAIN;
            if (xrl_io_wait(seen, -1) < 0) return done + i ? (slong)(done + i) : -LX_EINTR;
        }
        done += k;
    }
    return (slong)done;
}

static slong slave_read(xfile *f, void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    pty *p = (pty *)f->priv;
    if (!n) return 0;
    u8 *tmp = (u8 *)malloc(n < 4096 ? (size_t)n : 4096);
    if (!tmp) return -LX_ENOMEM;
    slong r;
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&p->lock);
        int canonical = (p->t.lflag & T_ICANON) != 0;
        size_t avail = canonical ? p->canon : p->in.len;
        int vmin = p->t.cc[V_MIN];
        size_t need = canonical ? 1 : vmin ? (p->t.cc[V_TIME] ? 1 : (size_t)vmin) : 0;   /* VTIME is not timed: any byte ends the wait */
        int gone = !p->master_open;
        if (canonical && !avail && p->eofs) { p->eofs--; ReleaseSRWLockExclusive(&p->lock); r = 0; break; }
        if (!canonical && !avail && vmin == 0) { ReleaseSRWLockExclusive(&p->lock); r = 0; break; }   /* VMIN=0: poll-like */
        if (avail && (avail >= need || gone)) {
            size_t want = n < 4096 ? (size_t)n : 4096;
            if (canonical) {   /* up to and including the first newline-ish byte, no further */
                size_t k = 0;
                while (k < avail && k < want) {
                    u8 c = p->in.b[(p->in.head + k) % PTY_BUF];
                    k++;
                    if (c == '\n' || (c == p->t.cc[V_EOL] && c) || (c == p->t.cc[V_EOL2] && c)) break;
                }
                want = k;
            } else if (want > avail) want = avail;
            size_t k = ring_take(&p->in, tmp, want);
            if (canonical) p->canon -= k;
            ReleaseSRWLockExclusive(&p->lock);
            r = xrl_ctu((u64)buf, tmp, k) ? -LX_EFAULT : (slong)k;
            xrl_io_notify();
            break;
        }
        int hup = !p->master_open;
        ReleaseSRWLockExclusive(&p->lock);
        if (hup) { r = 0; break; }
        if (nonblock(f)) { r = -LX_EAGAIN; break; }
        if (xrl_io_wait(seen, -1) < 0) { r = -LX_EINTR; break; }
    }
    free(tmp);
    return r;
}

static slong slave_write(xfile *f, const void *buf, u64 n, s64 off)
{
    if (off >= 0) return -LX_ESPIPE;
    pty *p = (pty *)f->priv;
    u8 tmp[1024];
    u64 done = 0;
    while (done < n) {
        size_t k = n - done < sizeof tmp ? (size_t)(n - done) : sizeof tmp;
        if (xrl_cfu(tmp, (u64)buf + done, k)) return done ? (slong)done : -LX_EFAULT;
        for (size_t i = 0; i < k;) {
            LONG seen = xrl_io_epoch;
            AcquireSRWLockExclusive(&p->lock);
            int hup = !p->master_open;
            int room = PTY_BUF - (int)p->out.len > 2;
            if (room && !hup) { out_char(p, tmp[i]); i++; }
            ReleaseSRWLockExclusive(&p->lock);
            if (hup) return done + i ? (slong)(done + i) : -LX_EIO;
            if (room) { if (i == k || !(i & 255)) xrl_io_notify(); continue; }
            xrl_io_notify();
            if (nonblock(f)) return done + i ? (slong)(done + i) : -LX_EAGAIN;
            if (xrl_io_wait(seen, -1) < 0) return done + i ? (slong)(done + i) : -LX_EINTR;
        }
        done += k;
    }
    xrl_io_notify();
    return (slong)done;
}

static slong pty_lseek(xfile *f, s64 o, int w) { (void)f; (void)o; (void)w; return -LX_ESPIPE; }

static slong master_fstat(xfile *f, lx_stat *st)
{
    pty *p = (pty *)f->priv;
    xrl_stat_synth(st, LX_S_IFCHR | 0620, 0x1000 + (u64)p->index, 0);
    st->st_rdev = (136u << 8) | (u32)p->index; st->st_uid = p->owner_uid; st->st_gid = 5;
    return 0;
}

static u32 master_poll(xfile *f, u32 ev)
{
    pty *p = (pty *)f->priv;
    u32 r = 0;
    AcquireSRWLockShared(&p->lock);
    if (p->out.len) r |= LX_POLLIN | LX_POLLRDNORM;
    if (p->slave_ever && p->slave_opens == 0) r |= LX_POLLHUP;
    if (PTY_BUF - p->in.len > 8) r |= LX_POLLOUT | LX_POLLWRNORM;
    ReleaseSRWLockShared(&p->lock);
    return r & (ev | LX_POLLERR | LX_POLLHUP);
}

static u32 slave_poll(xfile *f, u32 ev)
{
    pty *p = (pty *)f->priv;
    u32 r = 0;
    AcquireSRWLockShared(&p->lock);
    size_t avail = (p->t.lflag & T_ICANON) ? p->canon : p->in.len;
    if (avail || ((p->t.lflag & T_ICANON) && p->eofs)) r |= LX_POLLIN | LX_POLLRDNORM;
    if (!p->master_open) r |= LX_POLLHUP | LX_POLLIN;
    if (PTY_BUF - p->out.len > 2) r |= LX_POLLOUT | LX_POLLWRNORM;
    ReleaseSRWLockShared(&p->lock);
    return r & (ev | LX_POLLERR | LX_POLLHUP);
}

static slong open_slave_of(pty *p, int flags);

/* termios/winsize/pgrp ioctls common to both sides */
static slong common_ioctl(xfile *f, pty *p, u64 req, u64 arg, int master)
{
    switch (req) {
    case IOC_TCGETS: case IOC_TCGETS2: {
        ktermios t;
        AcquireSRWLockShared(&p->lock);
        t = p->t;
        ReleaseSRWLockShared(&p->lock);
        u8 b[44];
        memcpy(b, &t.iflag, 16); b[16] = t.line; memcpy(b + 17, t.cc, 19);
        if (req == IOC_TCGETS) return xrl_ctu(arg, b, 36) ? -LX_EFAULT : 0;
        memcpy(b + 36, &t.ispeed, 8);
        return xrl_ctu(arg, b, 44) ? -LX_EFAULT : 0;
    }
    case IOC_TCSETS: case IOC_TCSETSW: case IOC_TCSETSF: case IOC_TCSETS2: case IOC_TCSETSW2: case IOC_TCSETSF2: {
        int two = req == IOC_TCSETS2 || req == IOC_TCSETSW2 || req == IOC_TCSETSF2;
        u8 b[44];
        if (xrl_cfu(b, arg, two ? 44 : 36)) return -LX_EFAULT;
        AcquireSRWLockExclusive(&p->lock);
        u32 old = p->t.lflag;
        memcpy(&p->t.iflag, b, 16); p->t.line = b[16]; memcpy(p->t.cc, b + 17, 19);
        if (two) memcpy(&p->t.ispeed, b + 36, 8);
        if (req == IOC_TCSETSF || req == IOC_TCSETSF2) flush_input(p);
        termios_changed(p, old);
        ReleaseSRWLockExclusive(&p->lock);
        xrl_io_notify();
        return 0;
    }
    case IOC_TIOCGWINSZ: {
        winsz w;
        AcquireSRWLockShared(&p->lock); w = p->ws; ReleaseSRWLockShared(&p->lock);
        return xrl_ctu(arg, &w, sizeof w) ? -LX_EFAULT : 0;
    }
    case IOC_TIOCSWINSZ: {
        winsz w;
        if (xrl_cfu(&w, arg, sizeof w)) return -LX_EFAULT;
        int pg, changed;
        AcquireSRWLockExclusive(&p->lock);
        changed = memcmp(&w, &p->ws, sizeof w) != 0; p->ws = w; pg = p->pgrp;
        ReleaseSRWLockExclusive(&p->lock);
        if (changed && pg) xrl_tty_signal(pg, 28 /*SIGWINCH*/);
        return 0;
    }
    case IOC_TIOCGPGRP: {
        int pg; AcquireSRWLockShared(&p->lock); pg = p->pgrp; ReleaseSRWLockShared(&p->lock);
        if (master || !pg) return -LX_ENOTTY;   /* not the controlling terminal of the caller's session (proc.c refines this) */
        return xrl_ctu(arg, &pg, 4) ? -LX_EFAULT : 0;
    }
    case IOC_TIOCSPGRP: {
        int pg;
        if (xrl_cfu(&pg, arg, 4)) return -LX_EFAULT;
        if (pg <= 0) return -LX_EINVAL;
        AcquireSRWLockExclusive(&p->lock); p->pgrp = pg; ReleaseSRWLockExclusive(&p->lock);
        return 0;
    }
    case IOC_TIOCGSID: { int s; AcquireSRWLockShared(&p->lock); s = p->sid ? p->sid : 1; ReleaseSRWLockShared(&p->lock); return xrl_ctu(arg, &s, 4) ? -LX_EFAULT : 0; }
    case IOC_TIOCSCTTY: {   /* the caller's session takes this terminal: its foreground group is the caller's group (proc.c: the real ids) */
        AcquireSRWLockExclusive(&p->lock);
        if (!p->pgrp) p->pgrp = xrl_cur_pid();
        if (!p->sid) p->sid = xrl_cur_pid();
        ReleaseSRWLockExclusive(&p->lock);
        return 0;
    }
    case IOC_TIOCNOTTY: return 0;
    case IOC_TCFLSH: {
        int q = (int)arg;   /* TCIFLUSH 0, TCOFLUSH 1, TCIOFLUSH 2 */
        if (q < 0 || q > 2) return -LX_EINVAL;
        AcquireSRWLockExclusive(&p->lock);
        if (q != 1) flush_input(p);
        if (q != 0) { p->out.len = 0; p->out.head = 0; }
        ReleaseSRWLockExclusive(&p->lock);
        xrl_io_notify();
        return 0;
    }
    case IOC_TCSBRK: return 0;
    case IOC_FIONREAD: {
        int n;
        AcquireSRWLockShared(&p->lock);
        n = master ? (int)p->out.len : (p->t.lflag & T_ICANON) ? (int)p->canon : (int)p->in.len;
        ReleaseSRWLockShared(&p->lock);
        return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0;
    }
    case IOC_TIOCOUTQ: { int n = 0; return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0; }
    case IOC_FIONBIO: { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    case LX_FIOCLEX: case LX_FIONCLEX: return 0;
    }
    return -LX_ENOTTY;
}

static slong master_ioctl(xfile *f, u64 req, u64 arg)
{
    pty *p = (pty *)f->priv;
    switch (req) {
    case IOC_TIOCGPTN: { u32 n = (u32)p->index; return xrl_ctu(arg, &n, 4) ? -LX_EFAULT : 0; }
    case IOC_TIOCSPTLCK: { int l; if (xrl_cfu(&l, arg, 4)) return -LX_EFAULT; AcquireSRWLockExclusive(&p->lock); p->locked = l != 0; ReleaseSRWLockExclusive(&p->lock); return 0; }
    case IOC_TIOCGPTLCK: { int l; AcquireSRWLockShared(&p->lock); l = p->locked; ReleaseSRWLockShared(&p->lock); return xrl_ctu(arg, &l, 4) ? -LX_EFAULT : 0; }
    case IOC_TIOCGPTPEER: return open_slave_of(p, (int)arg);
    }
    return common_ioctl(f, p, req, arg, 1);
}

static slong slave_ioctl(xfile *f, u64 req, u64 arg) { return common_ioctl(f, (pty *)f->priv, req, arg, 0); }

static slong slave_fstat(xfile *f, lx_stat *st)
{
    pty *p = (pty *)f->priv;
    xrl_stat_synth(st, LX_S_IFCHR | 0620, 0x2000 + (u64)p->index, 0);
    st->st_rdev = (136u << 8) | (u32)p->index; st->st_uid = p->owner_uid; st->st_gid = 5;
    return 0;
}

static void master_close(xfile *f)
{
    pty *p = (pty *)f->priv;
    int pg, last;
    AcquireSRWLockExclusive(&p->lock);
    p->master_open = 0;
    pg = p->pgrp;
    last = p->slave_opens == 0;
    ReleaseSRWLockExclusive(&p->lock);
    xrl_io_notify();
    if (pg && !last) xrl_tty_signal(pg, LX_SIGHUP);   /* the session loses its terminal */
    if (last) pty_release_index(p);
    pty_unref(p);
}

static void slave_close(xfile *f)
{
    pty *p = (pty *)f->priv;
    int last;
    AcquireSRWLockExclusive(&p->lock);
    last = --p->slave_opens == 0 && !p->master_open;
    ReleaseSRWLockExclusive(&p->lock);
    xrl_io_notify();
    if (last) pty_release_index(p);
    pty_unref(p);
}

static const xfile_ops g_pm_ops = { "ptmx", master_read, master_write, pty_lseek, master_fstat, NULL, master_ioctl, NULL, NULL, master_poll, master_close, NULL };
static const xfile_ops g_ps_ops = { "pts", slave_read, slave_write, pty_lseek, slave_fstat, NULL, slave_ioctl, NULL, NULL, slave_poll, slave_close, NULL };

/* ------------------------------------------------------------------ open */
xfile *xrl_pty_open_master(int flags, int *err)
{
    pty *p = NULL;
    AcquireSRWLockExclusive(&g_pty_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        if (g_pty[i]) continue;
        p = pty_new(i);
        if (p) g_pty[i] = p;   /* the table's reference is the initial one */
        break;
    }
    ReleaseSRWLockExclusive(&g_pty_lock);
    if (!p) { *err = LX_ENOSPC; return NULL; }
    p->master_open = 1;
    pty_ref(p);   /* the description's */
    xfile *f = xfile_new(&g_pm_ops, LX_O_RDWR | (flags & LX_O_NONBLOCK));
    if (!f) { pty_unref(p); *err = LX_ENOMEM; return NULL; }
    f->priv = p;
    f->mode = LX_S_IFCHR;
    return f;
}

static xfile *slave_file(pty *p, int flags)   /* takes a reference of its own */
{
    xfile *f = xfile_new(&g_ps_ops, (flags & (LX_O_ACCMODE | LX_O_NONBLOCK)) ? (flags & (LX_O_ACCMODE | LX_O_NONBLOCK)) : LX_O_RDWR);
    if (!f) return NULL;
    pty_ref(p);
    AcquireSRWLockExclusive(&p->lock);
    p->slave_opens++; p->slave_ever = 1;
    ReleaseSRWLockExclusive(&p->lock);
    f->priv = p;
    f->mode = LX_S_IFCHR;
    return f;
}

static slong open_slave_of(pty *p, int flags)
{
    xfile *f = slave_file(p, flags);
    if (!f) return -LX_ENOMEM;
    int fd = xrl_fd_install(f, (flags & LX_O_CLOEXEC) != 0, 0);
    xfile_unref(f);
    return fd;
}

static pty *pty_by_index(int n)   /* a reference, or NULL */
{
    pty *p = NULL;
    if (n < 0 || n >= PTY_MAX) return NULL;
    AcquireSRWLockShared(&g_pty_lock);
    p = g_pty[n];
    if (p) pty_ref(p);
    ReleaseSRWLockShared(&g_pty_lock);
    return p;
}

xfile *xrl_pty_open_slave(int index, int flags, int *err)
{
    pty *p = pty_by_index(index);
    if (!p) { *err = LX_ENOENT; return NULL; }
    AcquireSRWLockShared(&p->lock);
    int locked = p->locked, master = p->master_open;
    ReleaseSRWLockShared(&p->lock);
    if (locked || !master) { pty_unref(p); *err = LX_EIO; return NULL; }
    xfile *f = slave_file(p, flags);
    pty_unref(p);
    if (!f) *err = LX_ENOMEM;
    return f;
}

/* stat of /dev/pts/N (1 = handled; st_nlink 0 = no such pty) and /dev/pts itself */
int xrl_pty_stat(const char *gabs, lx_stat *st)
{
    if (!strcmp(gabs, "/dev/pts")) { xrl_stat_synth(st, LX_S_IFDIR | 0755, 0x3000, 4096); st->st_nlink = 2; return 1; }
    if (strncmp(gabs, "/dev/pts/", 9)) return 0;
    char *end;
    long n = strtol(gabs + 9, &end, 10);
    if (end == gabs + 9 || *end) return 0;
    pty *p = pty_by_index((int)n);
    if (!p) { memset(st, 0, sizeof *st); return 1; }
    xrl_stat_synth(st, LX_S_IFCHR | 0620, 0x2000 + (u64)n, 0);
    st->st_rdev = (136u << 8) | (u32)n; st->st_uid = p->owner_uid; st->st_gid = 5;
    pty_unref(p);
    return 1;
}

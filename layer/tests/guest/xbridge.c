/*
 * xbridge.c - the display bridge between an X server and the layer's /dev/fb0 and /dev/input/event*.
 * Freestanding guest (build: build.sh -> out/xbridge.elf); no libc, no Xlib: it speaks the X11 wire protocol itself.
 *
 *   xbridge.elf --xwd <file> [--tcp <ip:port> | --unix <path>] [--seconds N] [--verify]
 *
 *   screen  : <file> is the screen of an Xvfb started with -fbdir (XWD format, 32 bpp, little-endian BGRX). The bridge
 *             polls it about 30 times a second and writes the rows that changed to /dev/fb0, which the host shows in the
 *             window; it first sets the framebuffer to the X screen's size.
 *   input   : /dev/input/event0 (keyboard), event1 (mouse), event2 (absolute pointer) become XTest FakeInput requests
 *             on the X connection: key codes are evdev's + 8, BTN_LEFT/MIDDLE/RIGHT -> buttons 1/2/3, wheels -> 4..7,
 *             ABS_X/ABS_Y -> a warp to the point, REL_X/REL_Y -> relative motion. (Opening event2 keeps the host cursor
 *             free; the bridge always opens event2 and event0, and event1 only when --rel is given, since opening event1
 *             alone would capture the host mouse.)
 *   --verify: at the end ask the server where the pointer is (QueryPointer) and print it: proves the injection landed.
 *
 * The X server is reached over TCP (Xvfb -listen tcp -ac; the layer's AF_INET) or a unix socket (needs AF_UNIX in the
 * layer). Everything is one thread: poll on the input devices and the X socket with a 16 ms timeout, a screen poll between.
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef int s32; typedef unsigned char u8; typedef unsigned short u16;
typedef long (*xsc_t)(long, long, long, long, long, long, long);
xsc_t xsc;
#define AT_XRL_SYSCALL 0x5801

#define sc(n, a, b, c, d, e, f) xsc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define sc1(n, a) sc(n, a, 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc(n, a, b, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc(n, a, b, c, 0, 0, 0)
#define sc4(n, a, b, c, d) sc(n, a, b, c, d, 0, 0)

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void out(const char *s) { sc3(1, 1, s, slen(s)); }
static void outn(const char *label, long v)
{
    char b[96]; int n = 0;
    for (const char *p = label; *p && n < 60; p++) b[n++] = *p;
    b[n++] = '=';
    if (v < 0) { b[n++] = '-'; v = -v; }
    char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = '0' + v % 10; v /= 10; }
    while (k) b[n++] = t[--k];
    b[n++] = '\n'; b[n] = 0;
    out(b);
}
static void die(const char *msg, long v) { out("xbridge: "); outn(msg, v); sc1(231, 1); for (;;) {} }

__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

static u64 mono_ns(void) { long ts[2]; sc2(228, 1, ts); return (u64)ts[0] * 1000000000ull + (u64)ts[1]; }
static void *mmap_anon(u64 len) { return (void *)sc(9, 0, len, 3, 0x22, -1, 0); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

struct input_event { long sec, usec; u16 type, code; s32 value; };
struct pollfd { int fd; short events, revents; };
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

/* ---- the X connection ---------------------------------------------------------------- */
static int xfd = -1;
static u32 xtest_major;
static u32 root_w, root_h, root_id;
static int nreq;

static int xsend(const void *b, long n)
{
    const u8 *p = b;
    while (n > 0) {
        long r = sc3(1, xfd, p, n);
        if (r == -11) { struct pollfd pf = { xfd, 4, 0 }; sc3(7, &pf, 1, 100); continue; }   /* EAGAIN: wait for POLLOUT */
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}
static int xread_full(void *b, long n)
{
    u8 *p = b;
    while (n > 0) {
        long r = sc3(0, xfd, p, n);
        if (r == -11) { struct pollfd pf = { xfd, 1, 0 }; sc3(7, &pf, 1, 1000); continue; }
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

static void x_connect_tcp(const char *spec)   /* a.b.c.d:port */
{
    u32 a[4] = {0}, port = 0; int i = 0, k = 0;
    for (; spec[i] && spec[i] != ':'; i++) { if (spec[i] == '.') k++; else if (k < 4) a[k] = a[k] * 10 + (u32)(spec[i] - '0'); }
    if (spec[i] == ':') for (i++; spec[i]; i++) port = port * 10 + (u32)(spec[i] - '0');
    xfd = (int)sc3(41, 2, 1, 0);   /* socket(AF_INET, SOCK_STREAM) */
    if (xfd < 0) die("socket", xfd);
    u8 sa[16]; memset(sa, 0, 16);
    sa[0] = 2; sa[2] = (u8)(port >> 8); sa[3] = (u8)port; sa[4] = (u8)a[0]; sa[5] = (u8)a[1]; sa[6] = (u8)a[2]; sa[7] = (u8)a[3];
    long r = sc3(42, xfd, sa, 16);
    if (r < 0) die("connect to the X server failed", r);
}
static void x_connect_unix(const char *path)
{
    xfd = (int)sc3(41, 1, 1, 0);   /* AF_UNIX */
    if (xfd < 0) die("socket(AF_UNIX) (the layer has no AF_UNIX yet?)", xfd);
    u8 sa[110]; memset(sa, 0, sizeof sa);
    sa[0] = 1;
    for (int i = 0; path[i] && i < 107; i++) sa[2 + i] = (u8)path[i];
    long r = sc3(42, xfd, sa, 2 + slen(path) + 1);
    if (r < 0) die("connect to the X server failed", r);
}

static void x_handshake(void)
{
    u8 req[12]; memset(req, 0, 12);
    req[0] = 'l'; put16(req + 2, 11);
    if (xsend(req, 12)) die("handshake: send", 0);
    u8 hdr[8];
    if (xread_full(hdr, 8)) die("handshake: no reply", 0);
    if (hdr[0] != 1) die("the X server refused the connection (needs Xvfb -ac?), status", hdr[0]);
    u32 n = le16(hdr + 6) * 4;
    static u8 body[65536];
    if (n > sizeof body || xread_full(body, n)) die("handshake: reply", n);
    u32 vlen = le16(body + 16), nfmt = body[21];
    u32 off = 32 + ((vlen + 3) & ~3u) + nfmt * 8;
    root_id = le32(body + off);
    root_w = le16(body + off + 20); root_h = le16(body + off + 22);
    outn("X root width", root_w); outn("X root height", root_h);
}

static void x_query_xtest(void)
{
    u8 q[16]; memset(q, 0, sizeof q);
    q[0] = 98; put16(q + 2, 4); put16(q + 4, 5);
    memcpy(q + 8, "XTEST", 5);
    if (xsend(q, 16)) die("QueryExtension: send", 0);
    u8 r[32];
    if (xread_full(r, 32)) die("QueryExtension: reply", 0);
    if (r[0] != 1 || !r[8]) die("the X server has no XTEST extension", r[0]);
    xtest_major = r[9];
    outn("XTEST major opcode", xtest_major);
}

#define X_KEYPRESS 2
#define X_KEYRELEASE 3
#define X_BUTTONPRESS 4
#define X_BUTTONRELEASE 5
#define X_MOTION 6
static void x_fake(u32 type, u32 detail, int rx, int ry)   /* XTestFakeInput */
{
    u8 q[36]; memset(q, 0, sizeof q);
    q[0] = (u8)xtest_major; q[1] = 2; put16(q + 2, 9);
    q[4] = (u8)type; q[5] = (u8)detail;
    put16(q + 24, (u32)rx); put16(q + 26, (u32)ry);
    if (xsend(q, 36)) die("XTest FakeInput: send", 0);
    nreq++;
}

/* drain whatever the server sends (errors/events we did not ask for) so its buffer never fills */
static void x_drain(void)
{
    u8 b[512];
    for (;;) {
        struct pollfd pf = { xfd, 1, 0 };
        if (sc3(7, &pf, 1, 0) <= 0 || !(pf.revents & 1)) return;
        long r = sc3(0, xfd, b, sizeof b);
        if (r <= 0) return;
        if (b[0] == 0) { out("xbridge: X error code "); outn("", b[1]); }
    }
}

static void x_query_pointer(void)
{
    u8 q[8]; memset(q, 0, sizeof q);
    q[0] = 38; put16(q + 2, 2);
    put32(q + 4, root_id);
    if (xsend(q, 8)) return;
    u8 r[32];
    /* skip any queued errors/events until the reply (type 1) */
    for (int i = 0; i < 64; i++) {
        if (xread_full(r, 32)) return;
        if (r[0] == 1) { outn("pointer x", le16(r + 16)); outn("pointer y", le16(r + 18)); return; }
    }
}
/* ---- the screen ------------------------------------------------------------------------ */
static int fb = -1, xwd = -1;
static u32 fb_w, fb_h, fb_line;
static u32 xw_w, xw_h, xw_line, xw_off;
static u8 *cur, *prev;
static u64 frames, rows_sent;

static int xwd_open(const char *path)
{
    xwd = (int)sc3(2, path, 0, 0);
    if (xwd < 0) return -1;
    u8 h[100];
    if (sc4(17, xwd, h, 100, 0) != 100) return -2;
    u32 hs = be32(h), ncol = be32(h + 76);
    xw_w = be32(h + 16); xw_h = be32(h + 20); xw_line = be32(h + 48);
    u32 bpp = be32(h + 44), order = be32(h + 28), rm = be32(h + 56), gm = be32(h + 60), bm = be32(h + 64);
    if (bpp != 32 || order != 0 || rm != 0xff0000 || gm != 0xff00 || bm != 0xff) { outn("unsupported XWD pixel format, bpp", bpp); return -3; }
    xw_off = hs + ncol * 12;
    return 0;
}

static void fb_setup(void)
{
    fb = (int)sc3(2, "/dev/fb0", 2, 0);
    if (fb < 0) die("open /dev/fb0", fb);
    u32 var[40];
    sc3(16, fb, FBIOGET_VSCREENINFO, var);
    if (var[0] != xw_w || var[1] != xw_h) {   /* the X screen's size becomes the framebuffer's */
        var[0] = var[2] = xw_w; var[1] = var[3] = xw_h; var[4] = var[5] = 0;
        long r = sc3(16, fb, FBIOPUT_VSCREENINFO, var);
        if (r < 0) outn("could not resize /dev/fb0, error", r);
        sc3(16, fb, FBIOGET_VSCREENINFO, var);
    }
    u32 fix[20];
    sc3(16, fb, FBIOGET_FSCREENINFO, fix);
    fb_w = var[0]; fb_h = var[1]; fb_line = fix[12];
    outn("fb width", fb_w); outn("fb height", fb_h);
}

/* one screen poll: read the XWD pixels, write the rows that differ from the last poll to the framebuffer */
static void screen_poll(void)
{
    u32 w = xw_w < fb_w ? xw_w : fb_w, h = xw_h < fb_h ? xw_h : fb_h;
    u64 need = (u64)xw_line * xw_h;
    long r = sc4(17, xwd, cur, need, xw_off);
    if (r < (long)need) return;   /* the file is being rewritten */
    int in_run = 0;
    u32 run0 = 0;
    for (u32 y = 0; y <= h; y++) {
        int diff = 0;
        if (y < h) {
            const u64 *a = (const u64 *)(cur + (u64)y * xw_line), *b = (const u64 *)(prev + (u64)y * xw_line);
            for (u32 i = 0; i < w / 2; i++) if (a[i] != b[i]) { diff = 1; break; }
        }
        if (diff && !in_run) { run0 = y; in_run = 1; }
        if (!diff && in_run) {   /* rows run0..y-1 changed */
            if (xw_line == fb_line && w * 4 == fb_line) sc4(18, fb, cur + (u64)run0 * xw_line, (u64)(y - run0) * xw_line, (u64)run0 * fb_line);
            else for (u32 yy = run0; yy < y; yy++) sc4(18, fb, cur + (u64)yy * xw_line, (u64)w * 4, (u64)yy * fb_line);
            rows_sent += y - run0;
            in_run = 0;
        }
    }
    memcpy(prev, cur, need);
    frames++;
}

/* ---- input ------------------------------------------------------------------------------ */
static u32 abs_x, abs_y;
static int rel_dx, rel_dy, have_abs, have_rel;
static u64 injected;

static void button_for(u32 code, u32 *b)
{
    *b = code == 0x110 ? 1 : code == 0x112 ? 2 : code == 0x111 ? 3 : code == 0x113 ? 8 : code == 0x114 ? 9 : 0;
}

static void on_event(const struct input_event *e)
{
    if (e->type == EV_KEY) {
        if (e->value == 2) return;   /* the server repeats keys itself */
        if (e->code < 0x100) { x_fake(e->value ? X_KEYPRESS : X_KEYRELEASE, e->code + 8u, 0, 0); injected++; }
        else { u32 b; button_for(e->code, &b); if (b) { x_fake(e->value ? X_BUTTONPRESS : X_BUTTONRELEASE, b, 0, 0); injected++; } }
    } else if (e->type == EV_ABS) {
        if (e->code == 0) abs_x = (u32)e->value * root_w / 32768; else if (e->code == 1) abs_y = (u32)e->value * root_h / 32768;
        have_abs = 1;
    } else if (e->type == EV_REL) {
        if (e->code == 0) { rel_dx += e->value; have_rel = 1; }
        else if (e->code == 1) { rel_dy += e->value; have_rel = 1; }
        else if (e->code == 8 || e->code == 6) {   /* wheels: one click = press + release of button 4/5 (vertical), 6/7 (horizontal) */
            u32 b = e->code == 8 ? (e->value > 0 ? 4u : 5u) : (e->value > 0 ? 7u : 6u);
            int n = e->value < 0 ? -e->value : e->value;
            for (int i = 0; i < n; i++) { x_fake(X_BUTTONPRESS, b, 0, 0); x_fake(X_BUTTONRELEASE, b, 0, 0); injected += 2; }
        }
    } else if (e->type == EV_SYN) {
        if (have_abs) { x_fake(X_MOTION, 0, (int)abs_x, (int)abs_y); injected++; have_abs = 0; }
        if (have_rel) { x_fake(X_MOTION, 1, rel_dx, rel_dy); injected++; rel_dx = rel_dy = 0; have_rel = 0; }
    }
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    const char *xwd_path = 0, *tcp = 0, *unixp = 0;
    int seconds = 30, verify = 0, rel = 0;
    for (long i = 1; i < argc; i++) {
        const char *a = (const char *)sp[1 + i];
        const char *nx = i + 1 < argc ? (const char *)sp[2 + i] : "";
        if (streq(a, "--xwd")) { xwd_path = nx; i++; }
        else if (streq(a, "--tcp")) { tcp = nx; i++; }
        else if (streq(a, "--unix")) { unixp = nx; i++; }
        else if (streq(a, "--seconds")) { int v = 0; for (int k = 0; nx[k] >= '0' && nx[k] <= '9'; k++) v = v * 10 + nx[k] - '0'; seconds = v; i++; }
        else if (streq(a, "--verify")) verify = 1;
        else if (streq(a, "--rel")) rel = 1;
    }
    if (!xwd_path) die("usage: xbridge --xwd <Xvfb_screen0 copy> [--tcp ip:port | --unix path] [--seconds N] [--verify] [--rel]", 0);
    out("xbridge start\n");

    if (xwd_open(xwd_path)) die("cannot read the XWD screen file", 0);
    outn("XWD width", xw_w); outn("XWD height", xw_h);
    fb_setup();
    cur = mmap_anon((u64)xw_line * xw_h); prev = mmap_anon((u64)xw_line * xw_h);
    if ((u64)cur < 0x10000 || (u64)prev < 0x10000) die("out of memory", 0);

    int kfd = -1, mfd = -1, afd = -1;
    if (tcp || unixp) {
        if (tcp) x_connect_tcp(tcp); else x_connect_unix(unixp);
        x_handshake();
        x_query_xtest();
        kfd = (int)sc3(2, "/dev/input/event0", 0x800, 0);
        afd = (int)sc3(2, "/dev/input/event2", 0x800, 0);
        if (rel) mfd = (int)sc3(2, "/dev/input/event1", 0x800, 0);
        if (kfd < 0 || afd < 0) die("cannot open the evdev devices", kfd < 0 ? kfd : afd);
        sc3(72, xfd, 4, 0x800);   /* fcntl(F_SETFL, O_NONBLOCK) */
    } else out("no X server given: screen only\n");
    out("input-ready\n");   /* the test harness starts its clock here */

    u64 t0 = mono_ns(), next_screen = 0;
    while (mono_ns() - t0 < (u64)seconds * 1000000000ull) {
        struct pollfd pf[4]; int n = 0;
        if (kfd >= 0) { pf[n].fd = kfd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (afd >= 0) { pf[n].fd = afd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (mfd >= 0) { pf[n].fd = mfd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (n) sc3(7, pf, n, 16); else { long ts[2] = { 0, 16000000 }; sc2(35, ts, 0); }
        for (int d = 0; d < n; d++) {
            if (!(pf[d].revents & 1)) continue;
            struct input_event ev[32];
            for (;;) {
                long r = sc3(0, pf[d].fd, ev, sizeof ev);
                if (r <= 0) break;
                for (long i = 0; i < r / (long)sizeof ev[0]; i++) on_event(&ev[i]);
            }
        }
        if (xfd >= 0) x_drain();
        u64 now = mono_ns();
        if (now >= next_screen) { screen_poll(); next_screen = now + 33000000ull; }
    }
    outn("screen polls", (long)frames); outn("rows written", (long)rows_sent); outn("input requests", nreq);
    if (verify && xfd >= 0) {
        x_query_pointer();
    }
    sc1(231, 0);
    for (;;) {}
}

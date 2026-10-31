/*
 * xbridge.c - the display bridge between an X server and the layer's /dev/fb0 and /dev/input/event*.
 * Freestanding guest (build: build.sh -> out/xbridge.elf); no libc, no Xlib: it speaks the X11 wire protocol itself.
 *
 *   xbridge.elf (--x | --xwd <file>) [--tcp <ip:port> | --unix <path>] [--seconds N] [--verify] [--dump <file>]
 *
 *   screen  : --x: over the X protocol. MIT-SHM ShmGetImage into a SysV segment (GetImage when the server has no
 *             MIT-SHM), only the rows the DAMAGE extension reports changed (the whole screen ~3 times a second when
 *             there is no DAMAGE); x11vnc does the same. --xwd <file>: the screen file of an Xvfb started with -fbdir
 *             (XWD, 32 bpp BGRX), polled. Either way about 30 times a second the changed rows go to /dev/fb0, which
 *             the host shows (the Xbox app on the TV); the framebuffer first takes the X screen's size.
 *   input   : /dev/input/event0 (keyboard), event1 (mouse), event2 (absolute pointer) become XTest FakeInput requests
 *             on the X connection: key codes are evdev's + 8, BTN_LEFT/MIDDLE/RIGHT -> buttons 1/2/3, wheels -> 4..7,
 *             ABS_X/ABS_Y -> a warp to the point, REL_X/REL_Y -> relative motion. (Opening event2 keeps the host cursor
 *             free; the bridge always opens event2 and event0 (and event3, the gamepad, when it exists: see
 *             on_pad_event), and event1 only when --rel is given, since opening event1
 *             alone would capture the host mouse.)
 *   --dump <file>: at the end write /dev/fb0's pixels (raw BGRX) to <file>
 *   --verify: at the end ask the server where the pointer is (QueryPointer) and print it: proves the injection landed.
 *
 * The X server is reached over TCP (Xvfb -listen tcp -ac; the layer's AF_INET) or a unix socket (needs AF_UNIX in the
 * layer). Everything is one thread: poll on the input devices and the X socket with a 16 ms timeout, a screen poll between.
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef int s32; typedef unsigned char u8; typedef unsigned short u16; typedef short s16;
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
/* outside the layer (a test on Linux itself): the syscall instruction, same argument order as the layer's entry */
long native_sc(long n, long a, long b, long c, long d, long e, long f);
__asm__(".text\n.globl native_sc\nnative_sc:\n  mov %rdi,%rax\n  mov %rsi,%rdi\n  mov %rdx,%rsi\n  mov %rcx,%rdx\n"
        "  mov %r8,%r10\n  mov %r9,%r8\n  mov 8(%rsp),%r9\n  syscall\n  ret\n");

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
static u32 root_w, root_h, root_id, id_base;
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
    if (r < 0) { sc1(3, xfd); xfd = -1; }   /* not up yet: the caller retries */
}
static void x_connect_unix(const char *path)
{
    xfd = (int)sc3(41, 1, 1, 0);   /* AF_UNIX */
    if (xfd < 0) die("socket(AF_UNIX) (the layer has no AF_UNIX yet?)", xfd);
    u8 sa[110]; memset(sa, 0, sizeof sa);
    sa[0] = 1;
    for (int i = 0; path[i] && i < 107; i++) sa[2 + i] = (u8)path[i];
    long r = sc3(42, xfd, sa, 2 + slen(path) + 1);
    if (r < 0) { sc1(3, xfd); xfd = -1; }   /* not up yet: the caller retries */
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
    id_base = le32(body + 4);
    root_id = le32(body + off);
    root_w = le16(body + off + 20); root_h = le16(body + off + 22);
    outn("X root width", root_w); outn("X root height", root_h);
}

/* QueryExtension: 1 if present; major opcode and first event code out */
static int x_query_ext(const char *name, u32 *major, u32 *first_event)
{
    u8 q[40]; memset(q, 0, sizeof q);
    u32 n = (u32)slen(name), len = 2 + ((n + 3) >> 2);
    q[0] = 98; put16(q + 2, len); put16(q + 4, n);
    memcpy(q + 8, name, n);
    if (xsend(q, len * 4)) die("QueryExtension: send", 0);
    u8 r[32];
    if (xread_full(r, 32)) die("QueryExtension: reply", 0);
    if (r[0] != 1 || !r[8]) return 0;
    *major = r[9];
    if (first_event) *first_event = r[10];
    return 1;
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

/* what the server sends: 32-byte events and errors, replies with 4*length more bytes. DAMAGE events grow the damaged
 * bounding box; a reply is handed to the caller that waits for it (x_next with want_reply) */
static u32 damage_ev = 0x100;   /* DAMAGE's DamageNotify code (first_event), 0x100: none */
static int dmg_x0 = 1 << 30, dmg_y0 = 1 << 30, dmg_x1 = -1, dmg_y1 = -1;
static u64 x_errors;
static int x_next(u8 *ev32, u8 *extra, u32 extra_cap, u32 *extra_len)   /* 0 ok, -1 connection gone */
{
    if (xread_full(ev32, 32)) return -1;
    if (extra_len) *extra_len = 0;
    if (ev32[0] == 1) {   /* a reply: its data follows */
        u32 n = le32(ev32 + 4) * 4;
        if (extra && n <= extra_cap) { if (xread_full(extra, n)) return -1; if (extra_len) *extra_len = n; }
        else { static u8 sink[4096]; while (n) { u32 c = n < sizeof sink ? n : sizeof sink; if (xread_full(sink, c)) return -1; n -= c; } }
    } else if (ev32[0] == 0) {
        if (++x_errors <= 8) { out("xbridge: X error code "); outn("", ev32[1]); outn("  major", ev32[10]); outn("  minor", le16(ev32 + 8)); }
    } else if ((ev32[0] & 0x7f) == damage_ev) {   /* DamageNotify: area at 16 (BoundingBox level) */
        int x = (int)(s16)le16(ev32 + 16), y = (int)(s16)le16(ev32 + 18), w = (int)le16(ev32 + 20), h = (int)le16(ev32 + 22);
        if (x < dmg_x0) dmg_x0 = x;
        if (y < dmg_y0) dmg_y0 = y;
        if (x + w > dmg_x1) dmg_x1 = x + w;
        if (y + h > dmg_y1) dmg_y1 = y + h;
    }
    return 0;
}
/* drain whatever is buffered (events, errors) so the server's output never fills */
static void x_drain(void)
{
    u8 b[32];
    for (;;) {
        struct pollfd pf = { xfd, 1, 0 };
        if (sc3(7, &pf, 1, 0) <= 0 || !(pf.revents & 1)) return;
        if (x_next(b, 0, 0, 0)) return;
    }
}
/* wait for the reply to the request just sent, handling events on the way; its data goes to extra */
static int x_reply(u8 *r32, u8 *extra, u32 cap, u32 *len)
{
    for (;;) {
        if (x_next(r32, extra, cap, len)) return -1;
        if (r32[0] == 1) return 0;
        if (r32[0] == 0) return -1;   /* the error for our request */
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
static const char *xwd_file;
static u32 fb_w, fb_h, fb_line;
static u32 xw_w, xw_h, xw_line, xw_off;
static u8 *cur, *prev;
static u64 frames, rows_sent;

static int xwd_open(const char *path)
{
    xwd_file = path;
    xwd = (int)sc3(2, path, 0, 0);
    if (xwd < 0) return -1;
    u8 h[100];
    long hr = sc4(17, xwd, h, 100, 0);
    sc1(3, xwd);   /* closed again: screen_poll reopens per poll */
    if (hr != 100) return -2;
    u32 hs = be32(h), ncol = be32(h + 76);
    xw_w = be32(h + 16); xw_h = be32(h + 20); xw_line = be32(h + 48);
    u32 bpp = be32(h + 44), order = be32(h + 28), rm = be32(h + 56), gm = be32(h + 60), bm = be32(h + 64);
    if (bpp != 32 || order != 0 || rm != 0xff0000 || gm != 0xff00 || bm != 0xff) { outn("unsupported XWD pixel format, bpp", bpp); return -3; }
    xw_off = hs + ncol * 12;
    return 0;
}

static const char *fb_path = "/dev/fb0";
static void fb_setup(void)
{
    fb = (int)sc3(2, fb_path, 0102, 0644);   /* O_RDWR|O_CREAT: --fb <file> on a plain file (a test outside the layer) */
    if (fb < 0) die("open the framebuffer", fb);
    u32 var[40];
    if (sc3(16, fb, FBIOGET_VSCREENINFO, var) < 0) {   /* not a framebuffer: a file of the X screen's size */
        fb_w = xw_w; fb_h = xw_h; fb_line = xw_w * 4;
        sc2(77, fb, (u64)fb_line * fb_h);   /* ftruncate */
        outn("fb (file) width", fb_w); outn("fb (file) height", fb_h);
        return;
    }
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
    /* open it afresh every time: a writer that replaces the file in place (the copier of the screenshot loop, a
     * Windows-side cp) must not find it held open */
    int fd = (int)sc3(2, xwd_file, 0, 0);
    if (fd < 0) return;
    long r = sc4(17, fd, cur, need, xw_off);
    sc1(3, fd);
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

/* ---- the screen over the X protocol (--x) ------------------------------------------------- */
static u32 shm_major, dmg_major, shm_seg, dmg_id;
static u8 *shm_buf;
static int use_shm, use_damage;
static u64 bytes_fetched;

static void xscreen_setup(void)
{
    xw_w = root_w; xw_h = root_h; xw_line = root_w * 4;
    u32 ev = 0;
    if (x_query_ext("MIT-SHM", &shm_major, 0)) {
        long id = sc3(29, 0, (u64)xw_line * xw_h, 01000 | 0777);   /* shmget(IPC_PRIVATE, IPC_CREAT|0777) */
        long a = id >= 0 ? sc3(30, id, 0, 0) : -1;                  /* shmat */
        if (id >= 0 && a > 0x10000) {
            shm_buf = (u8 *)a;
            shm_seg = id_base | 1;
            u8 q[16]; memset(q, 0, sizeof q);
            q[0] = (u8)shm_major; q[1] = 1; put16(q + 2, 4);          /* ShmAttach(seg, shmid, read-only 0) */
            put32(q + 4, shm_seg); put32(q + 8, (u32)id);
            if (!xsend(q, 16)) { nreq++; use_shm = 1; }
            sc3(31, id, 0, 0);   /* shmctl(IPC_RMID): gone once both detach */
        } else outn("MIT-SHM: shared memory refused, GetImage instead", id >= 0 ? a : id);
    }
    if (x_query_ext("DAMAGE", &dmg_major, &ev)) {
        u8 q[12]; memset(q, 0, sizeof q);
        q[0] = (u8)dmg_major; q[1] = 0; put16(q + 2, 3); put32(q + 4, 1); put32(q + 8, 1);   /* DamageQueryVersion 1.1 */
        u8 r[32];
        if (!xsend(q, 12) && !x_reply(r, 0, 0, 0)) {
            damage_ev = ev;
            dmg_id = id_base | 2;
            u8 c[16]; memset(c, 0, sizeof c);
            c[0] = (u8)dmg_major; c[1] = 1; put16(c + 2, 4);              /* DamageCreate(damage, root, BoundingBox) */
            put32(c + 4, dmg_id); put32(c + 8, root_id); c[12] = 2;
            if (!xsend(c, 16)) { nreq++; use_damage = 1; }
        }
    }
    outn("screen over X: MIT-SHM", use_shm); outn("screen over X: DAMAGE", use_damage);
    dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = (int)xw_w; dmg_y1 = (int)xw_h;   /* the first frame is all of it */
}

/* rows y0..y1 of the root window into cur (rows of xw_line bytes): 0 ok */
static int xscreen_fetch(u32 y0, u32 y1)
{
    u32 h = y1 - y0;
    if (use_shm) {
        u8 q[32]; memset(q, 0, sizeof q);
        q[0] = (u8)shm_major; q[1] = 4; put16(q + 2, 8);   /* ShmGetImage(root, 0, y0, w, h, ~0, ZPixmap, seg, offset) */
        put32(q + 4, root_id); put16(q + 8, 0); put16(q + 10, y0); put16(q + 12, xw_w); put16(q + 14, h);
        put32(q + 16, 0xffffffffu); q[20] = 2; put32(q + 24, shm_seg); put32(q + 28, y0 * xw_line);
        u8 r[32];
        if (xsend(q, 32) || x_reply(r, 0, 0, 0)) return -1;
        nreq++;
        memcpy(cur + (u64)y0 * xw_line, shm_buf + (u64)y0 * xw_line, (u64)h * xw_line);
    } else {
        for (u32 y = y0; y < y1;) {   /* GetImage in bands of up to 64 rows (a reply is length * 4 bytes) */
            u32 n = y1 - y < 64 ? y1 - y : 64;
            u8 q[20]; memset(q, 0, sizeof q);
            q[0] = 73; q[1] = 2; put16(q + 2, 5);
            put32(q + 4, root_id); put16(q + 8, 0); put16(q + 10, y); put16(q + 12, xw_w); put16(q + 14, n);
            put32(q + 16, 0xffffffffu);
            u8 r[32]; u32 len = 0;
            if (xsend(q, 20) || x_reply(r, cur + (u64)y * xw_line, n * xw_line, &len)) return -1;
            nreq++;
            y += n;
        }
    }
    bytes_fetched += (u64)h * xw_line;
    return 0;
}

/* one frame: fetch what DAMAGE says changed (everything every ~0.3 s without DAMAGE), write the changed rows */
static void cursor_update(u32 y0, u32 y1);
static void xscreen_poll(void)
{
    static u32 tick;
    u32 y0, y1;
    if (use_damage) {
        if (dmg_y1 <= dmg_y0) { cursor_update(0, 0); return; }
        y0 = dmg_y0 < 0 ? 0 : (u32)dmg_y0; y1 = dmg_y1 > (int)xw_h ? xw_h : (u32)dmg_y1;
        dmg_x0 = dmg_y0 = 1 << 30; dmg_x1 = dmg_y1 = -1;
        u8 q[16]; memset(q, 0, sizeof q);
        q[0] = (u8)dmg_major; q[1] = 3; put16(q + 2, 4); put32(q + 4, dmg_id);   /* DamageSubtract(damage, None, None) */
        if (xsend(q, 16)) return;
        nreq++;
    } else {
        if (tick++ % 10) { cursor_update(0, 0); return; }
        y0 = 0; y1 = xw_h;
    }
    if (y1 <= y0 || xscreen_fetch(y0, y1)) return;
    u32 w = xw_w < fb_w ? xw_w : fb_w, yl = y1 < fb_h ? y1 : fb_h;
    for (u32 y = y0; y < yl;) {   /* runs of rows that differ from what fb0 has */
        const u64 *a = (const u64 *)(cur + (u64)y * xw_line), *b = (const u64 *)(prev + (u64)y * xw_line);
        int diff = 0;
        for (u32 i = 0; i < w / 2; i++) if (a[i] != b[i]) { diff = 1; break; }
        if (!diff) { y++; continue; }
        u32 e = y + 1;
        for (; e < yl; e++) {
            const u64 *c = (const u64 *)(cur + (u64)e * xw_line), *d = (const u64 *)(prev + (u64)e * xw_line);
            int df = 0;
            for (u32 i = 0; i < w / 2; i++) if (c[i] != d[i]) { df = 1; break; }
            if (!df) break;
        }
        if (xw_line == fb_line && w * 4 == fb_line) sc4(18, fb, cur + (u64)y * xw_line, (u64)(e - y) * xw_line, (u64)y * fb_line);
        else for (u32 yy = y; yy < e; yy++) sc4(18, fb, cur + (u64)yy * xw_line, (u64)w * 4, (u64)yy * fb_line);
        memcpy(prev + (u64)y * xw_line, cur + (u64)y * xw_line, (u64)(e - y) * xw_line);
        rows_sent += e - y;
        y = e;
    }
    frames++;
    cursor_update(y0, yl);
}

/* ---- the pointer --------------------------------------------------------------------------
 * No GetImage includes the cursor (the X server draws it into the framebuffer of a real screen only), so on the TV the
 * pointer was invisible. Each frame (--x) the bridge asks where it is (QueryPointer) and draws an arrow there into
 * /dev/fb0, over the true picture it keeps in prev; the pixels under the old spot are put back when it moves, and the
 * arrow is redrawn when a frame rewrote rows under it. */
#define CUR_W 12
#define CUR_H 17
static const char *const arrow[CUR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
    "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ", "      XX    ",
};
static int cur_x = -1, cur_y = -1;   /* where the arrow is drawn now (-1: nowhere) */
static u64 cursor_draws;

static int x_pointer_xy(int *x, int *y)   /* QueryPointer on the root: 0 ok */
{
    u8 q[8]; memset(q, 0, sizeof q);
    q[0] = 38; put16(q + 2, 2); put32(q + 4, root_id);
    u8 r[32];
    if (xsend(q, 8) || x_reply(r, 0, 0, 0)) return -1;
    nreq++;
    *x = (int)(short)le16(r + 16); *y = (int)(short)le16(r + 18);
    return 0;
}

/* the true picture (prev) back into fb0 for the arrow's box at (x, y) */
static void cursor_erase(int x, int y)
{
    for (int r = 0; r < CUR_H; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= (int)fb_h || yy >= (int)xw_h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + CUR_W > (int)fb_w ? (int)fb_w : x + CUR_W;
        if (x1 > (int)xw_w) x1 = (int)xw_w;
        if (x1 <= x0) continue;
        sc4(18, fb, prev + (u64)yy * xw_line + (u64)x0 * 4, (u64)(x1 - x0) * 4, (u64)yy * fb_line + (u64)x0 * 4);
    }
}

static void cursor_draw(int x, int y)
{
    u32 line[CUR_W];
    for (int r = 0; r < CUR_H; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= (int)fb_h || yy >= (int)xw_h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + CUR_W > (int)fb_w ? (int)fb_w : x + CUR_W;
        if (x1 > (int)xw_w) x1 = (int)xw_w;
        if (x1 <= x0) continue;
        const u32 *src = (const u32 *)(prev + (u64)yy * xw_line);
        for (int xx = x0; xx < x1; xx++) {
            char c = arrow[r][xx - x];
            line[xx - x0] = c == 'X' ? 0x00000000u : c == '.' ? 0x00ffffffu : src[xx];
        }
        sc4(18, fb, line, (u64)(x1 - x0) * 4, (u64)yy * fb_line + (u64)x0 * 4);
    }
    cursor_draws++;
}

/* after a frame: rows y0..y1 of fb0 were rewritten from the true picture (y1 <= y0: none) */
static void cursor_update(u32 y0, u32 y1)
{
    int x, y;
    if (x_pointer_xy(&x, &y)) return;
    int under = cur_y >= 0 && y1 > y0 && (int)y0 < cur_y + CUR_H && (int)y1 > cur_y;   /* the frame drew over it */
    if (x == cur_x && y == cur_y && !under) return;
    if (cur_x >= 0 && !under) cursor_erase(cur_x, cur_y);
    else if (cur_x >= 0 && (cur_x != x || cur_y != y)) cursor_erase(cur_x, cur_y);
    cursor_draw(x, y);
    cur_x = x; cur_y = y;
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

/* the gamepad (/dev/input/event3, evdev.c) as a mouse + keyboard: left stick moves the pointer (dead zone, then
 * faster the further it is pushed), right stick scrolls, A/X/B = buttons 1/2/3, D-pad = arrow keys, Start = Enter,
 * Back = Escape, Y = Space, LB = Backspace, RB = Tab, Guide = Super. */
static int pad_lx, pad_ly, pad_ry, pad_scroll_acc;
static long pad_events;
static void key_tap_state(u32 key, int down) { x_fake(down ? X_KEYPRESS : X_KEYRELEASE, key + 8u, 0, 0); injected++; }
static void on_pad_event(const struct input_event *e)
{
    if (e->type == EV_KEY) {
        if (e->value == 2) return;
        pad_events++;
        int down = e->value != 0;
        switch (e->code) {
        case 0x130: x_fake(down ? X_BUTTONPRESS : X_BUTTONRELEASE, 1, 0, 0); injected++; break;   /* A */
        case 0x134: x_fake(down ? X_BUTTONPRESS : X_BUTTONRELEASE, 2, 0, 0); injected++; break;   /* X */
        case 0x131: x_fake(down ? X_BUTTONPRESS : X_BUTTONRELEASE, 3, 0, 0); injected++; break;   /* B */
        case 0x13b: key_tap_state(28, down); break;    /* Start: KEY_ENTER */
        case 0x13a: key_tap_state(1, down); break;     /* Back: KEY_ESC */
        case 0x133: key_tap_state(57, down); break;    /* Y: KEY_SPACE */
        case 0x136: key_tap_state(14, down); break;    /* LB: KEY_BACKSPACE */
        case 0x137: key_tap_state(15, down); break;    /* RB: KEY_TAB */
        case 0x13c: key_tap_state(125, down); break;   /* Guide: KEY_LEFTMETA */
        }
    } else if (e->type == EV_ABS) {
        static int hx, hy;
        if (e->code == 0) pad_lx = e->value;
        else if (e->code == 1) pad_ly = e->value;
        else if (e->code == 4) pad_ry = e->value;
        else if (e->code == 0x10 || e->code == 0x11) {   /* D-pad: arrows, press on change from 0, release back to 0 */
            int *h = e->code == 0x10 ? &hx : &hy;
            u32 neg = e->code == 0x10 ? 105u : 103u, pos = e->code == 0x10 ? 106u : 108u;   /* LEFT/RIGHT, UP/DOWN */
            if (*h < 0) key_tap_state(neg, 0); else if (*h > 0) key_tap_state(pos, 0);
            *h = e->value;
            if (*h < 0) key_tap_state(neg, 1); else if (*h > 0) key_tap_state(pos, 1);
            pad_events++;
        }
    }
}
static int stick_speed(int v)   /* pixels per 16 ms frame */
{
    int a = v < 0 ? -v : v;
    const int dz = 7000;
    if (a <= dz) return 0;
    long f = (long)(a - dz) * 1000 / (32767 - dz);   /* 0..1000 */
    long px = 1 + f * f * 24 / 1000000;              /* 1..25, quadratic */
    return (int)(v < 0 ? -px : px);
}
static void pad_frame(void)   /* each loop pass (~16 ms): the sticks' motion and scrolling */
{
    int dx = stick_speed(pad_lx), dy = stick_speed(pad_ly);
    if (dx || dy) { x_fake(X_MOTION, 1, dx, dy); injected++; }
    pad_scroll_acc += stick_speed(pad_ry);
    while (pad_scroll_acc >= 40 || pad_scroll_acc <= -40) {   /* one wheel click per 40 "pixels" */
        u32 b = pad_scroll_acc > 0 ? 5u : 4u;
        x_fake(X_BUTTONPRESS, b, 0, 0); x_fake(X_BUTTONRELEASE, b, 0, 0); injected += 2;
        pad_scroll_acc += pad_scroll_acc > 0 ? -40 : 40;
    }
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) xsc = native_sc;
    const char *xwd_path = 0, *tcp = 0, *unixp = 0;
    int seconds = 30, verify = 0, rel = 0, xscreen = 0, noinput = 0;
    const char *dump = 0;
    for (long i = 1; i < argc; i++) {
        const char *a = (const char *)sp[1 + i];
        const char *nx = i + 1 < argc ? (const char *)sp[2 + i] : "";
        if (streq(a, "--xwd")) { xwd_path = nx; i++; }
        else if (streq(a, "--tcp")) { tcp = nx; i++; }
        else if (streq(a, "--unix")) { unixp = nx; i++; }
        else if (streq(a, "--seconds")) { int v = 0; for (int k = 0; nx[k] >= '0' && nx[k] <= '9'; k++) v = v * 10 + nx[k] - '0'; seconds = v; i++; }
        else if (streq(a, "--verify")) verify = 1;
        else if (streq(a, "--rel")) rel = 1;
        else if (streq(a, "--dump")) { dump = nx; i++; }
        else if (streq(a, "--x")) xscreen = 1;
        else if (streq(a, "--fb")) { fb_path = nx; i++; }
        else if (streq(a, "--no-input")) noinput = 1;
    }
    if (!xwd_path && !xscreen) die("usage: xbridge (--x | --xwd <Xvfb_screen0>) [--tcp ip:port | --unix path] [--seconds N] [--verify] [--rel] [--dump file]", 0);
    if (xscreen && !tcp && !unixp) die("--x needs --unix or --tcp", 0);
    out("xbridge start\n");

    int kfd = -1, mfd = -1, afd = -1, pfd = -1;
    if (tcp || unixp) {
        int ct = 0;   /* the server may still be starting */
        for (;;) {
            if (tcp) x_connect_tcp(tcp); else x_connect_unix(unixp);
            if (xfd >= 0) break;
            if (++ct > 100) die("no X server", 0);
            long ts[2] = { 0, 200000000 }; sc2(35, ts, 0);
        }
        x_handshake();
        x_query_xtest();
    }
    if (xscreen) xscreen_setup();
    else {
        int tries = 0, orr;
        while ((orr = xwd_open(xwd_path)) != 0 && ++tries < 100) { long ts[2] = { 0, 100000000 }; sc2(35, ts, 0); }   /* a copier may be rewriting it */
        if (orr) die("cannot read the XWD screen file", orr);
        outn("XWD width", xw_w); outn("XWD height", xw_h);
    }
    fb_setup();
    cur = mmap_anon((u64)xw_line * xw_h); prev = mmap_anon((u64)xw_line * xw_h);
    if ((u64)cur < 0x10000 || (u64)prev < 0x10000) die("out of memory", 0);

    if ((tcp || unixp) && !noinput) {
        kfd = (int)sc3(2, "/dev/input/event0", 0x800, 0);
        afd = (int)sc3(2, "/dev/input/event2", 0x800, 0);
        if (rel) mfd = (int)sc3(2, "/dev/input/event1", 0x800, 0);
        if (kfd < 0 || afd < 0) die("cannot open the evdev devices", kfd < 0 ? kfd : afd);
        pfd = (int)sc3(2, "/dev/input/event3", 0x800, 0);   /* the gamepad: optional */
        out(pfd >= 0 ? "gamepad: event3 as mouse + keyboard\n" : "gamepad: none\n");
        sc3(72, xfd, 4, 0x800);   /* fcntl(F_SETFL, O_NONBLOCK) */
    } else if (!tcp && !unixp) out("no X server given: screen only\n");
    out("input-ready\n");   /* the test harness starts its clock here */

    u64 t0 = mono_ns(), next_screen = 0;
    while (mono_ns() - t0 < (u64)seconds * 1000000000ull) {
        struct pollfd pf[5]; int n = 0;
        if (kfd >= 0) { pf[n].fd = kfd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (afd >= 0) { pf[n].fd = afd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (mfd >= 0) { pf[n].fd = mfd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (pfd >= 0) { pf[n].fd = pfd; pf[n].events = 1; pf[n].revents = 0; n++; }
        if (n) sc3(7, pf, n, 16); else { long ts[2] = { 0, 16000000 }; sc2(35, ts, 0); }
        for (int d = 0; d < n; d++) {
            if (!(pf[d].revents & 1)) continue;
            struct input_event ev[32];
            for (;;) {
                long r = sc3(0, pf[d].fd, ev, sizeof ev);
                if (r <= 0) break;
                for (long i = 0; i < r / (long)sizeof ev[0]; i++) { if (pf[d].fd == pfd) on_pad_event(&ev[i]); else on_event(&ev[i]); }
            }
        }
        if (pfd >= 0 && xfd >= 0) pad_frame();
        if (xfd >= 0) x_drain();
        u64 now = mono_ns();
        if (now >= next_screen) { if (xscreen) xscreen_poll(); else screen_poll(); next_screen = now + 33000000ull; }
    }
    outn("pad events", pad_events);
    outn("screen frames", (long)frames); outn("rows written", (long)rows_sent); outn("X requests", nreq);
    outn("cursor draws", (long)cursor_draws);
    if (xscreen) outn("KiB fetched from the X server", (long)(bytes_fetched >> 10));
    if (verify && xfd >= 0) {
        x_query_pointer();
    }
    if (dump) {   /* the framebuffer as the host sees it, raw BGRX (convert on the host), for a check without a screenshot */
        int df = (int)sc3(2, dump, 0x241, 0644);   /* O_WRONLY|O_CREAT|O_TRUNC */
        u64 bytes = (u64)fb_line * fb_h;
        u8 *buf = mmap_anon(bytes);
        if (df >= 0 && (u64)buf > 0x10000 && sc4(17, fb, buf, bytes, 0) == (long)bytes) { sc3(1, df, buf, bytes); out("dumped the framebuffer\n"); }
        if (df >= 0) sc1(3, df);
    }
    sc1(231, 0);
    for (;;) {}
}

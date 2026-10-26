/*
 * fbdemo.c - freestanding guest for the display/input devices (build: build.sh -> out/fbdemo.elf).
 *
 * Opens /dev/fb0 (mmap, draws a test pattern), /dev/input/event0 (keyboard), event1 (mouse), event2 (absolute
 * pointer). Draws a cursor that follows the absolute pointer (event2) and the relative mouse (event1), a
 * strip of lamps for the keys held, and prints every key and button it sees.
 *
 *   fbdemo.elf                  interactive: runs until Esc or 60 s
 *   fbdemo.elf --selftest [--hold N]
 *       checks the device interfaces (ioctls, mmap, the events the host test harness injects with
 *       XRL_INPUT_SCRIPT, layer/tests/fbdemo-test.sh) and exits with the number of failed checks;
 *       with --hold N it then keeps the picture up for N seconds (screenshots).
 *   --seconds N                 run time of the interactive mode
 */
typedef unsigned long u64; typedef long s64; typedef unsigned int u32; typedef int s32; typedef unsigned char u8; typedef unsigned short u16;
typedef long (*xsc_t)(long, long, long, long, long, long, long);
xsc_t xsc;
#define AT_XRL_SYSCALL 0x5801

#define sc(n, a, b, c, d, e, f) xsc((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))
#define sc1(n, a) sc(n, a, 0, 0, 0, 0, 0)
#define sc2(n, a, b) sc(n, a, b, 0, 0, 0, 0)
#define sc3(n, a, b, c) sc(n, a, b, c, 0, 0, 0)

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
static int fails;
#define CHECK(cond, name) do { if (cond) out("  ok   " name "\n"); else { out("  FAIL " name "\n"); fails++; } } while (0)

__asm__(".text\n.globl _start\n_start:\n  xor %ebp,%ebp\n  mov %rsp,%rdi\n  and $-16,%rsp\n  call cstart\n  ud2\n");

static u64 mono_ns(void) { long ts[2]; sc2(228, 1, ts); return (u64)ts[0] * 1000000000ull + (u64)ts[1]; }

/* ---- linux uapi bits ----------------------------------------------------- */
struct input_event { long sec, usec; u16 type, code; s32 value; };
struct pollfd { int fd; short events, revents; };
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define IOC(dir, nr, sz) (((u64)(dir) << 30) | ((u64)(sz) << 16) | ((u64)'E' << 8) | (nr))
#define EVIOCGVERSION IOC(2, 0x01, 4)
#define EVIOCGNAME(len) IOC(2, 0x06, len)
#define EVIOCGBIT(ev, len) IOC(2, 0x20 + (ev), len)
#define EVIOCGABS(a) IOC(2, 0x40 + (a), 24)
#define EVIOCGKEY(len) IOC(2, 0x18, len)
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606
#define FBIO_WAITFORVSYNC 0x40044620ul
#define KEY_ESC 1
#define BTN_LEFT 0x110

static int tst(const u8 *bits, int n) { return (bits[n >> 3] >> (n & 7)) & 1; }

/* ---- the picture --------------------------------------------------------- */
static u32 *fbp;
static u32 xres, yres, stride_px;

static u32 pattern(int x, int y)
{
    if (y < 48) {   /* colour bars */
        int bar = x * 8 / (int)xres;
        static const u32 c[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
        return c[bar];
    }
    if (x % 80 == 0 || y % 80 == 0) return 0xc0c0c0;
    u32 r = (u32)(x * 255 / (int)xres), g = (u32)(y * 255 / (int)yres), b = (u32)(255 - r) / 2;
    return (r << 16) | (g << 8) | b;
}
static void paint(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= (int)yres) continue;
        for (int x = x0; x < x0 + w; x++) if (x >= 0 && x < (int)xres) fbp[y * stride_px + x] = pattern(x, y);
    }
}
static void cursor(int cx, int cy, u32 col)
{
    for (int d = -10; d <= 10; d++) {
        if (cx + d >= 0 && cx + d < (int)xres && cy >= 0 && cy < (int)yres) fbp[cy * stride_px + cx + d] = col;
        if (cy + d >= 0 && cy + d < (int)yres && cx >= 0 && cx < (int)xres) fbp[(cy + d) * stride_px + cx] = col;
    }
}

/* ---- state fed by the events --------------------------------------------- */
static int ax_c = -1, ay_c = -1;   /* absolute pointer, fb pixels */
static int rx_c, ry_c;             /* relative mouse position */
static int old_ax = -1, old_ay = -1, old_rx = -1, old_ry = -1;
static u8 held[256];
static int esc_pressed, nkeys, nbuttons, nrel, nabs;

static void lamps(void)   /* one 10x16 lamp per key code below 128 along the bottom */
{
    for (int k = 0; k < 128; k++) {
        int x = 20 + k * 9, y = (int)yres - 28;
        for (int dy = 0; dy < 16; dy++) for (int dx = 0; dx < 7; dx++) fbp[(y + dy) * stride_px + x + dx] = held[k] ? 0xffffff : 0x303030;
    }
}

static void key_name(char *dst, int code)
{
    static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`", row4[] = "zxcvbnm,./";
    dst[0] = 0;
    char c = 0;
    if (code >= 2 && code <= 13) c = row1[code - 2];
    else if (code >= 16 && code <= 27) c = row2[code - 16];
    else if (code >= 30 && code <= 41) c = row3[code - 30];
    else if (code >= 44 && code <= 53) c = row4[code - 44];
    if (c) { dst[0] = c; dst[1] = 0; return; }
    const char *n = code == 1 ? "Esc" : code == 14 ? "Backspace" : code == 15 ? "Tab" : code == 28 ? "Enter" : code == 29 ? "LCtrl" :
        code == 42 ? "LShift" : code == 54 ? "RShift" : code == 56 ? "LAlt" : code == 57 ? "Space" : code == 58 ? "CapsLock" :
        code == 103 ? "Up" : code == 105 ? "Left" : code == 106 ? "Right" : code == 108 ? "Down" : code == 0x110 ? "BTN_LEFT" :
        code == 0x111 ? "BTN_RIGHT" : code == 0x112 ? "BTN_MIDDLE" : "?";
    int i = 0; while (n[i]) { dst[i] = n[i]; i++; } dst[i] = 0;
}

static void on_event(int dev, const struct input_event *e)
{
    if (e->type == EV_KEY) {
        char nm[16]; key_name(nm, e->code);
        char b[96]; int n = 0;
        const char *p = dev == 0 ? "key " : "btn ";
        while (*p) b[n++] = *p++;
        for (int i = 0; nm[i]; i++) b[n++] = nm[i];
        p = e->value == 0 ? " up\n" : e->value == 1 ? " down\n" : " repeat\n";
        while (*p) b[n++] = *p++;
        b[n] = 0; out(b);
        if (e->code < 256) held[e->code] = e->value != 0;
        if (e->code == KEY_ESC && e->value == 1) esc_pressed = 1;
        if (dev == 0) nkeys++; else nbuttons++;
    } else if (e->type == EV_ABS) {
        if (e->code == 0) ax_c = e->value * (int)xres / 32768; else ay_c = e->value * (int)yres / 32768;
        nabs++;
    } else if (e->type == EV_REL) {
        if (e->code == 0) rx_c += e->value; else if (e->code == 1) ry_c += e->value;
        if (rx_c < 0) rx_c = 0;
        if (rx_c >= (int)xres) rx_c = (int)xres - 1;
        if (ry_c < 0) ry_c = 0;
        if (ry_c >= (int)yres) ry_c = (int)yres - 1;
        nrel++;
    }
}

static void redraw(void)
{
    if (old_ax >= 0) paint(old_ax - 11, old_ay - 11, 23, 23);
    if (old_rx >= 0) paint(old_rx - 11, old_ry - 11, 23, 23);
    if (ax_c >= 0) cursor(ax_c, ay_c, 0xffffff);          /* white: the absolute pointer */
    cursor(rx_c, ry_c, 0xff0000);                          /* red: the relative mouse */
    old_ax = ax_c; old_ay = ay_c; old_rx = rx_c; old_ry = ry_c;
    lamps();
}

/* Read what is queued on `fd`; returns the number of events (SYN included). */
static int drain(int fd, struct input_event *ev, int max)
{
    int n = 0;
    for (;;) {
        long r = sc3(0, fd, &ev[n], (max - n) * (long)sizeof ev[0]);
        if (r <= 0) break;
        n += (int)(r / (long)sizeof ev[0]);
        if (n >= max) break;
    }
    return n;
}

/* ---- the self-test -------------------------------------------------------- */
struct want { u16 type, code; s32 value; };

static int same(const struct want *a, const struct want *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i].type != b[i].type || a[i].code != b[i].code || a[i].value != b[i].value) return 0;
    return 1;
}

static void selftest_events(int kfd, int mfd, int afd)
{
    /* the harness script (layer/tests/fbdemo-test.sh): see its XRL_INPUT_SCRIPT */
    static const struct want wk[] = {
        { EV_KEY, 30, 1 }, { EV_KEY, 30, 0 }, { EV_KEY, 42, 1 }, { EV_KEY, 48, 1 }, { EV_KEY, 48, 0 }, { EV_KEY, 42, 0 },
        { EV_KEY, 30, 1 }, { EV_KEY, 30, 2 }, { EV_KEY, 30, 0 }, { EV_KEY, 30, 1 }, { EV_KEY, 30, 0 } };
    static const struct want wm[] = { { EV_REL, 0, 5 }, { EV_REL, 1, -3 } };
    static const struct want wa[] = { { EV_ABS, 0, 16384 }, { EV_ABS, 1, 8192 }, { EV_KEY, BTN_LEFT, 1 }, { EV_KEY, BTN_LEFT, 0 }, { EV_REL, 8, 1 } };
    out("input-ready\n");   /* the harness clock starts at this log line */
    /* keep the picture alive while the events arrive: drain the three devices round-robin into the demo state */
    u64 t0 = mono_ns();
    struct want gk[16], gm[16], ga[16];
    int nk = 0, nm = 0, na = 0, sk = 0, sm = 0, sa = 0;
    while (mono_ns() - t0 < 4000000000ull && (nk < 11 || nm < 2 || na < 5)) {
        struct pollfd p[3] = { { kfd, 1, 0 }, { mfd, 1, 0 }, { afd, 1, 0 } };
        if (sc3(7, p, 3, 50) <= 0) continue;
        for (int d = 0; d < 3; d++) {
            if (!(p[d].revents & 1)) continue;
            struct input_event ev[32];
            int k = drain(d == 0 ? kfd : d == 1 ? mfd : afd, ev, 32);
            for (int i = 0; i < k; i++) {
                if (ev[i].type == EV_SYN) { if (d == 0) sk++; else if (d == 1) sm++; else sa++; continue; }
                struct want w = { ev[i].type, ev[i].code, ev[i].value };
                if (d == 0 && nk < 16) gk[nk++] = w; else if (d == 1 && nm < 16) gm[nm++] = w; else if (d == 2 && na < 16) ga[na++] = w;
                on_event(d, &ev[i]);
            }
        }
        redraw();
    }
    outn("keyboard events", nk); outn("mouse events", nm); outn("abs events", na);
    CHECK(nk == 11 && same(gk, wk, 11), "event0: A, shift+B, auto-repeat, and the key released when the window loses focus");
    CHECK(sk == nk, "event0: one SYN_REPORT after each key event");
    CHECK(nm == 2 && same(gm, wm, 2), "event1: REL_X 5, REL_Y -3");
    CHECK(na == 5 && same(ga, wa, 5), "event2: ABS_X/ABS_Y, BTN_LEFT down/up and the wheel (buttons follow the open absolute pointer)");
    CHECK(sa >= 4, "event2: SYN_REPORTs (motion packet + two buttons + wheel)");
    CHECK(ax_c == 640 && ay_c == 180, "the absolute pointer lands at (640,180) in 1280x720 (16384/32768, 8192/32768)");
    CHECK(sm >= 1, "event1: SYN_REPORT after the motion");
}

static void selftest_ioctls(int kfd, int mfd, int afd, int fb)
{
    char name[64]; int ver = 0; u8 bits[16]; u32 ai[6];
    CHECK(sc3(16, kfd, EVIOCGVERSION, &ver) == 0 && ver == 0x010001, "EVIOCGVERSION = 0x010001");
    memset(name, 0, sizeof name);
    long r = sc3(16, kfd, EVIOCGNAME(64), name);
    CHECK(r > 0 && streq(name, "xbox-rl keyboard"), "event0 EVIOCGNAME 'xbox-rl keyboard'");
    memset(name, 0, sizeof name);
    sc3(16, afd, EVIOCGNAME(64), name);
    CHECK(streq(name, "xbox-rl pointer (absolute)"), "event2 EVIOCGNAME");
    memset(bits, 0, sizeof bits);
    CHECK(sc3(16, kfd, EVIOCGBIT(0, 4), bits) > 0 && tst(bits, EV_KEY) && !tst(bits, EV_REL) && !tst(bits, EV_ABS), "event0 EV bits: KEY, not REL/ABS");
    u8 kb[96]; memset(kb, 0, sizeof kb);
    CHECK(sc3(16, kfd, EVIOCGBIT(EV_KEY, 96), kb) > 0 && tst(kb, 30) && tst(kb, 57) && tst(kb, 1) && !tst(kb, BTN_LEFT), "event0 KEY bits: A, Space, Esc; no BTN_LEFT");
    memset(kb, 0, sizeof kb);
    CHECK(sc3(16, mfd, EVIOCGBIT(EV_KEY, 96), kb) > 0 && tst(kb, BTN_LEFT) && !tst(kb, 30), "event1 KEY bits: BTN_LEFT only");
    memset(bits, 0, sizeof bits);
    CHECK(sc3(16, mfd, EVIOCGBIT(EV_REL, 2), bits) > 0 && tst(bits, 0) && tst(bits, 1) && tst(bits, 8), "event1 REL bits: X, Y, WHEEL");
    memset(ai, 0, sizeof ai);
    CHECK(sc3(16, afd, EVIOCGABS(0), ai) == 0 && ai[1] == 0 && ai[2] == 32767, "event2 EVIOCGABS(ABS_X): 0..32767");
    CHECK(sc3(16, kfd, EVIOCGABS(0), ai) < 0, "event0 EVIOCGABS: EINVAL (no axes)");

    u32 var[40], fix[20];
    memset(var, 0, sizeof var); memset(fix, 0, sizeof fix);
    CHECK(sc3(16, fb, FBIOGET_VSCREENINFO, var) == 0 && var[0] == xres && var[1] == yres && var[6] == 32, "FBIOGET_VSCREENINFO: 32 bpp");
    CHECK(var[7 + 1] == 16 && var[7 + 4] == 8 && var[7 + 7] == 0, "FBIOGET_VSCREENINFO: red@16 green@8 blue@0 (BGRX)");
    CHECK(sc3(16, fb, FBIOGET_FSCREENINFO, fix) == 0 && fix[12] == xres * 4 && fix[6] >= xres * yres * 4, "FBIOGET_FSCREENINFO: line_length and smem_len");
    var[5] = 0;
    CHECK(sc3(16, fb, FBIOPAN_DISPLAY, var) == 0, "FBIOPAN_DISPLAY yoffset 0");
    var[5] = 5;
    CHECK(sc3(16, fb, FBIOPAN_DISPLAY, var) < 0, "FBIOPAN_DISPLAY beyond the virtual size: error");
    u32 sync = 0;
    CHECK(sc3(16, fb, FBIO_WAITFORVSYNC, &sync) == 0, "FBIO_WAITFORVSYNC");
    /* the mapping and read()/write() are the same pixels */
    u32 px = 0x00123456, back = 0;
    fbp[10 * stride_px + 20] = px;
    sc(17, fb, &back, 4, (10 * stride_px + 20) * 4, 0, 0);   /* pread64 */
    CHECK(back == px, "pread of /dev/fb0 sees what the mapping wrote");
    u32 w = 0x00654321;
    sc(18, fb, &w, 4, (11 * stride_px + 20) * 4, 0, 0);  /* pwrite64 */
    CHECK(fbp[11 * stride_px + 20] == w, "pwrite to /dev/fb0 shows in the mapping");
    paint(0, 0, (int)xres, (int)yres);
}

void cstart(u64 *sp)
{
    long argc = (long)sp[0];
    u64 *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    for (; p[0]; p += 2) if (p[0] == AT_XRL_SYSCALL) xsc = (xsc_t)p[1];
    if (!xsc) { __asm__ volatile("ud2"); }
    int selftest = 0, hold = 0, seconds = 60;
    for (long i = 1; i < argc; i++) {
        const char *a = (const char *)sp[1 + i];
        const char *nx = i + 1 < argc ? (const char *)sp[2 + i] : "0";
        int v = 0; for (int k = 0; nx[k] >= '0' && nx[k] <= '9'; k++) v = v * 10 + nx[k] - '0';
        if (streq(a, "--selftest")) selftest = 1;
        else if (streq(a, "--hold")) hold = v;
        else if (streq(a, "--seconds")) seconds = v;
    }
    out("fbdemo start\n");
    int fb = (int)sc3(2, "/dev/fb0", 2, 0);
    int kfd = (int)sc3(2, "/dev/input/event0", 0x800 | 0, 0);   /* O_NONBLOCK */
    int mfd = (int)sc3(2, "/dev/input/event1", 0x800 | 0, 0);
    int afd = (int)sc3(2, "/dev/input/event2", 0x800 | 0, 0);
    CHECK(fb >= 0, "open /dev/fb0");
    CHECK(kfd >= 0 && mfd >= 0 && afd >= 0, "open /dev/input/event0, event1, event2");
    if (fb < 0 || kfd < 0 || mfd < 0 || afd < 0) { outn("fb", fb); outn("kbd", kfd); sc1(231, 100 + fails); }
    u32 var[40], fix[20];
    sc3(16, fb, FBIOGET_VSCREENINFO, var); sc3(16, fb, FBIOGET_FSCREENINFO, fix);
    xres = var[0]; yres = var[1]; stride_px = fix[12] / 4;
    outn("xres", xres); outn("yres", yres);
    fbp = (u32 *)sc(9, 0, fix[6], 3, 1, fb, 0);   /* PROT_READ|WRITE, MAP_SHARED */
    CHECK((long)fbp > 0 && (long)fbp != -1 && (u64)fbp > 0x10000, "mmap /dev/fb0 shared read/write");
    if ((u64)fbp < 0x10000) sc1(231, 100 + fails);
    paint(0, 0, (int)xres, (int)yres);
    rx_c = (int)xres / 2; ry_c = (int)yres / 2;
    if (selftest) {
        selftest_events(kfd, mfd, afd);
        selftest_ioctls(kfd, mfd, afd, fb);
        /* leave the picture the events drew: pointer at (640,180), then hold it for screenshots */
        ax_c = 640; ay_c = 180; old_ax = old_ay = -1; old_rx = -1;
        paint(0, 0, (int)xres, (int)yres);
        redraw();
        u64 t0 = mono_ns();
        while (mono_ns() - t0 < (u64)hold * 1000000000ull) { long ts[2] = { 0, 50000000 }; sc2(35, ts, 0); }
        outn("TOTAL FAILS", fails);
        sc1(231, fails);
    }
    /* interactive */
    out("input-ready\n");
    u64 t0 = mono_ns();
    while (!esc_pressed && mono_ns() - t0 < (u64)seconds * 1000000000ull) {
        struct pollfd pf[3] = { { kfd, 1, 0 }, { mfd, 1, 0 }, { afd, 1, 0 } };
        sc3(7, pf, 3, 16);
        for (int d = 0; d < 3; d++) {
            struct input_event ev[32];
            int k = drain(d == 0 ? kfd : d == 1 ? mfd : afd, ev, 32);
            for (int i = 0; i < k; i++) on_event(d, &ev[i]);
        }
        redraw();
    }
    outn("keys", nkeys); outn("buttons", nbuttons); outn("abs", nabs); outn("rel", nrel);
    sc1(231, fails);
    for (;;) {}
}

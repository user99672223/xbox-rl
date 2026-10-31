/*
 * evdev.c - /dev/input/event0..2: the host's keyboard and mouse as Linux evdev devices.
 *
 *   event0  keyboard       EV_KEY (KEY_*), EV_REP, EV_SYN
 *   event1  mouse          EV_KEY (BTN_LEFT..BTN_EXTRA), EV_REL (X, Y, HWHEEL, WHEEL)
 *   event2  absolute       EV_KEY (BTN_LEFT..), EV_REL (HWHEEL, WHEEL), EV_ABS (X, Y: 0..32767) - what a
 *                          VM's USB tablet looks like to Xorg/libinput, so the cursor follows the host's
 *   event3  gamepad 0      what the kernel's xpad driver makes of an Xbox One pad: BTN_SOUTH/EAST/NORTH/WEST,
 *                          TL/TR, SELECT/START/MODE, THUMBL/R; ABS_X/Y/RX/RY (-32768..32767), ABS_Z/RZ triggers
 *                          (0..1023), ABS_HAT0X/Y (-1..1). Polled from xrl_input_ops get_pad_state(0) at 60 Hz
 *                          while it is open (the TV bridge turns it into a mouse + keyboard)
 *
 * Source: xrl_input_ops (ABI v3) read_events/wait_events. One pump thread (started by the first open)
 * drains the host queue, routes each event to the devices' clients, and wakes pollers with
 * xrl_io_notify(). Each open file has its own event ring, filled only while it is open, as in the
 * kernel. Events reach the pointer device the guest is actually using: with an event2 client open the host
 * is in absolute mode and buttons/wheel go to event2; with only event1 clients the host is put in
 * relative mode (cursor captured) and they go to event1.
 *
 * The uapi structs and ioctls are Linux's (include/uapi/linux/input.h): the EVIOCG* queries libevdev,
 * Xorg's evdev/libinput drivers and evtest make are answered from the per-device tables below.
 */
#include "xrl_internal.h"

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define EV_MSC 4
#define EV_REP 0x14
#define EV_CNT 0x20
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define KEY_CNT 0x300
#define REL_CNT 0x10
#define ABS_CNT 0x40
#define LXD_BTN_LEFT 0x110
#define LXD_BTN_EXTRA 0x114
#define LXD_REL_X 0
#define LXD_REL_Y 1
#define LXD_REL_HWHEEL 6
#define LXD_REL_WHEEL 8
#define MAX_CLIENTS 16
#define RING 512

typedef struct lx_input_event { s64 sec, usec; u16 type, code; s32 value; } lx_input_event;   /* 24 bytes */
typedef struct lx_input_absinfo { s32 value, minimum, maximum, fuzz, flat, resolution; } lx_input_absinfo;

typedef struct evclient evclient;
typedef struct evdev_dev {
    const char *name;
    u16 product;
    u8 evbits[(EV_CNT + 7) / 8];
    u8 keybits[(KEY_CNT + 7) / 8];
    u8 relbits[(REL_CNT + 7) / 8];
    u8 absbits[(ABS_CNT + 7) / 8];
    u8 keystate[(KEY_CNT + 7) / 8];
    int absmax;                       /* ABS_X/ABS_Y maximum, 0 = no abs axes */
    int nclients;
    evclient *client[MAX_CLIENTS];
    evclient *grab;
} evdev_dev;

struct evclient {
    evdev_dev *dev;
    lx_input_event ring[RING];
    unsigned head, count;
    int dropped;                      /* SYN_DROPPED owed to the reader */
};

enum { DEV_KBD, DEV_MOUSE, DEV_ABS, DEV_PAD, NDEV };
#define PAD_HAT0X 0x10
#define PAD_HAT0Y 0x11
static evdev_dev g_dev[NDEV];
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_inited;
static volatile LONG g_pump_started;

static void setbit(u8 *b, unsigned n) { b[n >> 3] |= (u8)(1u << (n & 7)); }
static int  getbit(const u8 *b, unsigned n) { return (b[n >> 3] >> (n & 7)) & 1; }

static void dev_init_all(void)
{
    if (g_inited) return;
    g_inited = 1;
    evdev_dev *k = &g_dev[DEV_KBD], *m = &g_dev[DEV_MOUSE], *a = &g_dev[DEV_ABS];
    k->name = "xbox-rl keyboard"; k->product = 1;
    setbit(k->evbits, EV_SYN); setbit(k->evbits, EV_KEY); setbit(k->evbits, EV_REP);
    for (unsigned c = 1; c <= 248; c++) setbit(k->keybits, c);   /* KEY_ESC..KEY_MICMUTE */
    m->name = "xbox-rl mouse"; m->product = 2;
    setbit(m->evbits, EV_SYN); setbit(m->evbits, EV_KEY); setbit(m->evbits, EV_REL);
    for (unsigned c = LXD_BTN_LEFT; c <= LXD_BTN_EXTRA; c++) setbit(m->keybits, c);
    setbit(m->relbits, LXD_REL_X); setbit(m->relbits, LXD_REL_Y); setbit(m->relbits, LXD_REL_HWHEEL); setbit(m->relbits, LXD_REL_WHEEL);
    a->name = "xbox-rl pointer (absolute)"; a->product = 3;
    setbit(a->evbits, EV_SYN); setbit(a->evbits, EV_KEY); setbit(a->evbits, EV_REL); setbit(a->evbits, EV_ABS);
    for (unsigned c = LXD_BTN_LEFT; c <= LXD_BTN_EXTRA; c++) setbit(a->keybits, c);
    setbit(a->relbits, LXD_REL_HWHEEL); setbit(a->relbits, LXD_REL_WHEEL);
    setbit(a->absbits, 0); setbit(a->absbits, 1);
    a->absmax = XRL_ABS_MAX;
    evdev_dev *p = &g_dev[DEV_PAD];
    p->name = "Microsoft X-Box One pad"; p->product = 4;
    setbit(p->evbits, EV_SYN); setbit(p->evbits, EV_KEY); setbit(p->evbits, EV_ABS);
    static const u16 keys[] = { 0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e };
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) setbit(p->keybits, keys[i]);
    static const u8 axes[] = { 0, 1, 2, 3, 4, 5, PAD_HAT0X, PAD_HAT0Y };
    for (size_t i = 0; i < ARRAY_LEN(axes); i++) setbit(p->absbits, axes[i]);
}

/* the pad's axis ranges (EVIOCGABS), as xpad reports them */
static int pad_absinfo(unsigned axis, s32 *mn, s32 *mx, s32 *fuzz, s32 *flat)
{
    switch (axis) {
    case 0: case 1: case 3: case 4: *mn = -32768; *mx = 32767; *fuzz = 16; *flat = 128; return 1;
    case 2: case 5: *mn = 0; *mx = 1023; *fuzz = 0; *flat = 0; return 1;
    case PAD_HAT0X: case PAD_HAT0Y: *mn = -1; *mx = 1; *fuzz = 0; *flat = 0; return 1;
    }
    return 0;
}

/* ---- host side ---------------------------------------------------------------------------- */
#define HOST_HAS(op) (G.ctx.input && G.ctx.input->struct_size >= offsetof(xrl_input_ops, op) + sizeof(void *) && G.ctx.input->op)

static void apply_mouse_mode_locked(void)
{
    if (!HOST_HAS(set_mouse_mode)) return;
    unsigned mode = g_dev[DEV_ABS].nclients ? XRL_MOUSE_ABSOLUTE : g_dev[DEV_MOUSE].nclients ? XRL_MOUSE_RELATIVE : XRL_MOUSE_ABSOLUTE;
    G.ctx.input->set_mouse_mode(mode);
}

static void push(evclient *c, u16 type, u16 code, s32 value, u64 real_ns)
{
    if (c->count >= RING - 1) { c->dropped = 1; return; }   /* keep a slot for the SYN_REPORT */
    lx_input_event *e = &c->ring[(c->head + c->count++) % RING];
    e->sec = (s64)(real_ns / 1000000000ull); e->usec = (s64)(real_ns % 1000000000ull / 1000);
    e->type = type; e->code = code; e->value = value;
}

static void deliver(int di, u16 type, u16 code, s32 value, int syn, u64 ns)
{
    evdev_dev *d = &g_dev[di];
    if (type == EV_KEY) { if (value) setbit(d->keystate, code); else d->keystate[code >> 3] &= (u8)~(1u << (code & 7)); }
    for (int i = 0; i < d->nclients; i++) {
        evclient *c = d->client[i];
        if (d->grab && c != d->grab) continue;
        push(c, type, code, value, ns);
        if (syn) push(c, EV_SYN, SYN_REPORT, 0, ns);
    }
}

/* Route a batch the way the header comment says. SYN_REPORT after every event except between the
 * two axes of one motion (X then Y), which the kernel reports as one packet. */
static void route(const xrl_input_event *ev, unsigned n)
{
    u64 ns = xrl_real_ns();
    AcquireSRWLockExclusive(&g_lock);
    int ptr_rel = g_dev[DEV_ABS].nclients == 0;   /* buttons and wheel follow the device in use */
    for (unsigned i = 0; i < n; i++) {
        const xrl_input_event *e = &ev[i];
        int syn = 1;
        if ((e->type == XRL_IEV_REL || e->type == XRL_IEV_ABS) && i + 1 < n && ev[i + 1].type == e->type && ev[i + 1].code > e->code) syn = 0;
        switch (e->type) {
        case XRL_IEV_KEY:
            if (e->code < 0x100) deliver(DEV_KBD, EV_KEY, e->code, e->value, syn, ns);
            else deliver(ptr_rel ? DEV_MOUSE : DEV_ABS, EV_KEY, e->code, e->value, syn, ns);
            break;
        case XRL_IEV_REL:
            if (e->code == LXD_REL_WHEEL || e->code == LXD_REL_HWHEEL) deliver(ptr_rel ? DEV_MOUSE : DEV_ABS, EV_REL, e->code, e->value, syn, ns);
            else deliver(DEV_MOUSE, EV_REL, e->code, e->value, syn, ns);
            break;
        case XRL_IEV_ABS:
            deliver(DEV_ABS, EV_ABS, e->code, e->value, syn, ns);
            break;
        default: break;   /* TEXT and FOCUS have no evdev form (the X server has its own keymap) */
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    xrl_io_notify();
}

/* gamepad 0 -> event3: the state's changes as events (one SYN_REPORT per poll) */
static xrl_pad_state g_pad_last;
static s32 g_pad_abs[0x12];
static void pad_poll(void)
{
    xrl_pad_state st;
    if (G.ctx.input->get_pad_state(0, &st) != XRL_OK || !st.connected) memset(&st, 0, sizeof st);
    static const struct { u32 mask; u16 code; } bt[] = {
        { 0x1000, 0x130 }, { 0x2000, 0x131 }, { 0x8000, 0x133 }, { 0x4000, 0x134 }, { 0x0100, 0x136 }, { 0x0200, 0x137 },
        { 0x0020, 0x13a }, { 0x0010, 0x13b }, { 0x0400, 0x13c }, { 0x0040, 0x13d }, { 0x0080, 0x13e } };
    s32 ax[0x12] = { 0 };
    ax[0] = st.thumb_lx; ax[1] = -(s32)st.thumb_ly - (st.thumb_ly == -32768 ? 1 : 0);   /* evdev: down is + */
    ax[3] = st.thumb_rx; ax[4] = -(s32)st.thumb_ry - (st.thumb_ry == -32768 ? 1 : 0);
    ax[2] = st.left_trigger * 1023 / 255; ax[5] = st.right_trigger * 1023 / 255;
    ax[PAD_HAT0X] = (st.buttons & 0x8 ? 1 : 0) - (st.buttons & 0x4 ? 1 : 0);
    ax[PAD_HAT0Y] = (st.buttons & 0x2 ? 1 : 0) - (st.buttons & 0x1 ? 1 : 0);
    u64 ns = xrl_real_ns();
    int any = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < ARRAY_LEN(bt); i++)
        if ((st.buttons ^ g_pad_last.buttons) & bt[i].mask) { deliver(DEV_PAD, EV_KEY, bt[i].code, (st.buttons & bt[i].mask) ? 1 : 0, 0, ns); any = 1; }
    for (unsigned a = 0; a < 0x12; a++)
        if (getbit(g_dev[DEV_PAD].absbits, a) && ax[a] != g_pad_abs[a]) { deliver(DEV_PAD, EV_ABS, (u16)a, ax[a], 0, ns); g_pad_abs[a] = ax[a]; any = 1; }
    if (any) for (int i = 0; i < g_dev[DEV_PAD].nclients; i++) push(g_dev[DEV_PAD].client[i], EV_SYN, SYN_REPORT, 0, ns);
    ReleaseSRWLockExclusive(&g_lock);
    g_pad_last = st;
    if (any) xrl_io_notify();
}

static DWORD WINAPI pump_main(void *arg)
{
    (void)arg;
    xrl_input_event buf[64];
    for (;;) {
        if (G.exiting) return 0;
        int pad = g_dev[DEV_PAD].nclients > 0 && HOST_HAS(get_pad_state);
        if (pad) pad_poll();
        if (G.ctx.input->wait_events(pad ? 16 : 250) != XRL_OK) continue;
        unsigned n;
        while ((n = G.ctx.input->read_events(buf, 64)) != 0) route(buf, n);
    }
}

static int pump_start(void)
{
    if (g_pump_started) return 0;
    if (!HOST_HAS(read_events) || !HOST_HAS(wait_events)) return -LX_ENODEV;   /* an ABI v1/v2 host */
    if (InterlockedCompareExchange(&g_pump_started, 1, 0)) return 0;          /* another opener won the race */
    HANDLE h = CreateThread(NULL, 0, pump_main, NULL, 0, NULL);
    if (!h) { g_pump_started = 0; return -LX_ENOMEM; }
    CloseHandle(h);
    LOGI("evdev: input pump started (host queue -> /dev/input/event0..2, gamepad 0 -> event3)");
    return 0;
}

/* ---- file operations ---------------------------------------------------------------------- */
static slong ev_read(xfile *f, void *buf, u64 n, s64 off)
{
    (void)off;
    evclient *c = (evclient *)f->priv;
    if (n < sizeof(lx_input_event)) return -LX_EINVAL;
    u64 want = n / sizeof(lx_input_event);
    for (;;) {
        LONG seen = xrl_io_epoch;
        AcquireSRWLockExclusive(&g_lock);
        if (c->dropped && c->count < RING) {   /* tell the reader its view is stale, then SYN_REPORT resumes */
            c->dropped = 0;
            c->head = (c->head + RING - 1) % RING; c->count++;
            lx_input_event *e = &c->ring[c->head];
            memset(e, 0, sizeof *e); e->type = EV_SYN; e->code = SYN_DROPPED;
        }
        if (c->count) {
            u64 k = c->count < want ? c->count : want;
            lx_input_event tmp[32];
            u64 done = 0;
            slong r = 0;
            while (done < k) {
                u64 chunk = k - done < 32 ? k - done : 32;
                for (u64 i = 0; i < chunk; i++) tmp[i] = c->ring[(c->head + i) % RING];
                if (xrl_ctu((u64)buf + done * sizeof(lx_input_event), tmp, (size_t)(chunk * sizeof(lx_input_event)))) { r = -LX_EFAULT; break; }
                c->head = (c->head + (unsigned)chunk) % RING; c->count -= (unsigned)chunk;
                done += chunk;
            }
            ReleaseSRWLockExclusive(&g_lock);
            return r ? r : (slong)(done * sizeof(lx_input_event));
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        if (xrl_io_wait(seen, -1) < 0) return -LX_EINTR;
    }
}

static slong ev_write(xfile *f, const void *b, u64 n, s64 off)   /* LED/repeat settings: accepted, no effect */
{
    (void)f; (void)b; (void)off;
    return (slong)n;
}

static u32 ev_poll(xfile *f, u32 want)
{
    evclient *c = (evclient *)f->priv;
    AcquireSRWLockShared(&g_lock);
    u32 r = (c->count || c->dropped) ? LX_POLLIN : 0;
    ReleaseSRWLockShared(&g_lock);
    return r & (want | LX_POLLERR | LX_POLLHUP);
}

static void ev_close(xfile *f)
{
    evclient *c = (evclient *)f->priv;
    if (!c) return;
    evdev_dev *d = c->dev;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < d->nclients; i++)
        if (d->client[i] == c) { d->client[i] = d->client[--d->nclients]; break; }
    if (d->grab == c) d->grab = NULL;
    apply_mouse_mode_locked();
    ReleaseSRWLockExclusive(&g_lock);
    free(c);
}

static slong ev_fstat(xfile *f, lx_stat *st)
{
    evclient *c = (evclient *)f->priv;
    int di = (int)(c->dev - g_dev);
    xrl_stat_synth(st, LX_S_IFCHR | 0660, 0x7000 + di, 0);
    st->st_rdev = (13u << 8) | (64u + di);
    return 0;
}

static slong copy_bits(u64 arg, size_t len, const u8 *bits, size_t nbytes)
{
    u8 *tmp = (u8 *)calloc(1, len ? len : 1);
    if (!tmp) return -LX_ENOMEM;
    memcpy(tmp, bits, len < nbytes ? len : nbytes);
    slong r = xrl_ctu(arg, tmp, len) ? -LX_EFAULT : (slong)len;
    free(tmp);
    return r;
}

static slong copy_str(u64 arg, size_t len, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > len) n = len;
    if (!n) return 0;
    return xrl_ctu(arg, s, n) ? -LX_EFAULT : (slong)n;
}

/* _IOC(dir, 'E', nr, size) decoding: nr = req & 0xff, size = (req >> 16) & 0x3fff */
static slong ev_ioctl(xfile *f, u64 req, u64 arg)
{
    evclient *c = (evclient *)f->priv;
    evdev_dev *d = c->dev;
    if (req == LX_FIOCLEX || req == LX_FIONCLEX) return 0;
    if (req == LX_FIONBIO) { int on = 0; if (xrl_cfu(&on, arg, 4)) return -LX_EFAULT; if (on) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
    if (((req >> 8) & 0xff) != 'E') return -LX_ENOTTY;
    unsigned nr = (unsigned)(req & 0xff);
    size_t len = (size_t)((req >> 16) & 0x3fff);
    int in = (int)(req >> 30) & 1;
    if (nr >= 0x20 && nr < 0x20 + EV_CNT) {   /* EVIOCGBIT(ev, len) */
        unsigned ev = nr - 0x20;
        if (ev == 0) return copy_bits(arg, len, d->evbits, sizeof d->evbits);
        if (ev == EV_KEY) return copy_bits(arg, len, d->keybits, sizeof d->keybits);
        if (ev == EV_REL) return copy_bits(arg, len, d->relbits, sizeof d->relbits);
        if (ev == EV_ABS) return copy_bits(arg, len, d->absbits, sizeof d->absbits);
        return copy_bits(arg, len, d->evbits, 0);   /* a type the device does not have: all zero */
    }
    if (nr >= 0x40 && nr < 0x40 + ABS_CNT && !in) {   /* EVIOCGABS(abs) */
        unsigned axis = nr - 0x40;
        lx_input_absinfo ai = { 0, 0, d->absmax, 0, 0, 0 };
        if (d == &g_dev[DEV_PAD]) {
            if (!pad_absinfo(axis, &ai.minimum, &ai.maximum, &ai.fuzz, &ai.flat)) return -LX_EINVAL;
            AcquireSRWLockShared(&g_lock); ai.value = axis < 0x12 ? g_pad_abs[axis] : 0; ReleaseSRWLockShared(&g_lock);
        } else if (!d->absmax || axis > 1) return -LX_EINVAL;
        return xrl_ctu(arg, &ai, len < sizeof ai ? len : sizeof ai) ? -LX_EFAULT : 0;
    }
    switch (nr) {
    case 0x01: { int v = 0x010001; return xrl_ctu(arg, &v, 4) ? -LX_EFAULT : 0; }                 /* EVIOCGVERSION */
    case 0x02: {                                                                                   /* EVIOCGID */
        u16 id[4] = { 0x06 /* BUS_VIRTUAL */, 0x1d6b, d->product, 1 };
        if (d == &g_dev[DEV_PAD]) { id[0] = 0x03 /* BUS_USB */; id[1] = 0x045e; id[2] = 0x02ea; id[3] = 0x0301; }   /* Xbox One S pad (SDL's mapping) */
        return xrl_ctu(arg, id, 8) ? -LX_EFAULT : 0;
    }
    case 0x03: {                                                                                   /* EVIOCGREP / EVIOCSREP */
        if (in) return 0;
        int rep[2] = { 250, 33 };
        return xrl_ctu(arg, rep, 8) ? -LX_EFAULT : 0;
    }
    case 0x06: return copy_str(arg, len, d->name);                                                 /* EVIOCGNAME */
    case 0x07: return copy_str(arg, len, "xbox-rl/input0");                                        /* EVIOCGPHYS */
    case 0x08: return -LX_ENOENT;                                                                  /* EVIOCGUNIQ */
    case 0x09: return copy_bits(arg, len, d->evbits, 0);                                           /* EVIOCGPROP: none */
    case 0x18: {                                                                                   /* EVIOCGKEY */
        AcquireSRWLockShared(&g_lock);
        slong r = copy_bits(arg, len, d->keystate, sizeof d->keystate);
        ReleaseSRWLockShared(&g_lock);
        return r;
    }
    case 0x19: case 0x1a: case 0x1b: return copy_bits(arg, len, d->evbits, 0);                    /* EVIOCGLED/SND/SW: none lit */
    case 0x90: {                                                                                   /* EVIOCGRAB */
        int on = (int)arg;
        AcquireSRWLockExclusive(&g_lock);
        slong r = 0;
        if (on) { if (d->grab && d->grab != c) r = -LX_EBUSY; else d->grab = c; }
        else if (d->grab == c) d->grab = NULL;
        else r = -LX_EINVAL;
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }
    case 0x91: return 0;                                                                           /* EVIOCREVOKE */
    case 0xa0: return 0;                                                                           /* EVIOCSCLOCKID (realtime is what we stamp) */
    case 0x92: case 0x93: return -LX_EINVAL;                                                       /* EVIOCGMASK/SMASK */
    }
    return -LX_ENOTTY;
}

static const xfile_ops g_ev_ops = { "evdev", ev_read, ev_write, NULL, ev_fstat, NULL, ev_ioctl, NULL, NULL, ev_poll, ev_close, NULL };

/* ---- vfs hooks ---------------------------------------------------------------------------- */
static int dev_index(const char *gabs)
{
    if (strncmp(gabs, "/dev/input/event", 16) || !gabs[16] || gabs[17]) return -1;
    int n = gabs[16] - '0';
    if (n == DEV_PAD && xrl_fork_tasks_enabled()) return -1;   /* Rocket League/EAC: Wine has its own gamepad path */
    return n >= 0 && n < NDEV ? n : -1;
}

xfile *xrl_dev_evdev_open(const char *gabs, int flags, int *err)
{
    int di = dev_index(gabs);
    if (di < 0) return NULL;
    dev_init_all();
    int r = pump_start();
    if (r < 0) { *err = -r; return NULL; }
    evclient *c = (evclient *)calloc(1, sizeof *c);
    xfile *f = c ? xfile_new(&g_ev_ops, flags) : NULL;
    if (!f) { free(c); *err = LX_ENOMEM; return NULL; }
    evdev_dev *d = &g_dev[di];
    c->dev = d;
    f->priv = c;
    AcquireSRWLockExclusive(&g_lock);
    if (d->nclients >= MAX_CLIENTS) { ReleaseSRWLockExclusive(&g_lock); free(c); f->priv = NULL; xfile_unref(f); *err = LX_EMFILE; return NULL; }
    d->client[d->nclients++] = c;
    apply_mouse_mode_locked();
    ReleaseSRWLockExclusive(&g_lock);
    return f;
}

int xrl_dev_evdev_stat(const char *gabs, lx_stat *st)
{
    int di = dev_index(gabs);
    if (di < 0) return 0;
    xrl_stat_synth(st, LX_S_IFCHR | 0660, 0x7000 + di, 0);
    st->st_rdev = (13u << 8) | (64u + di);
    return 1;
}

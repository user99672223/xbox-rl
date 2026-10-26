/*
 * fbdev.c - /dev/fb0: a Linux framebuffer device whose pixels the host shows in the app window.
 *
 * Geometry: 32 bits per pixel (B,G,R,X in memory = DXGI B8G8R8X8), default 1280x720, XRL_FB=WxH to
 * change it before boot, FBIOPUT_VSCREENINFO afterwards (a virtual height up to 4x the visible one
 * enables FBIOPAN_DISPLAY page flipping). The usual fbdev surface is implemented: FBIOGET_VSCREENINFO,
 * FBIOPUT_VSCREENINFO, FBIOGET_FSCREENINFO, FBIOPAN_DISPLAY, FBIO_WAITFORVSYNC, FBIOBLANK, plus read/write/
 * lseek and mmap - what Xorg's fbdev driver, SDL's fbcon, fbi and `cat image > /dev/fb0` use.
 *
 * Pixels: the device owns `vram`. A MAP_SHARED mmap is ordinary guest memory filled from vram (the layer's
 * file mappings are copies); the mmap hooks in xfile_ops tell this file where that memory is and when it
 * goes away, and from then on that mapping, not vram, is what the screen shows (and what read/write touch).
 * There is no scan-out hardware to ask, so a presenter thread watches the visible rows (a memcmp against its
 * last copy, about every refresh) and when they changed copies them into a staging buffer under the device
 * lock and hands that to the host: xrl_gfx_ops.present_framebuffer, which scales to the window and waits for
 * vblank, so the screen updates at the display's refresh rate at most. The staging copy lets munmap proceed
 * while the host is blocked in vblank.
 *
 * One framebuffer, one live mapping: a second mapping replaces the first as the source.
 */
#include "xrl_internal.h"

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP 0x4604
#define FBIOPUTCMAP 0x4605
#define FBIOPAN_DISPLAY 0x4606
#define FBIOBLANK 0x4611
#define FBIO_WAITFORVSYNC 0x40044620u   /* _IOW('F', 0x20, __u32) */
#define FB_ACTIVATE_TEST 2u
#define FB_ACTIVATE_MASK 15u
#define FB_VISUAL_TRUECOLOR 2
#define FB_MAXVIRT 4                    /* virtual height / visible height */

typedef struct fb_bitfield { u32 offset, length, msb_right; } fb_bitfield;
typedef struct fb_var_screeninfo {
    u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    fb_bitfield red, green, blue, transp;
    u32 nonstd, activate, height, width, accel_flags;
    u32 pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    u32 reserved[4];
} fb_var_screeninfo;
typedef struct fb_fix_screeninfo {
    char id[16];
    u64 smem_start;
    u32 smem_len, type, type_aux, visual;
    u16 xpanstep, ypanstep, ywrapstep;
    u32 line_length;
    u64 mmio_start;
    u32 mmio_len, accel;
    u16 capabilities, reserved[2];
} fb_fix_screeninfo;
C_ASSERT(sizeof(fb_var_screeninfo) == 160);
C_ASSERT(sizeof(fb_fix_screeninfo) == 80);

static struct {
    SRWLOCK lock;
    int inited;
    u32 xres, yres, yvirt, yoff;
    u8 *vram;                      /* line * yvirt bytes */
    u64 map_addr, map_len;         /* the live guest mapping, 0 = none */
    int nopen, blank;
    u8 *stage;                     /* what the presenter last handed to the host */
    size_t stage_cap;
    int have_stage;
    HANDLE presenter;
    volatile LONG vsync_seq;       /* bumped after each frame handed over, for FBIO_WAITFORVSYNC */
    volatile LONG frames;
} fb = { SRWLOCK_INIT };

#define LINE(f) ((f).xres * 4u)
#define VRAM_BYTES(f) ((size_t)LINE(f) * (f).yvirt)

#define HOST_GFX_HAS(op) (G.ctx.gfx && G.ctx.gfx->struct_size >= offsetof(xrl_gfx_ops, op) + sizeof(void *) && G.ctx.gfx->op)
#define HOST_IN_HAS(op) (G.ctx.input && G.ctx.input->struct_size >= offsetof(xrl_input_ops, op) + sizeof(void *) && G.ctx.input->op)

static int geometry_ok(u32 w, u32 h, u32 vh) { return w >= 64 && w <= 4096 && h >= 64 && h <= 4096 && vh >= h && vh <= h * FB_MAXVIRT; }

static void tell_input_content(void)
{
    if (HOST_IN_HAS(set_content_size)) G.ctx.input->set_content_size(fb.xres, fb.yres);
}

/* lock held; the mapping (if any) must already be absent or the same size */
static int geometry_set(u32 w, u32 h, u32 vh)
{
    size_t bytes = (size_t)w * 4 * vh;
    u8 *nv = (u8 *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!nv) return -LX_ENOMEM;
    if (fb.vram) {   /* keep the picture's top-left where it was when the pitch changes */
        size_t ol = LINE(fb), nl = (size_t)w * 4;
        for (u32 y = 0; y < fb.yvirt && y < vh; y++) memcpy(nv + y * nl, fb.vram + y * ol, ol < nl ? ol : nl);
        VirtualFree(fb.vram, 0, MEM_RELEASE);
    }
    fb.vram = nv;
    fb.xres = w; fb.yres = h; fb.yvirt = vh;
    if (fb.yoff > vh - h) fb.yoff = vh - h;
    fb.have_stage = 0;
    return 0;
}

static void var_fill(fb_var_screeninfo *v)
{
    memset(v, 0, sizeof *v);
    v->xres = v->xres_virtual = fb.xres;
    v->yres = fb.yres; v->yres_virtual = fb.yvirt;
    v->yoffset = fb.yoff;
    v->bits_per_pixel = 32;
    v->red.offset = 16; v->red.length = 8;
    v->green.offset = 8; v->green.length = 8;
    v->blue.offset = 0; v->blue.length = 8;
    v->transp.offset = 24; v->transp.length = 0;
    v->activate = 0;
    v->width = fb.xres * 254 / 960;    /* mm at 96 dpi */
    v->height = fb.yres * 254 / 960;
    v->pixclock = 6000;                 /* ps: nominal, there is no CRTC */
    v->left_margin = v->right_margin = 48; v->upper_margin = 16; v->lower_margin = 8; v->hsync_len = 32; v->vsync_len = 8;
}

/* Copy `src` into `stage` if it differs from what is there (or `have` is 0): 1 changed, 0 same, -1 the
 * guest memory cannot be read (the guest changed its protection under us). */
static int snapshot(u8 *stage, const u8 *src, size_t bytes, int have)
{
    __try {
        if (have && !memcmp(stage, src, bytes)) return 0;
        memcpy(stage, src, bytes);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

/* the live mapping is the screen only if it covers the whole virtual surface (a guest that mapped less keeps vram as the screen) */
static int map_covers(void) { return fb.map_addr && fb.map_len >= VRAM_BYTES(fb); }

/* ---- the presenter ------------------------------------------------------------------------ */
static DWORD WINAPI presenter_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (G.exiting) return 0;
        int live;
        u32 w = 0, h = 0, stride = 0;
        int changed = 0;
        AcquireSRWLockExclusive(&fb.lock);
        live = fb.nopen > 0 || fb.map_addr;
        if (live && fb.vram && !fb.blank) {
            const u8 *src = (map_covers() ? (const u8 *)fb.map_addr : fb.vram) + (size_t)fb.yoff * LINE(fb);
            size_t bytes = (size_t)LINE(fb) * fb.yres;
            if (fb.stage_cap < bytes) { free(fb.stage); fb.stage = (u8 *)malloc(bytes); fb.stage_cap = fb.stage ? bytes : 0; fb.have_stage = 0; }
            int c = fb.stage ? snapshot(fb.stage, src, bytes, fb.have_stage) : 0;
            if (c < 0) { fb.map_addr = 0; fb.map_len = 0; fb.have_stage = 0; LOGW("fbdev: the mapped framebuffer is unreadable (mprotect/munmap race?); showing vram again"); }
            else if (c) { fb.have_stage = 1; changed = 1; w = fb.xres; h = fb.yres; stride = LINE(fb); }
        }
        ReleaseSRWLockExclusive(&fb.lock);
        if (!changed) { Sleep(live ? 8 : 100); continue; }
        /* the staging buffer is only touched by this thread; blocks until vblank */
        if (HOST_GFX_HAS(present_framebuffer)) {
            xrl_status st = G.ctx.gfx->present_framebuffer(fb.stage, w, h, stride, 0);
            if (st != XRL_OK) { LOGW("fbdev: present_framebuffer failed (%d)", (int)st); Sleep(100); }
        } else Sleep(16);
        InterlockedIncrement(&fb.frames);
        InterlockedIncrement(&fb.vsync_seq);
        xrl_io_notify();
    }
}

/* lock held */
static void presenter_start(void)
{
    if (fb.presenter) return;
    fb.presenter = CreateThread(NULL, 0, presenter_main, NULL, 0, NULL);
    if (fb.presenter) LOGI("fbdev: presenter thread started (%ux%u, %s)", fb.xres, fb.yres,
                           HOST_GFX_HAS(present_framebuffer) ? "host present_framebuffer" : "NO host present op: nothing will be shown");
}

/* ---- file operations ---------------------------------------------------------------------- */
static u8 *src_locked(void) { return map_covers() ? (u8 *)fb.map_addr : fb.vram; }

static slong fb_rw(xfile *f, void *buf, u64 n, s64 off, int is_write)
{
    if (n > ((u64)1 << 30)) n = (u64)1 << 30;
    if (xrl_mem_populate((u64)buf, n)) return -LX_ENOMEM;   /* lazy user pages commit now, not under fb.lock */
    AcquireSRWLockExclusive(&fb.lock);
    u64 size = (u64)VRAM_BYTES(fb);
    u64 pos = off >= 0 ? (u64)off : (u64)f->pos;
    slong r = 0;
    if (pos >= size) r = is_write && n ? -LX_ENOSPC : 0;
    else {
        if (n > size - pos) n = size - pos;
        if (is_write) r = xrl_cfu(src_locked() + pos, (u64)buf, (size_t)n) ? -LX_EFAULT : (slong)n;
        else r = xrl_ctu((u64)buf, src_locked() + pos, (size_t)n) ? -LX_EFAULT : (slong)n;
        if (r > 0 && off < 0) f->pos = (s64)(pos + (u64)r);
    }
    ReleaseSRWLockExclusive(&fb.lock);
    return r;
}
static slong fb_read(xfile *f, void *b, u64 n, s64 o) { return fb_rw(f, b, n, o, 0); }
static slong fb_write(xfile *f, const void *b, u64 n, s64 o) { return fb_rw(f, (void *)b, n, o, 1); }

static slong fb_lseek(xfile *f, s64 off, int whence)
{
    AcquireSRWLockShared(&fb.lock);
    s64 size = (s64)VRAM_BYTES(fb), np;
    ReleaseSRWLockShared(&fb.lock);
    if (whence == 0) np = off; else if (whence == 1) np = f->pos + off; else if (whence == 2) np = size + off; else return -LX_EINVAL;
    if (np < 0) return -LX_EINVAL;
    f->pos = np;
    return np;
}

static slong fb_fstat(xfile *f, lx_stat *st)
{
    (void)f;
    xrl_stat_synth(st, LX_S_IFCHR | 0660, 0x7100, 0);
    st->st_rdev = (29u << 8) | 0;
    return 0;
}

static u32 fb_poll(xfile *f, u32 want) { (void)f; return want & (LX_POLLIN | LX_POLLOUT); }

static slong ioc_get_var(u64 arg)
{
    fb_var_screeninfo v;
    AcquireSRWLockShared(&fb.lock);
    var_fill(&v);
    ReleaseSRWLockShared(&fb.lock);
    return xrl_ctu(arg, &v, sizeof v) ? -LX_EFAULT : 0;
}

static slong ioc_put_var(u64 arg)
{
    fb_var_screeninfo v;
    if (xrl_cfu(&v, arg, sizeof v)) return -LX_EFAULT;
    if (v.bits_per_pixel != 32) return -LX_EINVAL;
    u32 w = v.xres, h = v.yres, vh = v.yres_virtual < v.yres ? v.yres : v.yres_virtual;
    if (v.xres_virtual != w && v.xres_virtual) return -LX_EINVAL;   /* no horizontal panning */
    if (!geometry_ok(w, h, vh)) return -LX_EINVAL;
    slong r = 0;
    int changed = 0;
    AcquireSRWLockExclusive(&fb.lock);
    if (w != fb.xres || h != fb.yres || vh != fb.yvirt) {
        if ((v.activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_TEST) { ReleaseSRWLockExclusive(&fb.lock); return 0; }
        if (fb.map_addr && (u64)w * 4 * vh > fb.map_len) r = -LX_EBUSY;   /* the live mapping could not hold it */
        else if (!(r = geometry_set(w, h, vh))) changed = 1;
    }
    if (!r && v.yoffset <= fb.yvirt - fb.yres && (v.activate & FB_ACTIVATE_MASK) != FB_ACTIVATE_TEST) fb.yoff = v.yoffset;
    ReleaseSRWLockExclusive(&fb.lock);
    if (changed) { tell_input_content(); LOGI("fbdev: mode set to %ux%u (virtual height %u)", w, h, vh); }
    return r;
}

static slong ioc_get_fix(u64 arg)
{
    fb_fix_screeninfo x;
    memset(&x, 0, sizeof x);
    memcpy(x.id, "xbox-rl fb", 11);
    AcquireSRWLockShared(&fb.lock);
    x.smem_start = 0;
    x.smem_len = (u32)VRAM_BYTES(fb);
    x.line_length = LINE(fb);
    ReleaseSRWLockShared(&fb.lock);
    x.type = 0;                      /* FB_TYPE_PACKED_PIXELS */
    x.visual = FB_VISUAL_TRUECOLOR;
    x.ypanstep = 1;
    return xrl_ctu(arg, &x, sizeof x) ? -LX_EFAULT : 0;
}

static slong fb_ioctl(xfile *f, u64 req, u64 arg)
{
    (void)f;
    switch (req) {
    case LX_FIOCLEX: case LX_FIONCLEX: return 0;
    case LX_FIONBIO: return 0;
    case FBIOGET_VSCREENINFO: return ioc_get_var(arg);
    case FBIOPUT_VSCREENINFO: return ioc_put_var(arg);
    case FBIOGET_FSCREENINFO: return ioc_get_fix(arg);
    case FBIOPAN_DISPLAY: {
        fb_var_screeninfo v;
        if (xrl_cfu(&v, arg, sizeof v)) return -LX_EFAULT;
        slong r = 0;
        AcquireSRWLockExclusive(&fb.lock);
        if (v.xoffset || v.yoffset > fb.yvirt - fb.yres) r = -LX_EINVAL; else fb.yoff = v.yoffset;
        ReleaseSRWLockExclusive(&fb.lock);
        return r;
    }
    case FBIOBLANK: AcquireSRWLockExclusive(&fb.lock); fb.blank = arg != 0; fb.have_stage = 0; ReleaseSRWLockExclusive(&fb.lock); return 0;
    case FBIO_WAITFORVSYNC: {   /* until the presenter has handed over another frame (or 50 ms: it hands nothing over when nothing changed) */
        LONG seen = fb.vsync_seq;
        xrl_io_wait(seen, 50000000);
        return 0;
    }
    case FBIOGETCMAP: case FBIOPUTCMAP: return -LX_EINVAL;   /* truecolor: no palette */
    }
    return -LX_ENOTTY;
}

static void fb_mapped(xfile *f, u64 addr, u64 len, s64 off)
{
    (void)f;
    if (off) return;
    /* g_lock (memory manager) is held by our caller; fb.lock is only ever taken below it or alone */
    AcquireSRWLockExclusive(&fb.lock);
    fb.map_addr = addr; fb.map_len = len;
    fb.have_stage = 0;
    presenter_start();
    ReleaseSRWLockExclusive(&fb.lock);
}

static void fb_unmapped(xfile *f, u64 addr, u64 len)
{
    (void)f;
    AcquireSRWLockExclusive(&fb.lock);
    if (fb.map_addr && addr < fb.map_addr + fb.map_len && addr + len > fb.map_addr) {   /* any overlap: the mapping is gone */
        fb.map_addr = 0; fb.map_len = 0;
        fb.have_stage = 0;
    }
    ReleaseSRWLockExclusive(&fb.lock);
}

static void fb_close(xfile *f)
{
    (void)f;
    AcquireSRWLockExclusive(&fb.lock);
    fb.nopen--;
    ReleaseSRWLockExclusive(&fb.lock);
}

static const xfile_ops g_fb_ops = { "fb", fb_read, fb_write, fb_lseek, fb_fstat, NULL, fb_ioctl, NULL, NULL, fb_poll, fb_close, NULL,
                                    fb_mapped, fb_unmapped };

xfile *xrl_dev_fb_open(const char *gabs, int flags, int *err)
{
    if (strcmp(gabs, "/dev/fb0")) return NULL;
    xfile *f = xfile_new(&g_fb_ops, flags);
    if (!f) { *err = LX_ENOMEM; return NULL; }
    AcquireSRWLockExclusive(&fb.lock);
    if (!fb.inited) {
        u32 w = 1280, h = 720;
        char e[32];
        if (GetEnvironmentVariableA("XRL_FB", e, sizeof e)) {
            unsigned a, b;
            if (sscanf(e, "%ux%u", &a, &b) == 2 && geometry_ok(a, b, b)) { w = a; h = b; }
            else LOGW("fbdev: ignoring XRL_FB=%s (want WxH, 64..4096)", e);
        }
        if (geometry_set(w, h, h)) { ReleaseSRWLockExclusive(&fb.lock); xfile_unref(f); *err = LX_ENOMEM; return NULL; }
        fb.inited = 1;
        LOGI("fbdev: /dev/fb0 %ux%u 32bpp BGRX", w, h);
        tell_input_content();
    }
    fb.nopen++;
    presenter_start();
    ReleaseSRWLockExclusive(&fb.lock);
    return f;
}

int xrl_dev_fb_stat(const char *gabs, lx_stat *st)
{
    if (strcmp(gabs, "/dev/fb0")) return 0;
    xrl_stat_synth(st, LX_S_IFCHR | 0660, 0x7100, 0);
    st->st_rdev = (29u << 8) | 0;
    return 1;
}

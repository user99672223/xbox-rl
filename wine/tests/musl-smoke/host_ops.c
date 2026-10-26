/*
 * TEST-ONLY fake "host": the xrl_*_ops tables a real host app would install, built into the
 * laptop-only hosttest libc next to the pass-through shim (shim.S).  It lets the Wine forwarders
 * (xinput, d3d11, d3d12, dxgi swapchains) be exercised end to end on a PC:
 *
 *   PE game code -> xinput1_4.dll / d3d11.dll -> xrlhost.dll -> xrlhost.so
 *        -> xrl_syscall(0x1000x) -> this file
 *
 * The pseudo-syscall numbers are ABI-CHANGE-REQUESTS.md #1.
 *
 * The gfx handles are fake native COM objects.  Their "methods" use the MS-x64 ABI (what real
 * native COM code looks like to a PE caller): this models, in miniature, the contract of
 * ABI-CHANGE-REQUESTS.md #2 / gfx R1 -- "handles are directly callable COM objects".  A magic
 * number sits at offset 8 so a test can recognise an object without calling it.
 */
#include "xrl_abi.h"

extern long write(int, const void *, unsigned long);
static void say(const char *s) { unsigned long n = 0; while (s[n]) n++; write(2, s, n); }

struct fakecom { void **vtbl; unsigned long long magic; };
#define MAGIC_DEV12   0xD3D12DE5ull
#define MAGIC_QUEUE   0x0D3D12C0ull
#define MAGIC_DEV11   0x0D3D11DEull
#define MAGIC_CTX11   0x0D3D11C7ull
#define MAGIC_SWAP    0x05C4A100ull

static struct fakecom fake_dev12, fake_queue, fake_dev11, fake_ctx11, fake_swap;

/* ID3D12Device::CreateCommandQueue is vtable slot 8 (IUnknown 3 + ID3D12Object 4 + GetNodeCount) */
static long __attribute__((ms_abi)) ht_CreateCommandQueue(void *self, const void *desc, const void *riid, void **out)
{
    (void)self; (void)desc; (void)riid;
    *out = &fake_queue;
    say("[host] ID3D12Device::CreateCommandQueue (called directly by the guest)\n");
    return 0;
}
static void *dev12_vtbl[64] = { [8] = (void *)ht_CreateCommandQueue };
static void *plain_vtbl[64];
static void init_fakes(void)
{
    fake_dev12 = (struct fakecom){ dev12_vtbl, MAGIC_DEV12 };
    fake_queue = (struct fakecom){ plain_vtbl, MAGIC_QUEUE };
    fake_dev11 = (struct fakecom){ plain_vtbl, MAGIC_DEV11 };
    fake_ctx11 = (struct fakecom){ plain_vtbl, MAGIC_CTX11 };
    fake_swap  = (struct fakecom){ plain_vtbl, MAGIC_SWAP };
}

/* ---- input ---- */
static xrl_status ht_get_pad_state(uint32_t slot, xrl_pad_state *out)
{
    if (slot != 0) return XRL_ERR_NOTAVAIL;
    out->buttons = 0x1000 /* A */ | 0x0001 /* DPAD_UP */;
    out->left_trigger = 200; out->right_trigger = 7;
    out->thumb_lx = 12345; out->thumb_ly = -321; out->thumb_rx = 0; out->thumb_ry = 1;
    out->connected = 1;
    return XRL_OK;
}

static xrl_status ht_set_pad_vibration(uint32_t slot, uint16_t l, uint16_t r)
{
    (void)l; (void)r;
    if (slot != 0) return XRL_ERR_NOTAVAIL;
    say("[host] set_pad_vibration\n");
    return XRL_OK;
}

/* ---- gfx ---- */
static xrl_status ht_d3d12_create_device(xrl_handle *out) { init_fakes(); *out = (xrl_handle)&fake_dev12; return XRL_OK; }

static xrl_status ht_create_swapchain(xrl_handle device, xrl_handle queue, uint32_t w, uint32_t h,
                                      uint32_t n, uint32_t fmt, xrl_handle *out)
{
    (void)w; (void)h; (void)n; (void)fmt;
    /* gfx/ contract: device = the D3D12 device, queue = the guest-created queue, both non-NULL */
    if (device != (xrl_handle)&fake_dev12 || queue != (xrl_handle)&fake_queue) return XRL_ERR_GENERIC;
    *out = (xrl_handle)&fake_swap;
    say("[host] create_swapchain_for_corewindow\n");
    return XRL_OK;
}

static xrl_status ht_get_buffer(xrl_handle sc, uint32_t i, xrl_handle *out) { (void)sc; (void)i; *out = 0xB0F; return XRL_OK; }
static xrl_status ht_present(xrl_handle sc, uint32_t si, uint32_t fl) { (void)sc; (void)si; (void)fl; return XRL_OK; }

static xrl_status ht_11on12(xrl_handle d12, xrl_handle q, xrl_handle *d11, xrl_handle *c11)
{
    if (d12 != (xrl_handle)&fake_dev12 || q != (xrl_handle)&fake_queue) return XRL_ERR_GENERIC;  /* queue is required */
    *d11 = (xrl_handle)&fake_dev11;
    *c11 = (xrl_handle)&fake_ctx11;
    say("[host] d3d11on12_create\n");
    return XRL_OK;
}

static void ht_release(xrl_handle h) { (void)h; say("[host] release\n"); }

static const xrl_input_ops input_ops = { sizeof(xrl_input_ops), ht_get_pad_state, ht_set_pad_vibration };
static const xrl_gfx_ops gfx_ops = { sizeof(xrl_gfx_ops), ht_d3d12_create_device, ht_create_swapchain,
                                     ht_get_buffer, ht_present, ht_11on12, ht_release };

/* called from shim.S for nr >= 0x10000 */
long xrl_hosttest_pseudo(long nr)
{
    switch (nr)
    {
    case 0x10001: return (long)&gfx_ops;
    case 0x10003: return (long)&input_ops;
    default:      return -XRL_ENOSYS;           /* 0x10002 audio: no audio ops in the fake host */
    }
}

/*
 * xrl_abi.h — xbox-rl shared ABI contract.
 *
 * This header is the FROZEN seam between the four workstreams. It is owned by
 * the integrator (top level), not by any single subagent. Changing anything in
 * here is a cross-workstream event: it must be discussed, versioned via
 * XRL_ABI_VERSION, and recorded in docs/decisions.md.
 *
 * Three seams live here:
 *
 *   1. The syscall gateway (wine/ -> layer/). musl's __syscall and Wine's few
 *      raw syscall sites are patched to call xrl_syscall() instead of emitting
 *      a `syscall` instruction. The binary therefore contains no syscall
 *      instructions and runs native on the locked-down Windows host.
 *
 *   2. The boot/loader entry (host/ -> layer/). The UWP host hands the layer a
 *      fully-populated boot context and asks it to load the ELF image and run.
 *
 *   3. The host op-table (wine/ -> host/ + gfx/). Wine's builtin d3d12/dxgi/
 *      d3d11/xaudio2/xinput DLLs forward to native Xbox D3D12/XAudio2/
 *      Windows.Gaming.Input through this vtable, which the host installs before
 *      the guest starts. The Wine side never touches a WinRT or D3D type
 *      directly; everything crosses as opaque handles + plain C.
 *
 * Calling convention for every function pointer and entry point here is the
 * platform C ABI (System V on the unix side as Wine sees it; the layer owns the
 * thunk to the host's native convention). Everything is extern "C".
 */
#ifndef XRL_ABI_H
#define XRL_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XRL_ABI_VERSION 1u

/* ------------------------------------------------------------------ *
 * Seam 1: the syscall gateway  (wine/ -> layer/)
 * ------------------------------------------------------------------ */

/*
 * The one entry every patched syscall site funnels through. `nr` is a Linux
 * x86-64 syscall number (see docs/syscall-abi.md for the frozen subset the
 * layer actually implements). Arguments are passed as 64-bit values; unused
 * trailing arguments are ignored by the callee.
 *
 * Return value follows the Linux kernel convention, NOT glibc/musl: success is
 * a non-negative value (or a pointer cast to long); failure is the negated
 * errno in the range [-4095, -1]. The musl __syscall shim that B patches in
 * returns this value verbatim, so musl's own errno translation keeps working
 * unchanged. The layer must never set a Windows last-error or throw across
 * this boundary.
 *
 * This call may block (futex, read, poll, wineserver round-trip). It is
 * reentrant and thread-safe: it is called from every guest thread.
 */
long xrl_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/*
 * Linux errno values the layer returns (negated) across xrl_syscall. Only the
 * ones the implemented subset can produce are enumerated; keep in sync with
 * docs/syscall-abi.md. Defined here so B and C agree on the numbers without
 * pulling in a Linux <errno.h>.
 */
enum {
    XRL_EPERM   = 1,   XRL_ENOENT  = 2,   XRL_ESRCH   = 3,   XRL_EINTR   = 4,
    XRL_EIO     = 5,   XRL_EBADF   = 9,   XRL_EAGAIN  = 11,  XRL_ENOMEM  = 12,
    XRL_EACCES  = 13,  XRL_EFAULT  = 14,  XRL_EBUSY   = 16,  XRL_EEXIST  = 17,
    XRL_ENODEV  = 19,  XRL_ENOTDIR = 20,  XRL_EISDIR  = 21,  XRL_EINVAL  = 22,
    XRL_ENFILE  = 23,  XRL_EMFILE  = 24,  XRL_ENOSPC  = 28,  XRL_ESPIPE  = 29,
    XRL_EPIPE   = 32,  XRL_ERANGE  = 34,  XRL_ENOSYS  = 38,  XRL_ENOTSUP = 95,
    XRL_ETIMEDOUT = 110,
};

/* ------------------------------------------------------------------ *
 * Seam 3: the host op-table  (wine/ -> host/ + gfx/)
 *
 * Declared before the boot context because the boot context carries a pointer
 * to it. Handles are opaque on the Wine side: a d3d12_device is really a native
 * ID3D12Device*, but B only ever sees xrl_handle.
 * ------------------------------------------------------------------ */

typedef uint64_t xrl_handle;          /* opaque; 0 means null/invalid        */
#define XRL_INVALID_HANDLE ((xrl_handle)0)

/* Result codes for host ops. 0 == success; negative == failure. These are NOT
 * HRESULTs; the host maps native HRESULTs to these so the Wine side needs no
 * Windows headers. On failure the Wine DLL synthesizes the matching DXGI/
 * HRESULT for the guest. */
typedef int32_t xrl_status;
#define XRL_OK            ((xrl_status)0)
#define XRL_ERR_GENERIC   ((xrl_status)-1)
#define XRL_ERR_NOTAVAIL  ((xrl_status)-2)   /* feature absent on this console */
#define XRL_ERR_DEVICELOST ((xrl_status)-3)

/*
 * Graphics forwarding. The host owns the CoreWindow and the native D3D12
 * device + swapchain (created with CreateSwapChainForCoreWindow on the host's
 * UI thread — see decisions.md: D3D11CreateDevice on the UI thread hangs on
 * Xbox, so device creation is marshalled to the right thread by the host).
 *
 * Wine's dxgi/d3d12 builtin calls create_device() once, then drives the native
 * device through the returned handle. d3d11.dll uses d3d11on12_create() which
 * wraps the SAME d3d12 device (shared queue) per the D3D11On12 contract.
 */
typedef struct xrl_gfx_ops {
    uint32_t struct_size;             /* sizeof(xrl_gfx_ops) for versioning   */

    /* Native D3D12 device bound to the Xbox adapter. out_device receives an
     * opaque handle wrapping ID3D12Device. */
    xrl_status (*d3d12_create_device)(xrl_handle *out_device);

    /* Create the flip-model swapchain against the host CoreWindow. `queue` is
     * the guest's direct command queue handle. width/height 0 = use window. */
    xrl_status (*create_swapchain_for_corewindow)(xrl_handle device,
                                                  xrl_handle queue,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t buffer_count,
                                                  uint32_t dxgi_format,
                                                  xrl_handle *out_swapchain);
    xrl_status (*swapchain_get_buffer)(xrl_handle swapchain, uint32_t index,
                                       xrl_handle *out_resource);
    xrl_status (*swapchain_present)(xrl_handle swapchain, uint32_t sync_interval,
                                    uint32_t flags);

    /* D3D11On12 wrapper over the existing d3d12 device/queue. Only reached if
     * the native D3D11On12 path is viable; otherwise the host leaves this NULL
     * and the Wine side falls back to DXVK->Dozen (see gfx/ branch). */
    xrl_status (*d3d11on12_create)(xrl_handle d3d12_device,
                                   xrl_handle d3d12_queue,
                                   xrl_handle *out_d3d11_device,
                                   xrl_handle *out_d3d11_context);

    /* Generic COM-style release for any handle this table produced. */
    void       (*release)(xrl_handle h);
} xrl_gfx_ops;

/* Audio forwarding -> native XAudio2 (xaudio2_9.dll on the console). */
typedef struct xrl_audio_ops {
    uint32_t struct_size;
    xrl_status (*xaudio2_create)(xrl_handle *out_xaudio2);
    xrl_status (*create_mastering_voice)(xrl_handle xaudio2, uint32_t channels,
                                         uint32_t sample_rate,
                                         xrl_handle *out_voice);
    xrl_status (*create_source_voice)(xrl_handle xaudio2,
                                      uint32_t channels, uint32_t sample_rate,
                                      uint32_t bits_per_sample,
                                      xrl_handle *out_voice);
    xrl_status (*voice_submit_buffer)(xrl_handle voice, const void *data,
                                      uint32_t bytes, uint32_t flags);
    xrl_status (*voice_start)(xrl_handle voice);
    xrl_status (*voice_stop)(xrl_handle voice);
    void       (*release)(xrl_handle h);
} xrl_audio_ops;

/* Input forwarding -> native Windows.Gaming.Input (+ SDL WinRT keyboard). */
typedef struct xrl_pad_state {
    uint32_t buttons;        /* XINPUT-compatible button bitmask              */
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    uint8_t  connected;      /* 0/1                                           */
    uint8_t  _pad[3];
} xrl_pad_state;

typedef struct xrl_input_ops {
    uint32_t struct_size;
    /* Poll gamepad `slot` (0..3). Returns XRL_OK and fills `out` if present,
     * XRL_ERR_NOTAVAIL if that slot is empty. */
    xrl_status (*get_pad_state)(uint32_t slot, xrl_pad_state *out);
    xrl_status (*set_pad_vibration)(uint32_t slot, uint16_t left_motor,
                                    uint16_t right_motor);
} xrl_input_ops;

/* ------------------------------------------------------------------ *
 * Seam 2: the boot context  (host/ -> layer/)
 * ------------------------------------------------------------------ */

/*
 * The host logging callback. The layer and the Wine side both log through this
 * so everything lands in LocalState/log.txt in order. level: 0=trace 1=info
 * 2=warn 3=error. `msg` is a NUL-terminated UTF-8 string already formatted.
 */
typedef void (*xrl_log_fn)(int level, const char *tag, const char *msg);

typedef struct xrl_boot_context {
    uint32_t struct_size;             /* sizeof(xrl_boot_context)             */
    uint32_t abi_version;             /* must equal XRL_ABI_VERSION           */

    /* Filesystem roots, UTF-8, host (Win32) paths. The VFS maps the guest's
     * unix tree onto these. vfs_root is writable LocalState; it holds the
     * pre-baked wine prefix, /tmp, and the guest's view of "C:". */
    const char *vfs_root;             /* e.g. <LocalState>\\rootfs            */
    const char *prefix_path;          /* guest path to the wine prefix (/prefix) */
    const char *game_path;            /* guest path to the game install        */

    /* argv/envp handed to the guest's ELF entry (the wine loader). Arrays are
     * NULL-terminated. This is where "RocketLeague.exe -nocrashdialog ..." and
     * the anti-cheat-off launch option are passed. */
    const char *const *argv;
    const char *const *envp;

    /* Services the host provides to the layer + Wine side. */
    xrl_log_fn            log;
    const xrl_gfx_ops    *gfx;        /* never NULL                            */
    const xrl_audio_ops  *audio;      /* never NULL                            */
    const xrl_input_ops  *input;      /* never NULL                            */

    /* Memory budget, bytes. The console reserves a large VA range at launch;
     * the layer must keep its ELF image, heaps and mmap arena inside this.
     * See docs/decisions.md on the address-space hole. */
    uint64_t guest_mem_budget;
} xrl_boot_context;

/*
 * The layer's single entry point, called by the host after it has installed
 * the op-tables and populated the boot context. It maps the ELF (Blink loader),
 * wires xrl_syscall, starts wineserver-as-thread, and transfers control to the
 * guest. Returns the guest's process exit code, or a negative xrl_status if the
 * image could not be brought up. Blocks until the guest exits.
 */
int xrl_layer_boot(const xrl_boot_context *ctx);

/*
 * Optional: ask a running layer to begin orderly shutdown (e.g. the host's
 * CoreApplication Suspending handler). Safe to call from another thread.
 */
void xrl_layer_request_exit(int exit_code);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XRL_ABI_H */

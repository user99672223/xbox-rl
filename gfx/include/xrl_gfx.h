/*
 * xrl_gfx.h — host-facing entry API of the gfx/ library (Workstream D).
 *
 * This is NOT part of the frozen ABI (include/xrl_abi.h). The frozen ABI is the
 * xrl_gfx_ops vtable that the Wine side reaches. This header is what the HOST
 * (Workstream A) and the probe/selftest call.
 *
 * PRODUCTION ENTRY (the seam A already built, host/src/gfx_seam.h):
 *
 *     const xrl_gfx_ops *xrl_gfx_ops_get(const xrl_gfx_host_services *host);
 *
 * gfx.lib exports it as a strong symbol; it overrides the host's
 * /alternatename default. The host passes its services (log, CoreWindow getter,
 * window size, run_on_gfx_thread); gfx/ initialises itself from them and returns
 * the vtable for xrl_boot_context.gfx. After that, optionally call
 * xrl_gfx_preflight() (before xrl_layer_boot) and xrl_gfx_report_caps().
 *
 * xrl_gfx_init / xrl_gfx_config is the lower-level, standalone path (desktop
 * harness; hosts that do not use the services seam).
 *
 * HANDLES: an xrl_handle is the raw COM interface pointer (ID3D12Device*,
 * ID3D12CommandQueue*, IDXGISwapChain3*, ID3D12Resource*, ID3D11Device*,
 * ID3D11DeviceContext*), owning one reference, dropped by ops->release(). That is
 * the convention include/xrl_abi.h describes, and what lets the layer thunk native
 * vtable calls on a handle. The guest creates its OWN command queue through such a
 * thunk (ID3D12Device::CreateCommandQueue) and passes that queue handle in.
 *
 * THREADING: no function here assumes or requires the CoreWindow UI thread. All
 * device / swapchain / D3D11On12 creation runs on the gfx thread (the host's, via
 * host->run_on_gfx_thread; else a library-owned MTA worker) because
 * D3D11CreateDevice/D3D12CreateDevice on the UI thread blocks indefinitely on
 * Xbox (docs/decisions.md). Creation waits with a timeout and fails loudly rather
 * than hanging forever. Present/GetBuffer/Release run on the caller's thread.
 *
 * Everything is extern "C"; no D3D/WinRT type appears here (C-includable).
 */
#ifndef XRL_GFX_H
#define XRL_GFX_H

#include "xrl_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XRL_GFX_SHARED)
#  if defined(XRL_GFX_BUILDING)
#    define XRL_GFX_API __declspec(dllexport)
#  else
#    define XRL_GFX_API __declspec(dllimport)
#  endif
#else
#  define XRL_GFX_API
#endif

#define XRL_GFX_API_VERSION 1u

/* Defined by the host in host/src/gfx_seam.h (mirrored privately in gfx/src). */
typedef struct xrl_gfx_host_services xrl_gfx_host_services;

/* ------------------------------------------------------------------ *
 * Production entry (A's seam) + lifecycle
 * ------------------------------------------------------------------ */

/* See header comment. Idempotent: a second call re-binds the services and returns
 * the same table. Never blocks on D3D (no device is created here). */
XRL_GFX_API const xrl_gfx_ops *xrl_gfx_ops_get(const xrl_gfx_host_services *host);

enum {
    /* Enable the D3D12/DXGI debug layers if present (usually absent on the
     * console; failure to enable is logged and ignored). */
    XRL_GFX_FLAG_DEBUG_LAYER        = 1u << 0,
    /* Create the swapchain on the CALLING thread instead of the gfx thread.
     * Escape hatch if CreateSwapChainForCoreWindow turns out to need the caller's
     * thread. Device creation is never inline. */
    XRL_GFX_FLAG_SWAPCHAIN_INLINE   = 1u << 1,
    /* Pretend D3D11On12 does not work (forces the DXVK->Dozen decision, to test
     * the fallback plumbing): d3d11on12_create becomes NULL. */
    XRL_GFX_FLAG_FORCE_NO_D3D11ON12 = 1u << 2,
    /* Accept a software (WARP) adapter when no hardware adapter exists. Desktop
     * test builds only; never on the console. */
    XRL_GFX_FLAG_ALLOW_WARP         = 1u << 3,
};

typedef struct xrl_gfx_config {
    uint32_t struct_size;        /* sizeof(xrl_gfx_config)                       */

    /* Host log callback (same one as xrl_boot_context.log). May be NULL ->
     * OutputDebugString only. Must be thread-safe. Overridden by host->log. */
    xrl_log_fn log;

    /* The host's services (A's seam). When host->run_on_gfx_thread is set, ALL
     * marshalling uses the host's gfx thread and no library thread is created.
     * Copied at init; NULL for the standalone/harness path. */
    const xrl_gfx_host_services *host;

    /* CoreWindow handover, STATIC form: raw ABI pointer (IUnknown*) of the
     * Windows.UI.Core.CoreWindow, not AddRef'd, must outlive gfx. Used only if
     * host->get_core_window is absent/NULL-returning. The DYNAMIC form
     * (host->get_core_window, called at swapchain-creation time) is preferred
     * because the window may not exist at init. gfx never touches any property of
     * the window (UI-thread affine); it only hands the pointer to
     * IDXGIFactory2::CreateSwapChainForCoreWindow. NULL is fine for a headless
     * probe: swapchain creation then returns XRL_ERR_NOTAVAIL. */
    void    *core_window;

    /* Fallback backbuffer size in PIXELS when the guest asks for 0 and
     * host->get_window_size reports 0. 0/0 -> 1920x1080 with a warning. */
    uint32_t window_width_px;
    uint32_t window_height_px;

    /* GetCurrentThreadId() of the host UI thread, or 0. Only used to WARN when a
     * blocking gfx call arrives on it. */
    uint32_t ui_thread_id;

    /* How long a creation call waits for the gfx thread. 0 = 60000. */
    uint32_t create_timeout_ms;

    uint32_t flags;              /* XRL_GFX_FLAG_*                               */

    /* DESKTOP TEST BUILDS ONLY (ignored when built for the UWP family): an HWND to
     * create the flip-model swapchain on, so the whole present path runs on a dev
     * PC with the same code as the console path. */
    void    *test_hwnd;
} xrl_gfx_config;

/* Standalone init (what xrl_gfx_ops_get does internally). Any thread. */
XRL_GFX_API xrl_status xrl_gfx_init(const xrl_gfx_config *cfg);

/* Stop the library worker (if any) and forget the host. Outstanding handles stay
 * valid COM objects; the host releases them via ops->release. A worker wedged in a
 * hung driver call is abandoned, never joined forever. */
XRL_GFX_API void xrl_gfx_shutdown(void);

/* The vtable (same pointer xrl_gfx_ops_get returns). Valid for the process
 * lifetime. If a preflight judged D3D11On12 non-viable, d3d11on12_create is NULL
 * per the comment in xrl_abi.h. */
XRL_GFX_API const xrl_gfx_ops *xrl_gfx_get_ops(void);

/* ------------------------------------------------------------------ *
 * Caps reporting
 * ------------------------------------------------------------------ */

typedef struct xrl_gfx_caps {
    uint32_t struct_size;
    char     adapter_name[128];          /* UTF-8                               */
    uint32_t vendor_id, device_id, revision;
    uint64_t dedicated_video_bytes;      /* DXGI_ADAPTER_DESC1                  */
    uint64_t dedicated_system_bytes;
    uint64_t shared_system_bytes;
    uint32_t max_feature_level;          /* D3D_FEATURE_LEVEL value, 0xb000=11_0 */
    uint32_t resource_binding_tier;      /* 1..3                                */
    uint32_t resource_heap_tier;         /* 1..2                                */
    uint32_t tiled_resources_tier;
    uint32_t highest_shader_model;       /* 0x60 = SM6.0 ...; 0 = query failed  */
    uint32_t root_signature_version;     /* D3D_ROOT_SIGNATURE_VERSION: 1=1.0 2=1.1 3=1.2; 0 = failed */
    uint32_t uma;                        /* D3D12_FEATURE_DATA_ARCHITECTURE1    */
    uint32_t cache_coherent_uma;
    uint32_t rovs_supported;
    uint32_t typed_uav_load_additional_formats;
    uint32_t conservative_raster_tier;
    uint32_t wave_ops;
    uint32_t raytracing_tier;            /* 0 = none                            */
    uint32_t mesh_shader_tier;
    uint32_t view_instancing_tier;
    uint32_t gpu_va_bits;                /* MaxGPUVirtualAddressBitsPerResource */
    uint64_t budget_local, usage_local;          /* DXGI_MEMORY_SEGMENT_GROUP_LOCAL      */
    uint64_t budget_nonlocal, usage_nonlocal;    /* DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL  */
    uint32_t bgra8_display_ok;           /* flip-model swapchain format usable   */
    uint32_t output_width, output_height;/* first DXGI output, 0 if none         */
} xrl_gfx_caps;

/* Log the adapter, feature level, binding/heap tiers, shader model, UMA and the
 * live memory budget through the host log callback (tag "gfx", one line per fact,
 * each prefixed "gfx caps:" so they grep cleanly in LocalState/log.txt), and
 * optionally return them. `device` is an ID3D12Device* handle, or
 * XRL_INVALID_HANDLE to create (and release) a temporary device. `out` may be
 * NULL. Does D3D work on the gfx thread, not the caller's. */
XRL_GFX_API xrl_status xrl_gfx_report_caps(xrl_handle device, xrl_gfx_caps *out);

/* ------------------------------------------------------------------ *
 * D3D11 backend decision point  (native D3D11On12 vs DXVK->Dozen)
 * ------------------------------------------------------------------ */

enum {
    XRL_GFX_D3D11_UNDECIDED   = 0,
    XRL_GFX_D3D11_ON12        = 1,   /* native D3D11On12 works: ship this path        */
    XRL_GFX_D3D11_DXVK_DOZEN  = 2,   /* D3D11On12 failed: fallback needed (NOT BUILT) */
    XRL_GFX_D3D11_NATIVE      = 3,   /* XRL_D3D11=native: a native D3D11 device, no D3D11On12 */
};
enum {
    XRL_GFX_REASON_NONE = 0,
    XRL_GFX_REASON_NO_DEVICE,          /* no d3d12 device at all (verdict stays UNDECIDED) */
    XRL_GFX_REASON_CREATE_FAILED,      /* D3D11On12CreateDevice returned failure   */
    XRL_GFX_REASON_CREATE_TIMEOUT,     /* D3D11On12CreateDevice did not return     */
    XRL_GFX_REASON_NO_ON12_INTERFACE,  /* device lacks ID3D11On12Device            */
    XRL_GFX_REASON_SMOKE_FAILED,       /* created, but resource/clear/flush failed */
    XRL_GFX_REASON_DEVICE_REMOVED,     /* queue reported device removed after use  */
    XRL_GFX_REASON_FORCED_BY_FLAG,
};
typedef struct xrl_gfx_d3d11_decision {
    uint32_t struct_size;
    uint32_t backend;                  /* XRL_GFX_D3D11_*                           */
    uint32_t reason;                   /* XRL_GFX_REASON_*                          */
    int32_t  hresult;                  /* native HRESULT behind the reason          */
    char     detail[160];
} xrl_gfx_d3d11_decision;

/* Create a throwaway device + queue + D3D11On12 device and push real work through
 * it (d3d11 clear, wrapped-d3d12-RT clear, flush, fence drain), recording the
 * verdict. On failure xrl_gfx_get_ops()->d3d11on12_create becomes NULL. Call
 * BEFORE xrl_layer_boot (the table is installed before the guest starts; do not
 * race it). Idempotent once decided. Returns XRL_OK iff D3D11On12 is viable. */
XRL_GFX_API xrl_status xrl_gfx_preflight(xrl_gfx_d3d11_decision *out);

/* The cached verdict (UNDECIDED until a preflight, or a d3d11on12_create call that
 * fails). */
XRL_GFX_API void xrl_gfx_get_d3d11_decision(xrl_gfx_d3d11_decision *out);

/* ------------------------------------------------------------------ *
 * Extras the frozen ops table cannot express  (see ABI-CHANGE-REQUESTS.md)
 * ------------------------------------------------------------------ */

/* IDXGISwapChain3::GetCurrentBackBufferIndex. */
XRL_GFX_API xrl_status xrl_gfx_swapchain_current_index(xrl_handle swapchain,
                                                       uint32_t *out_index);

/* IDXGISwapChain::ResizeBuffers. All buffer references (swapchain_get_buffer
 * handles, wrapped D3D11On12 resources) must be released first or DXGI fails the
 * call. 0 = keep current for buffer_count / width / height / dxgi_format. */
XRL_GFX_API xrl_status xrl_gfx_swapchain_resize(xrl_handle swapchain,
                                                uint32_t buffer_count,
                                                uint32_t width, uint32_t height,
                                                uint32_t dxgi_format);

/* The native HRESULT behind the most recent failing gfx call on THIS thread (the
 * ABI carries only coarse xrl_status). Lets the Wine DLL synthesize an accurate
 * DXGI error for the guest. 0 if the last call succeeded. */
XRL_GFX_API int32_t xrl_gfx_last_hresult(void);

/* ------------------------------------------------------------------ *
 * On-console probe / selftest
 * ------------------------------------------------------------------ */

typedef struct xrl_gfx_selftest_result {
    uint32_t struct_size;
    xrl_status device;            /* d3d12_create_device                        */
    xrl_status d3d11on12;         /* d3d11on12_create                           */
    xrl_status swapchain;         /* create_swapchain_for_corewindow            */
    xrl_status d3d12_present;     /* clear via D3D12 + present + pixel readback */
    xrl_status d3d11on12_present; /* clear via wrapped D3D11 RT + present + readback */
    xrl_status resize;            /* ResizeBuffers + GetBuffer size check        */
    uint32_t   d3d12_pixel;       /* 0xAARRGGBB readback of the D3D12-cleared frame   */
    uint32_t   d3d11_pixel;       /* 0xAARRGGBB readback of the D3D11On12-cleared frame */
    double     avg_present_ms;    /* mean Present(1,0)+wait cost over the D3D12 phase */
    int32_t    pass;              /* 1 if every stage that could run passed     */
} xrl_gfx_selftest_result;

/* Exercises the real ops table end to end, the way the layer will: device ->
 * guest-style queue (native CreateCommandQueue on the handle) -> D3D11On12 ->
 * flip-model swapchain on the host's window -> `frames` presented frames cleared
 * via D3D12, a few more cleared through a D3D11On12-wrapped backbuffer, then a
 * ResizeBuffers; every drawn frame verified by GPU readback. With no window only
 * the first two stages run (rest report XRL_ERR_NOTAVAIL). Needs xrl_gfx_ops_get
 * or xrl_gfx_init first. Call from a non-UI thread. */
XRL_GFX_API xrl_status xrl_gfx_selftest(uint32_t frames, xrl_gfx_selftest_result *out);

/* One call for a probe: init (if `cfg` given and not yet inited), caps report,
 * preflight verdict, selftest(60), a PASS/FAIL summary line, shutdown only if this
 * call did the init. Returns 0 on pass, nonzero on failure. If the host already
 * called xrl_gfx_ops_get, pass cfg == NULL. Call from a NON-UI thread. */
XRL_GFX_API int xrl_gfx_run_probe(const xrl_gfx_config *cfg);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XRL_GFX_H */

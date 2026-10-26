// gfx_seam.h - the seam between host/ (Workstream A) and gfx/ (Workstream D).
//
// HOW D PLUGS IN
// --------------
// host/ already ships a *default* xrl_gfx_ops (gfx_ops_default.cpp): a real but
// minimal D3D12 / CreateSwapChainForCoreWindow / D3D11On12 implementation that
// is enough for the probe build and for bring-up. When D has the production
// version, D builds a static lib that exports exactly ONE strong symbol:
//
//     extern "C" const xrl_gfx_ops* xrl_gfx_ops_get(const xrl_gfx_host_services*);
//
// and passes it to the host build with  /p:XrlGfxLib=<path to gfx.lib>.  The
// host's default is bound through `/alternatename:xrl_gfx_ops_get=
// xrl_gfx_ops_get_default`, so a strong definition from gfx.lib wins and no host
// source needs to change. (TODO(D): this is the line to replace; nothing else.)
//
// RULES D MUST FOLLOW (all measured, see docs/decisions.md)
//   * Never create a D3D device / swapchain on the CoreWindow UI thread - it
//     blocks forever on Xbox. Use host->run_on_gfx_thread() for every create.
//   * The CoreWindow pointer is agile; it may be handed to
//     IDXGIFactory2::CreateSwapChainForCoreWindow from the gfx thread.
//   * D3D12 max feature level on this console is 11_0 (binding tier 3).
//   * Present()/Release()/GetBuffer() are free-threaded and need NOT be marshalled.
//
// This header is plain C/C++ with no Windows/WinRT types on purpose, so gfx/ can
// include it from either WINAPI_FAMILY.
#pragma once
#include "xrl_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xrl_gfx_host_services {
    uint32_t struct_size;               /* sizeof(xrl_gfx_host_services)          */
    xrl_log_fn log;                     /* same log sink as everything else       */

    /* IUnknown* ABI pointer of Windows.UI.Core.CoreWindow, or NULL until the
     * front-end (probe view / SDL) has created the window. Not AddRef'd; valid
     * for the process lifetime. */
    void *(*get_core_window)(void);
    /* Window size in physical pixels (0,0 until the window exists). */
    void  (*get_window_size)(uint32_t *w, uint32_t *h);

    /* Run fn(ctx) on the host's dedicated MTA gfx thread and wait for it. Logs a
     * stall warning every 5 s while waiting. timeout_ms 0xFFFFFFFF = forever.
     * Returns 0 on completion, -1 on timeout (fn may still run later; capture
     * only state that outlives the caller). If already on the gfx thread, fn is
     * simply called inline. */
    int   (*run_on_gfx_thread)(const char *what, void (*fn)(void *), void *ctx,
                               uint32_t timeout_ms);
    int   (*on_gfx_thread)(void);       /* 1 if the caller IS the gfx thread      */
} xrl_gfx_host_services;

/* Provided by gfx/ (D) in production; by gfx_ops_default.cpp until then. */
const xrl_gfx_ops *xrl_gfx_ops_get(const xrl_gfx_host_services *host);

#ifdef __cplusplus
}
#endif

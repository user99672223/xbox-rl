// gfx_ops_default.cpp - DEFAULT xrl_gfx_ops for the host.
//
// REAL (not a stub), but deliberately minimal: DXGI factory + D3D12 device +
// flip-model CreateSwapChainForCoreWindow + D3D11On12, with every *create*
// marshalled onto the host's gfx thread (never the CoreWindow UI thread - see
// gfx_thread.cpp). It is what the probe build exercises, so the first run on the
// console tests the exact code path the layer will use.
//
// TODO(D): gfx/ replaces this wholesale by exporting a strong
// `xrl_gfx_ops_get` (see gfx_seam.h). D owns: caps reporting, swapchain
// resize/HDR/tearing policy, device-lost recovery, the D3D11On12 wrapper
// policy, and the DXVK->Dozen branch if D3D11On12 proves broken.
//
// Handle convention: an xrl_handle is the raw COM interface pointer
// (ID3D12Device*, IDXGISwapChain1*, ID3D12Resource*, ID3D11Device*, ...), as
// include/xrl_abi.h describes ("a d3d12_device is really a native ID3D12Device*").
// Each handle returned owns ONE reference; xrl_gfx_ops::release() drops it.
#include "pch.h"
#include "xrl_host.h"
#include "gfx_seam.h"

#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>

using Microsoft::WRL::ComPtr;

// Bind the host default as the "weak" definition of the seam symbol. A strong
// xrl_gfx_ops_get from gfx/ (D) overrides this at link time.
extern "C" const xrl_gfx_ops* xrl_gfx_ops_get_default(const xrl_gfx_host_services* host);
#pragma comment(linker, "/alternatename:xrl_gfx_ops_get=xrl_gfx_ops_get_default")

namespace {

const xrl_gfx_host_services* g_host = nullptr;
ComPtr<IDXGIFactory4> g_factory;       // only touched on the gfx thread

xrl_status map_hr(HRESULT hr) {
    if (SUCCEEDED(hr)) return XRL_OK;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return XRL_ERR_DEVICELOST;
    if (hr == E_NOINTERFACE || hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_NOT_FOUND)
        return XRL_ERR_NOTAVAIL;
    return XRL_ERR_GENERIC;
}

template <class T> xrl_handle to_handle(ComPtr<T>& p) { return (xrl_handle)(uintptr_t)p.Detach(); }

// Runs fn on the gfx thread; result is returned via shared state so a timed-out
// (abandoned) job can never write into a dead stack frame.
struct Result { xrl_status st = XRL_ERR_GENERIC; xrl_handle h1 = 0, h2 = 0; };

template <class F>
xrl_status on_gfx(const char* what, uint32_t timeout_ms, std::shared_ptr<Result> r, F fn) {
    bool ok = xrl::gfx_thread_invoke(what, [r, fn]() mutable { fn(*r); }, timeout_ms);
    if (!ok) return XRL_ERR_GENERIC;
    return r->st;
}

HRESULT ensure_factory() {
    if (g_factory) return S_OK;
    return CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory));
}

xrl_status op_d3d12_create_device(xrl_handle* out) {
    if (!out) return XRL_ERR_GENERIC;
    *out = XRL_INVALID_HANDLE;
    auto r = std::make_shared<Result>();
    xrl_status st = on_gfx("d3d12_create_device", 60000, r, [](Result& res) {
        HRESULT hr = ensure_factory();
        if (FAILED(hr)) { XLOGE("gfx", "CreateDXGIFactory2 failed 0x%08x", (unsigned)hr); res.st = map_hr(hr); return; }
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; g_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
        ComPtr<ID3D12Device> dev;
        // FL 11_0 is the console's AppContainer cap (decisions.md); do not ask for more.
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr) && adapter) {
            XLOGW("gfx", "D3D12CreateDevice(adapter) failed 0x%08x, retrying with default adapter", (unsigned)hr);
            hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        }
        if (FAILED(hr)) { XLOGE("gfx", "D3D12CreateDevice failed 0x%08x", (unsigned)hr); res.st = map_hr(hr); return; }
        res.h1 = to_handle(dev);
        res.st = XRL_OK;
    });
    if (st == XRL_OK) *out = r->h1;
    return st;
}

xrl_status op_create_swapchain(xrl_handle /*device*/, xrl_handle queue, uint32_t width, uint32_t height,
                               uint32_t buffer_count, uint32_t dxgi_format, xrl_handle* out_sc) {
    if (!out_sc || !queue) return XRL_ERR_GENERIC;
    *out_sc = XRL_INVALID_HANDLE;
    void* cw = g_host ? g_host->get_core_window() : nullptr;
    if (!cw) { XLOGE("gfx", "create_swapchain: no CoreWindow yet"); return XRL_ERR_NOTAVAIL; }
    if (!width || !height) {
        uint32_t w = 0, h = 0;
        g_host->get_window_size(&w, &h);
        if (!width)  width  = w ? w : 1920;
        if (!height) height = h ? h : 1080;
    }
    auto r = std::make_shared<Result>();
    xrl_status st = on_gfx("create_swapchain_for_corewindow", 60000, r, [=](Result& res) {
        HRESULT hr = ensure_factory();
        if (FAILED(hr)) { res.st = map_hr(hr); return; }
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = width; d.Height = height;
        d.Format = (DXGI_FORMAT)dxgi_format;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = buffer_count < 2 ? 2 : buffer_count;
        d.Scaling = DXGI_SCALING_NONE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> sc;
        hr = g_factory->CreateSwapChainForCoreWindow((IUnknown*)(uintptr_t)queue, (IUnknown*)cw, &d, nullptr, &sc);
        if (FAILED(hr)) {
            XLOGE("gfx", "CreateSwapChainForCoreWindow(%ux%u fmt=%u n=%u) failed 0x%08x", width, height, dxgi_format,
                  d.BufferCount, (unsigned)hr);
            res.st = map_hr(hr); return;
        }
        XLOGI("gfx", "swapchain created %ux%u fmt=%u buffers=%u", width, height, dxgi_format, d.BufferCount);
        res.h1 = to_handle(sc);
        res.st = XRL_OK;
    });
    if (st == XRL_OK) *out_sc = r->h1;
    return st;
}

xrl_status op_swapchain_get_buffer(xrl_handle sc, uint32_t index, xrl_handle* out) {
    if (!sc || !out) return XRL_ERR_GENERIC;
    *out = XRL_INVALID_HANDLE;
    ComPtr<ID3D12Resource> res;
    HRESULT hr = ((IDXGISwapChain1*)(uintptr_t)sc)->GetBuffer(index, IID_PPV_ARGS(&res));
    if (FAILED(hr)) return map_hr(hr);
    *out = to_handle(res);
    return XRL_OK;
}

xrl_status op_swapchain_present(xrl_handle sc, uint32_t sync_interval, uint32_t flags) {
    if (!sc) return XRL_ERR_GENERIC;
    return map_hr(((IDXGISwapChain1*)(uintptr_t)sc)->Present(sync_interval, flags));
}

xrl_status op_d3d11on12_create(xrl_handle dev12, xrl_handle q12, xrl_handle* out_dev, xrl_handle* out_ctx) {
    if (!dev12 || !q12 || !out_dev || !out_ctx) return XRL_ERR_GENERIC;
    *out_dev = *out_ctx = XRL_INVALID_HANDLE;
    auto r = std::make_shared<Result>();
    xrl_status st = on_gfx("d3d11on12_create", 60000, r, [=](Result& res) {
        IUnknown* queues[] = { (IUnknown*)(uintptr_t)q12 };
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> c11;
        HRESULT hr = D3D11On12CreateDevice((IUnknown*)(uintptr_t)dev12, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           &fl, 1, queues, 1, 0, &d11, &c11, nullptr);
        if (FAILED(hr)) { XLOGE("gfx", "D3D11On12CreateDevice failed 0x%08x", (unsigned)hr); res.st = map_hr(hr); return; }
        res.h1 = to_handle(d11);
        res.h2 = to_handle(c11);
        res.st = XRL_OK;
    });
    if (st == XRL_OK) { *out_dev = r->h1; *out_ctx = r->h2; }
    return st;
}

void op_release(xrl_handle h) {
    if (h) ((IUnknown*)(uintptr_t)h)->Release();
}

xrl_gfx_ops g_ops = {
    sizeof(xrl_gfx_ops),
    op_d3d12_create_device,
    op_create_swapchain,
    op_swapchain_get_buffer,
    op_swapchain_present,
    op_d3d11on12_create,
    op_release,
};

// --- host services handed to whichever xrl_gfx_ops_get is linked ----------------
void* svc_get_core_window() { return xrl::window_info_get().core_window; }
void svc_get_window_size(uint32_t* w, uint32_t* h) {
    auto wi = xrl::window_info_get();
    if (w) *w = wi.width;
    if (h) *h = wi.height;
}
int svc_run_on_gfx_thread(const char* what, void (*fn)(void*), void* ctx, uint32_t timeout_ms) {
    return xrl::gfx_thread_invoke(what ? what : "gfx job", [fn, ctx] { fn(ctx); }, timeout_ms) ? 0 : -1;
}
int svc_on_gfx_thread() { return xrl::gfx_thread_is_current() ? 1 : 0; }

xrl_gfx_host_services g_services = {
    sizeof(xrl_gfx_host_services),
    xrl_host_log,
    svc_get_core_window,
    svc_get_window_size,
    svc_run_on_gfx_thread,
    svc_on_gfx_thread,
};

}  // namespace

extern "C" const xrl_gfx_ops* xrl_gfx_ops_get_default(const xrl_gfx_host_services* host) {
    g_host = host;
    return &g_ops;
}

extern "C" const xrl_gfx_ops* xrl_host_gfx_ops(void) {
    const xrl_gfx_ops* ops = xrl_gfx_ops_get(&g_services);
    if (!ops) XLOGE("gfx", "xrl_gfx_ops_get returned NULL");
    return ops;
}

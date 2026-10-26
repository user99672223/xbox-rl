// swapchain.cpp — flip-model swapchain against the host CoreWindow.
//
// The swapchain is created against the CoreWindow* the host exposes
// (host->get_core_window, or cfg.core_window) via
// IDXGIFactory2::CreateSwapChainForCoreWindow, on the gfx thread (never the UI
// thread). Handles are raw: swapchain = IDXGISwapChain3*, buffer = ID3D12Resource*.
// Present/GetBuffer are plain pass-throughs on the caller's (guest render) thread.
#include "common.h"

namespace xrl {

namespace {

// Flip-model only allows these formats; sRGB variants must be made as UNORM and
// viewed as sRGB by the app's RTV. Returns UNKNOWN if the format cannot be a
// flip-model backbuffer at all.
DXGI_FORMAT FlipFormat(DXGI_FORMAT f, bool* was_srgb) {
    *was_srgb = false;
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return f;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            *was_srgb = true;
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            *was_srgb = true;
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

xrl_status SwapchainCreateOp(xrl_handle device, xrl_handle queue, uint32_t w, uint32_t h, uint32_t buffer_count,
                             uint32_t dxgi_format, xrl_handle* out) {
    if (!out) return XRL_ERR_GENERIC;
    *out = XRL_INVALID_HANDLE;
    SetLastHr(S_OK);

    if (!device || !queue) {
        Log(LOG_ERROR, "create_swapchain: device/queue handle is NULL (queue is the guest's direct queue)");
        return XRL_ERR_GENERIC;
    }
    // Own references across the (possibly abandoned) job.
    ComPtr<ID3D12CommandQueue> q = FromHandle<ID3D12CommandQueue>(queue);
    ComPtr<ID3D12Device> dev12 = FromHandle<ID3D12Device>(device);

    void* core = GetCoreWindow();
    void* hwnd = GetTestHwnd();
    if (!core && !hwnd) {
        Log(LOG_ERROR, "create_swapchain: no CoreWindow yet (host->get_core_window returned NULL)");
        return XRL_ERR_NOTAVAIL;
    }

    bool srgb = false;
    DXGI_FORMAT fmt = FlipFormat((DXGI_FORMAT)dxgi_format, &srgb);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        Log(LOG_ERROR, "create_swapchain: DXGI format %u is not valid for a flip-model swapchain", dxgi_format);
        SetLastHr(DXGI_ERROR_INVALID_CALL);
        return XRL_ERR_GENERIC;
    }
    if (srgb)
        Log(LOG_INFO, "create_swapchain: format %u is sRGB; creating %u (flip model forbids sRGB buffers). "
            "The guest must use an sRGB RTV view.", dxgi_format, (unsigned)fmt);

    uint32_t bc = buffer_count;
    if (bc < 2) bc = 2;
    if (bc > DXGI_MAX_SWAP_CHAIN_BUFFERS) bc = DXGI_MAX_SWAP_CHAIN_BUFFERS;
    if (bc != buffer_count)
        Log(LOG_WARN, "create_swapchain: buffer_count %u clamped to %u (flip model needs 2..16)", buffer_count, bc);

    if (!w || !h) {
        uint32_t ww = 0, hh = 0;
        GetWindowSize(&ww, &hh);
        if (!w) w = ww;
        if (!h) h = hh;
    }
    if (!w || !h) {
        w = 1920;
        h = 1080;
        Log(LOG_WARN, "create_swapchain: no size from guest or host; defaulting to %ux%u", w, h);
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = fmt;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bc;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    Log(LOG_INFO, "create_swapchain: %ux%u fmt=%u buffers=%u flip_discard stretch via %s window=%p", w, h,
        (unsigned)fmt, bc, core ? "CreateSwapChainForCoreWindow" : "CreateSwapChainForHwnd (desktop test)",
        core ? core : hwnd);

    struct Res {
        ComPtr<IDXGISwapChain1> sc;
    };
    auto res = std::make_shared<Res>();
    auto make = [res, q, desc, core, hwnd]() -> HRESULT {
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = GetFactory(factory);
        if (FAILED(hr)) return hr;
        if (core)
            return factory->CreateSwapChainForCoreWindow(q.Get(), static_cast<IUnknown*>(core), &desc, nullptr,
                                                         res->sc.GetAddressOf());
#if XRL_GFX_HAVE_HWND
        hr = factory->CreateSwapChainForHwnd(q.Get(), (HWND)hwnd, &desc, nullptr, nullptr, res->sc.GetAddressOf());
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation((HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
        return hr;
#else
        (void)hwnd;
        return E_NOTIMPL;
#endif
    };

    HRESULT hr;
    if (G().cfg.flags & XRL_GFX_FLAG_SWAPCHAIN_INLINE) {
        Log(LOG_INFO, "create_swapchain: INLINE on caller thread %lu", (unsigned long)GetCurrentThreadId());
        hr = make();
    } else {
        WarnIfUiThread("create_swapchain_for_corewindow");
        hr = RunOnGfxThread("create_swapchain_for_corewindow", make);
    }
    if (FAILED(hr)) return Fail(hr, core ? "CreateSwapChainForCoreWindow" : "CreateSwapChainForHwnd");

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(res->sc.As(&sc3))) return Fail(E_NOINTERFACE, "swapchain QI IDXGISwapChain3");
    IDXGISwapChain3* raw = sc3.Get();
    *out = ToHandle(sc3);
    Log(LOG_INFO, "create_swapchain -> IDXGISwapChain3=%p", raw);
    return XRL_OK;
}

xrl_status SwapchainGetBufferOp(xrl_handle sc, uint32_t index, xrl_handle* out) {
    if (!out) return XRL_ERR_GENERIC;
    *out = XRL_INVALID_HANDLE;
    if (!sc) return XRL_ERR_GENERIC;
    ComPtr<ID3D12Resource> r;
    HRESULT hr = FromHandle<IDXGISwapChain3>(sc)->GetBuffer(index, IID_PPV_ARGS(&r));
    if (FAILED(hr)) return Fail(hr, "IDXGISwapChain::GetBuffer");
    SetLastHr(S_OK);
    *out = ToHandle(r);
    return XRL_OK;
}

xrl_status SwapchainPresentOp(xrl_handle sc, uint32_t sync, uint32_t flags) {
    if (!sc) return XRL_ERR_GENERIC;
    IDXGISwapChain3* s = FromHandle<IDXGISwapChain3>(sc);
    if (sync > 4) sync = 4;
    // CoreWindow flip swapchains: no tearing; pass only flags that are valid there.
    flags &= (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE | DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT);
    HRESULT hr = s->Present(sync, flags);
    if (SUCCEEDED(hr)) {  // includes DXGI_STATUS_OCCLUDED (a success code)
        SetLastHr(S_OK);
        return XRL_OK;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // For D3D12 swapchains GetDevice yields the queue; its device knows why.
        ComPtr<ID3D12CommandQueue> q;
        ComPtr<ID3D12Device> d;
        HRESULT why = hr;
        if (SUCCEEDED(s->GetDevice(IID_PPV_ARGS(&q))) && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))))
            why = d->GetDeviceRemovedReason();
        Log(LOG_ERROR, "Present: device removed (reason 0x%08lX)", (unsigned long)why);
    }
    return Fail(hr, "IDXGISwapChain::Present");
}

}  // namespace xrl

// ---------------------------------------------------------------- C API extras
using namespace xrl;

extern "C" XRL_GFX_API xrl_status xrl_gfx_swapchain_current_index(xrl_handle swapchain, uint32_t* out_index) {
    if (!out_index || !swapchain) return XRL_ERR_GENERIC;
    *out_index = FromHandle<IDXGISwapChain3>(swapchain)->GetCurrentBackBufferIndex();
    return XRL_OK;
}

extern "C" XRL_GFX_API xrl_status xrl_gfx_swapchain_resize(xrl_handle swapchain, uint32_t buffer_count, uint32_t width,
                                                          uint32_t height, uint32_t dxgi_format) {
    SetLastHr(S_OK);
    if (!swapchain) return XRL_ERR_GENERIC;
    IDXGISwapChain3* s = FromHandle<IDXGISwapChain3>(swapchain);
    DXGI_SWAP_CHAIN_DESC1 cur{};
    HRESULT hr = s->GetDesc1(&cur);
    if (FAILED(hr)) return Fail(hr, "IDXGISwapChain1::GetDesc1");
    DXGI_FORMAT fmt = cur.Format;
    if (dxgi_format) {
        bool srgb = false;
        fmt = FlipFormat((DXGI_FORMAT)dxgi_format, &srgb);
        if (fmt == DXGI_FORMAT_UNKNOWN) return Fail(DXGI_ERROR_INVALID_CALL, "swapchain_resize: bad format");
    }
    UINT bc = buffer_count ? buffer_count : cur.BufferCount;
    UINT w = width ? width : cur.Width, h = height ? height : cur.Height;
    hr = s->ResizeBuffers(bc, w, h, fmt, cur.Flags);
    if (FAILED(hr)) return Fail(hr, "IDXGISwapChain::ResizeBuffers (are all buffer references released?)");
    Log(LOG_INFO, "swapchain_resize -> %ux%u buffers=%u fmt=%u", w, h, bc, (unsigned)fmt);
    return XRL_OK;
}

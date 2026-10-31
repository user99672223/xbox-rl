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
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

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
        // XRL_GFX_WARP=1: the WARP software adapter (a CI runner has no GPU; the console never sets it)
        char warp[4] = "";
        if (GetEnvironmentVariableA("XRL_GFX_WARP", warp, sizeof warp) && warp[0] == '1') {
            ComPtr<IDXGIFactory4> f4;
            ComPtr<IDXGIAdapter1> wa;
            if (SUCCEEDED(g_factory.As(&f4)) && SUCCEEDED(f4->EnumWarpAdapter(IID_PPV_ARGS(&wa)))) { adapter = wa; XLOGI("gfx", "XRL_GFX_WARP=1: the WARP adapter"); }
            else XLOGW("gfx", "XRL_GFX_WARP=1 but no WARP adapter");
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
// The game asks for the window when it makes its swapchain: the keepalive (below) must be gone by then. A
// window takes one swapchain, and its second device presenting a black frame every refresh for the whole
// game competed with the game for the GPU.
void keepalive_stop_wait();
void* svc_get_core_window() { keepalive_stop_wait(); return xrl::window_info_get().core_window; }
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

// ---------------------------------------------------------------------------
// Keep-alive present loop  (host-internal; not part of the gfx seam)
// ---------------------------------------------------------------------------
// The EAC path runs the bootstrapper (RocketLeague_EAC.exe) in THIS windowed
// process; the bootstrapper renders nothing, and the real game renders in a
// headless 2nd process. So the CoreWindow never presents a frame, and the Xbox
// PLM suspends the whole package at ~3 s (APP_WILLENTERBACKGROUND, extended
// execution revoked, process exits 0x40010006) - right in the middle of EAC's
// ptrace injection, so EAC never reaches "301 loaded".
//
// Fix: drive a real D3D12 clear+present loop on the CoreWindow from a dedicated
// MTA thread so the compositor keeps seeing frames and the OS leaves us
// foreground/active. Entirely self-contained (its own device/queue/swapchain),
// so it never races the layer's own gfx objects.
namespace {

std::atomic<bool> g_keepalive_run{false};
std::thread       g_keepalive_th;
HANDLE            g_keepalive_ended;   // set once the loop has returned and released its swapchain and device

void keepalive_loop() {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}

    void* cw = xrl::window_info_get().core_window;
    if (!cw) { XLOGW("gfx", "keepalive: no CoreWindow; not presenting"); return; }

    uint32_t w = 0, h = 0;
    { auto wi = xrl::window_info_get(); w = wi.width ? wi.width : 1920; h = wi.height ? wi.height : 1080; }

    HRESULT hr = ensure_factory();
    if (FAILED(hr)) { XLOGE("gfx", "keepalive: CreateDXGIFactory2 0x%08x", (unsigned)hr); return; }

    ComPtr<ID3D12Device> dev;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { XLOGE("gfx", "keepalive: D3D12CreateDevice 0x%08x", (unsigned)hr); return; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) { XLOGE("gfx", "keepalive: CreateCommandQueue 0x%08x", (unsigned)hr); return; }

    const UINT kBuffers = 2;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers; d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    hr = g_factory->CreateSwapChainForCoreWindow(queue.Get(), (IUnknown*)cw, &d, nullptr, &sc1);
    if (FAILED(hr)) { XLOGE("gfx", "keepalive: CreateSwapChainForCoreWindow(%ux%u) 0x%08x", w, h, (unsigned)hr); return; }
    ComPtr<IDXGISwapChain3> sc;
    if (FAILED(sc1.As(&sc))) { XLOGE("gfx", "keepalive: no IDXGISwapChain3"); return; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) { XLOGE("gfx", "keepalive: RTV heap"); return; }
    UINT rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bufs[kBuffers];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; ++i) {
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&bufs[i])))) { XLOGE("gfx", "keepalive: GetBuffer %u", i); return; }
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = rtv0; h2.ptr += (SIZE_T)i * rtvStep;
        dev->CreateRenderTargetView(bufs[i].Get(), nullptr, h2);
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) { XLOGE("gfx", "keepalive: alloc"); return; }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)))) { XLOGE("gfx", "keepalive: cmdlist"); return; }
    cl->Close();

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { XLOGE("gfx", "keepalive: fence"); return; }
    HANDLE fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;

    XLOGI("gfx", "keepalive: presenting %ux%u to keep the app foreground", w, h);
    const float kClear[4] = { 0.f, 0.f, 0.f, 1.f };   // black
    while (g_keepalive_run.load()) {
        UINT idx = sc->GetCurrentBackBufferIndex();
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[idx].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0; rtv.ptr += (SIZE_T)idx * rtvStep;
        cl->ClearRenderTargetView(rtv, kClear, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = { cl.Get() };
        queue->ExecuteCommandLists(1, lists);
        HRESULT ph = sc->Present(1, 0);
        if (FAILED(ph)) {
            XLOGW("gfx", "keepalive: Present 0x%08x; stopping", (unsigned)ph);
            if (ph == DXGI_ERROR_DEVICE_REMOVED || ph == DXGI_ERROR_DEVICE_RESET)
                XLOGW("gfx", "keepalive: device removed, reason 0x%08x", (unsigned)dev->GetDeviceRemovedReason());
            break;
        }
        const UINT64 want = ++fenceVal;
        queue->Signal(fence.Get(), want);
        if (fence->GetCompletedValue() < want) {
            fence->SetEventOnCompletion(want, fenceEvt);
            WaitForSingleObject(fenceEvt, 1000);
        }
    }
    if (fenceEvt) CloseHandle(fenceEvt);
    XLOGI("gfx", "keepalive: present loop ended");
}

// XRL_PRESENT_TEST=1: what reaches the screen, without the game. A battery of swapchain variants on the CoreWindow,
// a few seconds each (a fresh swapchain per variant, released before the next: a window takes one). Each logs:
//   presents/s       how often Present returned
//   shown/s          distinct frames the console reports on screen (GetFrameStatistics: PresentCount moved)
//   refresh          the period between the refreshes frames were shown on (SyncQPCTime / SyncRefreshCount)
//   spacing          a histogram of the time between shown frames: ~8.3 ms = 120 Hz, ~16.7 ms = 60 Hz
// Variants: size (window / HDMI mode / 4K), scaling, buffer count, flip mode, format, waitable latency 1, tearing,
// a D3D11 device instead of D3D12, and an HDMI switch to 60 Hz and back to 120 (a control and a fresh 120 Hz set).
// IDXGIOutput::WaitForVBlank, where the console gives an output, times the display's own vblank.
namespace {

struct PtVariant {
    const char* name;
    bool d3d11 = false;
    uint32_t w = 0, h = 0;   // 0: the HDMI mode's size
    DXGI_SCALING scaling = DXGI_SCALING_STRETCH;
    UINT buffers = 3;
    DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool waitable = false, tearing = false;
    UINT sync = 1;
    UINT extra_flags = 0;        // more DXGI_SWAP_CHAIN_FLAG_* (FULLSCREEN_VIDEO for a custom present duration)
    UINT present_duration = 0;   // IDXGISwapChainMedia::SetPresentDuration, in 100 ns (83333 = 120 Hz); 0: not asked
    bool composition = false;    // CreateSwapChainForComposition on a Windows.UI.Composition visual, not the CoreWindow
    bool handle = false;         // IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle on a composition surface
};

// A Windows.UI.Composition visual tree on the CoreWindow showing one swapchain: made and torn down on the UI thread
struct PtComp {
    winrt::Windows::UI::Composition::Compositor comp{nullptr};
    winrt::Windows::UI::Composition::CompositionTarget target{nullptr};
};
winrt::Windows::UI::Core::CoreDispatcher pt_dispatcher(void* cw) {
    winrt::Windows::UI::Core::CoreWindow w{nullptr};
    winrt::copy_from_abi(w, cw);
    return w.Dispatcher();
}
bool pt_comp_attach(void* cw, IDXGISwapChain1* sc, uint32_t w, uint32_t h, PtComp& out) {
    bool ok = false;
    try {
        pt_dispatcher(cw).RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [&] {
            try {
                namespace wc = winrt::Windows::UI::Composition;
                out.comp = wc::Compositor();
                out.target = out.comp.CreateTargetForCurrentView();
                auto interop = out.comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
                winrt::com_ptr<ABI::Windows::UI::Composition::ICompositionSurface> surf;
                if (FAILED(interop->CreateCompositionSurfaceForSwapChain(sc, surf.put()))) return;
                wc::ICompositionSurface surface{nullptr};
                winrt::copy_from_abi(surface, surf.get());
                auto visual = out.comp.CreateSpriteVisual();
                visual.Size({(float)w, (float)h});
                visual.Brush(out.comp.CreateSurfaceBrush(surface));
                out.target.Root(visual);
                ok = true;
            } catch (const winrt::hresult_error& e) {
                XLOGW("ptest", "composition: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            }
        }).get();
    } catch (...) { XLOGW("ptest", "composition: the UI thread did not run the setup"); }
    return ok;
}
void pt_comp_detach(void* cw, PtComp& c) {
    try {
        pt_dispatcher(cw).RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [&] {
            try {   // nothing may escape onto the UI thread: an exception there ends the app
                if (c.target) c.target.Root(nullptr);
                c.target = nullptr;
                c.comp = nullptr;
            } catch (const winrt::hresult_error& e) {
                XLOGW("ptest", "composition teardown: 0x%08x", (unsigned)e.code());
            } catch (...) {}
        }).get();
    } catch (...) {}
}

// A composition surface handle (DCompositionCreateSurfaceHandle) shown on a Windows.UI.Composition visual of the
// CoreWindow: the swapchain is bound to the handle, the path Windows uses for direct and independent flips.
bool pt_comp_handle(void* cw, uint32_t w, uint32_t h, PtComp& out, HANDLE& sh) {
    bool ok = false;
    try {
        pt_dispatcher(cw).RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [&] {
            try {
                namespace wc = winrt::Windows::UI::Composition;
                out.comp = wc::Compositor();
                out.target = out.comp.CreateTargetForCurrentView();
                auto mk = (HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*))GetProcAddress(GetModuleHandleW(L"dcomp.dll"),
                                                                                           "DCompositionCreateSurfaceHandle");
                if (!mk) { XLOGW("ptest", "handle: DCompositionCreateSurfaceHandle not found"); return; }
                HRESULT hr = mk(0x0003 /* COMPOSITIONOBJECT_ALL_ACCESS */, nullptr, &sh);
                if (FAILED(hr)) { XLOGW("ptest", "handle: DCompositionCreateSurfaceHandle 0x%08x", (unsigned)hr); return; }
                auto interop = out.comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
                winrt::com_ptr<ABI::Windows::UI::Composition::ICompositionSurface> surf;
                hr = interop->CreateCompositionSurfaceForHandle(sh, surf.put());
                if (FAILED(hr)) { XLOGW("ptest", "handle: CreateCompositionSurfaceForHandle 0x%08x", (unsigned)hr); return; }
                wc::ICompositionSurface surface{nullptr};
                winrt::copy_from_abi(surface, surf.get());
                auto visual = out.comp.CreateSpriteVisual();
                visual.Size({(float)w, (float)h});
                visual.Brush(out.comp.CreateSurfaceBrush(surface));
                out.target.Root(visual);
                ok = true;
            } catch (const winrt::hresult_error& e) {
                XLOGW("ptest", "handle: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            } catch (...) { XLOGW("ptest", "handle: failed"); }
        }).get();
    } catch (...) { XLOGW("ptest", "handle: the UI thread did not run the setup"); }
    return ok;
}

// What custom present durations the console offers this swapchain, then ask for v.present_duration
void pt_media(IDXGISwapChain1* sc, const PtVariant& v) {
    ComPtr<IDXGISwapChainMedia> m;
    HRESULT hr = sc->QueryInterface(IID_PPV_ARGS(&m));
    if (FAILED(hr)) { XLOGI("ptest", "%-16s IDXGISwapChainMedia: 0x%08x", v.name, (unsigned)hr); return; }
    const UINT want[] = {83333, 111111, 166667, 416667};   // 120, 90, 60, 24 Hz
    for (UINT d : want) {
        UINT lo = 0, hi = 0;
        hr = m->CheckPresentDurationSupport(d, &lo, &hi);
        XLOGI("ptest", "%-16s CheckPresentDurationSupport(%u = %.1f Hz) 0x%08x: closest smaller %u, larger %u", v.name, d, 1e7 / d,
              (unsigned)hr, lo, hi);
    }
    hr = m->SetPresentDuration(v.present_duration);
    XLOGI("ptest", "%-16s SetPresentDuration(%u) 0x%08x", v.name, v.present_duration, (unsigned)hr);
}

struct PtResult { double presents = 0, shown = 0, period = 0; uint64_t bucket[6] = {}; uint64_t stat_fail = 0; HRESULT hr = S_OK; };

// The measurement loop around one swapchain: `frame(i)` draws and presents frame i.
template <class F>
PtResult pt_measure(IDXGISwapChain1* sc, double secs, HANDLE waitable, F&& frame) {
    PtResult r;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    auto qnow = [] { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; };
    int64_t t0 = qnow();
    uint64_t presents = 0, shown = 0, per_n = 0;
    double per_sum = 0;
    UINT last_pc = 0, last_rc = 0;
    int64_t last_q = 0;
    while (qnow() - t0 < (int64_t)(secs * qf.QuadPart)) {
        if (waitable) WaitForSingleObjectEx(waitable, 1000, TRUE);
        r.hr = frame(presents);
        if (FAILED(r.hr)) break;
        ++presents;
        DXGI_FRAME_STATISTICS st{};
        if (FAILED(sc->GetFrameStatistics(&st))) { ++r.stat_fail; continue; }
        if (st.PresentCount != last_pc) {
            if (last_q) {
                double ms = (double)(st.SyncQPCTime.QuadPart - last_q) * 1000.0 / (double)qf.QuadPart;
                r.bucket[ms < 7 ? 0 : ms < 9.5 ? 1 : ms < 12.5 ? 2 : ms < 14.5 ? 3 : ms < 18 ? 4 : 5]++;
                if (st.SyncRefreshCount > last_rc) { per_sum += ms / (double)(st.SyncRefreshCount - last_rc); per_n++; }
                ++shown;
            }
            last_pc = st.PresentCount; last_q = st.SyncQPCTime.QuadPart; last_rc = st.SyncRefreshCount;
        }
    }
    double el = (double)(qnow() - t0) / (double)qf.QuadPart;
    r.presents = presents / el;
    r.shown = shown / el;
    r.period = per_n ? per_sum / (double)per_n : 0;
    return r;
}

void pt_log(const PtVariant& v, uint32_t w, uint32_t h, const PtResult& r) {
    XLOGI("ptest", "%-16s %4ux%-4u %s: %6.1f presents/s, %6.1f shown/s, refresh %5.2f ms (%5.1f Hz); spacing <7 %llu, ~8.3 %llu, "
          "~11 %llu, 13 %llu, ~16.7 %llu, >18 %llu%s", v.name, w, h, v.d3d11 ? "d3d11" : "d3d12", r.presents, r.shown, r.period,
          r.period > 0 ? 1000.0 / r.period : 0.0, (unsigned long long)r.bucket[0], (unsigned long long)r.bucket[1],
          (unsigned long long)r.bucket[2], (unsigned long long)r.bucket[3], (unsigned long long)r.bucket[4], (unsigned long long)r.bucket[5],
          FAILED(r.hr) ? " (Present failed)" : "");
}

DXGI_SWAP_CHAIN_DESC1 pt_desc(const PtVariant& v, uint32_t w, uint32_t h) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w; d.Height = h; d.Format = v.fmt;
    d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = v.buffers; d.Scaling = v.scaling; d.SwapEffect = v.effect; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = (v.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | (v.waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0) |
              v.extra_flags;
    return d;
}

HANDLE pt_waitable(IDXGISwapChain1* sc, bool on) {
    if (!on) return nullptr;
    ComPtr<IDXGISwapChain2> s2;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&s2)))) return nullptr;
    s2->SetMaximumFrameLatency(1);
    return s2->GetFrameLatencyWaitableObject();
}

const float* pt_color(uint64_t i) {
    static const float c[2][4] = {{0.08f, 0.08f, 0.10f, 1.f}, {0.11f, 0.11f, 0.14f, 1.f}};
    return c[(i / 4) & 1];   // two close dark greys: every frame differs from the one 4 back, gently
}

void pt_run_d3d12(ID3D12Device* dev, ID3D12CommandQueue* queue, void* cw, const PtVariant& v, uint32_t w, uint32_t h, double secs) {
    DXGI_SWAP_CHAIN_DESC1 d = pt_desc(v, w, h);
    ComPtr<IDXGISwapChain1> sc1;
    PtComp comp;
    HANDLE sh = nullptr;
    HRESULT hr;
    if (v.handle) {
        ComPtr<IDXGIFactoryMedia> fm;
        if (!pt_comp_handle(cw, w, h, comp, sh) || FAILED(g_factory.As(&fm))) { XLOGW("ptest", "%-16s: no surface handle path", v.name); pt_comp_detach(cw, comp); if (sh) CloseHandle(sh); return; }
        hr = fm->CreateSwapChainForCompositionSurfaceHandle(queue, sh, &d, nullptr, &sc1);
    } else {
        hr = v.composition ? g_factory->CreateSwapChainForComposition(queue, &d, nullptr, &sc1)
                           : g_factory->CreateSwapChainForCoreWindow(queue, (IUnknown*)cw, &d, nullptr, &sc1);
    }
    if (FAILED(hr)) {
        XLOGW("ptest", "%-16s %ux%u: CreateSwapChainFor%s 0x%08x", v.name, w, h,
              v.handle ? "CompositionSurfaceHandle" : v.composition ? "Composition" : "CoreWindow", (unsigned)hr);
        pt_comp_detach(cw, comp);
        if (sh) CloseHandle(sh);
        return;
    }
    if (v.composition && !pt_comp_attach(cw, sc1.Get(), w, h, comp)) { XLOGW("ptest", "%-16s: no composition visual", v.name); return; }
    if (v.present_duration) pt_media(sc1.Get(), v);
    ComPtr<IDXGISwapChain3> sc;
    sc1.As(&sc);
    const UINT n = v.buffers;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = n;
    ComPtr<ID3D12DescriptorHeap> heap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = heap->GetCPUDescriptorHandleForHeapStart();
    std::vector<ComPtr<ID3D12Resource>> bufs(n);
    std::vector<ComPtr<ID3D12CommandAllocator>> alloc(n);
    std::vector<UINT64> slot(n, 0);
    for (UINT i = 0; i < n; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&bufs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE hh = rtv0; hh.ptr += (SIZE_T)i * step;
        dev->CreateRenderTargetView(bufs[i].Get(), nullptr, hh);
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
    }
    ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    HANDLE wt = pt_waitable(sc1.Get(), v.waitable);
    UINT pflags = v.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    PtResult r = pt_measure(sc1.Get(), secs, wt, [&](uint64_t i) -> HRESULT {
        UINT bi = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < slot[bi]) { fence->SetEventOnCompletion(slot[bi], fev); WaitForSingleObject(fev, 1000); }
        alloc[bi]->Reset();
        cl->Reset(alloc[bi].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[bi].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0; rtv.ptr += (SIZE_T)bi * step;
        cl->ClearRenderTargetView(rtv, pt_color(i), 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        HRESULT ph = sc->Present(v.sync, pflags);
        queue->Signal(fence.Get(), ++fv);
        slot[bi] = fv;
        return ph;
    });
    pt_log(v, w, h, r);
    queue->Signal(fence.Get(), ++fv);
    if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, fev); WaitForSingleObject(fev, 2000); }
    CloseHandle(fev);
    if (wt) CloseHandle(wt);
    sc.Reset(); sc1.Reset();
    if (v.composition || v.handle) pt_comp_detach(cw, comp);
    if (sh) CloseHandle(sh);
}

void pt_run_d3d11(void* cw, const PtVariant& v, uint32_t w, uint32_t h, double secs) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { XLOGW("ptest", "%-16s: D3D11CreateDevice 0x%08x", v.name, (unsigned)hr); return; }
    DXGI_SWAP_CHAIN_DESC1 d = pt_desc(v, w, h);
    ComPtr<IDXGISwapChain1> sc;
    hr = g_factory->CreateSwapChainForCoreWindow(dev.Get(), (IUnknown*)cw, &d, nullptr, &sc);
    if (FAILED(hr)) { XLOGW("ptest", "%-16s %ux%u: CreateSwapChainForCoreWindow (d3d11) 0x%08x", v.name, w, h, (unsigned)hr); return; }
    ComPtr<ID3D11Texture2D> b0;
    sc->GetBuffer(0, IID_PPV_ARGS(&b0));
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(b0.Get(), nullptr, &rtv);
    HANDLE wt = pt_waitable(sc.Get(), v.waitable);
    UINT pflags = v.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    PtResult r = pt_measure(sc.Get(), secs, wt, [&](uint64_t i) -> HRESULT {
        ctx->ClearRenderTargetView(rtv.Get(), pt_color(i));
        return sc->Present(v.sync, pflags);
    });
    pt_log(v, w, h, r);
    ctx->ClearState();
    ctx->Flush();
    if (wt) CloseHandle(wt);
}

// The display's own vblank, where the console gives the swapchain an output
void pt_vblank(IDXGISwapChain1* sc) {
    ComPtr<IDXGIOutput> out;
    HRESULT hr = sc->GetContainingOutput(&out);
    if (FAILED(hr)) { XLOGI("ptest", "vblank: GetContainingOutput 0x%08x (no output object for a UWP window)", (unsigned)hr); return; }
    LARGE_INTEGER qf, a, b; QueryPerformanceFrequency(&qf);
    out->WaitForVBlank();
    QueryPerformanceCounter(&a);
    int n = 0;
    for (; n < 240; ++n) if (FAILED(out->WaitForVBlank())) break;
    QueryPerformanceCounter(&b);
    double ms = n ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart / n : 0;
    XLOGI("ptest", "vblank: IDXGIOutput::WaitForVBlank every %.3f ms (%.1f Hz) over %d vblanks", ms, ms > 0 ? 1000.0 / ms : 0.0, n);
}

}  // namespace

void present_test() {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    void* cw = xrl::window_info_get().core_window;
    if (!cw) { XLOGE("ptest", "no CoreWindow"); return; }
    if (FAILED(ensure_factory())) { XLOGE("ptest", "no DXGI factory"); return; }
    auto mode = xrl::display_mode();
    auto wi = xrl::window_info_get();
    BOOL tearing = FALSE;
    {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(g_factory.As(&f5)) &&
            FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)))
            tearing = FALSE;
    }
    XLOGI("ptest", "HDMI mode %ux%u @ %u Hz; window %ux%u px; ALLOW_TEARING %d", mode.width, mode.height, mode.refresh_hz,
          wi.width, wi.height, (int)tearing);
    // a fresh D3D12 device per variant: one that fails (device removed) must not take the rest down with it
    auto make12 = [](ComPtr<ID3D12Device>& dev, ComPtr<ID3D12CommandQueue>& queue) {
        dev.Reset(); queue.Reset();
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
               SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    };
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    if (!make12(dev, queue)) { XLOGE("ptest", "no D3D12 device/queue"); return; }

    const uint32_t mw = mode.width ? mode.width : 1920, mh = mode.height ? mode.height : 1080;
    const uint32_t ww = wi.width ? wi.width : 1920, wh = wi.height ? wi.height : 1080;
    const double secs = 3.0;
    auto run = [&](PtVariant v) {
        uint32_t w = v.w ? v.w : mw, h = v.h ? v.h : mh;
        if (v.tearing && !tearing) return;
        if (v.d3d11) { pt_run_d3d11(cw, v, w, h, secs); return; }
        if (!make12(dev, queue)) { XLOGW("ptest", "%-16s: no D3D12 device", v.name); return; }
        pt_run_d3d12(dev.Get(), queue.Get(), cw, v, w, h, secs);
    };
    auto vblank = [&] {   // the display's vblank, through a throwaway swapchain's output
        PtVariant v{"vblank"};
        DXGI_SWAP_CHAIN_DESC1 d = pt_desc(v, mw, mh);
        ComPtr<IDXGISwapChain1> sc;
        if (make12(dev, queue) && SUCCEEDED(g_factory->CreateSwapChainForCoreWindow(queue.Get(), (IUnknown*)cw, &d, nullptr, &sc)))
            pt_vblank(sc.Get());
    };
    vblank();
    PtVariant v;
    v = {"control"};                                              run(v);
    v = {"no-vsync"};      v.sync = 0;                            run(v);
    v = {"window-size"};   v.w = ww; v.h = wh;                    run(v);
    v = {"d3d11"};         v.d3d11 = true;                        run(v);
    v = {"handle"};        v.handle = true;                       run(v);
    v = {"handle-nv"};     v.handle = true; v.sync = 0;           run(v);
    v = {"handle-tear"};   v.handle = true; v.sync = 0; v.tearing = true; run(v);
    v = {"handle-dur120"}; v.handle = true; v.present_duration = 83333; run(v);
    vblank();
    XLOGI("ptest", "present test done");
}

// XRL_PRESENT_TEST=2: a test to watch on the TV (or film in slow motion). A white bar crosses the screen at a constant
// speed (moved by the clock, not per frame), so a smoother phase shows a smoother bar; N small squares top-left
// say which phase it is. 15 s each:
//   1     vsync              Present(1, 0): today's path (the window's 60 Hz clock)
//   2     tearing @120       ALLOW_TEARING, paced to 120 fps by the CPU clock: straight to the 120 Hz scanout, a tear line
//   3..7  tearing in vblank  ALLOW_TEARING at the window's vblank tick (WaitForVBlank, 60 Hz) + 0/2/4/6/8 ms and 8.333 ms
//                            later: the tear line moves with the offset; where it disappears, both flips land in the
//                            real vblanks: 120 Hz with no tear
void present_visual_test(int kind) {   // 0: 7-phase sweep, 1: 60/120 A/B, 2: early-flip sweep, 3: fine sweep, 4: stripes
    const bool ab = kind != 0;
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    void* cw = xrl::window_info_get().core_window;
    if (!cw || FAILED(ensure_factory())) { XLOGE("ptest", "no CoreWindow/factory"); return; }
    auto mode = xrl::display_mode();
    const uint32_t w = mode.width ? mode.width : 1920, h = mode.height ? mode.height : 1080;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) ||
        FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { XLOGE("ptest", "no D3D12 device/queue"); return; }
    const UINT n = 3;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = n; d.Scaling = DXGI_SCALING_STRETCH; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE; d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = g_factory->CreateSwapChainForCoreWindow(queue.Get(), (IUnknown*)cw, &d, nullptr, &sc1);
    if (FAILED(hr)) { XLOGE("ptest", "visual: CreateSwapChainForCoreWindow 0x%08x", (unsigned)hr); return; }
    ComPtr<IDXGISwapChain3> sc;
    sc1.As(&sc);
    ComPtr<IDXGIOutput> out;
    sc1->GetContainingOutput(&out);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = n;
    ComPtr<ID3D12DescriptorHeap> heap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = heap->GetCPUDescriptorHandleForHeapStart();
    ComPtr<ID3D12Resource> bufs[n];
    ComPtr<ID3D12CommandAllocator> alloc[n];
    UINT64 slot[n] = {};
    for (UINT i = 0; i < n; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&bufs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE hh = rtv0; hh.ptr += (SIZE_T)i * step;
        dev->CreateRenderTargetView(bufs[i].Get(), nullptr, hh);
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]));
    }
    ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    UINT64 fv = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    auto qnow = [] { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; };
    // The window's vblank tick, watched on its own thread so the flip loop never waits on it (waiting made it miss
    // cycles). Each tick is stamped when WaitForVBlank returns, which is late by a varying wake-up delay: the period
    // is the long-run average, and the phase the earliest stamp of the last 32 against that period (the delay only
    // ever adds). tick_est = the estimated time of the latest tick.
    std::atomic<int64_t> tick_est{0};
    std::atomic<bool> watch{out != nullptr};
    std::thread watcher;
    if (out) watcher = std::thread([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        int64_t t0 = 0, n = 0, res[32];
        for (int64_t& r : res) r = INT64_MAX;
        while (watch.load()) {
            if (FAILED(out->WaitForVBlank())) break;
            int64_t t = qnow();
            if (!t0) { t0 = t; continue; }
            ++n;
            double per = (double)(t - t0) / (double)n;
            res[n % 32] = t - t0 - (int64_t)(per * (double)n);
            int64_t best = res[n % 32];
            for (int i = 0; i < 32 && i < n; ++i) best = std::min(best, res[i]);
            if (n >= 8) tick_est.store(t0 + (int64_t)(per * (double)n) + best);
        }
    });
    // a high-resolution sleep to ~2 ms before, then spin: the sleep can overshoot by ~0.5 ms, the spin is exact
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    auto wait_until = [&](int64_t t) {
        int64_t left = t - qnow();
        int64_t slack = qf.QuadPart / 500;
        if (left > slack && timer) {
            LARGE_INTEGER due; due.QuadPart = -((left - slack) * 10000000 / qf.QuadPart);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }
        while (qnow() < t) YieldProcessor();
    };
    const int64_t t_start = qnow();
    // draws the frame for the clock time `at` into the current buffer and queues it (no Present yet)
    auto draw = [&](int phase, int64_t at) {
        UINT bi = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < slot[bi]) { fence->SetEventOnCompletion(slot[bi], fev); WaitForSingleObject(fev, 1000); }
        alloc[bi]->Reset();
        cl->Reset(alloc[bi].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[bi].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0; rtv.ptr += (SIZE_T)bi * step;
        const float bg[4] = {0.06f, 0.06f, 0.09f, 1.f}, white[4] = {1.f, 1.f, 1.f, 1.f}, mark[4] = {0.9f, 0.6f, 0.1f, 1.f};
        cl->ClearRenderTargetView(rtv, bg, 0, nullptr);
        double secs = (double)(at - t_start) / (double)qf.QuadPart;
        LONG x = (LONG)(fmod(secs * (double)w * 0.75, (double)w));   // 3/4 of the width per second
        D3D12_RECT bar = {x, 0, (LONG)std::min<uint32_t>(w, (uint32_t)x + 40), (LONG)h};
        if (kind == 4) {   // 1-px stripes left, 2-px stripes right: sharp at a true 1:1 1440p, grey or moire if rescaled
            static std::vector<D3D12_RECT> stripes;
            if (stripes.empty()) {
                for (uint32_t sx = 0; sx < w / 2; sx += 2) stripes.push_back({(LONG)sx, 0, (LONG)sx + 1, (LONG)h});
                for (uint32_t sx = w / 2; sx < w; sx += 4) stripes.push_back({(LONG)sx, 0, (LONG)sx + 2, (LONG)h});
            }
            cl->ClearRenderTargetView(rtv, white, (UINT)stripes.size(), stripes.data());
        } else {
            cl->ClearRenderTargetView(rtv, white, 1, &bar);
        }
        if (ab) {   // a big seven-segment "1" (today's 60 Hz path) or "2" (the 120 Hz trick) top-left
            const LONG X = 60, Y = 60, W = 110, H = 220, T = 24;
            const D3D12_RECT seg[7] = {
                {X, Y, X + W, Y + T},                       // a top
                {X + W - T, Y, X + W, Y + H / 2},           // b top right
                {X + W - T, Y + H / 2, X + W, Y + H},       // c bottom right
                {X, Y + H - T, X + W, Y + H},               // d bottom
                {X, Y + H / 2, X + T, Y + H},               // e bottom left
                {X, Y, X + T, Y + H / 2},                   // f top left
                {X, Y + H / 2 - T / 2, X + W, Y + H / 2 + T / 2}};   // g middle
            static const unsigned char digit_segs[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};   // bit k = seg k
            const int shown = kind == 1 || kind == 4 ? (phase == 1 ? 1 : 2) : kind == 2 ? phase - 7 : phase - 11;   // kind 2: 8..11, 3: 12..16
            for (int k = 0; k < 7; ++k)
                if (digit_segs[shown % 10] & (1 << k)) cl->ClearRenderTargetView(rtv, mark, 1, &seg[k]);
        } else {
            for (int k = 0; k < phase; ++k) {
                D3D12_RECT sq = {40 + k * 70, 40, 40 + k * 70 + 50, 90};
                cl->ClearRenderTargetView(rtv, mark, 1, &sq);
            }
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        return bi;
    };
    auto present = [&](UINT bi, UINT sync, UINT flags) {
        HRESULT ph = sc->Present(sync, flags);
        queue->Signal(fence.Get(), ++fv);
        slot[bi] = fv;
        return ph;
    };
    // phases 3..7: flips at the window's vblank tick + 0/2/4/6/8 ms and 8.333 ms after that; the tear line moves
    // down the screen with the offset, and the offset that hides it is where the real vblanks are
    // ab (XRL_PRESENT_TEST=3): phase 1 and phase 3 (+0 ms, the tear-free 120 candidate) in turn, 10 s each, 3 times
    const int64_t phase_len = qf.QuadPart * (ab ? 10 : 15);
    const int64_t f120 = qf.QuadPart * 1000 / 120000;   // 8.333 ms
    // kind 2: phases 8..11 flip 0.5/1/1.5/2 ms BEFORE the 120 Hz grid (vblank tick + 8.333 and + 16.667 ms), twice
    // kind 3: phases 12..16 flip 0.05..0.25 ms before the 120 Hz grid, twice (0.30 was tear-free, 0.33+ tore at the bottom)
    const int nsteps = kind == 0 ? 7 : kind == 1 ? 6 : kind == 2 ? 8 : kind == 3 ? 10 : 4;
    {   // let the tick watcher settle (its phase estimate needs a few dozen ticks) before the first timed phase
        int64_t ready = qnow() + qf.QuadPart * 2;
        while (qnow() < ready) present(draw(0, qnow()), 1, 0);
    }
    for (int stepn = 1; stepn <= nsteps; ++stepn) {
        const int phase = kind == 0 ? stepn : kind == 1 ? (stepn % 2 ? 1 : 3) : kind == 2 ? 8 + (stepn - 1) % 4
                        : kind == 3 ? 12 + (stepn - 1) % 5 : (stepn % 2 ? 1 : 17);   // 17: the tear-free 120 Hz timing
        int64_t t0 = qnow(), next = t0;
        uint64_t frames = 0;
        int64_t late_sum = 0, late_max = 0, late_n = 0;   // Present called this long after its target (QPC ticks)
        static const double kFine[5] = {0.05, 0.10, 0.15, 0.20, 0.25};   // ms before the grid, numbers 1..5
        const int64_t off = phase == 17 ? f120 - (int64_t)((double)qf.QuadPart * 0.30 / 1000.0)   // 0.30 ms early: tear-free
                          : phase >= 12 ? f120 - (int64_t)((double)qf.QuadPart * kFine[phase - 12] / 1000.0)
                          : phase >= 8 ? f120 - qf.QuadPart * (phase - 7) / 2000   // 8.333 ms minus 0.5 * (1..4) ms
                          : phase >= 3 ? qf.QuadPart * 2 * (phase - 3) / 1000 : 0;
        while (qnow() - t0 < phase_len) {
            HRESULT ph = S_OK;
            if (phase == 1) {
                ph = present(draw(phase, qnow()), 1, 0);
            } else if (phase == 2) {
                next += f120;
                UINT bi = draw(phase, next);
                wait_until(next);
                ph = present(bi, 0, DXGI_PRESENT_ALLOW_TEARING);
            } else {
                if (!out) { XLOGW("ptest", "visual: no output for WaitForVBlank; phase %d skipped", phase); break; }
                int64_t tk = tick_est.load();
                if (!tk) { Sleep(1); continue; }
                // the next flip on the 120 Hz grid tick + off + k * 8.333 ms that is at least 1 ms away
                int64_t base = tk + off, diff = qnow() + qf.QuadPart / 1000 - base;
                int64_t at = base + (diff <= 0 ? 0 : (diff + f120 - 1) / f120) * f120;
                UINT bi = draw(phase, at);
                wait_until(at);
                int64_t late = qnow() - at;
                ph = present(bi, 0, DXGI_PRESENT_ALLOW_TEARING);
                late_sum += late; if (late > late_max) late_max = late; ++late_n;
            }
            if (FAILED(ph)) { XLOGE("ptest", "visual phase %d: Present 0x%08x", phase, (unsigned)ph); break; }
            ++frames;
        }
        XLOGI("ptest", "visual phase %d (flip offset %.2f ms): %.1f presents/s; Present %.3f ms after target avg, %.3f max",
              phase, (double)off * 1000.0 / (double)qf.QuadPart, frames * (double)qf.QuadPart / (double)(qnow() - t0),
              late_n ? (double)late_sum * 1000.0 / (double)qf.QuadPart / (double)late_n : 0.0,
              (double)late_max * 1000.0 / (double)qf.QuadPart);
    }
    watch.store(false);
    if (watcher.joinable()) watcher.join();
    queue->Signal(fence.Get(), ++fv);
    if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, fev); WaitForSingleObject(fev, 2000); }
    CloseHandle(fev);
    if (timer) CloseHandle(timer);
    XLOGI("ptest", "present test done");
}

// XRL_DISCOVER (discover.cpp): the interfaces our graphics objects answer, the GPU's features, the adapters and outputs
// with their display modes. The window's swapchain is a real CoreWindow one (the keepalive is stopped first).
namespace {
#define XRL_IID(T) {__uuidof(T), #T}
struct NamedIid { IID iid; const char* name; };
const NamedIid kIids[] = {
    XRL_IID(IDXGIFactory1), XRL_IID(IDXGIFactory2), XRL_IID(IDXGIFactory3), XRL_IID(IDXGIFactory4), XRL_IID(IDXGIFactory5),
    XRL_IID(IDXGIFactory6), XRL_IID(IDXGIFactory7), XRL_IID(IDXGIFactoryMedia), XRL_IID(IDXGIDisplayControl),
    XRL_IID(IDXGIAdapter1), XRL_IID(IDXGIAdapter2), XRL_IID(IDXGIAdapter3), XRL_IID(IDXGIAdapter4),
    XRL_IID(IDXGIOutput), XRL_IID(IDXGIOutput1), XRL_IID(IDXGIOutput2), XRL_IID(IDXGIOutput3), XRL_IID(IDXGIOutput4),
    XRL_IID(IDXGIOutput5), XRL_IID(IDXGIOutput6),
    XRL_IID(IDXGISwapChain1), XRL_IID(IDXGISwapChain2), XRL_IID(IDXGISwapChain3), XRL_IID(IDXGISwapChain4),
    XRL_IID(IDXGISwapChainMedia), XRL_IID(IDXGIDecodeSwapChain),
    XRL_IID(IDXGIDevice), XRL_IID(IDXGIDevice1), XRL_IID(IDXGIDevice2), XRL_IID(IDXGIDevice3), XRL_IID(IDXGIDevice4),
    XRL_IID(ID3D12Device), XRL_IID(ID3D12Device1), XRL_IID(ID3D12Device2), XRL_IID(ID3D12Device3), XRL_IID(ID3D12Device4),
    XRL_IID(ID3D12Device5), XRL_IID(ID3D12Device6), XRL_IID(ID3D12Device7), XRL_IID(ID3D12Device8), XRL_IID(ID3D12Device9),
    XRL_IID(ID3D12Device10), XRL_IID(ID3D12Device11), XRL_IID(ID3D12Device12), XRL_IID(ID3D12Device13), XRL_IID(ID3D12Device14),
    XRL_IID(ID3D12CommandQueue),
    XRL_IID(ID3D11Device), XRL_IID(ID3D11Device1), XRL_IID(ID3D11Device2), XRL_IID(ID3D11Device3), XRL_IID(ID3D11Device4),
    XRL_IID(ID3D11Device5), XRL_IID(ID3D11DeviceContext1), XRL_IID(ID3D11DeviceContext2), XRL_IID(ID3D11DeviceContext3),
    XRL_IID(ID3D11DeviceContext4), XRL_IID(ID3D11Multithread), XRL_IID(ID3D11On12Device), XRL_IID(ID3D11On12Device1),
    XRL_IID(ID3D11On12Device2),
};
#undef XRL_IID

void gd_ifaces(FILE* f, const char* what, IUnknown* o) {
    if (!o) { fprintf(f, "%s: (none)\n", what); return; }
    std::string yes, no;
    for (const auto& i : kIids) {
        ComPtr<IUnknown> q;
        (SUCCEEDED(o->QueryInterface(i.iid, (void**)&q)) ? yes : no) += std::string(i.name) + " ";
    }
    fprintf(f, "%s answers: %s\n", what, yes.c_str());
}

void gd_outputs(FILE* f, IDXGIOutput* o, const char* what) {
    if (!o) return;
    DXGI_OUTPUT_DESC d{};
    o->GetDesc(&d);
    fprintf(f, "%s: %ls, desktop %ldx%ld, attached %d\n", what, d.DeviceName, d.DesktopCoordinates.right - d.DesktopCoordinates.left,
            d.DesktopCoordinates.bottom - d.DesktopCoordinates.top, (int)d.AttachedToDesktop);
    ComPtr<IDXGIOutput6> o6;
    if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6)))) {
        DXGI_OUTPUT_DESC1 d1{};
        if (SUCCEEDED(o6->GetDesc1(&d1)))
            fprintf(f, "  bits %u, color space %d, luminance %.0f..%.0f nits\n", d1.BitsPerColor, (int)d1.ColorSpace, d1.MinLuminance, d1.MaxLuminance);
        UINT flags = 0;
        if (SUCCEEDED(o6->CheckHardwareCompositionSupport(&flags))) fprintf(f, "  hardware composition support flags 0x%x\n", flags);
    }
    for (DXGI_FORMAT fmt : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
        UINT n = 0;
        HRESULT hr = o->GetDisplayModeList(fmt, DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING, &n, nullptr);
        std::vector<DXGI_MODE_DESC> m(n);
        if (SUCCEEDED(hr) && n) o->GetDisplayModeList(fmt, DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING, &n, m.data());
        fprintf(f, "  modes fmt %d: 0x%08x, %u\n", (int)fmt, (unsigned)hr, n);
        for (UINT i = 0; i < n && i < 40; i++)
            fprintf(f, "    %ux%u @ %u/%u scaling %d\n", m[i].Width, m[i].Height, m[i].RefreshRate.Numerator, m[i].RefreshRate.Denominator,
                    (int)m[i].Scaling);
    }
}

void gd_features12(FILE* f, ID3D12Device* d) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    for (int v = D3D_SHADER_MODEL_6_9; v >= D3D_SHADER_MODEL_5_1; v--) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)v;
        if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) break;
    }
    fprintf(f, "d3d12 highest shader model 0x%x\n", (unsigned)sm.HighestShaderModel);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
        fprintf(f, "d3d12 binding tier %d, tiled tier %d, conservative raster %d, ROVs %d, typed UAV load additional formats %d\n",
                (int)o.ResourceBindingTier, (int)o.TiledResourcesTier, (int)o.ConservativeRasterizationTier, (int)o.ROVsSupported,
                (int)o.TypedUAVLoadAdditionalFormats);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1)))
        fprintf(f, "d3d12 wave ops %d, lanes %u..%u\n", (int)o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)))
        fprintf(f, "d3d12 raytracing tier %d, render passes tier %d\n", (int)o5.RaytracingTier, (int)o5.RenderPassesTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6)))
        fprintf(f, "d3d12 variable rate shading tier %d\n", (int)o6.VariableShadingRateTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7)))
        fprintf(f, "d3d12 mesh shader tier %d, sampler feedback tier %d\n", (int)o7.MeshShaderTier, (int)o7.SamplerFeedbackTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12)))
        fprintf(f, "d3d12 enhanced barriers %d, relaxed format casting %d\n", (int)o12.EnhancedBarriersSupported, (int)o12.RelaxedFormatCastingSupported);
    for (int k = (int)D3D12_FEATURE_D3D12_OPTIONS13; k <= (int)D3D12_FEATURE_D3D12_OPTIONS21; k++) {
        BYTE buf[256] = {};
        fprintf(f, "d3d12 feature %d: %s\n", k, SUCCEEDED(d->CheckFeatureSupport((D3D12_FEATURE)k, buf, sizeof buf)) ? "answered" : "not answered");
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 a{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &a, sizeof a)))
        fprintf(f, "d3d12 UMA %d, cache-coherent UMA %d, isolated MMU %d, tile-based %d\n", (int)a.UMA, (int)a.CacheCoherentUMA,
                (int)a.IsolatedMMU, (int)a.TileBasedRenderer);
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
    if (SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof va)))
        fprintf(f, "d3d12 GPU VA bits per resource %u, per process %u\n", va.MaxGPUVirtualAddressBitsPerResource, va.MaxGPUVirtualAddressBitsPerProcess);
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs{D3D_ROOT_SIGNATURE_VERSION_1_2};
    if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs))) { rs.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1; d->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs); }
    fprintf(f, "d3d12 root signature 0x%x\n", (unsigned)rs.HighestVersion);
}
}  // namespace

void gfx_discover(FILE* f) {
    keepalive_stop_wait();   // the window takes one swapchain
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    if (FAILED(ensure_factory())) { fprintf(f, "no DXGI factory\n"); return; }
    gd_ifaces(f, "DXGI factory", g_factory.Get());
    BOOL tear = FALSE;
    { ComPtr<IDXGIFactory5> f5; if (SUCCEEDED(g_factory.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear); }
    fprintf(f, "ALLOW_TEARING %d\n", (int)tear);
    for (UINT a = 0;; a++) {
        ComPtr<IDXGIAdapter1> ad;
        if (g_factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        fprintf(f, "adapter %u: %ls, vendor %04x device %04x, dedicated %llu MiB, shared %llu MiB, flags 0x%x\n", a, d.Description, d.VendorId,
                d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned long long)(d.SharedSystemMemory >> 20), d.Flags);
        char what[32]; snprintf(what, sizeof what, "adapter %u", a);
        gd_ifaces(f, what, ad.Get());
        for (UINT o = 0;; o++) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            snprintf(what, sizeof what, "adapter %u output %u", a, o);
            gd_outputs(f, out.Get(), what);
            gd_ifaces(f, what, out.Get());
        }
    }
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) && SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        gd_ifaces(f, "D3D12 device", dev.Get());
        gd_ifaces(f, "D3D12 queue", queue.Get());
        gd_features12(f, dev.Get());
        void* cw = xrl::window_info_get().core_window;
        auto mode = xrl::display_mode();
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = mode.width ? mode.width : 1920; d.Height = mode.height ? mode.height : 1080; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.Flags = tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc;
        if (cw && SUCCEEDED(g_factory->CreateSwapChainForCoreWindow(queue.Get(), (IUnknown*)cw, &d, nullptr, &sc))) {
            gd_ifaces(f, "CoreWindow swapchain", sc.Get());
            ComPtr<IDXGIOutput> out;
            HRESULT hr = sc->GetContainingOutput(&out);
            fprintf(f, "swapchain GetContainingOutput 0x%08x\n", (unsigned)hr);
            if (out) { gd_outputs(f, out.Get(), "swapchain output"); gd_ifaces(f, "swapchain output", out.Get()); }
            ComPtr<IDXGISwapChain3> s3;
            if (SUCCEEDED(sc.As(&s3))) {
                UINT cs = 0;
                s3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &cs);
                fprintf(f, "swapchain HDR10 color space support 0x%x\n", cs);
            }
        } else fprintf(f, "CoreWindow swapchain: not created\n");
    } else fprintf(f, "no D3D12 device\n");
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, &fl, &c11);
    fprintf(f, "D3D11CreateDevice 0x%08x, feature level 0x%x\n", (unsigned)hr, (unsigned)fl);
    if (d11) {
        gd_ifaces(f, "D3D11 device", d11.Get());
        gd_ifaces(f, "D3D11 context", c11.Get());
        D3D11_FEATURE_DATA_THREADING th{};
        d11->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof th);
        fprintf(f, "d3d11 concurrent creates %d, command lists %d\n", (int)th.DriverConcurrentCreates, (int)th.DriverCommandLists);
    }
}

void keepalive_stop_wait() {
    if (!g_keepalive_run.exchange(false)) return;
    if (g_keepalive_ended && WaitForSingleObject(g_keepalive_ended, 3000) == WAIT_OBJECT_0)
        XLOGI("gfx", "keepalive: stopped; the game's swapchain has the window");
    else XLOGW("gfx", "keepalive: did not stop within 3 s");
}

}  // namespace

extern "C" const xrl_gfx_ops* xrl_gfx_ops_get_default(const xrl_gfx_host_services* host) {
    g_host = host;
    return &g_ops;
}

namespace xrl {

void gfx_keepalive_start() {
    bool was = g_keepalive_run.exchange(true);
    if (was) return;   // already running
    try {
        if (!g_keepalive_ended) g_keepalive_ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ResetEvent(g_keepalive_ended);
        g_keepalive_th = std::thread([] { keepalive_loop(); SetEvent(g_keepalive_ended); });
        g_keepalive_th.detach();
    } catch (...) {
        g_keepalive_run.store(false);
        XLOGE("gfx", "keepalive: could not start thread");
    }
}

void gfx_keepalive_stop() { g_keepalive_run.store(false); }

void gfx_discover_run(FILE* f) { gfx_discover(f); }

// The corner test: a swapchain at the HDMI mode's size (1440p) shown with DXGI_SCALING_NONE, then STRETCH, an orange
// square in each corner. With NONE a 1080p plane crops it (the right and bottom squares are gone), a 1440p plane shows
// all four.
void corners_test() {
    void* cw = xrl::window_info_get().core_window;
    if (!cw || FAILED(ensure_factory())) return;
    auto mode = xrl::display_mode();
    const uint32_t w = mode.width ? mode.width : 2560, h = mode.height ? mode.height : 1440;
    for (DXGI_SCALING sc_mode : {DXGI_SCALING_NONE, DXGI_SCALING_STRETCH}) {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.Scaling = sc_mode;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> sc;
        HRESULT hr = g_factory->CreateSwapChainForCoreWindow(dev.Get(), (IUnknown*)cw, &d, nullptr, &sc);
        XLOGI("ptest", "corners: %ux%u scaling %s: 0x%08x", w, h, sc_mode == DXGI_SCALING_NONE ? "NONE" : "STRETCH", (unsigned)hr);
        if (FAILED(hr)) continue;
        ComPtr<ID3D11Texture2D> b0;
        sc->GetBuffer(0, IID_PPV_ARGS(&b0));
        ComPtr<ID3D11RenderTargetView> rtv;
        dev->CreateRenderTargetView(b0.Get(), nullptr, &rtv);
        ComPtr<ID3D11DeviceContext1> c1;
        ctx.As(&c1);
        const float bg[4] = {0.06f, 0.06f, 0.09f, 1.f}, orange[4] = {0.95f, 0.55f, 0.1f, 1.f}, white[4] = {1, 1, 1, 1};
        const LONG s = 160;
        const D3D11_RECT corners[4] = {{0, 0, s, s}, {(LONG)w - s, 0, (LONG)w, s}, {0, (LONG)h - s, s, (LONG)h}, {(LONG)w - s, (LONG)h - s, (LONG)w, (LONG)h}};
        const D3D11_RECT cross[2] = {{(LONG)w / 2 - 4, 0, (LONG)w / 2 + 4, (LONG)h}, {0, (LONG)h / 2 - 4, (LONG)w, (LONG)h / 2 + 4}};
        ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 15000) {
            c1->ClearView(rtv.Get(), bg, nullptr, 0);
            c1->ClearView(rtv.Get(), white, cross, 2);
            c1->ClearView(rtv.Get(), orange, corners, sc_mode == DXGI_SCALING_NONE ? 4 : 2);   // STRETCH: only the top two
            if (FAILED(sc->Present(1, 0))) break;
        }
        rtv.Reset(); b0.Reset(); sc.Reset();
        ctx->ClearState(); ctx->Flush();
    }
    XLOGI("ptest", "present test done");
}

// XRL_PRESENT_TEST=8: the video plane. A decode swapchain (IDXGIFactoryMedia::CreateDecodeSwapChainForCompositionSurfaceHandle)
// takes NV12 frames and is meant to be shown on a hardware video plane at the output's own resolution, apart from the app's
// 1080p window. 20 s of 1-px (left) and 2-px (right) luma stripes at the HDMI mode's size, neutral chroma; the present
// rate and every call's result are logged. No teardown: the composition teardown crashed before, and the app exits after.
void decode_test() {
    void* cw = xrl::window_info_get().core_window;
    if (!cw || FAILED(ensure_factory())) return;
    auto mode = xrl::display_mode();
    const uint32_t w = mode.width ? mode.width : 2560, h = mode.height ? mode.height : 1440;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    XLOGI("ptest", "decode: video-capable D3D11 device 0x%08x", (unsigned)hr);
    if (FAILED(hr)) return;
    // the frame: NV12, luma stripes, chroma 128
    const UINT n = 4;
    std::vector<uint8_t> nv12((size_t)w * h * 3 / 2);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            nv12[(size_t)y * w + x] = (x < w / 2 ? (x & 1) : ((x >> 1) & 1)) ? 235 : 16;
    memset(nv12.data() + (size_t)w * h, 128, (size_t)w * h / 2);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init{nv12.data(), w, 0};
    ComPtr<ID3D11Texture2D> src;
    hr = dev->CreateTexture2D(&td, &init, &src);
    XLOGI("ptest", "decode: NV12 source texture 0x%08x", (unsigned)hr);
    td.ArraySize = n; td.BindFlags = D3D11_BIND_DECODER;
    ComPtr<ID3D11Texture2D> bufs;
    hr = dev->CreateTexture2D(&td, nullptr, &bufs);
    XLOGI("ptest", "decode: NV12 decoder buffer array (%u) 0x%08x", n, (unsigned)hr);
    if (FAILED(hr) || !src) return;
    for (UINT i = 0; i < n; i++) ctx->CopySubresourceRegion(bufs.Get(), i, 0, 0, 0, src.Get(), 0, nullptr);
    ctx->Flush();
    PtComp comp;
    HANDLE sh = nullptr;
    auto wi = xrl::window_info_get();
    if (!pt_comp_handle(cw, wi.width ? wi.width : 1920, wi.height ? wi.height : 1080, comp, sh)) { XLOGW("ptest", "decode: no surface handle"); return; }
    ComPtr<IDXGIFactoryMedia> fm;
    ComPtr<IDXGIResource> res;
    if (FAILED(g_factory.As(&fm)) || FAILED(bufs.As(&res))) { XLOGW("ptest", "decode: no media factory"); return; }
    DXGI_DECODE_SWAP_CHAIN_DESC dd{0};
    ComPtr<IDXGIDecodeSwapChain> dsc;
    hr = fm->CreateDecodeSwapChainForCompositionSurfaceHandle(dev.Get(), sh, &dd, res.Get(), nullptr, &dsc);
    XLOGI("ptest", "decode: CreateDecodeSwapChainForCompositionSurfaceHandle 0x%08x", (unsigned)hr);
    if (FAILED(hr)) return;
    RECT r{0, 0, (LONG)w, (LONG)h};
    XLOGI("ptest", "decode: SetSourceRect 0x%08x, SetDestSize %ux%u 0x%08x, SetColorSpace BT709 0x%08x", (unsigned)dsc->SetSourceRect(&r), w, h,
          (unsigned)dsc->SetDestSize(w, h), (unsigned)dsc->SetColorSpace(DXGI_MULTIPLANE_OVERLAY_YCbCr_FLAG_BT709));
    ComPtr<IDXGISwapChain1> as1;
    XLOGI("ptest", "decode: also an IDXGISwapChain1 (frame statistics): %s", SUCCEEDED(dsc.As(&as1)) ? "yes" : "no");
    LARGE_INTEGER qf, a, b; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&a);
    uint64_t presents = 0, shown = 0;
    UINT last_pc = 0;
    HRESULT ph = S_OK;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 20000) {
        ph = dsc->PresentBuffer((UINT)(presents % n), 1, 0);
        if (FAILED(ph)) break;
        ++presents;
        DXGI_FRAME_STATISTICS st{};
        if (as1 && SUCCEEDED(as1->GetFrameStatistics(&st)) && st.PresentCount != last_pc) { ++shown; last_pc = st.PresentCount; }
    }
    QueryPerformanceCounter(&b);
    double secs = (double)(b.QuadPart - a.QuadPart) / (double)qf.QuadPart;
    XLOGI("ptest", "decode: %.1f presents/s (PresentBuffer sync 1), %.1f shown/s by frame statistics, last result 0x%08x",
          presents / secs, shown / secs, (unsigned)ph);
    XLOGI("ptest", "present test done");
    ExitProcess(0);   // no teardown (see above)
}

// XRL_PRESENT_TEST=9: the video path. First what the output says about hardware overlay planes (CheckOverlaySupport),
// then LocalState\stripes1440p120.mp4 (2560x1440, 120 fps, 1-px / 2-px luma stripes, a moving bar) played by the console's
// MediaPlayer onto a composition surface on the window, 25 s, looping. The video player is the one path the system gives
// its own "max resolution" swapchains; the user judges whether the 1-px stripes survive. Exits without teardown.
void media_test() {
    void* cw = xrl::window_info_get().core_window;
    if (!cw || FAILED(ensure_factory())) return;
    {   // overlay planes
        ComPtr<ID3D11Device> dev;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        ComPtr<IDXGIAdapter1> ad;
        ComPtr<IDXGIOutput> out;
        ComPtr<IDXGIOutput3> o3;
        ComPtr<IDXGIOutput4> o4;
        if (dev && SUCCEEDED(g_factory->EnumAdapters1(0, &ad)) && SUCCEEDED(ad->EnumOutputs(0, &out)) && SUCCEEDED(out.As(&o3))) {
            for (DXGI_FORMAT f : {DXGI_FORMAT_NV12, DXGI_FORMAT_YUY2, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
                UINT fl = 0;
                HRESULT hr = o3->CheckOverlaySupport(f, dev.Get(), &fl);
                XLOGI("ptest", "media: CheckOverlaySupport(format %d) 0x%08x flags 0x%x", (int)f, (unsigned)hr, fl);
            }
            if (SUCCEEDED(out.As(&o4))) {
                UINT fl = 0;
                HRESULT hr = o4->CheckOverlayColorSpaceSupport(DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, dev.Get(), &fl);
                XLOGI("ptest", "media: CheckOverlayColorSpaceSupport(NV12, 709) 0x%08x flags 0x%x", (unsigned)hr, fl);
            }
        } else XLOGW("ptest", "media: no output to ask about overlays");
    }
    PtComp comp;
    winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
    auto wi = xrl::window_info_get();
    try {
        pt_dispatcher(cw).RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [&] {
            try {
                namespace wc = winrt::Windows::UI::Composition;
                comp.comp = wc::Compositor();
                comp.target = comp.comp.CreateTargetForCurrentView();
                player = winrt::Windows::Media::Playback::MediaPlayer();
                player.IsLoopingEnabled(true);
                player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromUri(
                    winrt::Windows::Foundation::Uri(L"ms-appdata:///local/stripes1440p120.mp4")));
                player.SetSurfaceSize({2560.f, 1440.f});
                auto surface = player.GetSurface(comp.comp);
                auto visual = comp.comp.CreateSpriteVisual();
                visual.Size({(float)(wi.width ? wi.width : 1920), (float)(wi.height ? wi.height : 1080)});
                visual.Brush(comp.comp.CreateSurfaceBrush(surface.CompositionSurface()));
                comp.target.Root(visual);
                player.Play();
                XLOGI("ptest", "media: playing");
            } catch (const winrt::hresult_error& e) {
                XLOGW("ptest", "media: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            } catch (...) { XLOGW("ptest", "media: setup failed"); }
        }).get();
    } catch (...) { XLOGW("ptest", "media: the UI thread did not run the setup"); return; }
    for (int i = 0; i < 25 && player; i++) {
        Sleep(1000);
        try {
            auto ps = player.PlaybackSession();
            if (i % 5 == 0 || i == 24)
                XLOGI("ptest", "media: t %d s, state %d, video %ux%u, position %.2f s, rate %.2f", i, (int)ps.PlaybackState(),
                      ps.NaturalVideoWidth(), ps.NaturalVideoHeight(), ps.Position().count() / 1e7, ps.PlaybackRate());
        } catch (...) {}
    }
    XLOGI("ptest", "present test done");
    ExitProcess(0);
}

void gfx_present_test(int mode) {
    keepalive_stop_wait();   // the window takes one swapchain
    if (mode == 9) { media_test(); return; }
    if (mode == 8) { decode_test(); return; }
    if (mode == 7) { corners_test(); return; }
    if (mode >= 2 && mode <= 6) present_visual_test(mode - 2);
    else present_test();
}

}  // namespace xrl

extern "C" const xrl_gfx_ops* xrl_host_gfx_ops(void) {
    const xrl_gfx_ops* ops = xrl_gfx_ops_get(&g_services);
    if (!ops) XLOGE("gfx", "xrl_gfx_ops_get returned NULL");
    return ops;
}

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
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <algorithm>
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
};

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
    d.Flags = (v.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | (v.waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
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
    HRESULT hr = g_factory->CreateSwapChainForCoreWindow(queue, (IUnknown*)cw, &d, nullptr, &sc1);
    if (FAILED(hr)) { XLOGW("ptest", "%-16s %ux%u: CreateSwapChainForCoreWindow 0x%08x", v.name, w, h, (unsigned)hr); return; }
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
    v = {"window-size"};   v.w = ww; v.h = wh;                    run(v);
    v = {"window-1:1"};    v.w = ww; v.h = wh; v.scaling = DXGI_SCALING_NONE; run(v);
    v = {"aspect"};        v.scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH; run(v);
    v = {"2-buffers"};     v.buffers = 2;                         run(v);
    v = {"4-buffers"};     v.buffers = 4;                         run(v);
    v = {"sequential"};    v.effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; run(v);
    v = {"10-bit"};        v.fmt = DXGI_FORMAT_R10G10B10A2_UNORM; run(v);
    v = {"fp16"};          v.fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; run(v);
    v = {"waitable-1"};    v.waitable = true;                     run(v);
    v = {"no-vsync"};      v.sync = 0;                            run(v);
    v = {"tearing"};       v.sync = 0; v.tearing = true;          run(v);
    v = {"d3d11"};         v.d3d11 = true;                        run(v);
    v = {"d3d11-window"};  v.d3d11 = true; v.w = ww; v.h = wh;    run(v);
    v = {"d3d11-waitable"}; v.d3d11 = true; v.waitable = true;    run(v);
    // the HDMI mode: 60 Hz as a control, then a fresh switch back to 120 Hz
    if (xrl::display_switch(mw, mh, 60)) { vblank(); v = {"hdmi-60"}; run(v); }
    if (xrl::display_switch(mw, mh, 120)) { vblank(); v = {"hdmi-120-again"}; run(v); v = {"no-vsync-again"}; v.sync = 0; run(v); }
    v = {"4k"};            v.w = 3840; v.h = 2160;                run(v);   // last: it removed the device once
    XLOGI("ptest", "present test done");
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

void gfx_present_test() {
    keepalive_stop_wait();   // the window takes one swapchain
    present_test();
}

}  // namespace xrl

extern "C" const xrl_gfx_ops* xrl_host_gfx_ops(void) {
    const xrl_gfx_ops* ops = xrl_gfx_ops_get(&g_services);
    if (!ops) XLOGE("gfx", "xrl_gfx_ops_get returned NULL");
    return ops;
}

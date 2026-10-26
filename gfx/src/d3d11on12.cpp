// d3d11on12.cpp — native D3D11On12 path + the DXVK->Dozen decision point.
//
// The ONLY d3d11 implementation we build is D3D11On12CreateDevice over the SAME
// ID3D12Device + ID3D12CommandQueue the swapchain uses (decisions.md: the export
// is present on the console). The fallback (DXVK -> Dozen) is NOT built; this file
// holds the detection: preflight() creates a real D3D11On12 device, runs a real
// clear + wrapped-resource + flush + fence round trip, and records WHY if any step
// fails. See gfx/notes-D.md "DXVK -> Dozen fallback".
#include "common.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace xrl {

HRESULT CreateD3D11On12Impl(ID3D12Device* dev12, ID3D12CommandQueue* q, ComPtr<ID3D11Device>& dev,
                            ComPtr<ID3D11DeviceContext>& ctx) {
    IUnknown* queues[1] = {q};
    const D3D_FEATURE_LEVEL lv[1] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen = {};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // wrapped swapchain buffers are BGRA
    if (DebugLayerRequested()) flags |= D3D11_CREATE_DEVICE_DEBUG;

    HRESULT hr = D3D11On12CreateDevice(dev12, flags, lv, 1, queues, 1, 0, &dev, &ctx, &chosen);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        Log(LOG_WARN, "D3D11On12CreateDevice(DEBUG) failed hr=0x%08lX; retrying without debug", (unsigned long)hr);
        dev.Reset();
        ctx.Reset();
        hr = D3D11On12CreateDevice(dev12, flags & ~D3D11_CREATE_DEVICE_DEBUG, lv, 1, queues, 1, 0, &dev, &ctx,
                                   &chosen);
    }
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11On12Device> on12;
    if (FAILED(dev.As(&on12))) {
        dev.Reset();
        ctx.Reset();
        return E_NOINTERFACE;  // device made, but no ID3D11On12Device: classified by caller
    }
    Log(LOG_INFO, "D3D11On12CreateDevice ok: ID3D11Device=%p ctx=%p chosen_fl=0x%X", dev.Get(), ctx.Get(),
        (unsigned)chosen);
    return S_OK;
}

HRESULT CreateNativeD3D11Impl(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    ComPtr<IDXGIAdapter1> ad;
    DXGI_ADAPTER_DESC1 desc{};
    HRESULT hr = PickAdapter(false, ad, &desc);
    if (FAILED(hr)) return hr;
    const D3D_FEATURE_LEVEL lv[1] = {D3D_FEATURE_LEVEL_11_0};   // the console caps at 11_0
    D3D_FEATURE_LEVEL chosen = {};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // the swapchain's buffers are BGRA
    if (DebugLayerRequested()) flags |= D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, lv, 1, D3D11_SDK_VERSION, &dev, &chosen, &ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        Log(LOG_WARN, "D3D11CreateDevice(DEBUG) failed hr=0x%08lX; retrying without debug", (unsigned long)hr);
        dev.Reset();
        ctx.Reset();
        hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, lv, 1,
                               D3D11_SDK_VERSION, &dev, &chosen, &ctx);
    }
    if (FAILED(hr)) {
        Log(LOG_ERROR, "native D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        return hr;
    }
    Log(LOG_INFO, "native D3D11CreateDevice ok on \"%s\": ID3D11Device=%p ctx=%p chosen_fl=0x%X",
        Narrow(desc.Description).c_str(), dev.Get(), ctx.Get(), (unsigned)chosen);
    return S_OK;
}

void ApplyD3D11Decision(const xrl_gfx_d3d11_decision& dec_in) {
    xrl_gfx_d3d11_decision dec = dec_in;
    if (dec.reason == XRL_GFX_REASON_NO_DEVICE) {
        // No d3d12 device at all says nothing about D3D11On12: leave the verdict
        // open (and the ops slot armed) so a later attempt can still decide.
        dec.backend = XRL_GFX_D3D11_UNDECIDED;
        std::lock_guard<std::mutex> l(G().mu);
        G().d11 = dec;
        G().d11.struct_size = sizeof G().d11;
        G().preflight_done = false;
        Log(LOG_ERROR, "D3D11 DECISION: UNDECIDED - no d3d12 device (hr=0x%08lX \"%s\")", (unsigned long)dec.hresult,
            dec.detail);
        return;
    }
    {
        std::lock_guard<std::mutex> l(G().mu);
        G().d11 = dec;
        G().d11.struct_size = sizeof G().d11;
        G().preflight_done = true;
    }
    if (dec.backend == XRL_GFX_D3D11_DXVK_DOZEN) {
        MutableOps()->d3d11on12_create = nullptr;  // ABI: host leaves NULL -> Wine falls back
        Log(LOG_ERROR,
            "D3D11 DECISION: native D3D11On12 NOT viable (reason=%u hr=0x%08lX \"%s\"). "
            "xrl_gfx_ops.d3d11on12_create is now NULL. The DXVK->Dozen fallback is NOT BUILT "
            "(no system Vulkan on console) - see gfx/notes-D.md.",
            dec.reason, (unsigned long)dec.hresult, dec.detail);
    } else if (dec.backend == XRL_GFX_D3D11_ON12) {
        Log(LOG_INFO, "D3D11 DECISION: native D3D11On12 viable (smoke test passed). Fallback not needed.");
    }
}

namespace {

void SetDetail(xrl_gfx_d3d11_decision& d, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d.detail, sizeof d.detail, fmt, ap);
    va_end(ap);
}

// Gfx-thread side: build a real D3D11On12 device and push real work through it.
void SmokeTest(ID3D12Device* dev12, std::atomic<int>& stage, xrl_gfx_d3d11_decision& out) {
    out = xrl_gfx_d3d11_decision{};
    out.struct_size = sizeof out;
    out.backend = XRL_GFX_D3D11_DXVK_DOZEN;  // pessimistic until proven

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q;
    HRESULT hr = dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        out.reason = XRL_GFX_REASON_NO_DEVICE;
        out.hresult = hr;
        SetDetail(out, "CreateCommandQueue failed");
        return;
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    stage = 1;
    hr = CreateD3D11On12Impl(dev12, q.Get(), dev, ctx);
    if (hr == E_NOINTERFACE) {
        out.reason = XRL_GFX_REASON_NO_ON12_INTERFACE;
        out.hresult = hr;
        SetDetail(out, "D3D11On12CreateDevice succeeded but device lacks ID3D11On12Device");
        return;
    }
    if (FAILED(hr)) {
        out.reason = XRL_GFX_REASON_CREATE_FAILED;
        out.hresult = hr;
        SetDetail(out, "D3D11On12CreateDevice failed");
        return;
    }
    stage = 2;
    ComPtr<ID3D11On12Device> on12;
    dev.As(&on12);

    // 1) a plain d3d11 render target: clear + flush must not fault.
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 64;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = dev->CreateTexture2D(&td, nullptr, &tex);
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
        if (FAILED(hr)) {
            out.reason = XRL_GFX_REASON_SMOKE_FAILED;
            out.hresult = hr;
            SetDetail(out, "d3d11 CreateTexture2D/RTV on the on12 device failed");
            return;
        }
        const float c[4] = {0.f, 0.5f, 1.f, 1.f};
        ctx->ClearRenderTargetView(rtv.Get(), c);
        ctx->Flush();
    }

    // 2) the real use: wrap a D3D12 render target (stand-in for a swapchain buffer).
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rd.Height = 64;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r12;
        hr = dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            nullptr, IID_PPV_ARGS(&r12));
        ComPtr<ID3D11Resource> w11;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(hr)) {
            D3D11_RESOURCE_FLAGS rf{};
            rf.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = on12->CreateWrappedResource(r12.Get(), &rf, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, IID_PPV_ARGS(&w11));
        }
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(w11.Get(), nullptr, &rtv);
        if (FAILED(hr)) {
            out.reason = XRL_GFX_REASON_SMOKE_FAILED;
            out.hresult = hr;
            SetDetail(out, "CreateWrappedResource / RTV over a d3d12 render target failed");
            return;
        }
        ID3D11Resource* arr[1] = {w11.Get()};
        const float c[4] = {1.f, 0.f, 0.f, 1.f};
        on12->AcquireWrappedResources(arr, 1);
        ctx->ClearRenderTargetView(rtv.Get(), c);
        on12->ReleaseWrappedResources(arr, 1);
        ctx->Flush();
    }

    // 3) the queue must drain with the device still alive.
    stage = 3;
    hr = WaitQueueIdle(dev12, q.Get(), 5000);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET) {
        out.reason = XRL_GFX_REASON_DEVICE_REMOVED;
        out.hresult = hr;
        SetDetail(out, "device removed after D3D11On12 work (removed reason 0x%08lX)", (unsigned long)hr);
        return;
    }
    if (FAILED(hr)) {
        out.reason = XRL_GFX_REASON_SMOKE_FAILED;
        out.hresult = hr;
        SetDetail(out, "queue did not drain after D3D11On12 work");
        return;
    }

    out.backend = XRL_GFX_D3D11_ON12;
    out.reason = XRL_GFX_REASON_NONE;
    out.hresult = S_OK;
    SetDetail(out, "create + d3d11 clear + wrapped d3d12 RT clear + fence drain all OK");
}

// Native D3D11 mode: device + a render-target clear + flush + event query, no D3D12 involved.
void NativeSmokeTest(xrl_gfx_d3d11_decision& out) {
    out = xrl_gfx_d3d11_decision{};
    out.struct_size = sizeof out;
    out.backend = XRL_GFX_D3D11_UNDECIDED;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = CreateNativeD3D11Impl(dev, ctx);
    if (FAILED(hr)) {
        out.reason = XRL_GFX_REASON_CREATE_FAILED;
        out.hresult = hr;
        SetDetail(out, "native D3D11CreateDevice failed");
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> q;
    if (SUCCEEDED(hr)) hr = dev->CreateQuery(&qd, &q);
    if (FAILED(hr)) {
        out.reason = XRL_GFX_REASON_SMOKE_FAILED;
        out.hresult = hr;
        SetDetail(out, "native d3d11 CreateTexture2D/RTV/Query failed");
        return;
    }
    const float c[4] = {0.f, 0.5f, 1.f, 1.f};
    ctx->ClearRenderTargetView(rtv.Get(), c);
    ctx->End(q.Get());
    ctx->Flush();
    BOOL done = FALSE;
    for (int i = 0; i < 5000 && !done; i++) {
        if (ctx->GetData(q.Get(), &done, sizeof done, 0) != S_OK) done = FALSE;
        if (!done) Sleep(1);
    }
    if (!done) {
        out.reason = XRL_GFX_REASON_SMOKE_FAILED;
        out.hresult = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        SetDetail(out, "native d3d11 queue did not drain");
        return;
    }
    out.backend = XRL_GFX_D3D11_NATIVE;
    out.reason = XRL_GFX_REASON_NONE;
    SetDetail(out, "native create + clear + flush + event query OK");
}

struct SmokeShared {
    std::atomic<int> stage{0};
    xrl_gfx_d3d11_decision dec{};
};

}  // namespace

xrl_status D3D11On12CreateOp(xrl_handle d3d12_device, xrl_handle d3d12_queue, xrl_handle* out_dev,
                             xrl_handle* out_ctx) {
    if (!out_dev || !out_ctx) return XRL_ERR_GENERIC;
    *out_dev = *out_ctx = XRL_INVALID_HANDLE;
    SetLastHr(S_OK);

    if (NativeD3D11()) {
        // Native D3D11 mode: the "D3D12 device" and "queue" the guest passes are the stand-ins of
        // stub12.cpp and are not used; the device is made straight on the adapter.
        WarnIfUiThread("d3d11on12_create (native)");
        struct NRes {
            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
        };
        auto nres = std::make_shared<NRes>();
        HRESULT nhr = RunOnGfxThread("d3d11 native create", [nres] { return CreateNativeD3D11Impl(nres->dev, nres->ctx); });
        if (FAILED(nhr)) return Fail(nhr, "d3d11on12_create (native)");
        ID3D11Device* nd = nres->dev.Get();
        ID3D11DeviceContext* nc = nres->ctx.Get();
        *out_dev = ToHandle(nres->dev);
        *out_ctx = ToHandle(nres->ctx);
        xrl_gfx_d3d11_decision dec{};
        dec.struct_size = sizeof dec;
        dec.backend = XRL_GFX_D3D11_NATIVE;
        dec.reason = XRL_GFX_REASON_NONE;
        {
            std::lock_guard<std::mutex> l(G().mu);
            G().d11 = dec;
            G().preflight_done = true;
        }
        Log(LOG_INFO, "d3d11on12_create -> NATIVE device %p ctx %p (no D3D11On12, no D3D12 device)", nd, nc);
        return XRL_OK;
    }

    xrl_gfx_d3d11_decision cur;
    xrl_gfx_get_d3d11_decision(&cur);
    if (cur.backend == XRL_GFX_D3D11_DXVK_DOZEN) {
        Log(LOG_ERROR, "d3d11on12_create: refused, D3D11On12 already judged non-viable (\"%s\")", cur.detail);
        return XRL_ERR_NOTAVAIL;
    }
    if (!d3d12_device || !d3d12_queue) {
        // The queue MUST be the one the swapchain will present on (D3D11On12
        // contract). The guest creates it and passes it in.
        Log(LOG_ERROR, "d3d11on12_create: device/queue handle is NULL (queue must be the guest's present queue)");
        return XRL_ERR_GENERIC;
    }
    WarnIfUiThread("d3d11on12_create");

    struct Res {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
    };
    auto res = std::make_shared<Res>();
    // Own references across the (possibly abandoned) job.
    ComPtr<ID3D12Device> dev12 = FromHandle<ID3D12Device>(d3d12_device);
    ComPtr<ID3D12CommandQueue> q = FromHandle<ID3D12CommandQueue>(d3d12_queue);
    HRESULT hr = RunOnGfxThread("d3d11on12_create", [res, dev12, q] {
        return CreateD3D11On12Impl(dev12.Get(), q.Get(), res->dev, res->ctx);
    });
    if (FAILED(hr)) {
        xrl_gfx_d3d11_decision dec{};
        dec.struct_size = sizeof dec;
        dec.backend = XRL_GFX_D3D11_DXVK_DOZEN;
        dec.hresult = hr;
        if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
            dec.reason = XRL_GFX_REASON_CREATE_TIMEOUT;
            SetDetail(dec, "D3D11On12CreateDevice did not return");
        } else if (hr == E_NOINTERFACE) {
            dec.reason = XRL_GFX_REASON_NO_ON12_INTERFACE;
            SetDetail(dec, "device lacks ID3D11On12Device");
        } else {
            dec.reason = XRL_GFX_REASON_CREATE_FAILED;
            SetDetail(dec, "D3D11On12CreateDevice failed");
        }
        ApplyD3D11Decision(dec);
        return Fail(hr, "d3d11on12_create");
    }
    ID3D11Device* d = res->dev.Get();
    ID3D11DeviceContext* c = res->ctx.Get();
    *out_dev = ToHandle(res->dev);
    *out_ctx = ToHandle(res->ctx);
    Log(LOG_INFO, "d3d11on12_create -> device %p ctx %p over d3d12 device %p / queue %p", d, c, dev12.Get(), q.Get());
    return XRL_OK;
}

}  // namespace xrl

// ---------------------------------------------------------------- C API
using namespace xrl;

extern "C" XRL_GFX_API xrl_status xrl_gfx_preflight(xrl_gfx_d3d11_decision* out) {
    {
        std::lock_guard<std::mutex> l(G().mu);
        if (G().preflight_done) {
            if (out) *out = G().d11;
            return G().d11.backend == XRL_GFX_D3D11_ON12 || G().d11.backend == XRL_GFX_D3D11_NATIVE ? XRL_OK : XRL_ERR_NOTAVAIL;
        }
    }
    if (NativeD3D11()) {
        auto sh = std::make_shared<SmokeShared>();
        HRESULT nhr = RunOnGfxThread("preflight: native d3d11 smoke test", [sh] {
            NativeSmokeTest(sh->dec);
            return S_OK;
        });
        xrl_gfx_d3d11_decision nd = sh->dec;
        if (FAILED(nhr)) {
            nd = xrl_gfx_d3d11_decision{};
            nd.struct_size = sizeof nd;
            nd.reason = XRL_GFX_REASON_CREATE_TIMEOUT;
            nd.hresult = nhr;
            SetDetail(nd, "native d3d11 smoke test did not finish");
        }
        {
            std::lock_guard<std::mutex> l(G().mu);
            G().d11 = nd;
            G().d11.struct_size = sizeof G().d11;
            G().preflight_done = nd.backend == XRL_GFX_D3D11_NATIVE;
        }
        Log(nd.backend == XRL_GFX_D3D11_NATIVE ? LOG_INFO : LOG_ERROR, "D3D11 DECISION: native D3D11 %s (\"%s\")",
            nd.backend == XRL_GFX_D3D11_NATIVE ? "viable" : "NOT viable", nd.detail);
        if (out) *out = nd;
        return nd.backend == XRL_GFX_D3D11_NATIVE ? XRL_OK : XRL_ERR_NOTAVAIL;
    }
    xrl_gfx_d3d11_decision dec{};
    dec.struct_size = sizeof dec;
    dec.backend = XRL_GFX_D3D11_DXVK_DOZEN;

    if (G().cfg.flags & XRL_GFX_FLAG_FORCE_NO_D3D11ON12) {
        dec.reason = XRL_GFX_REASON_FORCED_BY_FLAG;
        SetDetail(dec, "XRL_GFX_FLAG_FORCE_NO_D3D11ON12");
    } else {
        WarnIfUiThread("preflight");
        struct DevRes {
            ComPtr<ID3D12Device> dev;
        };
        auto dres = std::make_shared<DevRes>();
        HRESULT hr = RunOnGfxThread("preflight: d3d12 device", [dres] { return CreateDeviceImpl(dres->dev, nullptr); });
        if (FAILED(hr)) {
            dec.reason = XRL_GFX_REASON_NO_DEVICE;
            dec.hresult = hr;
            SetDetail(dec, "d3d12 device creation failed/timed out");
        } else {
            auto sh = std::make_shared<SmokeShared>();
            ComPtr<ID3D12Device> dev = dres->dev;
            hr = RunOnGfxThread("preflight: d3d11on12 smoke test", [sh, dev] {
                SmokeTest(dev.Get(), sh->stage, sh->dec);
                return S_OK;
            });
            if (FAILED(hr)) {  // timeout (or gfx thread gone)
                dec.reason = sh->stage.load() <= 1 ? XRL_GFX_REASON_CREATE_TIMEOUT : XRL_GFX_REASON_SMOKE_FAILED;
                dec.hresult = hr;
                SetDetail(dec, "smoke test hung at stage %d (1=create 2=d3d11/wrapped work 3=queue drain)",
                          sh->stage.load());
            } else {
                dec = sh->dec;
            }
        }
    }
    ApplyD3D11Decision(dec);
    if (out) *out = dec;
    return dec.backend == XRL_GFX_D3D11_ON12 ? XRL_OK : XRL_ERR_NOTAVAIL;
}

extern "C" XRL_GFX_API void xrl_gfx_get_d3d11_decision(xrl_gfx_d3d11_decision* out) {
    if (!out) return;
    std::lock_guard<std::mutex> l(G().mu);
    *out = G().d11;
    out->struct_size = sizeof *out;
}

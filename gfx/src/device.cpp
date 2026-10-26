// device.cpp — DXGI factory, adapter selection, d3d12_create_device.
#include "common.h"

#include <cstring>

namespace xrl {

std::string Narrow(const wchar_t* w) {
    char buf[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

// Process-wide factory. Created lazily and only ever on the gfx thread (DXGI
// calls are suspect on the UI thread too). Recreated if DXGI says it is stale.
HRESULT GetFactory(ComPtr<IDXGIFactory4>& out) {
    static std::mutex mu;
    static ComPtr<IDXGIFactory4> factory;
    std::lock_guard<std::mutex> l(mu);
    if (factory && !factory->IsCurrent()) {
        Log(LOG_INFO, "DXGI factory no longer current; recreating");
        factory.Reset();
    }
    if (!factory) {
        const bool dbg = DebugLayerRequested();
        HRESULT hr = CreateDXGIFactory2(dbg ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory));
        if (FAILED(hr) && dbg) {
            Log(LOG_WARN, "CreateDXGIFactory2(DEBUG) failed hr=0x%08lX, retrying without", (unsigned long)hr);
            hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        }
        if (FAILED(hr)) {
            Log(LOG_ERROR, "CreateDXGIFactory2 failed hr=0x%08lX", (unsigned long)hr);
            return hr;
        }
    }
    out = factory;
    return S_OK;
}

// Gfx-thread side. Picks the first hardware adapter that can make a FL11_0 D3D12
// device (the AppContainer view caps at 11_0 even though the GPU is higher —
// decisions.md) and creates the device there.
HRESULT CreateDeviceImpl(ComPtr<ID3D12Device>& out, DXGI_ADAPTER_DESC1* desc_out) {
    const bool dbg = DebugLayerRequested();
    const bool allow_warp = (G().cfg.flags & XRL_GFX_FLAG_ALLOW_WARP) != 0;

    if (dbg) {
        ComPtr<ID3D12Debug> dc;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dc)))) {
            dc->EnableDebugLayer();
            Log(LOG_INFO, "D3D12 debug layer enabled");
        } else {
            Log(LOG_WARN, "D3D12 debug layer requested but unavailable (expected on console)");
        }
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = GetFactory(factory);
    if (FAILED(hr)) return hr;

    // Enumerate, log everything we see, pick the first usable one.
    ComPtr<IDXGIAdapter1> pick, warp_pick;
    DXGI_ADAPTER_DESC1 pick_desc{}, warp_desc{};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        const bool soft = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        HRESULT probe = D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
        Log(LOG_INFO,
            "adapter[%u] \"%s\" vendor=0x%04X device=0x%04X flags=0x%X dedicated=%llu MiB shared=%llu MiB "
            "d3d12@11_0=%s(0x%08lX)",
            i, Narrow(desc.Description).c_str(), desc.VendorId, desc.DeviceId, desc.Flags,
            (unsigned long long)(desc.DedicatedVideoMemory >> 20),
            (unsigned long long)(desc.SharedSystemMemory >> 20), SUCCEEDED(probe) ? "yes" : "no",
            (unsigned long)probe);
        if (FAILED(probe)) continue;
        if (!soft && !pick) {
            pick = ad;
            pick_desc = desc;
        } else if (soft && !warp_pick && allow_warp) {
            warp_pick = ad;
            warp_desc = desc;
        }
    }
    if (!pick && warp_pick) {
        pick = warp_pick;
        pick_desc = warp_desc;
        Log(LOG_WARN, "no hardware adapter; using software adapter (XRL_GFX_FLAG_ALLOW_WARP)");
    }
    if (!pick) {
        Log(LOG_ERROR, "no D3D12-capable adapter at FL11_0");
        return DXGI_ERROR_UNSUPPORTED;
    }

    ComPtr<ID3D12Device> dev;
    hr = D3D12CreateDevice(pick.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        Log(LOG_ERROR, "D3D12CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        return hr;
    }
    out = dev;
    if (desc_out) *desc_out = pick_desc;
    return S_OK;
}

xrl_status DeviceCreateOp(xrl_handle* out_device) {
    if (!out_device) return XRL_ERR_GENERIC;
    *out_device = XRL_INVALID_HANDLE;
    SetLastHr(S_OK);
    WarnIfUiThread("d3d12_create_device");
    struct Res {
        ComPtr<ID3D12Device> dev;
    };
    auto res = std::make_shared<Res>();
    HRESULT hr = RunOnGfxThread("d3d12_create_device", [res] { return CreateDeviceImpl(res->dev, nullptr); });
    if (FAILED(hr)) return Fail(hr, "d3d12_create_device");
    ID3D12Device* raw = res->dev.Get();
    *out_device = ToHandle(res->dev);
    Log(LOG_INFO, "d3d12_create_device -> %p", raw);
    return XRL_OK;
}

xrl_status AdapterInfoOp(xrl_adapter_info* out) {
    if (!out) return XRL_ERR_GENERIC;
    SetLastHr(S_OK);
    // D3D12 devices are per-adapter singletons, so after the guest's first
    // d3d12_create_device this returns the same device cheaply.
    struct Res {
        xrl_adapter_info info{};
    };
    auto res = std::make_shared<Res>();
    HRESULT hr = RunOnGfxThread("get_adapter_info", [res]() -> HRESULT {
        ComPtr<ID3D12Device> dev;
        DXGI_ADAPTER_DESC1 d{};
        HRESULT h = CreateDeviceImpl(dev, &d);
        if (FAILED(h)) return h;
        xrl_adapter_info& i = res->info;
        i.vendor_id = d.VendorId;
        i.device_id = d.DeviceId;
        i.subsys_id = d.SubSysId;
        i.revision = d.Revision;
        i.dedicated_video_memory = d.DedicatedVideoMemory;
        i.dedicated_system_memory = d.DedicatedSystemMemory;
        i.shared_system_memory = d.SharedSystemMemory;
        i.adapter_luid = (uint64_t)d.AdapterLuid.LowPart | ((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32);
        static_assert(sizeof(WCHAR) == sizeof(uint16_t), "UTF-16");
        size_t n = 0;
        for (; n + 1 < _countof(i.description) && n < _countof(d.Description) && d.Description[n]; n++)
            i.description[n] = (uint16_t)d.Description[n];
        i.description[n] = 0;
        static const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                               D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
        fl.NumFeatureLevels = _countof(lv);
        fl.pFeatureLevelsRequested = lv;
        i.max_feature_level = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl))
                                  ? (uint32_t)fl.MaxSupportedFeatureLevel
                                  : (uint32_t)D3D_FEATURE_LEVEL_11_0;
        return S_OK;
    });
    if (FAILED(hr)) return Fail(hr, "get_adapter_info");
    *out = res->info;
    out->struct_size = sizeof *out;
    return XRL_OK;
}

HRESULT WaitQueueIdle(ID3D12Device* dev, ID3D12CommandQueue* q, DWORD timeout_ms) {
    ComPtr<ID3D12Fence> f;
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return hr;
    HANDLE ev = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return HRESULT_FROM_WIN32(GetLastError());
    hr = q->Signal(f.Get(), 1);
    if (SUCCEEDED(hr)) hr = f->SetEventOnCompletion(1, ev);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObjectEx(ev, timeout_ms, FALSE) != WAIT_OBJECT_0)
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    CloseHandle(ev);
    if (SUCCEEDED(hr)) hr = dev->GetDeviceRemovedReason();
    return hr;
}

}  // namespace xrl

// caps.cpp — adapter / feature-level / tier / memory-budget reporting.
//
// Purpose: a run on the console must confirm the 2026-10-21 probe numbers
// (docs/decisions.md) against THIS app's manifest. One log line per fact so a
// grep of LocalState/log.txt for "gfx caps" gives the whole picture.
#include "common.h"

#include <cstring>

namespace xrl {

namespace {

const char* FlName(uint32_t fl) {
    switch (fl) {
        case 0x9100: return "9_1";
        case 0x9200: return "9_2";
        case 0x9300: return "9_3";
        case 0xa000: return "10_0";
        case 0xa100: return "10_1";
        case 0xb000: return "11_0";
        case 0xb100: return "11_1";
        case 0xc000: return "12_0";
        case 0xc100: return "12_1";
        case 0xc200: return "12_2";
        default: return "?";
    }
}

template <class T>
bool Check(ID3D12Device* d, D3D12_FEATURE f, T& v) {
    v = T{};
    return SUCCEEDED(d->CheckFeatureSupport(f, &v, sizeof v));
}

void CopyUtf8(char* dst, size_t cap, const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, (int)cap, nullptr, nullptr);
    if (n <= 0) strcpy_s(dst, cap, "?");
}

}  // namespace

void FillAndLogCaps(ID3D12Device* dev, xrl_gfx_caps* out_opt) {
    xrl_gfx_caps c{};
    c.struct_size = sizeof c;

    // Adapter behind this device (by LUID). Must run on the gfx thread (factory).
    ComPtr<IDXGIFactory4> fac;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(GetFactory(fac)) && SUCCEEDED(fac->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        adapter->GetDesc1(&desc);
    else
        Log(LOG_WARN, "gfx caps: could not resolve the DXGI adapter for this device (no adapter/budget/output info)");

    // ---- adapter
    CopyUtf8(c.adapter_name, sizeof c.adapter_name, desc.Description);
    c.vendor_id = desc.VendorId;
    c.device_id = desc.DeviceId;
    c.revision = desc.Revision;
    c.dedicated_video_bytes = desc.DedicatedVideoMemory;
    c.dedicated_system_bytes = desc.DedicatedSystemMemory;
    c.shared_system_bytes = desc.SharedSystemMemory;
    Log(LOG_INFO, "gfx caps: adapter=\"%s\" vendor=0x%04X device=0x%04X rev=%u (probe: SraKmd_arden 0x1414/0xD000)",
        c.adapter_name, c.vendor_id, c.device_id, c.revision);
    Log(LOG_INFO, "gfx caps: memory dedicated_video=%llu MiB dedicated_system=%llu MiB shared_system=%llu MiB "
        "(probe: 128 / ? / 1280 MiB)",
        (unsigned long long)(c.dedicated_video_bytes >> 20), (unsigned long long)(c.dedicated_system_bytes >> 20),
        (unsigned long long)(c.shared_system_bytes >> 20));

    // ---- feature level
    {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                                   D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                                   D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
        fl.NumFeatureLevels = (UINT)(sizeof levels / sizeof levels[0]);
        fl.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
            c.max_feature_level = (uint32_t)fl.MaxSupportedFeatureLevel;
        Log(LOG_INFO, "gfx caps: max_feature_level=%s (0x%X) (probe: 11_0 — AppContainer view caps it)",
            FlName(c.max_feature_level), c.max_feature_level);
    }

    // ---- D3D12_OPTIONS (binding / heap tiers etc.)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o;
        if (Check(dev, D3D12_FEATURE_D3D12_OPTIONS, o)) {
            c.resource_binding_tier = (uint32_t)o.ResourceBindingTier;
            c.resource_heap_tier = (uint32_t)o.ResourceHeapTier;
            c.tiled_resources_tier = (uint32_t)o.TiledResourcesTier;
            c.rovs_supported = o.ROVsSupported;
            c.typed_uav_load_additional_formats = o.TypedUAVLoadAdditionalFormats;
            c.conservative_raster_tier = (uint32_t)o.ConservativeRasterizationTier;
        }
        Log(LOG_INFO, "gfx caps: resource_binding_tier=%u resource_heap_tier=%u tiled_resources_tier=%u "
            "(probe: binding 3, heap 2)",
            c.resource_binding_tier, c.resource_heap_tier, c.tiled_resources_tier);
        Log(LOG_INFO, "gfx caps: rovs=%u typed_uav_load_additional=%u conservative_raster_tier=%u",
            c.rovs_supported, c.typed_uav_load_additional_formats, c.conservative_raster_tier);
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o;
        if (Check(dev, D3D12_FEATURE_D3D12_OPTIONS1, o)) c.wave_ops = o.WaveOps;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o;
        if (Check(dev, D3D12_FEATURE_D3D12_OPTIONS3, o)) c.view_instancing_tier = (uint32_t)o.ViewInstancingTier;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o;
        if (Check(dev, D3D12_FEATURE_D3D12_OPTIONS5, o)) c.raytracing_tier = (uint32_t)o.RaytracingTier;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o;
        if (Check(dev, D3D12_FEATURE_D3D12_OPTIONS7, o)) c.mesh_shader_tier = (uint32_t)o.MeshShaderTier;
    }
    Log(LOG_INFO, "gfx caps: wave_ops=%u view_instancing_tier=%u raytracing_tier=%u mesh_shader_tier=%u",
        c.wave_ops, c.view_instancing_tier, c.raytracing_tier, c.mesh_shader_tier);

    // ---- shader model (probe upward; the call fails for models above the max)
    {
        static const D3D_SHADER_MODEL sms[] = {
            D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
            D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
            D3D_SHADER_MODEL_5_1};
        for (D3D_SHADER_MODEL sm : sms) {
            D3D12_FEATURE_DATA_SHADER_MODEL s{sm};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &s, sizeof s))) {
                c.highest_shader_model = (uint32_t)s.HighestShaderModel;
                break;
            }
        }
        Log(LOG_INFO, "gfx caps: highest_shader_model=%u.%u (0x%X)", c.highest_shader_model >> 4,
            c.highest_shader_model & 0xF, c.highest_shader_model);
    }
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE rs{D3D_ROOT_SIGNATURE_VERSION_1_1};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs)))
            c.root_signature_version = (uint32_t)rs.HighestVersion;
        Log(LOG_INFO, "gfx caps: root_signature_highest=0x%X", c.root_signature_version);
    }

    // ---- architecture (UMA matters a lot for Xbox: one shared pool)
    {
        D3D12_FEATURE_DATA_ARCHITECTURE1 a{};
        a.NodeIndex = 0;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &a, sizeof a))) {
            c.uma = a.UMA;
            c.cache_coherent_uma = a.CacheCoherentUMA;
        }
        D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof va)))
            c.gpu_va_bits = va.MaxGPUVirtualAddressBitsPerResource;
        Log(LOG_INFO, "gfx caps: uma=%u cache_coherent_uma=%u gpu_va_bits_per_resource=%u", c.uma,
            c.cache_coherent_uma, c.gpu_va_bits);
    }

    // ---- flip-model display format
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{DXGI_FORMAT_B8G8R8A8_UNORM};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
            c.bgra8_display_ok = (fs.Support1 & D3D12_FORMAT_SUPPORT1_DISPLAY) ? 1 : 0;
        Log(LOG_INFO, "gfx caps: B8G8R8A8_UNORM display_support=%u", c.bgra8_display_ok);
    }

    // ---- live video memory budget
    {
        ComPtr<IDXGIAdapter3> a3;
        if (adapter && SUCCEEDED(adapter.As(&a3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO li{}, ni{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
                c.budget_local = li.Budget;
                c.usage_local = li.CurrentUsage;
            }
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni))) {
                c.budget_nonlocal = ni.Budget;
                c.usage_nonlocal = ni.CurrentUsage;
            }
            Log(LOG_INFO, "gfx caps: budget LOCAL budget=%llu MiB usage=%llu MiB | NON_LOCAL budget=%llu MiB usage=%llu MiB",
                (unsigned long long)(c.budget_local >> 20), (unsigned long long)(c.usage_local >> 20),
                (unsigned long long)(c.budget_nonlocal >> 20), (unsigned long long)(c.usage_nonlocal >> 20));
        } else {
            Log(LOG_WARN, "gfx caps: IDXGIAdapter3 unavailable; no live memory budget");
        }
    }

    // ---- first output (display resolution), best effort
    {
        ComPtr<IDXGIOutput> o;
        if (adapter && SUCCEEDED(adapter->EnumOutputs(0, &o))) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od))) {
                c.output_width = (uint32_t)(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
                c.output_height = (uint32_t)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
            }
        }
        Log(LOG_INFO, "gfx caps: output0=%ux%u%s", c.output_width, c.output_height,
            c.output_width ? "" : " (no output reported)");
    }

    Log(LOG_INFO, "gfx caps: D3D11On12 verdict=%s (see xrl_gfx_preflight)",
        G().d11.backend == XRL_GFX_D3D11_ON12          ? "ON12 OK"
        : G().d11.backend == XRL_GFX_D3D11_NATIVE     ? "NATIVE D3D11 (XRL_D3D11=native)"
        : G().d11.backend == XRL_GFX_D3D11_DXVK_DOZEN ? "FAILED -> DXVK/Dozen"
                                                      : "not yet probed");

    if (out_opt) *out_opt = c;
}

}  // namespace xrl

extern "C" XRL_GFX_API xrl_status xrl_gfx_report_caps(xrl_handle device, xrl_gfx_caps* out) {
    using namespace xrl;
    SetLastHr(S_OK);
    WarnIfUiThread("report_caps");
    struct Res {
        xrl_gfx_caps caps{};
    };
    auto res = std::make_shared<Res>();
    // Handle is a raw ID3D12Device*; hold our own reference across the (possibly
    // abandoned) job so the pointer cannot dangle.
    ComPtr<ID3D12Device> held;
    if (device != XRL_INVALID_HANDLE) held = FromHandle<ID3D12Device>(device);
    HRESULT hr = RunOnGfxThread("report_caps", [res, held] {
        ComPtr<ID3D12Device> dev = held;
        if (!dev) {
            HRESULT hr = CreateDeviceImpl(dev, nullptr);  // temporary device
            if (FAILED(hr)) return hr;
        }
        FillAndLogCaps(dev.Get(), &res->caps);
        return S_OK;
    });
    if (FAILED(hr)) return Fail(hr, "report_caps");
    if (out) *out = res->caps;
    return XRL_OK;
}

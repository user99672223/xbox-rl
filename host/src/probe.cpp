// probe.cpp - the FIRST RUNNABLE MILESTONE: before any Wine/layer exists, log to
// LocalState/log.txt the facts the rest of the project stands on, measured on
// THIS app's own manifest/capabilities:
//
//   REQUIRED   rwx        VirtualAlloc(PAGE_EXECUTE_READWRITE), execute, RW->RX flip
//              alloc4g    a 4 GiB committed+touched allocation
//              d3d12      D3D12 device (adapter + feature level + caps)
//              d3d11on12  D3D11On12 device on that D3D12 device/queue
//   EXTRA      sysinfo    package, OS build, CPU, memory limits
//              vamap      user-VA free-region map (answers the "address-space
//                         hole" question the ABI header refers to)
//              swapchain  CreateSwapChainForCoreWindow from the gfx thread +
//                         120 vsynced presents of a cycling clear colour
//              audio      XAudio2 engine + 48 kHz stereo mastering voice + a
//                         200 ms quiet 440 Hz beep
//              input      Windows.Gaming.Input pad snapshot
//
// The D3D / audio / input steps go through the SAME xrl_*_ops tables the layer
// will receive, so a green probe means the production code path works.
// Steps are ordered cheapest/safest first and every line is flushed as written,
// so if a later step hangs (the known Xbox D3D hang) the log shows exactly where.
//
// Everything runs on probe/gfx worker threads - NEVER the CoreWindow UI thread.
#include "pch.h"
#include "xrl_host.h"

#include <intrin.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace xrl {

ProbeOptions ProbeOptions::all() { return ProbeOptions{}; }
ProbeOptions ProbeOptions::quick() {
    ProbeOptions o;
    o.alloc4g = false; o.swapchain = false; o.audio = false; o.input = false; o.ceiling = false;
    return o;
}

}  // namespace xrl

namespace {

constexpr const char* T = "probe";

// ---- step plumbing -------------------------------------------------------------

struct StepResult { std::string name; bool ok; bool required; };
std::vector<StepResult> g_results;

double now_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

bool seh_call(void (*fn)(void*), void* ctx, DWORD* code) {
    __try { fn(ctx); return true; }
    __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Runs `body` (returns true on success). Catches C++ and SEH exceptions so one
// broken step can never prevent the following ones from logging.
void run_step(const char* name, bool required, const std::function<bool()>& body) {
    XLOGI(T, "---- %s: BEGIN%s", name, required ? " (required)" : "");
    double t0 = now_ms();
    bool ok = false;
    try {
        struct Ctx { const std::function<bool()>* b; bool ok; } c{&body, false};
        DWORD code = 0;
        bool completed = seh_call([](void* p) { auto* x = (Ctx*)p; x->ok = (*x->b)(); }, &c, &code);
        if (!completed) XLOGE(T, "%s: SEH exception 0x%08lx", name, code);
        ok = completed && c.ok;
    } catch (const winrt::hresult_error& e) {
        XLOGE(T, "%s: hresult_error 0x%08x", name, (unsigned)e.code().value);
    } catch (const std::exception& e) {
        XLOGE(T, "%s: std::exception %s", name, e.what());
    } catch (...) {
        XLOGE(T, "%s: unknown C++ exception", name);
    }
    XLOGI(T, "---- %s: %s (%.1f ms)", name, ok ? "OK" : "FAIL", now_ms() - t0);
    g_results.push_back({name, ok, required});
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ---- sysinfo -------------------------------------------------------------------

bool step_sysinfo() {
    XLOGI(T, "host build: %s %s, ABI v%u, ptr=%zu-bit", __DATE__, __TIME__, XRL_ABI_VERSION, sizeof(void*) * 8);
    try {
        auto pkg = winrt::Windows::ApplicationModel::Package::Current();
        auto id = pkg.Id();
        auto v = id.Version();
        XLOGI(T, "package: %ls  (v%u.%u.%u.%u)", id.FullName().c_str(), v.Major, v.Minor, v.Build, v.Revision);
        XLOGI(T, "package family: %ls", id.FamilyName().c_str());
        XLOGI(T, "installed at: %ls", pkg.InstalledLocation().Path().c_str());
    } catch (...) { XLOGW(T, "no package identity (not running packaged?)"); }
    XLOGI(T, "LocalState: %s", xrl::local_state_utf8().c_str());
    try {
        auto vi = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
        XLOGI(T, "device family: %ls  os-version-string: %ls", vi.DeviceFamily().c_str(), vi.DeviceFamilyVersion().c_str());
    } catch (...) {}

    typedef LONG (WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion")) {
            OSVERSIONINFOW vi{sizeof vi};
            if (fn(&vi) == 0) XLOGI(T, "OS: %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        }
    }

    int r[4];
    __cpuid(r, 0);
    char vendor[13]; memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4); vendor[12] = 0;
    int maxleaf = r[0];
    char brand[49] = {};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004) {
        for (int i = 0; i < 3; ++i) { __cpuid(r, 0x80000002 + i); memcpy(brand + i * 16, r, 16); }
    }
    XLOGI(T, "CPU: %s | %s", vendor, brand);
    __cpuid(r, 1);
    int ecx1 = r[2];
    int ext7 = 0, ext7b = 0;
    if (maxleaf >= 7) { __cpuidex(r, 7, 0); ext7 = r[1]; ext7b = r[2]; }
    XLOGI(T, "CPU features: avx=%d avx2=%d bmi2=%d avx512f=%d rdrand=%d hypervisor-bit=%d",
          !!(ecx1 & (1 << 28)), !!(ext7 & (1 << 5)), !!(ext7 & (1 << 8)), !!(ext7 & (1 << 16)),
          !!(ecx1 & (1 << 30)), !!(ecx1 & (1u << 31)));
    (void)ext7b;
    __cpuid(r, 0x40000000);
    char hv[13]; memcpy(hv, &r[1], 4); memcpy(hv + 4, &r[2], 4); memcpy(hv + 8, &r[3], 4); hv[12] = 0;
    XLOGI(T, "hypervisor leaf 0x40000000: max=0x%x id='%s'", (unsigned)r[0], hv);

    SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
    XLOGI(T, "SYSTEM_INFO: %lu logical procs, page %lu, alloc-gran %lu, user VA 0x%llx..0x%llx",
          si.dwNumberOfProcessors, si.dwPageSize, si.dwAllocationGranularity,
          (unsigned long long)(uintptr_t)si.lpMinimumApplicationAddress,
          (unsigned long long)(uintptr_t)si.lpMaximumApplicationAddress);
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        XLOGI(T, "memory: phys total %.0f MiB avail %.0f MiB | commit total %.0f MiB avail %.0f MiB | load %lu%%",
              ms.ullTotalPhys / 1048576.0, ms.ullAvailPhys / 1048576.0,
              ms.ullTotalPageFile / 1048576.0, ms.ullAvailPageFile / 1048576.0, ms.dwMemoryLoad);
    }
    try {
        using MM = winrt::Windows::System::MemoryManager;
        XLOGI(T, "MemoryManager: AppMemoryUsage %.1f MiB, AppMemoryUsageLimit %.1f MiB, level %d",
              MM::AppMemoryUsage() / 1048576.0, MM::AppMemoryUsageLimit() / 1048576.0, (int)MM::AppMemoryUsageLevel());
    } catch (...) { XLOGW(T, "MemoryManager unavailable"); }
    return true;
}

// ---- RWX -----------------------------------------------------------------------

bool step_rwx() {
    const SIZE_T sz = 64 * 1024;
    void* p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!p) { XLOGE(T, "VirtualAlloc(RWX, 64 KiB) FAILED, GetLastError=%lu (codeGeneration capability missing?)", GetLastError()); return false; }
    XLOGI(T, "VirtualAlloc(PAGE_EXECUTE_READWRITE, 64 KiB) = %p", p);

    // mov eax, imm32 ; ret
    auto emit = [&](uint32_t v) {
        unsigned char* c = (unsigned char*)p;
        c[0] = 0xB8; memcpy(c + 1, &v, 4); c[5] = 0xC3;
        FlushInstructionCache(GetCurrentProcess(), p, 16);
    };
    typedef int (*fn_t)();
    bool ok = true;

    emit(42);
    int r1 = ((fn_t)p)();
    XLOGI(T, "RWX execute in place: returned %d (expect 42) %s", r1, r1 == 42 ? "OK" : "BAD");
    ok &= (r1 == 42);

    DWORD old = 0;
    if (!VirtualProtect(p, sz, PAGE_EXECUTE_READ, &old)) { XLOGE(T, "VirtualProtect(RX) failed %lu", GetLastError()); ok = false; }
    else {
        int r2 = ((fn_t)p)();
        XLOGI(T, "RWX -> RX flip, execute: returned %d (expect 42) %s", r2, r2 == 42 ? "OK" : "BAD");
        ok &= (r2 == 42);
        if (!VirtualProtect(p, sz, PAGE_READWRITE, &old)) { XLOGE(T, "VirtualProtect(RW) failed %lu", GetLastError()); ok = false; }
        else {
            emit(1337);
            if (!VirtualProtect(p, sz, PAGE_EXECUTE_READ, &old)) { XLOGE(T, "VirtualProtect(RX #2) failed %lu", GetLastError()); ok = false; }
            else {
                int r3 = ((fn_t)p)();
                XLOGI(T, "RX -> RW (patch) -> RX, execute: returned %d (expect 1337) %s", r3, r3 == 1337 ? "OK" : "BAD");
                ok &= (r3 == 1337);
            }
        }
    }

    // JIT-from-worker-thread (the Blink loader JITs on whatever thread runs guest code).
    struct W { void* p; int r; } w{p, 0};
    VirtualProtect(p, sz, PAGE_EXECUTE_READWRITE, &old);
    emit(7);
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID a) -> DWORD { auto* x = (W*)a; x->r = ((fn_t)x->p)(); return 0; }, &w, 0, nullptr);
    if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
    XLOGI(T, "execute from worker thread: returned %d (expect 7) %s", w.r, w.r == 7 ? "OK" : "BAD");
    ok &= (w.r == 7);

    VirtualFree(p, 0, MEM_RELEASE);
    return ok;
}

// ---- 4 GiB ---------------------------------------------------------------------

bool step_alloc4g() {
    const SIZE_T sz = 4ull << 30;
    double t0 = now_ms();
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) { XLOGE(T, "VirtualAlloc(4 GiB, COMMIT|RESERVE, RW) FAILED, GetLastError=%lu", GetLastError()); return false; }
    XLOGI(T, "VirtualAlloc(4 GiB, commit) = %p in %.1f ms", p, now_ms() - t0);

    // Touch every page so the commit is real (not just charged).
    t0 = now_ms();
    for (SIZE_T off = 0; off < sz; off += 4096) {
        p[off] = (uint8_t)(off >> 12);
        if ((off & ((512ull << 20) - 1)) == 0 && off) XLOGT(T, "touched %zu MiB", (size_t)(off >> 20));
    }
    bool ok = true;
    for (SIZE_T off = 0; off < sz; off += (64ull << 20)) if (p[off] != (uint8_t)(off >> 12)) { ok = false; break; }
    XLOGI(T, "touched all %zu pages in %.0f ms; readback %s", (size_t)(sz >> 12), now_ms() - t0, ok ? "OK" : "MISMATCH");
    try {
        using MM = winrt::Windows::System::MemoryManager;
        XLOGI(T, "MemoryManager while holding 4 GiB: usage %.0f MiB / limit %.0f MiB",
              MM::AppMemoryUsage() / 1048576.0, MM::AppMemoryUsageLimit() / 1048576.0);
    } catch (...) {}
    VirtualFree(p, 0, MEM_RELEASE);
    return ok;
}

// ---- allocation ceiling (runs LAST: if the console's memory manager reacts badly to
// ---- an over-limit commit, everything above is already in the log) ------------------

bool step_ceiling() {
    // Where is the ceiling? Commit-only (never touched), freed immediately. Informs
    // xrl_boot_context::guest_mem_budget. A denial here is information, not a FAIL.
    const double gib[] = {4.5, 5.0, 6.0, 8.0};
    for (double g : gib) {
        SIZE_T s = (SIZE_T)(g * (1ull << 30));
        XLOGI(T, "ceiling probe: trying commit of %.1f GiB ...", g);
        void* q = VirtualAlloc(nullptr, s, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (q) { XLOGI(T, "ceiling probe: commit %.1f GiB -> ok", g); VirtualFree(q, 0, MEM_RELEASE); }
        else   { XLOGI(T, "ceiling probe: commit %.1f GiB -> denied (GetLastError=%lu)", g, GetLastError()); }
    }
    // Reserve-only is the layer's real use (arena reserve, commit on demand).
    const double rgib[] = {16.0, 64.0, 256.0, 1024.0};
    for (double g : rgib) {
        SIZE_T s = (SIZE_T)(g * (1ull << 30));
        void* q = VirtualAlloc(nullptr, s, MEM_RESERVE, PAGE_NOACCESS);
        XLOGI(T, "reserve-only probe: %.0f GiB -> %s", g, q ? "ok" : "denied");
        if (q) VirtualFree(q, 0, MEM_RELEASE);
    }
    return true;
}

// ---- VA map --------------------------------------------------------------------

bool step_vamap() {
    SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
    uint64_t a = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress;
    const uint64_t end = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;
    struct FR { uint64_t base, size; };
    std::vector<FR> top;   // largest free regions
    uint64_t free_total = 0, reserved = 0, committed = 0, regions = 0, free_regions = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof mbi) == sizeof mbi) {
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = mbi.RegionSize;
        ++regions;
        if (mbi.State == MEM_FREE) {
            ++free_regions; free_total += sz;
            top.push_back({base, sz});
            if (top.size() > 64) {
                std::partial_sort(top.begin(), top.begin() + 16, top.end(), [](const FR& x, const FR& y) { return x.size > y.size; });
                top.resize(16);
            }
        } else if (mbi.State == MEM_RESERVE) reserved += sz;
        else committed += sz;
        if (base + sz <= a) break;   // no forward progress
        a = base + sz;
    }
    std::sort(top.begin(), top.end(), [](const FR& x, const FR& y) { return x.size > y.size; });
    XLOGI(T, "VA map: %llu regions (%llu free) | free %.1f GiB, reserved %.1f GiB, committed %.1f GiB (of user VA %.1f TiB)",
          (unsigned long long)regions, (unsigned long long)free_regions, free_total / 1073741824.0,
          reserved / 1073741824.0, committed / 1073741824.0, (end + 1) / 1099511627776.0);
    for (size_t i = 0; i < top.size() && i < 12; ++i)
        XLOGI(T, "  free #%zu: 0x%016llx .. 0x%016llx  (%.2f GiB)", i + 1, (unsigned long long)top[i].base,
              (unsigned long long)(top[i].base + top[i].size), top[i].size / 1073741824.0);
    return regions > 0;
}

// ---- graphics ------------------------------------------------------------------

struct GfxState {
    xrl_handle dev12 = 0, queue = 0, dev11 = 0, ctx11 = 0;
};
GfxState g_gfx;

const char* fl_name(D3D_FEATURE_LEVEL l) {
    switch (l) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2"; case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0"; case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0"; case D3D_FEATURE_LEVEL_10_1: return "10_1";
        default: return "?";
    }
}

void log_adapters_on_gfx_thread() {
    ComPtr<IDXGIFactory4> f;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    if (FAILED(hr)) { XLOGE(T, "CreateDXGIFactory2 failed 0x%08x", (unsigned)hr); return; }
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        XLOGI(T, "DXGI adapter %u: '%s' vendor=0x%04x device=0x%04x rev=%u flags=0x%x LUID=%08lx:%08lx | dedicated VRAM %.0f MiB, dedicated sys %.0f MiB, shared %.0f MiB",
              i, wide_to_utf8(d.Description).c_str(), d.VendorId, d.DeviceId, d.Revision, d.Flags,
              (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
              d.DedicatedVideoMemory / 1048576.0, d.DedicatedSystemMemory / 1048576.0, d.SharedSystemMemory / 1048576.0);
    }
}

bool step_d3d12() {
    // Adapter enumeration happens on the gfx thread (any DXGI/D3D call may hang on the UI thread).
    if (!xrl::gfx_thread_invoke("adapter enumeration", [] { log_adapters_on_gfx_thread(); }, 60000)) {
        XLOGE(T, "adapter enumeration timed out");
        return false;
    }
    const xrl_gfx_ops* ops = xrl_host_gfx_ops();
    if (!ops || !ops->d3d12_create_device) return false;

    XLOGI(T, "calling xrl_gfx_ops::d3d12_create_device (marshalled to gfx thread)...");
    double t0 = now_ms();
    xrl_status st = ops->d3d12_create_device(&g_gfx.dev12);
    if (st != XRL_OK || !g_gfx.dev12) { XLOGE(T, "d3d12_create_device -> status %d", st); return false; }
    XLOGI(T, "D3D12 device created: %p in %.1f ms", (void*)(uintptr_t)g_gfx.dev12, now_ms() - t0);

    auto* dev = (ID3D12Device*)(uintptr_t)g_gfx.dev12;
    LUID luid = dev->GetAdapterLuid();
    XLOGI(T, "device adapter LUID %08lx:%08lx, node count %u", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart, dev->GetNodeCount());

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    fl.NumFeatureLevels = _countof(levels); fl.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
        XLOGI(T, "D3D12 max supported feature level: %s  (expected 11_0 for the AppContainer view)", fl_name(fl.MaxSupportedFeatureLevel));
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
        XLOGI(T, "options: resource-binding tier %d, tiled-resources tier %d, heap tier %d, conservative-raster tier %d, ROVs %d, typed-UAV-extra %d",
              (int)o.ResourceBindingTier, (int)o.TiledResourcesTier, (int)o.ResourceHeapTier,
              (int)o.ConservativeRasterizationTier, o.ROVsSupported, o.TypedUAVLoadAdditionalFormats);
    D3D12_FEATURE_DATA_SHADER_MODEL sm{};
    for (D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                               D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1}) {
        sm.HighestShaderModel = m;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) {
            XLOGI(T, "highest shader model: %d.%d", (sm.HighestShaderModel >> 4) & 0xF, sm.HighestShaderModel & 0xF);
            break;
        }
    }
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch)))
        XLOGI(T, "architecture: UMA=%d cache-coherent-UMA=%d tile-based=%d", arch.UMA, arch.CacheCoherentUMA, arch.TileBasedRenderer);
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof va)))
        XLOGI(T, "GPU VA bits per resource %u, per process %u", va.MaxGPUVirtualAddressBitsPerResource, va.MaxGPUVirtualAddressBitsPerProcess);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1)))
        XLOGI(T, "options1: wave-ops %d (lane count %u..%u)", o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)))
        XLOGI(T, "options5: raytracing tier %d", (int)o5.RaytracingTier);

    // The guest creates its own queues through the device vtable; the probe makes one
    // so D3D11On12 and the swapchain test have something real to bind to.
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q;
    HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) { XLOGE(T, "CreateCommandQueue(DIRECT) failed 0x%08x", (unsigned)hr); return false; }
    g_gfx.queue = (xrl_handle)(uintptr_t)q.Detach();
    XLOGI(T, "direct command queue created: %p", (void*)(uintptr_t)g_gfx.queue);
    return true;
}

bool step_d3d11on12() {
    if (!g_gfx.dev12 || !g_gfx.queue) { XLOGE(T, "no D3D12 device/queue (d3d12 step failed)"); return false; }
    const xrl_gfx_ops* ops = xrl_host_gfx_ops();
    if (!ops || !ops->d3d11on12_create) {
        XLOGE(T, "xrl_gfx_ops::d3d11on12_create is NULL -> Wine side would fall back to DXVK->Dozen");
        return false;
    }
    double t0 = now_ms();
    xrl_status st = ops->d3d11on12_create(g_gfx.dev12, g_gfx.queue, &g_gfx.dev11, &g_gfx.ctx11);
    if (st != XRL_OK) { XLOGE(T, "d3d11on12_create -> status %d (D3D11On12 not viable: DXVK->Dozen branch needed)", st); return false; }
    auto* d11 = (ID3D11Device*)(uintptr_t)g_gfx.dev11;
    XLOGI(T, "D3D11On12 device created: device=%p context=%p in %.1f ms, feature level %s",
          (void*)(uintptr_t)g_gfx.dev11, (void*)(uintptr_t)g_gfx.ctx11, now_ms() - t0, fl_name(d11->GetFeatureLevel()));
    ComPtr<ID3D11On12Device> on12;
    XLOGI(T, "QI ID3D11On12Device: %s", SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&on12))) ? "ok" : "FAILED");
    D3D11_FEATURE_DATA_THREADING th{};
    if (SUCCEEDED(d11->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof th)))
        XLOGI(T, "D3D11 threading: concurrent-creates=%d command-lists=%d", th.DriverConcurrentCreates, th.DriverCommandLists);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = d11->CreateTexture2D(&td, nullptr, &tex);
    XLOGI(T, "D3D11On12 CreateTexture2D(256x256 RGBA8, SRV|RTV): 0x%08x %s", (unsigned)hr, SUCCEEDED(hr) ? "ok" : "FAILED");
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = d11->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
        XLOGI(T, "D3D11On12 CreateRenderTargetView: 0x%08x %s", (unsigned)hr, SUCCEEDED(hr) ? "ok" : "FAILED");
        if (SUCCEEDED(hr)) {
            auto* ctx = (ID3D11DeviceContext*)(uintptr_t)g_gfx.ctx11;
            const float c[4] = {0.f, 0.5f, 1.f, 1.f};
            ctx->ClearRenderTargetView(rtv.Get(), c);
            ctx->Flush();
            XLOGI(T, "D3D11On12 ClearRenderTargetView + Flush: ok");
        }
    }
    return SUCCEEDED(hr);
}

bool step_swapchain() {
    if (!g_gfx.dev12 || !g_gfx.queue) { XLOGE(T, "no D3D12 device/queue"); return false; }
    auto wi = xrl::window_info_get();
    if (!wi.valid || !wi.core_window) { XLOGW(T, "no CoreWindow published yet - skipping swapchain test"); return false; }
    XLOGI(T, "CoreWindow %p, size %ux%u px", wi.core_window, wi.width, wi.height);
    const xrl_gfx_ops* ops = xrl_host_gfx_ops();
    auto* dev = (ID3D12Device*)(uintptr_t)g_gfx.dev12;
    auto* queue = (ID3D12CommandQueue*)(uintptr_t)g_gfx.queue;

    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    xrl_handle sch = 0;
    double t0 = now_ms();
    xrl_status st = ops->create_swapchain_for_corewindow(g_gfx.dev12, g_gfx.queue, 0, 0, 2, (uint32_t)fmt, &sch);
    if (st != XRL_OK) { XLOGE(T, "create_swapchain_for_corewindow -> status %d", st); return false; }
    XLOGI(T, "swapchain created: %p in %.1f ms", (void*)(uintptr_t)sch, now_ms() - t0);

    auto* sc1 = (IDXGISwapChain1*)(uintptr_t)sch;
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc3)))) { XLOGE(T, "swapchain lacks IDXGISwapChain3"); ops->release(sch); return false; }

    xrl_handle bh[2] = {};
    ComPtr<ID3D12Resource> bufs[2];
    for (uint32_t i = 0; i < 2; ++i) {
        if (ops->swapchain_get_buffer(sch, i, &bh[i]) != XRL_OK) { XLOGE(T, "swapchain_get_buffer(%u) failed", i); ops->release(sch); return false; }
        bufs[i].Attach((ID3D12Resource*)(uintptr_t)bh[i]);   // takes the reference the op returned
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    bool ok = SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))
           && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
           && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))
           && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (!ok) { XLOGE(T, "failed to create D3D12 objects for the present loop"); ops->release(sch); return false; }
    list->Close();

    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {h0, {h0.ptr + inc}};
    for (int i = 0; i < 2; ++i) dev->CreateRenderTargetView(bufs[i].Get(), nullptr, rtv[i]);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    const int frames = 120;
    int done = 0;
    double tl = now_ms();
    for (int f = 0; f < frames; ++f) {
        UINT bi = sc3->GetCurrentBackBufferIndex();
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[bi].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        float t = f / (float)frames;
        const float col[4] = {0.5f + 0.5f * sinf(t * 6.2832f), 0.5f + 0.5f * sinf(t * 6.2832f + 2.1f), 0.5f + 0.5f * sinf(t * 6.2832f + 4.2f), 1.f};
        list->ClearRenderTargetView(rtv[bi], col, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        st = ops->swapchain_present(sch, 1, 0);
        if (st != XRL_OK) { XLOGE(T, "present #%d failed status %d", f, st); break; }
        queue->Signal(fence.Get(), ++fv);
        if (fence->GetCompletedValue() < fv) {
            fence->SetEventOnCompletion(fv, ev);
            if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { XLOGE(T, "fence wait timed out at frame %d (GPU/present stalled)", f); break; }
        }
        ++done;
        if (f == 0) XLOGI(T, "first present returned after %.1f ms", now_ms() - tl);
    }
    double el = now_ms() - tl;
    XLOGI(T, "presented %d/%d frames in %.0f ms (%.1f fps; ~60 expected with vsync)", done, frames, el, done * 1000.0 / (el > 0 ? el : 1));
    CloseHandle(ev);
    // Drain before tearing down.
    queue->Signal(fence.Get(), ++fv);
    if (fence->GetCompletedValue() < fv) { HANDLE e2 = CreateEventW(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(fv, e2); WaitForSingleObject(e2, 5000); CloseHandle(e2); }
    bufs[0].Reset(); bufs[1].Reset();
    sc3.Reset();
    ops->release(sch);
    return done == frames;
}

void gfx_cleanup() {
    const xrl_gfx_ops* ops = xrl_host_gfx_ops();
    if (!ops) return;
    if (g_gfx.ctx11)  ops->release(g_gfx.ctx11);
    if (g_gfx.dev11)  ops->release(g_gfx.dev11);
    if (g_gfx.queue)  ops->release(g_gfx.queue);
    if (g_gfx.dev12)  ops->release(g_gfx.dev12);
    g_gfx = {};
}

// ---- audio ---------------------------------------------------------------------

bool step_audio() {
    const xrl_audio_ops* ops = xrl_host_audio_ops();
    xrl_handle xa = 0, mv = 0, sv = 0;
    bool ok = false;
    do {
        if (ops->xaudio2_create(&xa) != XRL_OK) { XLOGE(T, "xaudio2_create failed"); break; }
        XLOGI(T, "XAudio2 engine created: %p", (void*)(uintptr_t)xa);
        if (ops->create_mastering_voice(xa, 2, 48000, &mv) != XRL_OK) { XLOGE(T, "create_mastering_voice(2ch, 48000) failed"); break; }
        XLOGI(T, "mastering voice (2 ch, 48 kHz) created");
        if (ops->create_source_voice(xa, 2, 48000, 16, &sv) != XRL_OK) { XLOGE(T, "create_source_voice(2ch, 48000, 16-bit) failed"); break; }
        const int n = 48000 / 5;   // 200 ms
        std::vector<int16_t> pcm((size_t)n * 2);
        for (int i = 0; i < n; ++i) {
            float env = std::min(1.f, std::min(i / 480.f, (n - i) / 480.f));
            int16_t s = (int16_t)(2000.f * env * sinf(6.2831853f * 440.f * i / 48000.f));
            pcm[2 * i] = pcm[2 * i + 1] = s;
        }
        if (ops->voice_submit_buffer(sv, pcm.data(), (uint32_t)(pcm.size() * 2), 0x40 /*END_OF_STREAM*/) != XRL_OK) { XLOGE(T, "voice_submit_buffer failed"); break; }
        if (ops->voice_start(sv) != XRL_OK) { XLOGE(T, "voice_start failed"); break; }
        XLOGI(T, "playing 200 ms 440 Hz beep (quiet) ...");
        Sleep(350);
        ops->voice_stop(sv);
        ok = true;
    } while (false);
    if (sv) ops->release(sv);
    if (mv) ops->release(mv);
    if (xa) ops->release(xa);
    return ok;
}

// ---- input ---------------------------------------------------------------------

bool step_input() {
    const xrl_input_ops* ops = xrl_host_input_ops();
    try {
        XLOGI(T, "Windows.Gaming.Input: %u gamepad(s) enumerated", (unsigned)winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size());
    } catch (const winrt::hresult_error& e) {
        XLOGE(T, "Gamepad::Gamepads() failed 0x%08x", (unsigned)e.code().value);
        return false;
    }
    for (uint32_t s = 0; s < 4; ++s) {
        xrl_pad_state ps{};
        xrl_status st = ops->get_pad_state(s, &ps);
        if (st == XRL_OK)
            XLOGI(T, "pad slot %u: buttons=0x%04x LT=%u RT=%u LS=(%d,%d) RS=(%d,%d)", s, ps.buttons, ps.left_trigger, ps.right_trigger,
                  ps.thumb_lx, ps.thumb_ly, ps.thumb_rx, ps.thumb_ry);
        else
            XLOGI(T, "pad slot %u: empty (status %d)", s, st);
    }
    return true;   // an empty console with no pad paired is not a failure
}

}  // namespace

namespace xrl {

bool probe_run(const ProbeOptions& opt) {
    g_results.clear();
    XLOGI(T, "================ PROBE BEGIN ================");
    double t0 = now_ms();

    if (opt.sysinfo)   run_step("sysinfo",   false, step_sysinfo);
    if (opt.rwx)       run_step("rwx",       true,  step_rwx);
    if (opt.alloc4g)   run_step("alloc4g",   true,  step_alloc4g);
    if (opt.vamap)     run_step("vamap",     false, step_vamap);
    if (opt.d3d12)     run_step("d3d12",     true,  step_d3d12);
    if (opt.d3d11on12) run_step("d3d11on12", true,  step_d3d11on12);
    if (opt.swapchain && config().probe_swapchain) run_step("swapchain", false, step_swapchain);
    if (opt.audio && config().probe_audio)         run_step("audio",     false, step_audio);
    if (opt.input)     run_step("input",     false, step_input);
    gfx_cleanup();

    bool required_ok = true;
    std::string line;
    for (auto& r : g_results) {
        line += r.name + "=" + (r.ok ? "OK" : "FAIL") + " ";
        if (r.required && !r.ok) required_ok = false;
    }
    XLOGI(T, "PROBE_RESULT %s| required=%s", line.c_str(), required_ok ? "PASS" : "FAIL");

    // Last, and after the verdict is already in the log (see step_ceiling).
    if (opt.ceiling)   run_step("ceiling",   false, step_ceiling);
    XLOGI(T, "================ PROBE END (%.0f ms) ================", now_ms() - t0);
    return required_ok;
}

// ===================================================================
// procprobe - evidence on whether the Xbox lets a packaged app run extra OS
// processes, and if not, HOW/when it stops them. A child is this exe re-run
// with XRL_PROCCHILD=<id>:<behavior>; it only heartbeats GetTickCount64 to
// <XRL_PP_DIR>\pp-child-<id>.log (pure Win32, no WinRT/log.txt). The parent
// (the foreground, window-owning app) spawns several and classifies each:
// ALIVE (heartbeats fresh), FROZEN (process alive but heartbeats stopped =
// PLM-suspended), or TERMINATED (+exit code). GetTickCount64 (system uptime)
// shares a base across processes, so staleness is comparable.
static unsigned long long pp_last_hb(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz; if (!GetFileSizeEx(f, &sz)) { CloseHandle(f); return 0; }
    LONG back = sz.QuadPart > 4096 ? 4096 : (LONG)sz.QuadPart;
    LARGE_INTEGER mv; mv.QuadPart = -(LONGLONG)back; SetFilePointerEx(f, mv, nullptr, FILE_END);
    char buf[4097]; DWORD rd = 0; ReadFile(f, buf, (DWORD)back, &rd, nullptr);
    buf[rd < 4096 ? rd : 4096] = 0; CloseHandle(f);
    unsigned long long last = 0;
    for (char* p = buf; (p = strstr(p, "t=")) != nullptr; p += 2) last = _strtoui64(p + 2, nullptr, 10);
    return last;
}

void procprobe_child_main(const char* spec) {
    int id = atoi(spec);
    const char* colon = strchr(spec, ':');
    std::string beh = colon ? std::string(colon + 1) : "idle";
    if (beh == "extexec") { try { winrt::init_apartment(winrt::apartment_type::multi_threaded); request_extended_execution(); } catch (...) {} }
    char dir[600] = "."; GetEnvironmentVariableA("XRL_PP_DIR", dir, sizeof dir);
    char path[720]; snprintf(path, sizeof path, "%s\\pp-child-%d.log", dir, id);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto wr = [&](const char* s) { if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, (DWORD)strlen(s), &w, nullptr); FlushFileBuffers(f); } };
    char line[256];
    snprintf(line, sizeof line, "START pid=%lu beh=%s t=%llu\n",
             GetCurrentProcessId(), beh.c_str(), (unsigned long long)GetTickCount64());
    wr(line);
    for (int n = 0; n < 1500; n++) {   // up to ~150 s of heartbeats
        unsigned long long t = GetTickCount64();
        snprintf(line, sizeof line, "HB n=%d t=%llu\n", n, (unsigned long long)t);
        wr(line);
        if (beh == "busy") { unsigned long long end = t + 100; volatile unsigned x = 0; while (GetTickCount64() < end) x++; }
        else Sleep(100);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

void procprobe_parent(const char* spec) {
    XLOGW("procprobe", "START main pid=%lu spec=%s", GetCurrentProcessId(), (spec && *spec) ? spec : "(default)");
    wchar_t exe[600]; if (!GetModuleFileNameW(nullptr, exe, 600)) { XLOGE("procprobe", "GetModuleFileName failed"); return; }
    std::vector<std::string> behs;
    { std::string s = (spec && *spec) ? std::string(spec) : "idle,busy,extexec,idle";
      size_t p = 0; for (;;) { size_t c = s.find(',', p);
          behs.push_back(s.substr(p, c == std::string::npos ? std::string::npos : c - p));
          if (c == std::string::npos) break; p = c + 1; } }
    int N = (int)behs.size();
    bool use_job = GetEnvironmentVariableA("XRL_PROCPROBE_JOB", nullptr, 0) != 0;
    HANDLE job = nullptr;
    if (use_job) { job = CreateJobObjectW(nullptr, nullptr); XLOGW("procprobe", "job object = %p (err %lu)", job, GetLastError()); }
    struct Kid { HANDLE h = nullptr; DWORD pid = 0; std::string beh; bool dead = true; char hb[720] = {0}; };
    std::vector<Kid> kids(N);
    const std::string& ls = local_state_utf8();
    unsigned long long t0 = GetTickCount64();
    for (int i = 0; i < N; i++) {
        kids[i].beh = behs[i];
        char v[64]; snprintf(v, sizeof v, "%d:%s", i, behs[i].c_str());
        SetEnvironmentVariableA("XRL_PROCCHILD", v);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        if (!ok) { XLOGE("procprobe", "child %d (%s): CreateProcess FAILED err=%lu", i, behs[i].c_str(), GetLastError()); continue; }
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) XLOGW("procprobe", "child %d: AssignProcessToJobObject err=%lu", i, GetLastError());
        ResumeThread(pi.hThread); CloseHandle(pi.hThread);
        kids[i].h = pi.hProcess; kids[i].pid = pi.dwProcessId; kids[i].dead = false;
        snprintf(kids[i].hb, sizeof kids[i].hb, "%s\\pp-child-%d.log", ls.c_str(), i);
        XLOGW("procprobe", "spawned child %d (%s) pid=%lu", i, behs[i].c_str(), pi.dwProcessId);
    }
    SetEnvironmentVariableA("XRL_PROCCHILD", nullptr);
    for (int s = 1; s <= 70; s++) {
        Sleep(1000);
        unsigned long long now = GetTickCount64();
        char sum[700]; int o = 0; o += snprintf(sum + o, sizeof sum - o, "t=%2ds", s);
        for (int i = 0; i < N; i++) {
            Kid& k = kids[i];
            if (!k.h)   { o += snprintf(sum + o, sizeof sum - o, " c%d=nospawn", i); continue; }
            if (k.dead) { o += snprintf(sum + o, sizeof sum - o, " c%d=.", i); continue; }
            if (WaitForSingleObject(k.h, 0) == WAIT_OBJECT_0) {
                DWORD code = 0; GetExitCodeProcess(k.h, &code);
                XLOGW("procprobe", "child %d (%s) TERMINATED at +%llums exit=0x%lx (%lu)", i, k.beh.c_str(), now - t0, code, code);
                o += snprintf(sum + o, sizeof sum - o, " c%d=TERM0x%lx", i, code); k.dead = true; continue;
            }
            unsigned long long last = pp_last_hb(k.hb);
            long long age = last ? (long long)(now - last) : -1;
            o += snprintf(sum + o, sizeof sum - o, " c%d=%s+%lldms", i, (age >= 0 && age < 1500) ? "ok" : "FROZEN", age);
        }
        XLOGW("procprobe", "%s", sum);
    }
    for (int i = 0; i < N; i++)
        XLOGW("procprobe", "VERDICT child %d (%s): %s", i, kids[i].beh.c_str(), kids[i].dead ? "TERMINATED" : "alive-at-end");
    XLOGW("procprobe", "DONE (main pid=%lu held foreground throughout)", GetCurrentProcessId());
}

}  // namespace xrl

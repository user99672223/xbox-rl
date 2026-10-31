// discover.cpp - XRL_DISCOVER=1 (host_env): ask the console what it actually offers, read-only, then exit.
// Everything lands in LocalState\discover\ for the laptop to pull and analyse (tools: scratchpad console-discover.ps1):
//   sysinfo.txt   CPU sets and caches, memory and job limits, timer resolution and Sleep(1), large pages, placeholder
//                 memory (VirtualAlloc2/MapViewOfFile3), exploit-protection policies, process limits
//   winrt.txt     which runtime classes activate here (display, gaming, composition...), and what
//                 Windows.Devices.Display.Core's DisplayManager reports about the display (modes, presentation rates)
//   gfx.txt       DXGI/D3D12/D3D11 interfaces our objects answer, feature support, adapters (gfx_discover)
//   files.txt     the names and sizes in System32 and System32\WinMetadata
//   winmd\, dll\  copies of the runtime API descriptions and of the graphics/display/kernel DLLs (laptop-only analysis)
#include "pch.h"
#include "xrl_host.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Xbox.Graphics.Display.h>
#include <winrt/Windows.Xbox.Graphics.Display.Internal.h>
#include <winrt/Windows.Xbox.System.Internal.TV.h>

namespace xrl {
namespace {

FILE* open_out(const std::wstring& dir, const wchar_t* name) {
    std::wstring p = dir + L"\\" + name;
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"w");
    return f;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void sysinfo(FILE* f) {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    fprintf(f, "cpus %lu, page %lu, granularity %lu, app VA %p..%p\n", si.dwNumberOfProcessors, si.dwPageSize,
            si.dwAllocationGranularity, si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress);
    {   // CPU sets: ids, cores, efficiency, parking
        ULONG len = 0;
        GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<BYTE> b(len);
        if (len && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)b.data(), len, &len, GetCurrentProcess(), 0)) {
            for (ULONG o = 0; o < len;) {
                auto* c = (PSYSTEM_CPU_SET_INFORMATION)(b.data() + o);
                if (c->Type == CpuSetInformation)
                    fprintf(f, "cpuset id %lu: group %u lp %u core %u last-level-cache %u numa %u efficiency %u parked %u allocated %u "
                            "realtime %u\n", c->CpuSet.Id, c->CpuSet.Group, c->CpuSet.LogicalProcessorIndex, c->CpuSet.CoreIndex,
                            c->CpuSet.LastLevelCacheIndex, c->CpuSet.NumaNodeIndex, c->CpuSet.EfficiencyClass, c->CpuSet.Parked,
                            c->CpuSet.Allocated, c->CpuSet.RealTime);
                o += c->Size;
            }
        } else fprintf(f, "GetSystemCpuSetInformation: error %lu\n", GetLastError());
    }
    {   // caches
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
        std::vector<BYTE> b(len);
        if (len && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)b.data(), &len))
            for (DWORD o = 0; o < len;) {
                auto* c = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(b.data() + o);
                fprintf(f, "cache L%u type %d %lu KiB line %u ways %u mask %llx\n", c->Cache.Level, (int)c->Cache.Type,
                        c->Cache.CacheSize >> 10, c->Cache.LineSize, c->Cache.Associativity, (unsigned long long)c->Cache.GroupMask.Mask);
                o += c->Size;
            }
    }
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    fprintf(f, "memory: phys %llu MiB (avail %llu), commit limit %llu MiB (avail %llu), VA %llu GiB\n", ms.ullTotalPhys >> 20,
            ms.ullAvailPhys >> 20, ms.ullTotalPageFile >> 20, ms.ullAvailPageFile >> 20, ms.ullTotalVirtual >> 30);
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION j{};
        BOOL in = FALSE;
        IsProcessInJob(GetCurrentProcess(), nullptr, &in);
        if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &j, sizeof j, nullptr))
            fprintf(f, "job: in job %d, flags 0x%lx, process memory %llu MiB, job memory %llu MiB, active processes %lu\n", in,
                    j.BasicLimitInformation.LimitFlags, (unsigned long long)(j.ProcessMemoryLimit >> 20),
                    (unsigned long long)(j.JobMemoryLimit >> 20), j.BasicLimitInformation.ActiveProcessLimit);
        else fprintf(f, "job: in job %d, query error %lu\n", in, GetLastError());
    }
    {   // timer resolution (ntdll, read-only) and what Sleep(1) really takes
        typedef LONG(NTAPI * QTR)(PULONG, PULONG, PULONG);
        auto q = (QTR)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
        ULONG mn = 0, mx = 0, cur = 0;
        if (q && q(&mn, &mx, &cur) == 0)
            fprintf(f, "timer resolution: coarsest %.3f ms, finest %.3f ms, current %.3f ms\n", mn / 1e4, mx / 1e4, cur / 1e4);
        else fprintf(f, "NtQueryTimerResolution: %s\n", q ? "failed" : "not exported");
        LARGE_INTEGER fq, a, b; QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 50; i++) Sleep(1);
        QueryPerformanceCounter(&b);
        fprintf(f, "Sleep(1): %.3f ms each; QPC %lld Hz\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart / 50, fq.QuadPart);
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (t) {
            QueryPerformanceCounter(&a);
            for (int i = 0; i < 50; i++) {
                LARGE_INTEGER due; due.QuadPart = -5000;   // 0.5 ms
                SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(t, INFINITE);
            }
            QueryPerformanceCounter(&b);
            fprintf(f, "high-resolution timer, 0.5 ms asked: %.3f ms each\n", (double)(b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart / 50);
            CloseHandle(t);
        } else fprintf(f, "high-resolution waitable timer: error %lu\n", GetLastError());
    }
    {   // large pages
        SIZE_T lp = GetLargePageMinimum();
        void* p = lp ? VirtualAlloc(nullptr, lp, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE) : nullptr;
        fprintf(f, "large pages: minimum %zu KiB, MEM_LARGE_PAGES alloc %s (error %lu)\n", lp >> 10, p ? "OK" : "refused",
                p ? 0ul : GetLastError());
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
    {   // placeholders: reserve, split, map a section view into a placeholder (cheap mmap/fork building blocks)
        const SIZE_T sz = 4 << 20;
        void* r = VirtualAlloc2FromApp(nullptr, nullptr, sz, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        fprintf(f, "VirtualAlloc2FromApp(MEM_RESERVE_PLACEHOLDER): %s (error %lu)\n", r ? "OK" : "refused", r ? 0ul : GetLastError());
        if (r) {
            BOOL split = VirtualFree(r, 1 << 20, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            fprintf(f, "  split a placeholder (VirtualFree MEM_PRESERVE_PLACEHOLDER): %s (error %lu)\n", split ? "OK" : "refused",
                    split ? 0ul : GetLastError());
            HANDLE s = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 1 << 20, nullptr);
            void* v = s ? MapViewOfFile3FromApp(s, nullptr, r, 0, 1 << 20, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) : nullptr;
            fprintf(f, "  MapViewOfFile3FromApp(MEM_REPLACE_PLACEHOLDER): %s (error %lu)\n", v ? "OK" : "refused", v ? 0ul : GetLastError());
            if (v) {
                ((volatile char*)v)[0] = 1;
                UnmapViewOfFile2(GetCurrentProcess(), v, MEM_PRESERVE_PLACEHOLDER);
            }
            if (s) CloseHandle(s);
            VirtualFree(r, 0, MEM_RELEASE);
            VirtualFree((char*)r + (1 << 20), 0, MEM_RELEASE);
        }
        // a view mapped twice (the same memory at two addresses: MAP_SHARED, fork sharing)
        HANDLE s = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 1 << 16, nullptr);
        void* a = s ? MapViewOfFileFromApp(s, FILE_MAP_WRITE, 0, 1 << 16) : nullptr;
        void* b = s ? MapViewOfFileFromApp(s, FILE_MAP_WRITE, 0, 1 << 16) : nullptr;
        if (a && b) { ((volatile int*)a)[0] = 42; fprintf(f, "double-mapped section: %s\n", ((volatile int*)b)[0] == 42 ? "coherent" : "NOT coherent"); }
        else fprintf(f, "double-mapped section: refused (error %lu)\n", GetLastError());
        if (a) UnmapViewOfFile(a);
        if (b) UnmapViewOfFile(b);
        if (s) CloseHandle(s);
        void* x = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        fprintf(f, "VirtualAlloc PAGE_EXECUTE_READWRITE: %s (error %lu)\n", x ? "OK" : "refused", x ? 0ul : GetLastError());
        if (x) VirtualFree(x, 0, MEM_RELEASE);
    }
    {   // exploit-protection policies of this process
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &dc, sizeof dc))
            fprintf(f, "mitigation dynamic code: prohibit %u, allow thread opt-out %u, allow remote downgrade %u\n",
                    dc.ProhibitDynamicCode, dc.AllowThreadOptOut, dc.AllowRemoteDowngrade);
        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof cfg))
            fprintf(f, "mitigation CFG: enabled %u, export suppression %u, strict %u\n", cfg.EnableControlFlowGuard,
                    cfg.EnableExportSuppression, cfg.StrictMode);
        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sg{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessSignaturePolicy, &sg, sizeof sg))
            fprintf(f, "mitigation signatures: microsoft only %u, store only %u\n", sg.MicrosoftSignedOnly, sg.StoreSignedOnly);
        PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY ss{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &ss, sizeof ss))
            fprintf(f, "mitigation shadow stack (CET): enabled %u, strict %u\n", ss.EnableUserShadowStack, ss.EnableUserShadowStackStrictMode);
        PROCESS_MITIGATION_IMAGE_LOAD_POLICY il{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessImageLoadPolicy, &il, sizeof il))
            fprintf(f, "mitigation image load: no remote %u, no low label %u, prefer system32 %u\n", il.NoRemoteImages,
                    il.NoLowMandatoryLabelImages, il.PreferSystem32Images);
    }
    {
        DWORD pc = GetPriorityClass(GetCurrentProcess());
        fprintf(f, "priority class 0x%lx; SetPriorityClass(HIGH): %s\n", pc,
                SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) ? "OK" : "refused");
        SetPriorityClass(GetCurrentProcess(), pc);
        fprintf(f, "SetThreadPriority(TIME_CRITICAL): %s\n", SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) ? "OK" : "refused");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
}

void winrt_probe(FILE* f) {
    static const wchar_t* classes[] = {
        L"Windows.Devices.Display.Core.DisplayManager", L"Windows.Devices.Display.DisplayMonitor",
        L"Windows.Graphics.Display.DisplayServices", L"Windows.Graphics.Display.Core.HdmiDisplayInformation",
        L"Windows.Graphics.Display.DisplayInformation", L"Windows.Graphics.Display.BrightnessOverride",
        L"Windows.Graphics.Display.DisplayEnhancementOverride", L"Windows.UI.Composition.Compositor",
        L"Windows.UI.Composition.Core.CompositorController", L"Windows.Graphics.Capture.GraphicsCaptureItem",
        L"Windows.Gaming.Input.Gamepad", L"Windows.Gaming.Input.RawGameController", L"Windows.Gaming.UI.GameBar",
        L"Windows.Gaming.Preview.GamesEnumeration.GameList", L"Windows.Media.Playback.MediaPlayer",
        L"Windows.System.Display.DisplayRequest", L"Windows.ApplicationModel.ExtendedExecution.ExtendedExecutionSession",
        L"Windows.Devices.Input.KeyboardCapabilities", L"Windows.Devices.Input.MouseDevice",
        L"Windows.Networking.Sockets.StreamSocket", L"Windows.System.Profile.AnalyticsInfo",
        L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext", L"Windows.Xbox.Input.Gamepad",
        L"Windows.Xbox.System.User", L"Windows.Xbox.Graphics.Display.DisplayConfiguration",
    };
    for (const wchar_t* c : classes) {
        fprintf(f, "trying %s\n", narrow(c).c_str());
        fflush(f);
        try {
            auto fac = winrt::get_activation_factory<winrt::Windows::Foundation::IUnknown>(winrt::hstring(c));
            fprintf(f, "class %s: activation factory OK\n", narrow(c).c_str());
            fflush(f);
        } catch (const winrt::hresult_error& e) {
            fprintf(f, "class %s: 0x%08x\n", narrow(c).c_str(), (unsigned)e.code());
            fflush(f);
        }
    }
    // the display through Windows.Devices.Display.Core (the lower-level display API): targets, paths, modes and rates
    try {
        namespace dc = winrt::Windows::Devices::Display::Core;
        fprintf(f, "DisplayManager::Create...\n"); fflush(f);
        auto mgr = dc::DisplayManager::Create(dc::DisplayManagerOptions::None);
        auto targets = mgr.GetCurrentTargets();
        fprintf(f, "DisplayManager: %u targets\n", targets.Size());
        for (const auto& t : targets) {
            try {
                fprintf(f, "  target: connected %d, usage %d, stale %d, monitor id %s\n", (int)t.IsConnected(), (int)t.UsageKind(),
                        (int)t.IsStale(), winrt::to_string(t.StableMonitorId()).c_str());
            } catch (const winrt::hresult_error& e) { fprintf(f, "  target: 0x%08x\n", (unsigned)e.code()); }
        }
        fprintf(f, "TryReadCurrentStateForAllTargets...\n"); fflush(f);
        auto res = mgr.TryReadCurrentStateForAllTargets();
        fprintf(f, "TryReadCurrentStateForAllTargets: error %d\n", (int)res.ErrorCode());
        if (auto st = res.State()) {
            for (const auto& v : st.Views()) {
                for (const auto& p : v.Paths()) {
                    auto src = p.SourceResolution(), tgt = p.TargetResolution();
                    auto pr = p.PresentationRate();
                    fprintf(f, "  path: source %dx%d target %dx%d, rate %s, interlaced %d, scaling %d, status %d\n",
                            src ? src.Value().Width : 0, src ? src.Value().Height : 0, tgt ? tgt.Value().Width : 0,
                            tgt ? tgt.Value().Height : 0,
                            pr ? (std::to_string(pr.Value().VerticalSyncRate.Numerator) + "/" + std::to_string(pr.Value().VerticalSyncRate.Denominator) +
                                  " vsyncs/present " + std::to_string(pr.Value().VerticalSyncsPerPresentation)).c_str() : "none",
                            (int)p.IsInterlaced().Value(), (int)p.Scaling(), (int)p.Status());
                    auto modes = p.FindModes(dc::DisplayModeQueryOptions::None);
                    fprintf(f, "  %u modes:\n", modes.Size());
                    for (const auto& m : modes) {
                        auto r = m.PresentationRate();
                        fprintf(f, "    %dx%d -> %dx%d @ %d/%d (vsyncs/present %d), interlaced %d, stereo %d\n",
                                m.SourceResolution().Width, m.SourceResolution().Height, m.TargetResolution().Width,
                                m.TargetResolution().Height, r.VerticalSyncRate.Numerator, r.VerticalSyncRate.Denominator,
                                r.VerticalSyncsPerPresentation, (int)m.IsInterlaced(), (int)m.IsStereo());
                    }
                }
            }
        }
    } catch (const winrt::hresult_error& e) {
        fprintf(f, "DisplayManager: 0x%08x %s\n", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
    } catch (...) {
        fprintf(f, "DisplayManager: unknown failure\n");
    }
}

// System32's file names and sizes; copies of the files matching `want` into `to`
void list_and_copy(FILE* f, const std::wstring& dir, const std::wstring& to, bool (*want)(const std::wstring&), uint64_t& copied) {
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) { fprintf(f, "%s: cannot list (error %lu)\n", narrow(dir).c_str(), GetLastError()); return; }
    do {
        if (d.cFileName[0] == L'.') continue;
        uint64_t sz = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        bool isdir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        fprintf(f, "%s\\%s %s %llu\n", narrow(dir).c_str(), narrow(d.cFileName).c_str(), isdir ? "dir" : "file", (unsigned long long)sz);
        if (!isdir && want && want(d.cFileName) && copied < (400ull << 20)) {
            if (CopyFile2((dir + L"\\" + d.cFileName).c_str(), (to + L"\\" + d.cFileName).c_str(), nullptr) == S_OK) copied += sz;
            else fprintf(f, "  (copy refused)\n");
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
}

bool want_dll(const std::wstring& n) {
    std::wstring l = n;
    for (auto& c : l) c = towlower(c);
    if (l.size() < 4 || l.compare(l.size() - 4, 4, L".dll")) return false;
    static const wchar_t* keys[] = {L"d3d", L"dxg", L"dxcore", L"dcomp", L"display", L"graphics", L"composition", L"coremessaging",
                                    L"gaming", L"xbox", L"umd", L"ntdll", L"kernelbase", L"kernel32", L"win32u", L"twinapi",
                                    L"windows.ui.dll", L"presentation", L"hdmi", L"vrr"};
    for (const wchar_t* k : keys) if (l.find(k) != std::wstring::npos) return true;
    return false;
}
bool want_winmd(const std::wstring& n) { return n.size() > 6 && !_wcsicmp(n.c_str() + n.size() - 6, L".winmd"); }

// The Xbox's own display and TV APIs (bindings generated from the console's Windows.Xbox.winmd into host/gen_xbox,
// local only), read-only: what the console reports about the display mode, VRR, 120 Hz and the TV. Logged straight to
// the app log line by line (a stuck call cannot hide what came before it). Nothing is set.
void xbox_display_probe() {
    namespace xd = winrt::Windows::Xbox::Graphics::Display;
    namespace xdi = winrt::Windows::Xbox::Graphics::Display::Internal;
    namespace tv = winrt::Windows::Xbox::System::Internal::TV;
    auto step = [](const char* what, auto&& fn) {
        XLOGI("xbox", "%s ...", what);
        try { fn(); } catch (const winrt::hresult_error& e) {
            XLOGI("xbox", "%s: 0x%08x %s", what, (unsigned)e.code(), winrt::to_string(e.message()).c_str());
        } catch (...) { XLOGI("xbox", "%s: failed", what); }
    };
    step("TVCapabilities", [] {
        XLOGI("xbox", "TV supports VRR %d, 120 Hz video %d, ALLM %d", (int)tv::TVCapabilities::SupportVrr(),
              (int)tv::TVCapabilities::Support120HzVideo(), (int)tv::TVCapabilities::SupportAllm());
    });
    step("TVProperties", [] {
        XLOGI("xbox", "VRR enabled %d, VRR policy %d (0 always on, 1 games only), ALLM enabled %d", (int)tv::TVProperties::VrrEnabled(),
              (int)tv::TVProperties::VrrPolicy(), (int)tv::TVProperties::AllmEnabled());
    });
    step("DisplayConfigurationInternal", [] {
        float lo = 0, hi = 0;
        xdi::DisplayConfigurationInternal::GetVariableRefreshRate(lo, hi);
        XLOGI("xbox", "display VRR enabled %d, variable refresh range %.2f..%.2f Hz",
              (int)xdi::DisplayConfigurationInternal::GetVrrEnabled(), lo, hi);
    });
    step("DisplayManager state dump", [] {
        int n = 0;
        for (const auto& line : xdi::DisplayManager::GetStateDump()) {
            std::string s = winrt::to_string(line);
            for (size_t at = 0; at < s.size() && at < 2000; at += 400) XLOGI("xbox", "dump: %s", s.substr(at, 400).c_str());
            if (++n >= 300) break;
        }
    });
    // DisplayConfiguration is per view: on the UI thread
    void* cw = window_info_get().core_window;
    if (cw) step("DisplayConfiguration (UI thread)", [cw] {
        winrt::Windows::UI::Core::CoreWindow w{nullptr};
        winrt::copy_from_abi(w, cw);
        w.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [] {
            try {
                auto dc = xd::DisplayConfiguration::GetForCurrentView();
                auto cur = dc.CurrentDisplayMode();
                XLOGI("xbox", "current mode %ux%u @ %.3f Hz, %u bpp", cur.RawWidth(), cur.RawHeight(), cur.RefreshRate(), cur.BitsPerPixel());
                auto blob = [](const winrt::Windows::Storage::Streams::IBuffer& b) {
                    std::string hex;
                    if (!b) return std::string("(none)");
                    const uint8_t* p = b.data();
                    for (uint32_t i = 0; i < b.Length() && i < 96; i++) { char t[4]; snprintf(t, sizeof t, "%02x", p[i]); hex += t; }
                    return std::to_string(b.Length()) + " bytes " + hex;
                };
                try { XLOGI("xbox", "current mode AdvancedData: %s", blob(cur.as<xd::IDisplayMode2>().AdvancedData()).c_str()); }
                catch (const winrt::hresult_error& e) { XLOGI("xbox", "AdvancedData: 0x%08x", (unsigned)e.code()); }
                for (const auto& m : dc.SupportedDisplayModes())
                    XLOGI("xbox", "  mode %ux%u @ %.3f Hz, %u bpp, stereo %d", m.RawWidth(), m.RawHeight(), m.RefreshRate(), m.BitsPerPixel(),
                          (int)m.IsStereoSupported());
            } catch (const winrt::hresult_error& e) {
                XLOGI("xbox", "DisplayConfiguration: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            } catch (...) { XLOGI("xbox", "DisplayConfiguration: failed"); }
        }).get();
    });
    // the view our app gets: its real pixel size, and whether it may be made bigger (our own window only)
    if (cw) step("view size (UI thread)", [cw] {
        winrt::Windows::UI::Core::CoreWindow w{nullptr};
        winrt::copy_from_abi(w, cw);
        w.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [w] {
            try {
                auto di = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
                auto b = w.Bounds();
                XLOGI("xbox", "view: bounds %.0fx%.0f DIPs, raw pixels per view pixel %.3f, screen %ux%u raw pixels, scale %d%%, logical dpi %.1f",
                      b.Width, b.Height, di.RawPixelsPerViewPixel(), di.ScreenWidthInRawPixels(), di.ScreenHeightInRawPixels(),
                      (int)di.ResolutionScale(), di.LogicalDpi());
                auto av = winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
                XLOGI("xbox", "view: full screen %d, bounds mode %d", (int)av.IsFullScreenMode(), (int)av.DesiredBoundsMode());
                bool core = av.SetDesiredBoundsMode(winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
                bool rs = av.TryResizeView({2560.f, 1440.f});
                auto b2 = w.Bounds();
                XLOGI("xbox", "view: SetDesiredBoundsMode(UseCoreWindow) %d, TryResizeView(2560x1440) %d -> bounds %.0fx%.0f, screen %ux%u",
                      (int)core, (int)rs, b2.Width, b2.Height, di.ScreenWidthInRawPixels(), di.ScreenHeightInRawPixels());
            } catch (const winrt::hresult_error& e) {
                XLOGI("xbox", "view: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
            } catch (...) { XLOGI("xbox", "view: failed"); }
        }).get();
    });
    XLOGI("xbox", "xbox display probe done");
}

}  // namespace

void discover_run() {
    std::wstring dir = widen(local_state_utf8()) + L"\\discover";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\dll").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\winmd").c_str(), nullptr);
    XLOGI("discover", "writing to %s", narrow(dir).c_str());
    {   // the Xbox display/TV APIs first, on a thread with a time limit (a call may never return)
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([done] {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
            xbox_display_probe();
            SetEvent(done);
        }).detach();
        if (WaitForSingleObject(done, 20000) != WAIT_OBJECT_0) XLOGW("discover", "xbox display probe still running after 20 s: left behind");
    }
    if (FILE* f = open_out(dir, L"sysinfo.txt")) { sysinfo(f); fclose(f); }
    XLOGI("discover", "sysinfo done");
    if (FILE* f = open_out(dir, L"gfx.txt")) { gfx_discover_run(f); fclose(f); }
    XLOGI("discover", "gfx done");
    if (FILE* f = open_out(dir, L"files.txt")) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        uint64_t copied = 0;
        list_and_copy(f, sys, dir + L"\\dll", want_dll, copied);
        list_and_copy(f, std::wstring(sys) + L"\\WinMetadata", dir + L"\\winmd", want_winmd, copied);
        fprintf(f, "copied %llu MiB\n", (unsigned long long)(copied >> 20));
        fclose(f);
        XLOGI("discover", "files done (%llu MiB copied)", (unsigned long long)(copied >> 20));
    }
    // last, on its own thread with a time limit: a runtime call that never returns (one did) must not stop the rest
    if (FILE* f = open_out(dir, L"winrt.txt")) {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([f, done] {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
            winrt_probe(f);
            fclose(f);
            SetEvent(done);
        }).detach();
        if (WaitForSingleObject(done, 30000) != WAIT_OBJECT_0) XLOGW("discover", "winrt probe still running after 30 s: left behind (see winrt.txt)");
        else XLOGI("discover", "winrt done");
    }
    XLOGI("discover", "discovery done");
}

}  // namespace xrl

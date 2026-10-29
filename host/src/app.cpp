// app.cpp - process entry, early init, and the PROBE-build front-end.
//
//  * XboxRL.App (App.idl) is the activation class the Xbox launcher validates.
//  * PROBE build  (default):  wWinMain -> CoreApplication::Run(make<App>()) with a
//    small C++/WinRT IFrameworkView. This is the exact shape that is proven to
//    launch on the console (the earlier capability probe). The UI thread only
//    owns the CoreWindow and pumps events; the probe runs on worker threads and
//    all D3D creation is marshalled to the gfx thread.
//  * FULL build (XRL_FULL):  wWinMain lives in sdl_main.cpp and SDL2's WinRT
//    backend runs the view instead; XboxRL.App still exists (the launcher needs
//    it in the winmd) but is not the object CoreApplication::Run is given.
#include "pch.h"
#include "App.h"
#include "App.g.cpp"
#include "xrl_host.h"

#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.ApplicationModel.ExtendedExecution.h>

namespace winrt {
    using namespace Windows::ApplicationModel::Core;
    using namespace Windows::UI::Core;
}

namespace xrl {

// Hold an extended-execution session so the OS does not SUSPEND the app when it drops out of the
// foreground. EAC launches the game as a 2nd process and ptrace-injects it over a couple of seconds;
// without this the Xbox suspends the app mid-injection (APP_WILLENTERBACKGROUND) and EAC never
// reaches "301 loaded". Must be requested while foreground; the session is kept alive (static).
void request_extended_execution() {
    using namespace winrt::Windows::ApplicationModel::ExtendedExecution;
    try {
        static ExtendedExecutionSession s{ nullptr };
        s = ExtendedExecutionSession();
        s.Reason(ExtendedExecutionReason::Unspecified);
        s.Description(L"xbox-rl: EAC in-game load");
        s.Revoked([](auto&&, ExtendedExecutionRevokedEventArgs const& a) {
            XLOGW("host", "extended execution revoked (reason %d)", (int)a.Reason());
        });
        auto r = s.RequestExtensionAsync().get();
        XLOGI("host", "extended execution request -> %d (0=Allowed, 1=Denied)", (int)r);
    } catch (...) {
        XLOGW("host", "extended execution request threw");
    }
}

void host_early_init(const char* mode_name) {
    // MTA on the main thread. CoreApplication::Run creates the ASTA UI thread itself.
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    log_init();
    install_crash_logger();
    XLOGI("host", "xbox-rl host starting, mode=%s, built %s %s, ABI v%u", mode_name, __DATE__, __TIME__, XRL_ABI_VERSION);
    load_config();
}

void host_exit() {
    XLOGI("host", "exiting application");
    log_shutdown();
    try { winrt::CoreApplication::Exit(); } catch (...) {}
}

}  // namespace xrl

#if !defined(XRL_FULL)
// ---------------------------------------------------------------------------
// PROBE front-end
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_probe_done{false};
std::atomic<bool> g_probe_ok{false};

struct ProbeView : winrt::implements<ProbeView, winrt::IFrameworkView> {
    void Initialize(winrt::CoreApplicationView const& v) {
        XLOGI("view", "Initialize (UI thread 0x%lx)", GetCurrentThreadId());
        v.Activated([](auto&&, auto&&) { XLOGI("view", "Activated"); });
        winrt::CoreApplication::Suspending([](auto&&, auto&&) { XLOGI("view", "Suspending"); });
        winrt::CoreApplication::Resuming([](auto&&, auto&&) { XLOGI("view", "Resuming"); });
    }
    void Load(winrt::hstring const&) {}
    void Uninitialize() {}

    void SetWindow(winrt::CoreWindow const& w) {
        window_ = w;
        xrl::WindowInfo wi;
        wi.core_window = winrt::get_abi(w);   // ICoreWindow*, agile; not AddRef'd (w outlives the process's use)
        try {
            auto b = w.Bounds();
            double scale = 1.0;
            try { scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel(); } catch (...) {}
            wi.width  = (uint32_t)(b.Width * scale + 0.5);
            wi.height = (uint32_t)(b.Height * scale + 0.5);
            XLOGI("view", "SetWindow: CoreWindow %p bounds %.0fx%.0f DIPs, scale %.2f -> %ux%u px", wi.core_window, b.Width, b.Height, scale, wi.width, wi.height);
        } catch (...) { XLOGW("view", "could not read CoreWindow bounds"); }
        wi.valid = true;
        xrl::window_info_set(wi);

        // Start the probe BEFORE anything that could block the UI thread.
        probe_ = std::thread([] {
            try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
            const auto& cfg = xrl::config();
            xrl::ProbeOptions opt = xrl::ProbeOptions::all();
            if (cfg.probe_mode == "quick") opt = xrl::ProbeOptions::quick();
            if (cfg.probe_mode == "none") { XLOGI("probe", "probe=none in launch.cfg; skipping"); g_probe_done = true; return; }
            xrl::gfx_thread_start();
            g_probe_ok = xrl::probe_run(opt);
            g_probe_done = true;
        });
    }

    void Run() {
        XLOGI("view", "Run (UI thread): activating window and pumping events");
        window_.Activate();
        auto dispatcher = window_.Dispatcher();
        ULONGLONG start = GetTickCount64();
        ULONGLONG done_at = 0;
        const int linger_ms = xrl::config().linger_seconds * 1000;
        for (;;) {
            dispatcher.ProcessEvents(winrt::CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(16);
            ULONGLONG now = GetTickCount64();
            if (g_probe_done && !done_at) done_at = now;
            if (done_at && now - done_at >= (ULONGLONG)linger_ms) break;
            if (now - start > 15 * 60 * 1000) { XLOGW("view", "15 minute safety timeout hit; exiting"); break; }
        }
        if (probe_.joinable()) probe_.join();
        XLOGI("view", "probe finished: required steps %s", g_probe_ok ? "PASS" : "FAIL");
        xrl::host_exit();
    }

    winrt::CoreWindow window_{nullptr};
    std::thread probe_;
};

}  // namespace

namespace winrt::XboxRL::implementation
{
    Windows::ApplicationModel::Core::IFrameworkView App::CreateView()
    {
        return winrt::make<ProbeView>();
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    xrl::host_early_init("probe");
    winrt::CoreApplication::Run(winrt::make<winrt::XboxRL::implementation::App>());
    return 0;
}

#else  // XRL_FULL
// In the full build the view is SDL's; XboxRL.App::CreateView is never called by
// CoreApplication::Run. Keep a correct implementation anyway (returns a view that
// only exits) so the runtimeclass is complete.
namespace winrt::XboxRL::implementation
{
    Windows::ApplicationModel::Core::IFrameworkView App::CreateView()
    {
        throw winrt::hresult_not_implemented(L"XboxRL.App is only the launcher-visible activation class in the SDL build");
    }
}
#endif

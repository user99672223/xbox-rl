// desktop_harness.cpp — runs the gfx probe on a dev PC (desktop Win32, NOT UWP).
//
// Same library code, same ops table, same selftest as the console; the only
// difference is the window source: an HWND via xrl_gfx_config.test_hwnd instead
// of a CoreWindow. It therefore validates everything EXCEPT
// CreateSwapChainForCoreWindow itself (device/queue/caps/D3D11On12/flip-model
// swapchain/present/readback, worker-thread marshalling, handle table).
//
//   xrl_gfx_harness.exe [--warp] [--headless] [--debug-layer] [--force-no-on12] [--timeout-ms N]
//                       [--host-svc]  drive gfx through a fake xrl_gfx_host_services
//                                     (host-owned gfx thread, as host/ provides)
//                       [--ops-get]   production entry xrl_gfx_ops_get(&svc) + headless probe
//
// Exit code 0 == PASS.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "xrl_gfx.h"
#include "../src/seam_mirror.h"  // xrl_gfx_host_services (mirror of host/src/gfx_seam.h)

static void LogToStdout(int level, const char* tag, const char* msg) {
    static const char* names[] = {"TRACE", "INFO ", "WARN ", "ERROR"};
    printf("[%s] %-4s %s\n", names[level & 3], tag, msg);
    fflush(stdout);
}

// ---- a stand-in for host/src/gfx_thread.cpp: one MTA thread, synchronous run with timeout.
namespace fakehost {
struct Item {
    void (*fn)(void*);
    void* ctx;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};
std::mutex g_m;
std::condition_variable g_cv;
std::deque<std::shared_ptr<Item>> g_q;
std::thread g_th;
DWORD g_tid = 0;
HWND g_hwnd = nullptr;

void thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g_tid = GetCurrentThreadId();
    for (;;) {
        std::shared_ptr<Item> it;
        {
            std::unique_lock<std::mutex> l(g_m);
            g_cv.wait(l, [] { return !g_q.empty(); });
            it = g_q.front();
            g_q.pop_front();
        }
        if (!it->fn) break;
        it->fn(it->ctx);
        { std::lock_guard<std::mutex> l(it->m); it->done = true; }
        it->cv.notify_all();
    }
}
int run(const char*, void (*fn)(void*), void* ctx, uint32_t timeout_ms) {
    auto it = std::make_shared<Item>();
    it->fn = fn;
    it->ctx = ctx;
    { std::lock_guard<std::mutex> l(g_m); g_q.push_back(it); }
    g_cv.notify_one();
    std::unique_lock<std::mutex> l(it->m);
    return it->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return it->done; }) ? 0 : -1;
}
int on_thread() { return GetCurrentThreadId() == g_tid ? 1 : 0; }
void* core_window() { return nullptr; }  // no CoreWindow on desktop
void window_size(uint32_t* w, uint32_t* h) { *w = 1280; *h = 720; }
void start() { g_th = std::thread(thread_main); }
void stop() {
    auto it = std::make_shared<Item>();
    it->fn = nullptr;
    { std::lock_guard<std::mutex> l(g_m); g_q.push_back(it); }
    g_cv.notify_one();
    g_th.join();
}
}  // namespace fakehost

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    bool warp = false, headless = false, dbg = false, no_on12 = false;
    unsigned timeout_ms = 0;
    bool host_svc = false, ops_get = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--warp")) warp = true;
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--debug-layer")) dbg = true;
        else if (!strcmp(argv[i], "--force-no-on12")) no_on12 = true;
        else if (!strcmp(argv[i], "--host-svc")) host_svc = true;
        else if (!strcmp(argv[i], "--ops-get")) { ops_get = true; headless = true; }
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) timeout_ms = (unsigned)atoi(argv[++i]);
    }

    HWND hwnd = nullptr;
    const UINT W = 1280, H = 720;
    if (!headless) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"xrl_gfx_harness";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        RECT r{0, 0, (LONG)W, (LONG)H};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(wc.lpszClassName, L"xrl_gfx harness", WS_OVERLAPPEDWINDOW, 100, 100, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOW);
    }

    xrl_gfx_config cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.log = LogToStdout;
    cfg.test_hwnd = hwnd;
    cfg.window_width_px = W;
    cfg.window_height_px = H;
    cfg.ui_thread_id = GetCurrentThreadId();  // this (main) thread plays the "UI thread"
    cfg.create_timeout_ms = timeout_ms;  // tiny value exercises the abandoned-job path
    cfg.flags = (warp ? XRL_GFX_FLAG_ALLOW_WARP : 0) | (dbg ? XRL_GFX_FLAG_DEBUG_LAYER : 0) |
                (no_on12 ? XRL_GFX_FLAG_FORCE_NO_D3D11ON12 : 0);

    xrl_gfx_host_services svc{};
    svc.struct_size = sizeof svc;
    svc.log = LogToStdout;
    svc.get_core_window = fakehost::core_window;
    svc.get_window_size = fakehost::window_size;
    svc.run_on_gfx_thread = fakehost::run;
    svc.on_gfx_thread = fakehost::on_thread;
    if (host_svc || ops_get) {
        fakehost::start();
        cfg.host = &svc;
    }
    if (ops_get) {
        const xrl_gfx_ops* ops = xrl_gfx_ops_get(&svc);
        printf("xrl_gfx_ops_get -> %p (size %u, d3d11on12_create=%p)\n", (const void*)ops, ops ? ops->struct_size : 0,
               ops ? (const void*)ops->d3d11on12_create : nullptr);
    }

    // The probe must NOT run on the UI thread (and on the console it must not
    // either): run it on its own thread while this thread pumps messages.
    int rc = -1;
    std::thread t([&] { rc = xrl_gfx_run_probe(ops_get ? nullptr : &cfg); });
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (rc != -1) done = true;
        else Sleep(5);
    }
    t.join();
    if (ops_get) xrl_gfx_shutdown();
    if (host_svc || ops_get) fakehost::stop();
    printf("harness exit code %d (%s)\n", rc, rc == 0 ? "PASS" : "FAIL");
    return rc;
}

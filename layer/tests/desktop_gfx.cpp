// desktop_gfx.cpp - real graphics for the desktop test host (XRL_REAL_GFX=1).
//
// The console host hands the guest gfx/'s op table bound to its CoreWindow. Here
// the same library runs in its desktop mode instead: a plain Win32 window on its
// own UI thread, an HWND flip-model swapchain, the laptop's GPU. Everything below
// the swapchain-creation call is the console code path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include "xrl_gfx.h"

namespace {
HWND g_hwnd;
DWORD g_ui_tid;
HANDLE g_ready;
const UINT W = 1280, H = 720;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;   // the guest decides when to exit
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI UiThread(void*) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xrl_desktop_host";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    RECT r{0, 0, (LONG)W, (LONG)H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"xbox-rl (desktop host)", WS_OVERLAPPEDWINDOW, 100, 100,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    g_ui_tid = GetCurrentThreadId();
    SetEvent(g_ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
}  // namespace

extern "C" const xrl_gfx_ops* xrl_desktop_gfx(void (*log)(int, const char*, const char*)) {
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr));
    WaitForSingleObject(g_ready, INFINITE);
    if (!g_hwnd) { fprintf(stderr, "desktop_gfx: no window\n"); return nullptr; }

    xrl_gfx_config cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.log = log;
    cfg.test_hwnd = g_hwnd;
    cfg.window_width_px = W;
    cfg.window_height_px = H;
    cfg.ui_thread_id = g_ui_tid;
    if (xrl_gfx_init(&cfg) != XRL_OK) { fprintf(stderr, "desktop_gfx: xrl_gfx_init failed\n"); return nullptr; }
    return xrl_gfx_get_ops();
}

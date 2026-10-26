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
#include "input_queue.h"

namespace {
HWND g_hwnd;
DWORD g_ui_tid;
HANDLE g_ready;
const UINT W = 1280, H = 720;

// ---- keyboard and mouse -> the host input queue (host/src/input_queue.cpp) ----
// Win32 hands us PS/2 set-1 scancodes; the queue speaks Linux KEY_* codes. For the main block the two
// are the same numbers, the extended (E0) keys differ.
uint16_t LinuxKeyFromScancode(unsigned sc, bool ext, unsigned vk) {
    if (vk == VK_PAUSE) return 119;
    if (vk == VK_NUMLOCK) return 69;
    if (ext) {
        switch (sc) {
            case 0x1C: return 96;  case 0x1D: return 97;  case 0x35: return 98;  case 0x37: return 99;
            case 0x38: return 100; case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
            case 0x4B: return 105; case 0x4D: return 106; case 0x4F: return 107; case 0x50: return 108;
            case 0x51: return 109; case 0x52: return 110; case 0x53: return 111; case 0x5B: return 125;
            case 0x5C: return 126; case 0x5D: return 127; default: return 0;
        }
    }
    return sc >= 1 && sc <= 0x58 ? (uint16_t)sc : 0;
}

bool g_captured;        // relative mode applied: cursor hidden, clipped to the window, raw deltas
bool g_released;        // the user pressed Ctrl+Alt: the capture is suspended until the next click
bool g_in_window;
int g_wheel_rem, g_hwheel_rem;
const UINT_PTR kTimer = 1;

void ApplyMouseMode(HWND h) {
    bool want = xrl_inq_mouse_mode() == XRL_MOUSE_RELATIVE && !g_released && GetForegroundWindow() == h;
    if (want == g_captured) return;
    g_captured = want;
    if (want) {
        RECT r; GetClientRect(h, &r);
        POINT a{r.left, r.top}, b{r.right, r.bottom};
        ClientToScreen(h, &a); ClientToScreen(h, &b);
        RECT cr{a.x, a.y, b.x, b.y};
        ClipCursor(&cr);
        RAWINPUTDEVICE rid{0x01, 0x02, 0, h};   // generic desktop / mouse
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof rid);
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    }
}

void PushButton(uint16_t code, int down) { xrl_inq_push(XRL_IEV_KEY, code, down); }

bool g_ignore_input;    // XRL_WINDOW_INPUT=0: the window feeds the guest nothing (a scripted test must not hear the real mouse)

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_ignore_input && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR ||
                           m == WM_MOUSEMOVE || m == WM_INPUT || (m >= WM_LBUTTONDOWN && m <= WM_MOUSEHWHEEL) || m == WM_KILLFOCUS || m == WM_SETFOCUS))
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_CLOSE: return 0;   // the guest decides when to exit
    case WM_CREATE: SetTimer(h, kTimer, 30, nullptr); break;
    case WM_TIMER: if (w == kTimer) ApplyMouseMode(h); break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        bool down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        uint16_t code = LinuxKeyFromScancode((l >> 16) & 0xff, (l >> 24) & 1, (unsigned)w);
        if (down && (w == VK_CONTROL || w == VK_MENU) && xrl_inq_mouse_mode() == XRL_MOUSE_RELATIVE &&
            (GetKeyState(VK_LCONTROL) & 0x8000) && (GetKeyState(VK_LMENU) & 0x8000)) { g_released = true; ApplyMouseMode(h); }
        if (code) xrl_inq_push(XRL_IEV_KEY, code, down);
        if (m == WM_SYSKEYDOWN && w == VK_F4) break;   // Alt+F4 still reaches WM_CLOSE
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: {   // text, UTF-16 -> code points
        static unsigned hi;
        unsigned c = (unsigned)w;
        if (c >= 0xD800 && c < 0xDC00) { hi = c; return 0; }
        if (c >= 0xDC00 && c < 0xE000 && hi) { c = 0x10000 + ((hi - 0xD800) << 10) + (c - 0xDC00); }
        hi = 0;
        if (c >= 0x20 && c != 0x7f) xrl_inq_push(XRL_IEV_TEXT, 0, (int32_t)c);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_captured) break;   // raw input gives the deltas
        int32_t ax, ay;
        RECT r; GetClientRect(h, &r);
        xrl_inq_abs_from_client((short)LOWORD(l), (short)HIWORD(l), r.right - r.left, r.bottom - r.top, &ax, &ay);
        xrl_inq_push(XRL_IEV_ABS, 0, ax);
        xrl_inq_push(XRL_IEV_ABS, 1, ay);
        break;
    }
    case WM_INPUT: {
        if (!g_captured) break;
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE &&
            !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (ri.data.mouse.lLastX) xrl_inq_push(XRL_IEV_REL, 0, ri.data.mouse.lLastX);
            if (ri.data.mouse.lLastY) xrl_inq_push(XRL_IEV_REL, 1, ri.data.mouse.lLastY);
        }
        break;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
        bool down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN;
        uint16_t code = (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) ? 0x110 : (m == WM_RBUTTONDOWN || m == WM_RBUTTONUP) ? 0x111 :
                        (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP) ? 0x112 : (HIWORD(w) == XBUTTON1 ? 0x113 : 0x114);
        if (down) {
            if (g_released) { g_released = false; ApplyMouseMode(h); }
            SetCapture(h);
        } else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2))) ReleaseCapture();
        PushButton(code, down);
        if (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) return TRUE;
        break;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        int& rem = m == WM_MOUSEWHEEL ? g_wheel_rem : g_hwheel_rem;
        rem += GET_WHEEL_DELTA_WPARAM(w);
        int notches = rem / WHEEL_DELTA;
        rem -= notches * WHEEL_DELTA;
        if (notches) xrl_inq_push(XRL_IEV_REL, m == WM_MOUSEWHEEL ? 8 : 6, notches);
        break;
    }
    case WM_KILLFOCUS: xrl_inq_focus_lost(); g_released = false; ApplyMouseMode(h); break;
    case WM_SETFOCUS: xrl_inq_push(XRL_IEV_FOCUS, 0, 1); break;
    case WM_SETCURSOR: if (g_captured && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; } break;
    }
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI UiThread(void*) {
    char v[8];
    g_ignore_input = GetEnvironmentVariableA("XRL_WINDOW_INPUT", v, sizeof v) && v[0] == '0';
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

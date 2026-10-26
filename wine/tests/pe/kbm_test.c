/*
 * kbm_test.c - a Windows program that logs the keyboard and mouse input it receives, for the host keyboard/mouse
 * pump (patches/wine/0027): one full-screen window, every message of interest printed to stdout and to
 * Z:\tmp\kbm.log (= <rootfs>/tmp/kbm.log on the host).
 *
 *   kbm_test.exe [seconds]      (default 20)
 *
 * Prints "kbm-ready" once the window has the focus; the host's XRL_INPUT_AFTER=kbm-ready starts the scripted events.
 * Build: x86_64-w64-mingw32-gcc -O1 -o kbm_test.exe kbm_test.c -luser32 -lkernel32 (wine/tests/build-pe-tests.sh).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void say(const char *fmt, ...)
{
    char buf[256];
    DWORD n;
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    buf[len++] = '\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &n, NULL);
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, len, &n, NULL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: say("%s vk=0x%02x scan=0x%02x ext=%d repeat=%d", m == WM_KEYDOWN ? "WM_KEYDOWN" : "WM_SYSKEYDOWN", (unsigned)w, (unsigned)((l >> 16) & 0xff), (int)((l >> 24) & 1), (int)((l >> 30) & 1)); break;
    case WM_KEYUP: case WM_SYSKEYUP: say("%s vk=0x%02x scan=0x%02x ext=%d", m == WM_KEYUP ? "WM_KEYUP" : "WM_SYSKEYUP", (unsigned)w, (unsigned)((l >> 16) & 0xff), (int)((l >> 24) & 1)); break;
    case WM_CHAR: say("WM_CHAR '%c' (U+%04X)", (int)(w >= 32 && w < 127 ? w : '?'), (unsigned)w); break;
    case WM_MOUSEMOVE: say("WM_MOUSEMOVE %d,%d", (int)(short)LOWORD(l), (int)(short)HIWORD(l)); break;
    case WM_LBUTTONDOWN: say("WM_LBUTTONDOWN %d,%d", (int)(short)LOWORD(l), (int)(short)HIWORD(l)); break;
    case WM_LBUTTONUP: say("WM_LBUTTONUP %d,%d", (int)(short)LOWORD(l), (int)(short)HIWORD(l)); break;
    case WM_RBUTTONDOWN: say("WM_RBUTTONDOWN"); break;
    case WM_RBUTTONUP: say("WM_RBUTTONUP"); break;
    case WM_MOUSEWHEEL: say("WM_MOUSEWHEEL delta=%d", (int)(short)HIWORD(w)); break;
    case WM_MOUSEHWHEEL: say("WM_MOUSEHWHEEL delta=%d", (int)(short)HIWORD(w)); break;
    case WM_INPUT:
    {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE)
            say("WM_INPUT mouse dx=%ld dy=%ld flags=0x%x", ri.data.mouse.lLastX, ri.data.mouse.lLastY, (unsigned)ri.data.mouse.usFlags);
        break;
    }
    case WM_TIMER: PostQuitMessage(0); break;
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    WNDCLASSW wc = { 0 };
    MSG msg;
    HWND h;
    RAWINPUTDEVICE rid;

    g_log = CreateFileA("Z:\\tmp\\kbm.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"kbm_test";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    h = CreateWindowW(L"kbm_test", L"kbm_test", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                      NULL, NULL, wc.hInstance, NULL);
    if (!h) { say("kbm: CreateWindow failed %lu", GetLastError()); return 1; }
    SetForegroundWindow(h);
    SetFocus(h);
    rid.usUsagePage = 1; rid.usUsage = 2; rid.dwFlags = 0; rid.hwndTarget = h;
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    say("screen %dx%d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    SetTimer(h, 1, seconds * 1000, NULL);
    say("kbm-ready");
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    say("kbm-done");
    return 0;
}

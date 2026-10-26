// sdl_main.cpp - FULL-build front-end: SDL2 on its WinRT backend owns the
// CoreWindow, the event pump and keyboard/gamepad events; our main runs on the UI
// thread inside SDL_WinRTApp::Run (that is how SDL's WinRT port works).
//
// COMPILED WITH WINAPI_FAMILY_APP (the stock UWP family), unlike the rest of the
// host: SDL's headers key off that to define __WINRT__ and to expose
// SDL_WinRTRunApp / the winrt member of SDL_SysWMinfo. This TU therefore must not
// include C++/WinRT, D3D or XAudio2 headers - it talks to the rest of the host
// only through xrl_host.h.
//
// Threading model (this is the whole point of the host):
//   UI thread (here)   : SDL_PollEvent loop + CoreWindow. NOTHING else.
//   gfx thread         : every D3D/DXGI create (gfx_thread.cpp)
//   boot worker thread : quick probe, then xrl_layer_boot() (blocks until guest exits)
#if defined(XRL_FULL)

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_main.h>
#include <SDL_syswm.h>

#include <atomic>
#include <bitset>
#include <cstdio>

#include "xrl_host.h"
#include "input_queue.h"
#include "input_keymap.h"

namespace {

std::atomic<bool> g_worker_done{false};
std::atomic<int>  g_exit_code{0};
std::bitset<512>  g_keys;   // indexed by SDL_Scancode; written/read on the UI thread + any reader (benign race, bool-ish)
SDL_GameController* g_pads[4] = {};
bool g_relative = false;   // SDL relative mouse mode currently on (the guest asked for XRL_MOUSE_RELATIVE)
bool g_released = false;   // Left Ctrl + Left Alt: capture suspended until the next click, so the user can leave the window

void sdl_log_cb(void*, int category, SDL_LogPriority prio, const char* msg) {
    int lvl = prio >= SDL_LOG_PRIORITY_ERROR ? xrl::kError : prio == SDL_LOG_PRIORITY_WARN ? xrl::kWarn : prio >= SDL_LOG_PRIORITY_INFO ? xrl::kInfo : xrl::kTrace;
    xrl::logf(lvl, "sdl", "[cat %d] %s", category, msg);
}

int WatchAppEvents(void*, SDL_Event* e) {
    switch (e->type) {
        case SDL_APP_TERMINATING:          XLOGI("sdl", "APP_TERMINATING"); break;
        case SDL_APP_LOWMEMORY:            XLOGW("sdl", "APP_LOWMEMORY"); break;
        case SDL_APP_WILLENTERBACKGROUND:  XLOGI("sdl", "APP_WILLENTERBACKGROUND (suspend path)"); break;
        case SDL_APP_DIDENTERBACKGROUND:   XLOGI("sdl", "APP_DIDENTERBACKGROUND"); break;
        case SDL_APP_WILLENTERFOREGROUND:  XLOGI("sdl", "APP_WILLENTERFOREGROUND"); break;
        case SDL_APP_DIDENTERFOREGROUND:   XLOGI("sdl", "APP_DIDENTERFOREGROUND (resume path)"); break;
        default: break;
    }
    return 1;
}

void open_pad(int joy_index) {
    if (!SDL_IsGameController(joy_index)) { XLOGI("sdl", "joystick %d is not a game controller", joy_index); return; }
    for (auto& p : g_pads) {
        if (!p) {
            p = SDL_GameControllerOpen(joy_index);
            XLOGI("sdl", "controller %d opened: %s", joy_index, p ? SDL_GameControllerName(p) : SDL_GetError());
            return;
        }
    }
}

// Keyboard and mouse -> the queue behind xrl_input_ops (input_queue.cpp). Keys are positional: SDL scancodes
// are USB HID usages, the queue speaks Linux KEY_* codes. SDL's key repeats arrive as further KEYDOWNs; the
// queue turns them into value-2 repeats. ABS positions are normalized over the aspect-fitted picture once
// the guest has told the host its size (set_content_size), over the whole window before.
void apply_mouse_mode() {
    bool want = xrl_inq_mouse_mode() == XRL_MOUSE_RELATIVE && !g_released;
    if (want != g_relative) { g_relative = want; SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE); }
}

uint16_t mouse_button_code(Uint8 b) {
    switch (b) {
        case SDL_BUTTON_LEFT:   return 0x110;
        case SDL_BUTTON_RIGHT:  return 0x111;
        case SDL_BUTTON_MIDDLE: return 0x112;
        case SDL_BUTTON_X1:     return 0x113;
        case SDL_BUTTON_X2:     return 0x114;
        default:                return 0;
    }
}

void push_text(const char* u8) {   // UTF-8 -> code points
    const unsigned char* p = (const unsigned char*)u8;
    while (*p) {
        unsigned c = *p++, n = 0;
        if (c >= 0xF0) { c &= 7; n = 3; } else if (c >= 0xE0) { c &= 15; n = 2; } else if (c >= 0xC0) { c &= 31; n = 1; }
        while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 63);
        if (c >= 0x20 && c != 0x7f) xrl_inq_push(XRL_IEV_TEXT, 0, (int32_t)c);
    }
}

void handle_input_event(const SDL_Event& e, int win_w, int win_h) {
    switch (e.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: {
            bool down = e.type == SDL_KEYDOWN;
            if (down && (e.key.keysym.scancode == SDL_SCANCODE_LCTRL || e.key.keysym.scancode == SDL_SCANCODE_LALT) && g_relative) {
                const Uint8* ks = SDL_GetKeyboardState(nullptr);
                if (ks[SDL_SCANCODE_LCTRL] && ks[SDL_SCANCODE_LALT]) { g_released = true; apply_mouse_mode(); }
            }
            if (uint16_t k = xrl_hid_to_linux_key((unsigned)e.key.keysym.scancode)) xrl_inq_push(XRL_IEV_KEY, k, down ? 1 : 0);
            break;
        }
        case SDL_TEXTINPUT: push_text(e.text.text); break;
        case SDL_MOUSEMOTION:
            if (g_relative) {
                if (e.motion.xrel) xrl_inq_push(XRL_IEV_REL, 0, e.motion.xrel);
                if (e.motion.yrel) xrl_inq_push(XRL_IEV_REL, 1, e.motion.yrel);
            } else {
                int32_t ax, ay;
                xrl_inq_abs_from_client(e.motion.x, e.motion.y, win_w, win_h, &ax, &ay);
                xrl_inq_push(XRL_IEV_ABS, 0, ax);
                xrl_inq_push(XRL_IEV_ABS, 1, ay);
            }
            break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            if (e.type == SDL_MOUSEBUTTONDOWN && g_released) { g_released = false; apply_mouse_mode(); }
            if (uint16_t b = mouse_button_code(e.button.button)) xrl_inq_push(XRL_IEV_KEY, b, e.type == SDL_MOUSEBUTTONDOWN ? 1 : 0);
            break;
        case SDL_MOUSEWHEEL: {
            int dir = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            if (e.wheel.y) xrl_inq_push(XRL_IEV_REL, 8, e.wheel.y * dir);
            if (e.wheel.x) xrl_inq_push(XRL_IEV_REL, 6, e.wheel.x * dir);
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { xrl_inq_focus_lost(); g_released = false; apply_mouse_mode(); }
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) xrl_inq_push(XRL_IEV_FOCUS, 0, 1);
            break;
        default: break;
    }
}

int sdl_main(int, char**) {
    XLOGI("sdl", "SDL main on UI thread 0x%lx", (unsigned long)GetCurrentThreadId());
    SDL_LogSetOutputFunction(sdl_log_cb, nullptr);
    SDL_version v; SDL_GetVersion(&v);
    XLOGI("sdl", "SDL runtime %d.%d.%d (compiled %d.%d.%d), revision %s", v.major, v.minor, v.patch,
          SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetRevision());

    // Audio is intentionally NOT initialised: the guest's XAudio2 calls go to native
    // XAudio2 via xrl_audio_ops, which owns the mastering voice (notes-A.md).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // B on an Xbox controller is also the system's "back": unless the app marks the
    // BackRequested event handled, the shell navigates away and suspends the app (a game
    // closing its own settings menu with B left the app). The game gets B from the pad.
    SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        XLOGE("sdl", "SDL_Init failed: %s", SDL_GetError());
        xrl::host_exit();
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("xbox-rl", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080, SDL_WINDOW_SHOWN);
    if (!win) {
        XLOGE("sdl", "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        xrl::host_exit();
        return 1;
    }
    xrl::WindowInfo wi;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(win, &wm) && wm.subsystem == SDL_SYSWM_WINRT) {
        wi.core_window = wm.info.winrt.window;   // IInspectable* of the CoreWindow
    } else {
        XLOGE("sdl", "SDL_GetWindowWMInfo failed/not WinRT: %s", SDL_GetError());
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);   // physical pixels on the WinRT backend
    wi.width = (uint32_t)w; wi.height = (uint32_t)h; wi.valid = wi.core_window != nullptr;
    XLOGI("sdl", "window created: CoreWindow=%p size %dx%d px, driver '%s'", wi.core_window, w, h, SDL_GetCurrentVideoDriver());
    xrl::window_info_set(wi);
    xrl::display_select();   // the console's output mode (before the boot thread reads it)

    SDL_AddEventWatch(WatchAppEvents, nullptr);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) open_pad(i);

    xrl::gfx_thread_start();

    void* worker = xrl::start_thread("boot", 256u << 20, [] {
        const auto& cfg = xrl::config();
        if (cfg.probe_mode != "none") {
            xrl::ProbeOptions opt = cfg.probe_mode == "all" ? xrl::ProbeOptions::all() : xrl::ProbeOptions::quick();
            xrl::probe_run(opt);
        }
        if (cfg.skip_layer) {
            XLOGI("host", "skip_layer=1: not booting the layer; staying up (close the app to exit)");
            return;
        }
        g_exit_code = xrl::boot_layer();
        XLOGI("host", "guest/layer finished, exit code %d", g_exit_code.load());
        g_worker_done = true;
    });
    if (!worker) { XLOGE("sdl", "could not start boot thread"); g_worker_done = true; }

    bool quit = false;
    while (!quit && !g_worker_done.load()) {
        SDL_Event e;
        apply_mouse_mode();   // the guest may have switched between the absolute pointer and a captured mouse
        while (SDL_WaitEventTimeout(&e, 10)) {
            switch (e.type) {
                case SDL_QUIT:
                    XLOGI("sdl", "SDL_QUIT -> requesting layer exit");
                    xrl::request_layer_exit(0);
                    quit = true;
                    break;
                case SDL_KEYDOWN: case SDL_KEYUP:
                    if (e.key.keysym.scancode < 512) g_keys[e.key.keysym.scancode] = (e.type == SDL_KEYDOWN);
                    handle_input_event(e, (int)wi.width, (int)wi.height);
                    break;
                case SDL_TEXTINPUT: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
                    handle_input_event(e, (int)wi.width, (int)wi.height);
                    break;
                case SDL_CONTROLLERDEVICEADDED:  open_pad(e.cdevice.which); break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    for (auto& p : g_pads)
                        if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == e.cdevice.which) { SDL_GameControllerClose(p); p = nullptr; XLOGI("sdl", "controller removed"); }
                    break;
                case SDL_WINDOWEVENT:
                    handle_input_event(e, (int)wi.width, (int)wi.height);
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        wi.width = (uint32_t)e.window.data1; wi.height = (uint32_t)e.window.data2;
                        xrl::window_info_set(wi);
                        XLOGI("sdl", "window size now %dx%d", e.window.data1, e.window.data2);
                    }
                    break;
                default: break;
            }
        }
    }
    XLOGI("sdl", "UI loop ended (quit=%d worker_done=%d)", (int)quit, (int)g_worker_done.load());
    SDL_DelEventWatch(WatchAppEvents, nullptr);
    SDL_DestroyWindow(win);
    SDL_Quit();
    xrl::host_exit();
    return g_exit_code.load();
}

}  // namespace

extern "C" int xrl_host_key_down(int sc) { return (sc >= 0 && sc < 512) ? (int)g_keys[sc] : 0; }

int xrl_sdl_entry() {
    return SDL_WinRTRunApp(sdl_main, nullptr);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    xrl::host_early_init("full");
    return xrl_sdl_entry();
}

#endif  // XRL_FULL

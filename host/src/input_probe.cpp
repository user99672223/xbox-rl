// input_probe.cpp - ask the console itself which mouse input a Dev Mode app can get (the USB mouse gave SDL nothing).
// Startup (background thread): the packages Edge / Dev Home come from and the capabilities their manifests declare
// (packageQuery), every mouse device the system enumerates, HID access to mice (humaninterfacedevice usage 1/2),
// GameInput (the GDK input API console games read the mouse with), Win32 raw input. Then, while the user moves the
// mouse, the events each source delivers: CoreWindow pointer, MouseDevice raw moves, HID input reports, GameInput
// mouse readings, raw input, plus SDL's own counts; every 5 s while they change.
#include "pch.h"
#include "xrl_host.h"
#include "input_queue.h"
#include "xrl_abi.h"
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Input.h>
#include <CoreWindow.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<long> g_ptr_mouse, g_ptr_other, g_press, g_wheel, g_raw, g_keys, g_chars, g_hid, g_gi, g_rawinput;
winrt::Windows::Devices::Input::MouseDevice g_md{nullptr};
std::vector<winrt::Windows::Devices::HumanInterfaceDevice::HidDevice> g_hids;
HWND g_hwnd;

// ---- GameInput, declared by hand (no GDK header here): only the slots used, each HRESULT logged
enum : uint32_t { GI_KIND_MOUSE = 0x00040000, GI_KIND_KEYBOARD = 0x00020000 };
struct GIMouseState { uint32_t buttons; int64_t x, y, wheel_x, wheel_y; };   // GameInputMouseState (v0)
struct IGIReading : IUnknown {   // IGameInputReading (v0) in vtable order, up to GetMouseState
    virtual uint32_t STDMETHODCALLTYPE GetInputKind() = 0;
    virtual uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t kind) = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
    virtual void STDMETHODCALLTYPE GetDevice(void** device) = 0;
    virtual bool STDMETHODCALLTYPE GetRawReport(void** report) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerAxisCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t n, float* s) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerButtonCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t n, bool* s) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t n, void* s) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetKeyCount() = 0;
    virtual uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t n, void* s) = 0;
    virtual bool STDMETHODCALLTYPE GetMouseState(GIMouseState* s) = 0;
};
GIMouseState g_gi_last;
bool g_gi_have;
long g_gi_logged;
struct IGI : IUnknown {          // IGameInput: GetCurrentTimestamp, GetCurrentReading, ...
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t kind, IUnknown* device, IGIReading** reading) = 0;
};
IGI* g_gameinput;
uint64_t g_gi_last_ts;

// LoadLibraryW / GetProcAddress for system DLLs the app partition does not declare
void* sysproc(const wchar_t* dll, const char* fn) {
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    auto ll = k ? (HMODULE(WINAPI*)(LPCWSTR))GetProcAddress(k, "LoadLibraryW") : nullptr;
    HMODULE m = GetModuleHandleW(dll);
    if (!m && ll) m = ll(dll);
    if (!m) return nullptr;
    return (void*)GetProcAddress(m, fn);
}

std::string narrow(winrt::hstring const& h) { return winrt::to_string(h); }

void probe_packages() {
    using namespace winrt::Windows::Management::Deployment;
    try {
        PackageManager pm;
        int n = 0, shown = 0;
        for (auto const& p : pm.FindPackagesForUser(L"")) {
            n++;
            std::string name = narrow(p.Id().Name());
            std::string low = name; for (auto& c : low) c = (char)tolower(c);
            bool want = low.find("edge") != std::string::npos || low.find("devhome") != std::string::npos ||
                        low.find("devmode") != std::string::npos || low.find("dev.home") != std::string::npos ||
                        low.find("webview") != std::string::npos || low.find("input") != std::string::npos;
            if (!want || shown >= 12) continue;
            shown++;
            std::string path;
            try { path = narrow(p.InstalledLocation().Path()); } catch (...) { path = "?"; }
            XLOGI("input", "probe: package %s (%s) at %s", name.c_str(), narrow(p.Id().FullName()).c_str(), path.c_str());
            // its manifest: every <...Capability ...> line
            std::wstring mf = winrt::to_hstring(path).c_str(); mf += L"\\AppxManifest.xml";
            HANDLE f = CreateFile2(mf.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            if (f == INVALID_HANDLE_VALUE) { XLOGI("input", "probe:   manifest not readable (error %lu)", GetLastError()); continue; }
            std::string text; char buf[65536]; DWORD got;
            while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
            CloseHandle(f);
            size_t pos = 0; int caps = 0;
            while ((pos = text.find("Capability", pos)) != std::string::npos) {
                size_t a = text.rfind('<', pos), b = text.find('>', pos);
                if (a != std::string::npos && b != std::string::npos && caps < 60) {
                    XLOGI("input", "probe:   %s", text.substr(a, b - a + 1).c_str());
                    caps++;
                }
                pos = b == std::string::npos ? text.size() : b;
            }
            if (!caps) XLOGI("input", "probe:   manifest read (%zu bytes), no capability lines", text.size());
        }
        XLOGI("input", "probe: %d packages for this user", n);
    } catch (winrt::hresult_error const& e) {
        XLOGW("input", "probe: package query failed: 0x%08X %s", (unsigned)e.code(), narrow(e.message()).c_str());
    }
}

void probe_devices() {
    using namespace winrt::Windows::Devices::Enumeration;
    using namespace winrt::Windows::Devices::HumanInterfaceDevice;
    try {   // every enabled device interface whose name says mouse/HID/pointer
        int n = 0, shown = 0;
        for (auto const& d : DeviceInformation::FindAllAsync().get()) {
            n++;
            std::string nm = narrow(d.Name()), id = narrow(d.Id());
            std::string low = nm + " " + id; for (auto& c : low) c = (char)tolower(c);
            if ((low.find("mouse") != std::string::npos || low.find("hid") != std::string::npos ||
                 low.find("pointer") != std::string::npos || low.find("keyboard") != std::string::npos) && shown < 40) {
                shown++;
                XLOGI("input", "probe: device '%s' %s enabled=%d", nm.c_str(), id.c_str(), d.IsEnabled() ? 1 : 0);
            }
        }
        XLOGI("input", "probe: %d device interfaces in all", n);
    } catch (winrt::hresult_error const& e) {
        XLOGW("input", "probe: device enumeration failed: 0x%08X", (unsigned)e.code());
    }
    for (uint16_t usage : {2, 1}) {   // HID mice (1/2), then pointers (1/1)
        try {
            auto sel = HidDevice::GetDeviceSelector(1, usage);
            auto list = DeviceInformation::FindAllAsync(sel).get();
            XLOGI("input", "probe: HID usage 1/%u: %u device(s)", usage, list.Size());
            for (auto const& d : list) {
                try {
                    HidDevice h = HidDevice::FromIdAsync(d.Id(), winrt::Windows::Storage::FileAccessMode::Read).get();
                    if (!h) { XLOGI("input", "probe:   %s: FromIdAsync gave null (access refused)", narrow(d.Id()).c_str()); continue; }
                    h.InputReportReceived([](auto&&, auto&&) { g_hid++; });
                    g_hids.push_back(h);
                    XLOGI("input", "probe:   %s: OPENED, vid %04x pid %04x", narrow(d.Id()).c_str(), h.VendorId(), h.ProductId());
                } catch (winrt::hresult_error const& e) {
                    XLOGI("input", "probe:   %s: FromIdAsync 0x%08X", narrow(d.Id()).c_str(), (unsigned)e.code());
                }
            }
        } catch (winrt::hresult_error const& e) {
            XLOGW("input", "probe: HID 1/%u failed: 0x%08X", usage, (unsigned)e.code());
        }
    }
    try {
        auto raws = winrt::Windows::Gaming::Input::RawGameController::RawGameControllers();
        XLOGI("input", "probe: Windows.Gaming.Input raw controllers: %u", raws.Size());
        for (auto const& r : raws) XLOGI("input", "probe:   raw controller vid %04x pid %04x, %d buttons %d axes", r.HardwareVendorId(), r.HardwareProductId(), r.ButtonCount(), r.AxisCount());
    } catch (winrt::hresult_error const& e) {
        XLOGW("input", "probe: RawGameControllers 0x%08X", (unsigned)e.code());
    }
}

void probe_gameinput() {
    for (const wchar_t* dll : {L"GameInput.dll", L"gameinput.dll", L"GameInputRedist.dll"}) {
        auto create = (HRESULT(WINAPI*)(IGI**))sysproc(dll, "GameInputCreate");
        if (!create) { XLOGI("input", "probe: GameInput: %ls / GameInputCreate not found (error %lu)", dll, GetLastError()); continue; }
        HRESULT hr = create(&g_gameinput);
        XLOGI("input", "probe: GameInput: %ls GameInputCreate -> 0x%08X", dll, (unsigned)hr);
        if (SUCCEEDED(hr) && g_gameinput) {
            IGIReading* r = nullptr;
            HRESULT h2 = g_gameinput->GetCurrentReading(GI_KIND_MOUSE, nullptr, &r);
            XLOGI("input", "probe: GameInput: current mouse reading -> 0x%08X%s", (unsigned)h2, r ? " (a reading)" : "");
            if (r) { g_gi_last_ts = r->GetTimestamp(); r->Release(); }
            IGIReading* k = nullptr;
            h2 = g_gameinput->GetCurrentReading(GI_KIND_KEYBOARD, nullptr, &k);
            XLOGI("input", "probe: GameInput: current keyboard reading -> 0x%08X%s", (unsigned)h2, k ? " (a reading)" : "");
            if (k) k->Release();
            return;
        }
    }
}

void probe_rawinput() {   // Win32 raw input on the CoreWindow's HWND (UI thread)
    typedef struct { USHORT page, usage; DWORD flags; HWND target; } RID;
    typedef struct { HANDLE dev; DWORD type; } RIDL;
    auto list = (UINT(WINAPI*)(RIDL*, PUINT, UINT))sysproc(L"user32.dll", "GetRawInputDeviceList");
    auto reg = (BOOL(WINAPI*)(const RID*, UINT, UINT))sysproc(L"user32.dll", "RegisterRawInputDevices");
    if (!list || !reg) { XLOGI("input", "probe: raw input: user32 functions not available"); return; }
    UINT n = 0;
    UINT r = list(nullptr, &n, sizeof(RIDL));
    std::vector<RIDL> v(n ? n : 1);
    if (n) r = list(v.data(), &n, sizeof(RIDL));
    int mice = 0, kbd = 0, hid = 0;
    for (UINT i = 0; i < n && r != (UINT)-1; i++) { if (v[i].type == 0) mice++; else if (v[i].type == 1) kbd++; else hid++; }
    XLOGI("input", "probe: raw input devices: %u (%d mice, %d keyboards, %d other HID), list -> %d err %lu", n, mice, kbd, hid, (int)r, GetLastError());
    RID rid = {1, 2, 0x00000100 /* RIDEV_INPUTSINK */, g_hwnd};
    BOOL ok = g_hwnd ? reg(&rid, 1, sizeof rid) : FALSE;
    XLOGI("input", "probe: raw input: RegisterRawInputDevices(mouse, hwnd %p) -> %d err %lu", g_hwnd, ok, GetLastError());
}
}  // namespace

void xrl_input_probe_install() {
    using namespace winrt::Windows::UI::Core;
    using namespace winrt::Windows::Devices::Input;
    try {
        CoreWindow cw = CoreWindow::GetForCurrentThread();
        if (cw) {
            cw.PointerMoved([](auto&&, PointerEventArgs const& a) {
                if (a.CurrentPoint().PointerDevice().PointerDeviceType() == PointerDeviceType::Mouse) g_ptr_mouse++; else g_ptr_other++;
            });
            cw.PointerPressed([](auto&&, auto&&) { g_press++; });
            cw.PointerWheelChanged([](auto&&, auto&&) { g_wheel++; });
            cw.KeyDown([](auto&&, auto&&) { g_keys++; });
            cw.CharacterReceived([](auto&&, auto&&) { g_chars++; });
            if (auto interop = cw.try_as<ICoreWindowInterop>()) interop->get_WindowHandle(&g_hwnd);
            XLOGI("input", "probe: CoreWindow handlers installed, hwnd %p", g_hwnd);
        }
    } catch (winrt::hresult_error const& e) {
        XLOGW("input", "probe: CoreWindow handlers failed: 0x%08X", (unsigned)e.code());
    }
    try {
        g_md = MouseDevice::GetForCurrentView();
        g_md.MouseMoved([](auto&&, MouseEventArgs const&) { g_raw++; });
        auto caps = MouseCapabilities();
        XLOGI("input", "probe: MouseCapabilities: present %d, buttons %u, vertical wheel %d", caps.MousePresent(), caps.NumberOfButtons(), caps.VerticalWheelPresent());
        auto kc = KeyboardCapabilities();
        XLOGI("input", "probe: KeyboardCapabilities: present %d", kc.KeyboardPresent());
    } catch (winrt::hresult_error const& e) {
        XLOGW("input", "probe: MouseDevice failed: 0x%08X", (unsigned)e.code());
    }
    probe_rawinput();
    std::thread([] {
        winrt::init_apartment();
        probe_packages();
        probe_devices();
        probe_gameinput();
        XLOGI("input", "probe: startup checks done: now move the USB mouse, click, scroll");
    }).detach();
}

// From the SDL loop (UI thread): drains raw input, polls GameInput, logs all counts every 5 s while anything changed.
void xrl_input_probe_tick(long sdl_mouse, long sdl_keys) {
    typedef struct { DWORD type, size; HANDLE dev; WPARAM w; } RIH;
    static auto rbuf = (UINT(WINAPI*)(void*, PUINT, UINT))sysproc(L"user32.dll", "GetRawInputBuffer");
    if (rbuf) {
        alignas(8) static unsigned char b[16384];
        UINT sz = sizeof b;
        UINT n = rbuf(b, &sz, sizeof(RIH));
        if (n != (UINT)-1 && n) g_rawinput += (long)n;
    }
    if (g_gameinput) {
        IGIReading* r = nullptr;
        if (SUCCEEDED(g_gameinput->GetCurrentReading(GI_KIND_MOUSE, nullptr, &r)) && r) {
            uint64_t ts = r->GetTimestamp();
            if (ts != g_gi_last_ts) {
                g_gi++; g_gi_last_ts = ts;
                GIMouseState m{};
                if (r->GetMouseState(&m)) {
                    if (g_gi_logged < 40) { g_gi_logged++; XLOGI("input", "probe: GameInput mouse: buttons %x x %lld y %lld wheel %lld/%lld", m.buttons, m.x, m.y, m.wheel_x, m.wheel_y); }
                    if (g_gi_have) {   // the deltas into the guest's mouse (REL, wheel, BTN_LEFT/RIGHT/MIDDLE)
                        long long dx = m.x - g_gi_last.x, dy = m.y - g_gi_last.y, dw = m.wheel_y - g_gi_last.wheel_y;
                        if (dx > -4096 && dx < 4096 && dx) xrl_inq_push(XRL_IEV_REL, 0, (int32_t)dx);
                        if (dy > -4096 && dy < 4096 && dy) xrl_inq_push(XRL_IEV_REL, 1, (int32_t)dy);
                        if (dw > -4096 && dw < 4096 && dw) xrl_inq_push(XRL_IEV_REL, 8, (int32_t)(dw > 0 ? 1 : -1));
                        uint32_t ch = m.buttons ^ g_gi_last.buttons;
                        static const uint16_t code[3] = {0x110, 0x111, 0x112};   // GameInputMouseLeft/Right/Middle bits 0..2
                        for (int b = 0; b < 3; b++) if (ch & (1u << b)) xrl_inq_push(XRL_IEV_KEY, code[b], (m.buttons >> b) & 1);
                    }
                    g_gi_last = m; g_gi_have = true;
                }
            }
            r->Release();
        }
    }
    static ULONGLONG last;
    static long prev[12];
    ULONGLONG now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    long cur[12] = { g_ptr_mouse, g_ptr_other, g_press, g_wheel, g_raw, g_hid, g_gi, g_rawinput, g_keys, g_chars, sdl_mouse, sdl_keys };
    if (!memcmp(cur, prev, sizeof cur)) return;
    memcpy(prev, cur, sizeof cur);
    XLOGI("input", "probe: mouse sources: CoreWindow pointer %ld (other %ld) presses %ld wheel %ld; MouseDevice %ld; HID reports %ld; "
          "GameInput mouse readings %ld; raw input %ld | keyboard: CoreWindow keys %ld chars %ld | SDL mouse %ld keys %ld",
          cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8], cur[9], cur[10], cur[11]);
}

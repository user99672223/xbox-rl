// input_ops.cpp - xrl_input_ops over native Windows.Gaming.Input (decisions.md
// cat N: Gamepad activation factory OK on the console).
//
// Slots 0..3 are assigned in arrival order and are STABLE across a pad's
// lifetime (a pad that disconnects frees its slot; a later pad takes the first
// free one) - the XInput behaviour the Wine xinput side expects.
// get_pad_state() returns the XINPUT-compatible mask documented in xrl_abi.h.
//
// Keyboard/mouse (ABI v3): the event queue in input_queue.cpp; the front-end that owns the window
// (sdl_main.cpp, or the test host's Win32 window) pushes into it, the slots below hand it to the guest.
#include "pch.h"
#include "xrl_host.h"
#include "input_queue.h"

#include <winrt/Windows.Gaming.Input.h>
#include <mutex>

namespace WGI = winrt::Windows::Gaming::Input;

namespace {

constexpr int kSlots = 4;

struct Pads {
    std::mutex m;
    WGI::Gamepad slot[kSlots]{nullptr, nullptr, nullptr, nullptr};
    winrt::event_token added{}, removed{};
    bool inited = false;

    void add_locked(const WGI::Gamepad& p) {
        for (int i = 0; i < kSlots; ++i) if (slot[i] && slot[i] == p) return;   // already known
        for (int i = 0; i < kSlots; ++i) {
            if (!slot[i]) { slot[i] = p; XLOGI("input", "gamepad connected -> slot %d", i); return; }
        }
        XLOGW("input", "gamepad connected but all %d slots are full", kSlots);
    }
    void remove_locked(const WGI::Gamepad& p) {
        for (int i = 0; i < kSlots; ++i) {
            if (slot[i] && slot[i] == p) { slot[i] = nullptr; XLOGI("input", "gamepad disconnected <- slot %d", i); return; }
        }
    }
};
Pads* P() { static Pads* p = new Pads(); return p; }

void ensure_init() {
    Pads* p = P();
    std::lock_guard<std::mutex> lk(p->m);
    if (p->inited) return;
    p->inited = true;
    try {
        p->added = WGI::Gamepad::GamepadAdded([p](winrt::Windows::Foundation::IInspectable const&, WGI::Gamepad const& g) {
            std::lock_guard<std::mutex> l(p->m); p->add_locked(g);
        });
        p->removed = WGI::Gamepad::GamepadRemoved([p](winrt::Windows::Foundation::IInspectable const&, WGI::Gamepad const& g) {
            std::lock_guard<std::mutex> l(p->m); p->remove_locked(g);
        });
        for (auto const& g : WGI::Gamepad::Gamepads()) p->add_locked(g);
        XLOGI("input", "Windows.Gaming.Input ready (%u gamepad(s) present at init)", (unsigned)WGI::Gamepad::Gamepads().Size());
    } catch (const winrt::hresult_error& e) {
        XLOGE("input", "Windows.Gaming.Input init failed 0x%08x", (unsigned)e.code().value);
    }
}

uint32_t map_buttons(WGI::GamepadButtons b) {
    using GB = WGI::GamepadButtons;
    auto has = [&](GB f) { return (static_cast<uint32_t>(b) & static_cast<uint32_t>(f)) != 0; };
    uint32_t m = 0;
    if (has(GB::DPadUp))          m |= 0x0001;
    if (has(GB::DPadDown))        m |= 0x0002;
    if (has(GB::DPadLeft))        m |= 0x0004;
    if (has(GB::DPadRight))       m |= 0x0008;
    if (has(GB::Menu))            m |= 0x0010;   // XINPUT_GAMEPAD_START
    if (has(GB::View))            m |= 0x0020;   // XINPUT_GAMEPAD_BACK
    if (has(GB::LeftThumbstick))  m |= 0x0040;
    if (has(GB::RightThumbstick)) m |= 0x0080;
    if (has(GB::LeftShoulder))    m |= 0x0100;
    if (has(GB::RightShoulder))   m |= 0x0200;
    if (has(GB::A))               m |= 0x1000;
    if (has(GB::B))               m |= 0x2000;
    if (has(GB::X))               m |= 0x4000;
    if (has(GB::Y))               m |= 0x8000;
    return m;
}

int16_t axis16(double v) {
    if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
    return (int16_t)(v >= 0 ? v * 32767.0 : v * 32768.0);
}
uint8_t trig8(double v) {
    if (v > 1.0) v = 1.0; else if (v < 0.0) v = 0.0;
    return (uint8_t)(v * 255.0 + 0.5);
}

xrl_status op_get_pad_state(uint32_t slot, xrl_pad_state* out) {
    if (!out || slot >= kSlots) return XRL_ERR_GENERIC;
    memset(out, 0, sizeof *out);
    ensure_init();
    WGI::Gamepad g{nullptr};
    { Pads* p = P(); std::lock_guard<std::mutex> lk(p->m); g = p->slot[slot]; }
    if (!g) return XRL_ERR_NOTAVAIL;
    try {
        WGI::GamepadReading r = g.GetCurrentReading();
        out->buttons = map_buttons(r.Buttons);
        out->left_trigger = trig8(r.LeftTrigger);
        out->right_trigger = trig8(r.RightTrigger);
        out->thumb_lx = axis16(r.LeftThumbstickX);
        out->thumb_ly = axis16(r.LeftThumbstickY);
        out->thumb_rx = axis16(r.RightThumbstickX);
        out->thumb_ry = axis16(r.RightThumbstickY);
        out->connected = 1;
    } catch (const winrt::hresult_error&) {
        return XRL_ERR_NOTAVAIL;   // raced with a disconnect
    }
    return XRL_OK;
}

xrl_status op_set_vibration(uint32_t slot, uint16_t left, uint16_t right) {
    if (slot >= kSlots) return XRL_ERR_GENERIC;
    ensure_init();
    WGI::Gamepad g{nullptr};
    { Pads* p = P(); std::lock_guard<std::mutex> lk(p->m); g = p->slot[slot]; }
    if (!g) return XRL_ERR_NOTAVAIL;
    try {
        WGI::GamepadVibration v{};
        v.LeftMotor = left / 65535.0;
        v.RightMotor = right / 65535.0;
        g.Vibration(v);
    } catch (const winrt::hresult_error&) {
        return XRL_ERR_NOTAVAIL;
    }
    return XRL_OK;
}

xrl_input_ops g_ops = { sizeof(xrl_input_ops), op_get_pad_state, op_set_vibration,
                        xrl_inq_read_events, xrl_inq_wait_events, xrl_inq_set_mouse_mode, xrl_inq_set_content_size };

}  // namespace

extern "C" const xrl_input_ops* xrl_host_input_ops(void) {
    ensure_init();
    return &g_ops;
}

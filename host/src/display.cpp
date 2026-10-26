// display.cpp - the console's video output mode, and what Wine is told about it.
//
// A UWP app on the Xbox gets the console's current HDMI mode (1080p60 by default) unless it asks
// for another one: HdmiDisplayInformation lists what the TV/monitor and the console's video
// settings allow, RequestSetCurrentDisplayModeAsync switches. display_select() picks one
// (launch.cfg display=, default auto: the largest resolution up to 2560x1440, then the highest
// refresh up to 120 Hz, SDR) and the guest gets it as XRL_DISPLAY=<w>x<h>@<hz> (boot.cpp), which
// win32u reports as the physical display (patches/wine/0025): the game's resolution list then
// goes up to it. The game's back buffer is presented at its own size with DXGI_SCALING_STRETCH,
// so a back buffer the size of the output mode is shown 1:1.
#include "pch.h"
#include "xrl_host.h"

#include <cmath>
#include <cstdio>
#include <winrt/Windows.Graphics.Display.Core.h>

namespace winrt {
    using namespace Windows::Graphics::Display::Core;
}

namespace xrl {
namespace {

std::mutex  g_mu;
DisplayMode g_mode;

const char* space_name(winrt::HdmiDisplayColorSpace c) {
    switch (c) {
        case winrt::HdmiDisplayColorSpace::RgbLimited: return "rgb-limited";
        case winrt::HdmiDisplayColorSpace::RgbFull:    return "rgb-full";
        case winrt::HdmiDisplayColorSpace::BT2020:     return "bt2020";
        case winrt::HdmiDisplayColorSpace::BT709:      return "bt709";
        default:                                       return "?";
    }
}

void describe(const char* what, const winrt::HdmiDisplayMode& m) {
    XLOGI("display", "%s %ux%u @ %.3f Hz, %u bpp, %s, sdr %d, hdr10 %d, stereo %d", what,
          m.ResolutionWidthInRawPixels(), m.ResolutionHeightInRawPixels(), m.RefreshRate(), m.BitsPerPixel(),
          space_name(m.ColorSpace()), (int)m.IsSdrLuminanceSupported(), (int)m.IsSmpte2084Supported(), (int)m.StereoEnabled());
}

// "<w>x<h>[@<hz>]"; false for "auto" and anything unparsable
bool parse_mode(const std::string& s, uint32_t& w, uint32_t& h, double& hz) {
    unsigned a = 0, b = 0; double r = 0;
    int n = sscanf_s(s.c_str(), "%ux%u@%lf", &a, &b, &r);
    if (n < 2 || !a || !b) return false;
    w = a; h = b; hz = n == 3 ? r : 0;
    return true;
}

}  // namespace

void display_select() {
    const std::string want = config().display.empty() ? "auto" : config().display;
    uint32_t ww = 0, wh = 0; double whz = 0;
    const bool exact = parse_mode(want, ww, wh, whz);
    DisplayMode chosen;
    if (want != "off") {
        try {
            auto hdmi = winrt::HdmiDisplayInformation::GetForCurrentView();
            if (!hdmi) {
                XLOGI("display", "no HDMI display information (not a console): keeping the window's size");
            } else {
                auto cur = hdmi.GetCurrentDisplayMode();
                describe("current", cur);
                winrt::HdmiDisplayMode best{nullptr};
                auto better = [&](const winrt::HdmiDisplayMode& m) {
                    if (!best) return true;
                    uint32_t mw = m.ResolutionWidthInRawPixels(), mh = m.ResolutionHeightInRawPixels();
                    uint32_t bw = best.ResolutionWidthInRawPixels(), bh = best.ResolutionHeightInRawPixels();
                    double mr = m.RefreshRate(), br = best.RefreshRate();
                    if (exact) {   // the asked size; then the refresh closest to the asked one (or the highest)
                        double tr = whz > 0 ? whz : 1000.0;
                        if (std::fabs(mr - tr) != std::fabs(br - tr)) return std::fabs(mr - tr) < std::fabs(br - tr);
                        return m.BitsPerPixel() > best.BitsPerPixel();
                    }
                    if ((uint64_t)mw * mh != (uint64_t)bw * bh) return (uint64_t)mw * mh > (uint64_t)bw * bh;
                    if (mr != br) return mr > br;
                    return m.BitsPerPixel() > best.BitsPerPixel();
                };
                auto modes = hdmi.GetSupportedDisplayModes();
                XLOGI("display", "%u supported modes:", modes.Size());
                for (const auto& m : modes) {
                    describe("  mode", m);
                    // a mode that does not claim HDR is SDR: the console reports IsSdrLuminanceSupported() false
                    // for both its modes (sdr 0), so that flag cannot filter; only BT.2020 and stereo do
                    if (m.StereoEnabled()) continue;
                    if (m.ColorSpace() == winrt::HdmiDisplayColorSpace::BT2020) continue;
                    uint32_t mw = m.ResolutionWidthInRawPixels(), mh = m.ResolutionHeightInRawPixels();
                    if (exact ? (mw != ww || mh != wh) : (mh > 1440 || m.RefreshRate() > 120.5)) continue;
                    if (better(m)) best = m;
                }
                if (!best) {
                    XLOGW("display", "no supported mode matches display=%s: keeping the current one", want.c_str());
                    best = cur;
                }
                describe("chosen", best);
                chosen = { best.ResolutionWidthInRawPixels(), best.ResolutionHeightInRawPixels(),
                           (uint32_t)std::lround(best.RefreshRate()) };
                if (!best.IsEqual(cur)) {
                    auto op = hdmi.RequestSetCurrentDisplayModeAsync(best, winrt::HdmiDisplayHdrOption::None);
                    op.Completed([](auto const& o, winrt::Windows::Foundation::AsyncStatus st) {
                        bool ok = false;
                        try { ok = st == winrt::Windows::Foundation::AsyncStatus::Completed && o.GetResults(); } catch (...) {}
                        XLOGI("display", "display mode switch %s (status %d)", ok ? "done" : "FAILED", (int)st);
                    });
                    XLOGI("display", "display mode switch requested");
                }
            }
        } catch (const winrt::hresult_error& e) {
            XLOGW("display", "HDMI display mode query/switch failed: 0x%08X %s", (unsigned)e.code(),
                  winrt::to_string(e.message()).c_str());
        }
    }
    if (!chosen.width) {
        auto wi = window_info_get();
        chosen = { wi.width ? wi.width : 1920, wi.height ? wi.height : 1080, 60 };
    }
    XLOGI("display", "guest display %ux%u @ %u Hz", chosen.width, chosen.height, chosen.refresh_hz);
    std::lock_guard<std::mutex> l(g_mu);
    g_mode = chosen;
}

DisplayMode display_mode() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_mode;
}

}  // namespace xrl

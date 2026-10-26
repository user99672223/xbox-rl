// desktop_probe_main.cpp - runs the host's probe + boot-context path as a plain
// desktop exe on the dev laptop, WITHOUT packaging/registering a UWP app (which
// needs Windows Developer Mode). Everything except the CoreWindow swapchain step is
// the same code the UWP host runs: log, config, gfx thread, default gfx ops
// (D3D12 + D3D11On12), XAudio2 ops, Gaming.Input ops, RWX, 4 GiB, VA map, boot
// context -> stub layer.
//
// Build + run:   pwsh tools/test-desktop.ps1
// Log lands in %TEMP%\log.txt (no package identity => LocalState falls back to TEMP).
#include "pch.h"
#include "xrl_host.h"

#include <cstdio>

namespace xrl {
void host_early_init(const char* mode_name) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    log_init();
    XLOGI("host", "desktop test harness, mode=%s", mode_name);
    load_config();
}
void host_exit() { XLOGI("host", "exit"); log_shutdown(); }
}  // namespace xrl

int main() {
    xrl::host_early_init("desktop-test");
    xrl::gfx_thread_start();

    xrl::ProbeOptions opt = xrl::ProbeOptions::all();
    opt.swapchain = false;   // needs a CoreWindow; only meaningful inside the UWP app
    bool ok = xrl::probe_run(opt);

    // Exercise the boot-context builder + stub layer (expected to return -1).
    int rc = xrl::boot_layer();
    printf("probe required steps: %s; boot_layer() -> %d (stub layer returns -1)\n", ok ? "PASS" : "FAIL", rc);
    printf("log: %s\\log.txt\n", xrl::local_state_utf8().c_str());
    xrl::host_exit();
    return ok ? 0 : 1;
}

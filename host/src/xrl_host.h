// xrl_host.h - host-internal API shared by the host's translation units.
//
// NOT part of the cross-workstream ABI (that is include/xrl_abi.h). Everything
// here is private to host/ except the "gfx seam" section, which is the contract
// Workstream D plugs into (see also gfx_seam.h).
//
// Deliberately free of WinRT / D3D / SDL types so it can be included from the
// SDL translation unit (compiled with WINAPI_FAMILY_APP) and from the C++/WinRT
// translation units (compiled with WINAPI_FAMILY_DESKTOP_APP) alike.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

#include "xrl_abi.h"

// ---------------------------------------------------------------------------
// Logging  (log.cpp)
// ---------------------------------------------------------------------------
// Everything - host, layer, wine - logs through xrl_host_log (an xrl_log_fn)
// into LocalState/log.txt. Lines are written with one WriteFile each (no
// user-space buffering); warn/error lines are additionally FlushFileBuffers'd.
namespace xrl {

enum LogLevel { kTrace = 0, kInfo = 1, kWarn = 2, kError = 3 };

// Opens LocalState/log.txt (rotating any previous one to log.prev.txt). Safe to
// call more than once. Falls back to %TEMP% if there is no package identity.
bool log_init();
void log_shutdown();
// Writes a line. printf-style. `tag` is a short subsystem name ("host","probe").
void logf(int level, const char* tag, const char* fmt, ...);
void log_set_min_level(int level);
// UTF-8 path of the package LocalState folder (no trailing slash).
const std::string& local_state_utf8();
const std::wstring& local_state_wide();
// Installs the unhandled-exception filter that logs a crash record + stack.
// NOTE: the layer (C) installs its own VEH for trap-and-emulate; this filter is
// only the last-chance recorder and is never reached for exceptions a VEH
// resumes.
void install_crash_logger();

}  // namespace xrl

// The xrl_log_fn handed to the layer in xrl_boot_context::log.
extern "C" void xrl_host_log(int level, const char* tag, const char* msg);

#define XLOGT(tag, ...) ::xrl::logf(::xrl::kTrace, tag, __VA_ARGS__)
#define XLOGI(tag, ...) ::xrl::logf(::xrl::kInfo,  tag, __VA_ARGS__)
#define XLOGW(tag, ...) ::xrl::logf(::xrl::kWarn,  tag, __VA_ARGS__)
#define XLOGE(tag, ...) ::xrl::logf(::xrl::kError, tag, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Config  (config.cpp)
// ---------------------------------------------------------------------------
// Optional LocalState/launch.cfg ("key=value" lines, '#' comments). Lets the
// integrator change argv/envp/budget/probe selection WITHOUT rebuilding the
// package. Unknown keys are logged and ignored.
namespace xrl {

struct HostConfig {
    // What the boot context will carry (UTF-8). Empty => derived default.
    std::string vfs_root;       // default <LocalState>\rootfs
    std::string prefix_path;    // default /prefix
    std::string game_path;      // default /game
    std::vector<std::string> argv;  // empty => default (see boot.cpp)
    std::vector<std::string> envp;  // empty => default (see boot.cpp)
    // mount=<guest dir>=<host dir> (repeatable): a guest directory served from outside
    // vfs_root; "pkg:" at the start of <host dir> = the package's install folder
    std::vector<std::string> mounts;
    uint64_t guest_mem_budget = 4096ull << 20;   // bytes; see notes-A.md

    // Probe selection: "all" | "quick" | "none"
    std::string probe_mode;     // "" => mode default (probe build: all, full: quick)
    bool probe_swapchain = true;
    bool probe_audio     = true;
    int  linger_seconds  = 5;   // probe build: stay alive this long after the probe
    int  log_level       = 0;   // minimum level written to log.txt
    bool skip_layer      = false;   // full build: run probe + window, do not boot layer
    // display=auto | off | <w>x<h>[@<hz>]: the console video mode to switch to (display.cpp)
    std::string display;
};

// Loads launch.cfg from LocalState (missing file is fine). Logs what it read.
const HostConfig& load_config();
const HostConfig& config();

}  // namespace xrl

// ---------------------------------------------------------------------------
// Display mode  (display.cpp)
// ---------------------------------------------------------------------------
namespace xrl {

struct DisplayMode { uint32_t width = 0, height = 0, refresh_hz = 0; };
// UI thread, after the window exists: picks (and on the console switches to) the output mode
// per launch.cfg display=; the guest is told about it as XRL_DISPLAY (boot.cpp).
void display_select();
DisplayMode display_mode();   // zero until display_select() ran
// Switches the HDMI output to w x h @ hz if the console lists it; waits for the switch (not on the UI thread)
bool display_switch(uint32_t w, uint32_t h, uint32_t hz);

}  // namespace xrl

// ---------------------------------------------------------------------------
// Gfx thread  (gfx_thread.cpp)
// ---------------------------------------------------------------------------
// D3D11CreateDevice / D3D12CreateDevice on the CoreWindow UI thread of a
// Game-bucket app BLOCKS INDEFINITELY on Xbox (docs/decisions.md). All native
// device and swapchain creation is therefore marshalled onto this dedicated MTA
// thread. Present()/Release() may be called from any thread and are not
// marshalled.
#include <functional>
namespace xrl {

// Starts the thread (idempotent). Returns false if it could not be started.
bool gfx_thread_start();
// Runs `fn` on the gfx thread and waits up to timeout_ms (0xFFFFFFFF = forever),
// logging a "still waiting" warning every 5 s so a UI-thread-style hang is
// visible in log.txt instead of silent. Returns false on timeout - in which
// case `fn` may STILL run later, so it must only capture shared/by-value state.
bool gfx_thread_invoke(const char* what, std::function<void()> fn,
                       uint32_t timeout_ms = 0xFFFFFFFFu);
bool gfx_thread_is_current();

}  // namespace xrl

// ---------------------------------------------------------------------------
// Window info published by whichever front-end owns the CoreWindow
// (probe view or SDL).   (window_info.cpp)
// ---------------------------------------------------------------------------
namespace xrl {

struct WindowInfo {
    void*    core_window = nullptr;   // IUnknown* ABI pointer of Windows.UI.Core.CoreWindow (agile)
    uint32_t width  = 0;              // physical pixels
    uint32_t height = 0;
    bool     valid  = false;
};
void window_info_set(const WindowInfo& w);
WindowInfo window_info_get();

}  // namespace xrl

// ---------------------------------------------------------------------------
// Op tables  (gfx_ops_default.cpp / audio_ops.cpp / input_ops.cpp)
// ---------------------------------------------------------------------------
extern "C" {
const xrl_gfx_ops*   xrl_host_gfx_ops(void);     // via the gfx seam (D may replace)
const xrl_audio_ops* xrl_host_audio_ops(void);
const xrl_input_ops* xrl_host_input_ops(void);
}

// ---------------------------------------------------------------------------
// Probe  (probe.cpp)
// ---------------------------------------------------------------------------
namespace xrl {

struct ProbeOptions {
    bool sysinfo = true, rwx = true, alloc4g = true, vamap = true;
    bool d3d12 = true, d3d11on12 = true, swapchain = true, audio = true, input = true, ceiling = true;
    static ProbeOptions all();
    static ProbeOptions quick();   // sysinfo + rwx + vamap + gfx device (no 4 GB, no swapchain/audio)
};
// Runs the probe sequence and logs a final "PROBE_RESULT ..." line. Never throws.
// Returns true iff every REQUIRED step (rwx, alloc4g, d3d12, d3d11on12) that was
// selected succeeded.
bool probe_run(const ProbeOptions& opt);

}  // namespace xrl

// ---------------------------------------------------------------------------
// Boot  (boot.cpp)
// ---------------------------------------------------------------------------
namespace xrl {

// Builds the xrl_boot_context from config + ops and calls xrl_layer_boot().
// Blocks until the guest exits. Returns the guest exit code / negative status.
int boot_layer();
// Requests orderly shutdown of a running layer (thread-safe).
void request_layer_exit(int code);
// Starts a worker thread with `stack_bytes` of RESERVED stack (0 = default). The
// layer's main thread wants a big one (Blink/Wine unwind deep). Returns a
// Win32 HANDLE (as void*; caller may CloseHandle) or nullptr.
void* start_thread(const char* name, size_t stack_bytes, std::function<void()> fn);
// Ends the process cleanly via CoreApplication::Exit (any thread).
void host_exit();

}  // namespace xrl

// Early process init shared by both entry paths (log, crash logger, config).
namespace xrl { void host_early_init(const char* mode_name); }

// Request an extended-execution session so the OS does not suspend the app when it backgrounds
// (EAC's multi-second inject must not be cut off). Call while foreground; safe to call once.
namespace xrl { void request_extended_execution(); }

// procprobe (probe.cpp): process-lifecycle evidence. Child runs procprobe_child_main
// (XRL_PROCCHILD set); the foreground app runs procprobe_parent (XRL_PROCPROBE set).
namespace xrl { void procprobe_child_main(const char* spec); void procprobe_parent(const char* spec); }

// Keep-alive present loop (gfx_ops_default.cpp): clears+presents the CoreWindow from a dedicated
// MTA thread so the Xbox PLM keeps the app foreground/active while the EAC bootstrapper (which
// renders nothing) runs and injects. No-op if there is no CoreWindow (e.g. the headless tracee).
namespace xrl { void gfx_keepalive_start(); void gfx_keepalive_stop(); void gfx_present_test(int mode); void gfx_discover_run(FILE* f); void discover_run(); }

// ---------------------------------------------------------------------------
// Full-build only: SDL front-end  (sdl_main.cpp, compiled WINAPI_FAMILY_APP)
// ---------------------------------------------------------------------------
#if defined(XRL_FULL)
int xrl_sdl_entry();   // calls SDL_WinRTRunApp; returns process exit code
// Host-internal keyboard state (SDL scancodes 0..511). ABI v1 has no keyboard
// op - see host/ABI-CHANGE-REQUESTS.md #3.
extern "C" int xrl_host_key_down(int sdl_scancode);
#endif

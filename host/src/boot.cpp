// boot.cpp - builds the xrl_boot_context and calls xrl_layer_boot().
//
// The layer (Workstream C) does not exist yet: layer_stub.cpp supplies a stand-in
// `xrl_layer_boot` that validates and logs the context, so the whole host path up
// to the seam is exercised. A strong xrl_layer_boot from layer.lib (see
// vcxproj property XrlLayerLib) replaces it at link time with no source change.
#include "pch.h"
#include "xrl_host.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

// Everything the boot context points to must outlive xrl_layer_boot(); keep it in
// statics owned by this TU.
std::string g_vfs_root, g_prefix_path, g_game_path;
std::vector<std::string> g_argv_s, g_envp_s;
std::vector<const char*> g_argv_p, g_envp_p;
std::atomic<bool> g_layer_running{false};

}  // namespace

namespace xrl {

// The package install folder. Package::Current() throws in a non-UWP-activated 2nd process
// (the EAC bootstrapper's spawned tracee): fall back to this exe's own directory, which for a
// loose install IS the install folder. Cached so both call sites agree.
static std::string pkg_install_path() {
    static std::string cached;
    if (!cached.empty()) return cached;
    try { cached = winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path()); }
    catch (...) {
        wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring w(exe, n); size_t s = w.find_last_of(L"\\/");
        if (s != std::wstring::npos) w.resize(s);
        cached = winrt::to_string(w);
        XLOGW("boot", "Package::Current() unavailable (2nd process); install path from exe dir: %s", cached.c_str());
    }
    return cached;
}

// Log the first error-severity SEH exception (an access violation here is not caught by catch(...)
// under /EHsc, so a crash in a spawned child vanishes with no trace). CONTINUE_SEARCH lets it crash
// as before, but now the code+address are flushed to the log first.
static LONG CALLBACK host_crash_veh(PEXCEPTION_POINTERS ep) {
    static std::atomic<bool> logged{false};
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code & 0xF0000000u) == 0xC0000000u && !logged.exchange(true)) {
        void* at = ep->ExceptionRecord->ExceptionAddress;
        void* addr = ep->ExceptionRecord->NumberParameters >= 2 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr;
        XLOGW("crash", "first SEH 0x%08lx at rip=%p accessing %p (tid 0x%lx)", code, at, addr, GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int boot_layer() {
    static std::atomic<bool> veh_done{false};
    if (!veh_done.exchange(true) && GetEnvironmentVariableA("XRL_TRACEE_R", nullptr, 0))
        AddVectoredExceptionHandler(1, host_crash_veh);   // only in a spawned tracee child
    const HostConfig& cfg = load_config();
    XLOGW("boot", "config loaded; resolving paths (tracee=%d)", GetEnvironmentVariableA("XRL_TRACEE_R", nullptr, 0) ? 1 : 0);

    g_vfs_root    = cfg.vfs_root.empty()    ? local_state_utf8() + "\\rootfs" : cfg.vfs_root;
    // Support pkg: prefix for vfs_root (same as mounts)
    if (g_vfs_root.rfind("pkg:", 0) == 0) {
        g_vfs_root = pkg_install_path() + "\\" + g_vfs_root.substr(4);
    }
    XLOGW("boot", "vfs_root resolved: %s", g_vfs_root.c_str());
    g_prefix_path = cfg.prefix_path.empty() ? "/prefix" : cfg.prefix_path;
    g_game_path   = cfg.game_path.empty()   ? "/game"   : cfg.game_path;

    // TODO(integrator): these defaults are PLACEHOLDERS for what B's wine layout
    // and RL's anti-cheat-off launch option turn out to be. Override without
    // rebuilding via LocalState/launch.cfg (argv= / env= lines).
    g_argv_s = cfg.argv;
    {   // procprobe "layer" child: XRL_PP_ARGV="prog|arg|..." replaces the configured program
        char pa[512];
        if (GetEnvironmentVariableA("XRL_PP_ARGV", pa, sizeof pa)) {
            g_argv_s.clear();
            for (char* t = strtok(pa, "|"); t; t = strtok(nullptr, "|")) g_argv_s.push_back(t);
        }
    }
    if (g_argv_s.empty()) {
        g_argv_s = { "/usr/bin/wine64", g_game_path + "/Binaries/Win64/RocketLeague.exe", "-nocrashdialog" };
    }
    g_envp_s = cfg.envp;
    if (g_envp_s.empty()) {
        g_envp_s = {
            "WINEPREFIX=" + g_prefix_path,
            "HOME=/root",
            "PATH=/usr/bin:/bin",
            "TMPDIR=/tmp",
            "LANG=C.UTF-8",
            "WINEDEBUG=err+all",
        };
    }
    // mounts -> XRL_MOUNTS for the layer's VFS (layer/src/vfs.c). "pkg:" is the install folder:
    // a big read-only tree (the game) ships in the package, not in LocalState's small data area.
    if (!cfg.mounts.empty()) {
        std::string pkg = pkg_install_path();
        std::string all;
        for (const auto& m : cfg.mounts) {
            size_t eq = m.find('=');
            if (eq == std::string::npos) { XLOGW("boot", "ignoring mount '%s' (want guest=host)", m.c_str()); continue; }
            std::string host = m.substr(eq + 1);
            if (host.rfind("pkg:", 0) == 0) host = pkg + "\\" + host.substr(4);
            if (!all.empty()) all += ';';
            all += m.substr(0, eq) + "=" + host;
            XLOGI("boot", "mount %s -> %s", m.substr(0, eq).c_str(), host.c_str());
        }
        SetEnvironmentVariableA("XRL_MOUNTS", all.c_str());
    }
    // the output mode for Wine's display list (win32u, patches/wine/0025), unless env= set one
    {
        DisplayMode dm = display_mode();
        bool given = false;
        for (const auto& e : g_envp_s) given |= e.rfind("XRL_DISPLAY=", 0) == 0;
        if (!given && dm.width) {
            char b[64]; snprintf(b, sizeof b, "XRL_DISPLAY=%ux%u@%u", dm.width, dm.height, dm.refresh_hz ? dm.refresh_hz : 60);
            g_envp_s.push_back(b);
        }
    }
    g_argv_p.clear(); g_envp_p.clear();
    for (auto& s : g_argv_s) g_argv_p.push_back(s.c_str());
    for (auto& s : g_envp_s) g_envp_p.push_back(s.c_str());
    g_argv_p.push_back(nullptr);
    g_envp_p.push_back(nullptr);

    xrl_boot_context ctx{};
    ctx.struct_size  = sizeof ctx;
    ctx.abi_version  = XRL_ABI_VERSION;
    ctx.vfs_root     = g_vfs_root.c_str();
    ctx.prefix_path  = g_prefix_path.c_str();
    ctx.game_path    = g_game_path.c_str();
    ctx.argv         = g_argv_p.data();
    ctx.envp         = g_envp_p.data();
    ctx.log          = xrl_host_log;
    ctx.gfx          = xrl_host_gfx_ops();
    ctx.audio        = xrl_host_audio_ops();
    ctx.input        = xrl_host_input_ops();
    ctx.guest_mem_budget = cfg.guest_mem_budget;

    XLOGI("boot", "xrl_boot_context: struct_size=%u abi=%u", ctx.struct_size, ctx.abi_version);
    XLOGI("boot", "  vfs_root    = %s", ctx.vfs_root);
    XLOGI("boot", "  prefix_path = %s", ctx.prefix_path);
    XLOGI("boot", "  game_path   = %s", ctx.game_path);
    for (size_t i = 0; i + 1 < g_argv_p.size(); ++i) XLOGI("boot", "  argv[%zu]     = %s", i, g_argv_p[i]);
    for (size_t i = 0; i + 1 < g_envp_p.size(); ++i) XLOGI("boot", "  envp[%zu]     = %s", i, g_envp_p[i]);
    XLOGI("boot", "  guest_mem_budget = %llu MiB", (unsigned long long)(ctx.guest_mem_budget >> 20));
    XLOGI("boot", "  ops: gfx=%p (size %u) audio=%p (size %u) input=%p (size %u)", (const void*)ctx.gfx, ctx.gfx ? ctx.gfx->struct_size : 0,
          (const void*)ctx.audio, ctx.audio ? ctx.audio->struct_size : 0, (const void*)ctx.input, ctx.input ? ctx.input->struct_size : 0);

    if (!ctx.gfx || !ctx.audio || !ctx.input) { XLOGE("boot", "an op table is NULL - refusing to boot"); return XRL_ERR_GENERIC; }

    XLOGI("boot", "calling xrl_layer_boot() ...");
    g_layer_running.store(true);
    int rc = xrl_layer_boot(&ctx);
    g_layer_running.store(false);
    XLOGI("boot", "xrl_layer_boot() returned %d", rc);
    return rc;
}

void* start_thread(const char* name, size_t stack_bytes, std::function<void()> fn) {
    struct Start { std::string name; std::function<void()> fn; };
    auto* st = new Start{name ? name : "worker", std::move(fn)};
    HANDLE h = CreateThread(nullptr, stack_bytes, [](LPVOID a) -> DWORD {
        std::unique_ptr<Start> s((Start*)a);
        XLOGI("thread", "'%s' started (tid 0x%lx)", s->name.c_str(), GetCurrentThreadId());
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
        try { s->fn(); }
        catch (const std::exception& e) { XLOGE("thread", "'%s' threw: %s", s->name.c_str(), e.what()); }
        catch (...) { XLOGE("thread", "'%s' threw unknown exception", s->name.c_str()); }
        XLOGI("thread", "'%s' finished", s->name.c_str());
        return 0;
    }, st, stack_bytes ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, nullptr);
    if (!h) { XLOGE("thread", "CreateThread('%s') failed: %lu", st->name.c_str(), GetLastError()); delete st; }
    return h;
}

void request_layer_exit(int code) {
    if (!g_layer_running.load()) return;
    XLOGI("boot", "requesting layer exit (code %d)", code);
    xrl_layer_request_exit(code);
}

}  // namespace xrl

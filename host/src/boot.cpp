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

int boot_layer() {
    const HostConfig& cfg = load_config();

    g_vfs_root    = cfg.vfs_root.empty()    ? local_state_utf8() + "\\rootfs" : cfg.vfs_root;
    g_prefix_path = cfg.prefix_path.empty() ? "/prefix" : cfg.prefix_path;
    g_game_path   = cfg.game_path.empty()   ? "/game"   : cfg.game_path;

    // TODO(integrator): these defaults are PLACEHOLDERS for what B's wine layout
    // and RL's anti-cheat-off launch option turn out to be. Override without
    // rebuilding via LocalState/launch.cfg (argv= / env= lines).
    g_argv_s = cfg.argv;
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

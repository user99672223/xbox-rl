// config.cpp - optional LocalState/launch.cfg ("key=value", '#' comments).
//
// Keys (all optional, repeatable ones marked *):
//   vfs_root=<win32 path>        default <LocalState>\rootfs
//   prefix_path=<guest path>     default /prefix
//   game_path=<guest path>       default /game
//   argv=<string>      *         one guest argv entry per line, in order
//   env=<K=V>          *         one guest environment entry per line
//   mem_budget_mb=<n>            xrl_boot_context::guest_mem_budget
//   probe=all|quick|none         which probe steps to run
//   probe_swapchain=0|1          probe: CoreWindow swapchain + 120 presents
//   probe_audio=0|1              probe: XAudio2 + short quiet beep
//   linger_s=<n>                 probe build: seconds to stay up after probe
//   log_level=0..3               minimum level written to log.txt
//   skip_layer=0|1               full build: run window+probe but do not boot layer
#include "pch.h"
#include "xrl_host.h"

#include <windows.h>
#include <cstdlib>
#include <string>

namespace {

xrl::HostConfig g_cfg;
bool g_loaded = false;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
bool as_bool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

std::string read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string s;
    if (sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
        s.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr);
        s.resize(got);
    }
    CloseHandle(h);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

}  // namespace

namespace xrl {

const HostConfig& load_config() {
    if (g_loaded) return g_cfg;
    g_loaded = true;
    std::wstring path = local_state_wide() + L"\\launch.cfg";
    std::string text = read_file(path);
    if (text.empty()) {
        XLOGI("config", "no launch.cfg in LocalState (using defaults)");
        return g_cfg;
    }
    XLOGI("config", "reading launch.cfg (%zu bytes)", text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { XLOGW("config", "ignoring malformed line: %s", line.c_str()); continue; }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if      (k == "vfs_root")        g_cfg.vfs_root = v;
        else if (k == "prefix_path")     g_cfg.prefix_path = v;
        else if (k == "game_path")       g_cfg.game_path = v;
        else if (k == "argv")            g_cfg.argv.push_back(v);
        else if (k == "env")             g_cfg.envp.push_back(v);
        else if (k == "mem_budget_mb")   g_cfg.guest_mem_budget = (uint64_t)strtoull(v.c_str(), nullptr, 10) << 20;
        else if (k == "probe")           g_cfg.probe_mode = v;
        else if (k == "probe_swapchain") g_cfg.probe_swapchain = as_bool(v);
        else if (k == "probe_audio")     g_cfg.probe_audio = as_bool(v);
        else if (k == "linger_s")        g_cfg.linger_seconds = atoi(v.c_str());
        else if (k == "log_level")       g_cfg.log_level = atoi(v.c_str());
        else if (k == "skip_layer")      g_cfg.skip_layer = as_bool(v);
        else XLOGW("config", "unknown key '%s' (ignored)", k.c_str());
        XLOGI("config", "  %s=%s", k.c_str(), v.c_str());
    }
    log_set_min_level(g_cfg.log_level);
    return g_cfg;
}

const HostConfig& config() { return g_cfg; }

}  // namespace xrl

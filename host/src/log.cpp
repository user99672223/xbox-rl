// log.cpp - LocalState/log.txt, the single log sink for host, layer and wine.
//
// Design points (see notes-A.md):
//   * One WriteFile per line, no user-space buffering, so a crash/kill never
//     loses the tail. Warn/error lines (and shutdown) also FlushFileBuffers.
//   * File is opened FILE_SHARE_READ|WRITE so Device Portal can read it while
//     the app is still running (logs.ps1 does this).
//   * The previous run's log is rotated to log.prev.txt - a crash on run N is
//     never overwritten by run N+1's startup lines before it is pulled.
#include "pch.h"
#include "xrl_host.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

namespace {

struct LogState {
    SRWLOCK     lock = SRWLOCK_INIT;
    HANDLE      file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER t0{};
    LARGE_INTEGER freq{};
    std::atomic<int> min_level{0};
    std::string path_utf8;
    std::wstring path_wide;
    bool inited = false;
    std::atomic<bool> crashing{false};      // crash path logs without taking the lock (it may be held)
    std::atomic<uint64_t> bytes{0};          // written so far
    uint64_t max_bytes = 256ull << 20;       // beyond this only warn/error lines are written
    bool truncated_noted = false;
};
LogState g;
std::string g_ls_utf8;
std::wstring g_ls_wide;

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring resolve_local_state() {
    try {
        return std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
    } catch (...) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, buf);
        std::wstring p = n ? std::wstring(buf, n) : std::wstring(L".\\");
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        return p;
    }
}

const char* level_char(int l) {
    switch (l) { case 0: return "T"; case 1: return "I"; case 2: return "W"; default: return "E"; }
}

void write_line_locked(int level, const char* tag, const char* msg) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double t = (double)(now.QuadPart - g.t0.QuadPart) / (double)g.freq.QuadPart;

    // Build "[ssss.mmm tTTTT] L tag: msg\r\n"
    char head[96];
    int hn = snprintf(head, sizeof head, "[%9.3f t%04lx] %s %s: ", t, GetCurrentThreadId(), level_char(level), tag ? tag : "?");
    std::string line;
    line.reserve(hn + strlen(msg) + 2);
    line.append(head, hn);
    // Keep one record per physical line: flatten embedded newlines.
    for (const char* p = msg; *p; ++p) {
        if (*p == '\r') continue;
        line.push_back(*p == '\n' ? '|' : *p);
    }
    line.append("\r\n");

    if (g.file != INVALID_HANDLE_VALUE) {
        uint64_t total = g.bytes.load();
        if (total > g.max_bytes && level < 2) {
            if (!g.truncated_noted) {
                g.truncated_noted = true;
                static const char note[] = "[log] size cap reached: dropping trace/info lines from here on (warn/error still written)\r\n";
                DWORD w = 0; WriteFile(g.file, note, (DWORD)sizeof(note) - 1, &w, nullptr);
            }
        } else {
            DWORD written = 0;
            WriteFile(g.file, line.data(), (DWORD)line.size(), &written, nullptr);
            g.bytes.fetch_add(written);
            if (level >= 2) FlushFileBuffers(g.file);
        }
    }
    OutputDebugStringA(line.c_str());
}

}  // namespace

namespace xrl {

bool log_init() {
    if (g.inited) return g.file != INVALID_HANDLE_VALUE;
    g_ls_wide = resolve_local_state();
    g_ls_utf8 = to_utf8(g_ls_wide);

    QueryPerformanceFrequency(&g.freq);
    QueryPerformanceCounter(&g.t0);

    std::wstring cur  = g_ls_wide + L"\\log.txt";
    std::wstring prev = g_ls_wide + L"\\log.prev.txt";
    MoveFileExW(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);   // ok if cur absent

    g.path_wide = cur;
    g.path_utf8 = to_utf8(cur);
    g.file = CreateFileW(cur.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g.inited = true;
    if (g.file == INVALID_HANDLE_VALUE) {
        OutputDebugStringW(L"xrl: cannot open log.txt\n");
        return false;
    }
    // UTF-8 BOM so Notepad/Device Portal viewers pick the right encoding.
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD w = 0; WriteFile(g.file, bom, 3, &w, nullptr);
    logf(kInfo, "log", "log opened: %s", g.path_utf8.c_str());
    return true;
}

void log_shutdown() {
    AcquireSRWLockExclusive(&g.lock);
    if (g.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g.file);
        CloseHandle(g.file);
        g.file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g.lock);
}

void log_set_min_level(int level) { g.min_level.store(level); }

const std::string& local_state_utf8() { return g_ls_utf8; }
const std::wstring& local_state_wide() { return g_ls_wide; }

void logf(int level, const char* tag, const char* fmt, ...) {
    if (level < g.min_level.load(std::memory_order_relaxed)) return;
    char stackbuf[1024];
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(stackbuf, sizeof stackbuf, fmt, ap);
    va_end(ap);
    std::string big;
    const char* msg = stackbuf;
    if (n >= (int)sizeof stackbuf) {
        big.resize((size_t)n + 1);
        vsnprintf(big.data(), big.size(), fmt, ap2);
        msg = big.c_str();
    } else if (n < 0) {
        msg = "<format error>";
    }
    va_end(ap2);
    const bool locked = !g.crashing.load();
    if (locked) AcquireSRWLockExclusive(&g.lock);
    write_line_locked(level, tag, msg);
    if (locked) ReleaseSRWLockExclusive(&g.lock);
}

// --- crash logger -------------------------------------------------------------

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> reentry{0};
    if (reentry.fetch_add(1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    g.crashing.store(true);   // another thread may hold the log lock; don't deadlock the last-gasp record

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    logf(kError, "crash", "UNHANDLED EXCEPTION code=0x%08lx addr=%p thread=0x%lx info0=0x%llx info1=0x%llx",
         er->ExceptionCode, er->ExceptionAddress, GetCurrentThreadId(),
         er->NumberParameters > 0 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
         er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
    logf(kError, "crash", "rip=%016llx rsp=%016llx rbp=%016llx rax=%016llx rcx=%016llx rdx=%016llx",
         c->Rip, c->Rsp, c->Rbp, c->Rax, c->Rcx, c->Rdx);
    void* frames[40];
    USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)frames[i], &m);
        wchar_t name[MAX_PATH] = L"?";
        if (m) GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        logf(kError, "crash", "  #%02u %p  %ls+0x%llx", (unsigned)i, frames[i], base ? base + 1 : name,
             (unsigned long long)((char*)frames[i] - (char*)m));
    }
    if (g.file != INVALID_HANDLE_VALUE) FlushFileBuffers(g.file);
    return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_logger() {
    SetUnhandledExceptionFilter(crash_filter);
    std::set_terminate([] {
        logf(kError, "crash", "std::terminate called");
        if (g.file != INVALID_HANDLE_VALUE) FlushFileBuffers(g.file);
        abort();
    });
}

}  // namespace xrl

extern "C" void xrl_host_log(int level, const char* tag, const char* msg) {
    if (level < 0) level = 0; else if (level > 3) level = 3;
    if (level < g.min_level.load(std::memory_order_relaxed)) return;
    const bool locked = !g.crashing.load();
    if (locked) AcquireSRWLockExclusive(&g.lock);
    write_line_locked(level, tag ? tag : "?", msg ? msg : "");
    if (locked) ReleaseSRWLockExclusive(&g.lock);
}

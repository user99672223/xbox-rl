// core.cpp — logging, HRESULT mapping, host-services plumbing, the gfx-thread
// dispatcher, init/shutdown and the production entry xrl_gfx_ops_get.
#include "common.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace xrl {

// ================================================================ state
State& G() {
    static State s;
    return s;
}

DWORD CreateTimeoutMs() {
    DWORD t = G().cfg.create_timeout_ms;
    return t ? t : 60000;  // matches the host's own create timeout
}

bool DebugLayerRequested() { return (G().cfg.flags & XRL_GFX_FLAG_DEBUG_LAYER) != 0; }

void WarnIfUiThread(const char* what) {
    DWORD ui = G().cfg.ui_thread_id;
    if (ui && ui == GetCurrentThreadId())
        Log(LOG_WARN,
            "%s called on the host UI thread (tid %lu). The D3D work is still marshalled to the gfx "
            "thread, but this thread is blocked while it waits.",
            what, (unsigned long)ui);
}

// ---- window source (resolved lazily: the host's window may not exist at init)
void* GetCoreWindow() {
    State& s = G();
    xrl_gfx_host_services h;
    void* fallback;
    bool have;
    {
        std::lock_guard<std::mutex> l(s.mu);
        h = s.host;
        have = s.have_host;
        fallback = s.cfg.core_window;
    }
    if (have && h.get_core_window) {
        if (void* w = h.get_core_window()) return w;
    }
    return fallback;
}

void GetWindowSize(uint32_t* w, uint32_t* h) {
    State& s = G();
    xrl_gfx_host_services hs;
    bool have;
    uint32_t cw, ch;
    {
        std::lock_guard<std::mutex> l(s.mu);
        hs = s.host;
        have = s.have_host;
        cw = s.cfg.window_width_px;
        ch = s.cfg.window_height_px;
    }
    uint32_t ww = 0, hh = 0;
    if (have && hs.get_window_size) hs.get_window_size(&ww, &hh);
    *w = ww ? ww : cw;
    *h = hh ? hh : ch;
}

void* GetTestHwnd() {
#if XRL_GFX_HAVE_HWND
    return G().cfg.test_hwnd;
#else
    return nullptr;
#endif
}

// ================================================================ logging
static std::atomic<xrl_log_fn> g_log{nullptr};

void Log(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) buf[sizeof buf - 1] = 0;
    if (xrl_log_fn fn = g_log.load(std::memory_order_acquire)) {
        fn(level, "gfx", buf);
    } else {
        OutputDebugStringA("[gfx] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }
}

// ================================================================ errors
static thread_local HRESULT t_last_hr = S_OK;

void SetLastHr(HRESULT hr) { t_last_hr = hr; }

xrl_status HrToStatus(HRESULT hr) {
    if (SUCCEEDED(hr)) return XRL_OK;
    switch (hr) {
        case DXGI_ERROR_DEVICE_REMOVED:
        case DXGI_ERROR_DEVICE_RESET:
        case DXGI_ERROR_DEVICE_HUNG:
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return XRL_ERR_DEVICELOST;
        case DXGI_ERROR_UNSUPPORTED:
        case DXGI_ERROR_NOT_FOUND:
        case DXGI_ERROR_SDK_COMPONENT_MISSING:
        case E_NOTIMPL:
        case E_NOINTERFACE:
        case HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED):
            return XRL_ERR_NOTAVAIL;
        default:
            return XRL_ERR_GENERIC;
    }
}

xrl_status Fail(HRESULT hr, const char* what) {
    SetLastHr(hr);
    Log(LOG_ERROR, "%s failed: hr=0x%08lX", what, (unsigned long)hr);
    return HrToStatus(hr);
}

// ================================================================ gfx thread
namespace {

struct Job {
    std::function<HRESULT()> fn;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    HRESULT hr = E_FAIL;
};

// Execute a job body on whatever thread we are now on; publish the result.
void RunJob(const std::shared_ptr<Job>& job) {
    HRESULT hr;
    try {
        hr = job->fn();
    } catch (...) {
        hr = E_UNEXPECTED;
    }
    job->fn = nullptr;  // destroy captured state here, on the gfx thread
    {
        std::lock_guard<std::mutex> l(job->m);
        job->hr = hr;
        job->done = true;
    }
    job->cv.notify_all();
}

// ---- host path: trampoline handed to host->run_on_gfx_thread. The box owns a
// shared_ptr so that a job the host runs AFTER we timed out still has valid state.
void HostTramp(void* ctx) {
    std::unique_ptr<std::shared_ptr<Job>> box(static_cast<std::shared_ptr<Job>*>(ctx));
    RunJob(*box);
}

// ---- library-owned worker path (standalone harness only)
struct WorkerState {
    std::mutex m;
    std::condition_variable cv;       // job available / stop
    std::condition_variable exit_cv;  // thread exited
    std::deque<std::shared_ptr<Job>> q;
    bool stop = false;
    bool exited = false;
    std::atomic<DWORD> tid{0};
};

std::shared_ptr<WorkerState> g_worker;
std::mutex g_worker_mu;

void WorkerMain(std::shared_ptr<WorkerState> ws) {
    // MTA: no window, no message pump. decisions.md: D3D create hangs on the UI
    // (STA/ASTA) thread, not on a plain MTA thread.
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ws->tid = GetCurrentThreadId();
    Log(LOG_INFO, "library gfx worker started (tid %lu, CoInitializeEx MTA hr=0x%08lX)",
        (unsigned long)ws->tid.load(), (unsigned long)co);
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> l(ws->m);
            ws->cv.wait(l, [&] { return ws->stop || !ws->q.empty(); });
            if (ws->q.empty()) break;
            job = std::move(ws->q.front());
            ws->q.pop_front();
        }
        RunJob(job);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    {
        std::lock_guard<std::mutex> l(ws->m);
        ws->exited = true;
    }
    ws->exit_cv.notify_all();
}

void WorkerStart() {
    std::lock_guard<std::mutex> l(g_worker_mu);
    if (g_worker) return;
    g_worker = std::make_shared<WorkerState>();
    std::thread(WorkerMain, g_worker).detach();  // lifetime managed via WorkerState
}

void WorkerStop() {
    std::shared_ptr<WorkerState> ws;
    {
        std::lock_guard<std::mutex> l(g_worker_mu);
        ws = std::move(g_worker);
        g_worker = nullptr;
    }
    if (!ws) return;
    {
        std::lock_guard<std::mutex> l(ws->m);
        ws->stop = true;
    }
    ws->cv.notify_all();
    std::unique_lock<std::mutex> l(ws->m);
    if (!ws->exit_cv.wait_for(l, std::chrono::milliseconds(2000), [&] { return ws->exited; }))
        Log(LOG_ERROR, "library gfx worker did not exit within 2s (wedged in a driver call?); abandoning it");
}

std::shared_ptr<WorkerState> WorkerGet() {
    std::lock_guard<std::mutex> l(g_worker_mu);
    return g_worker;
}

}  // namespace

HRESULT RunOnGfxThread(const char* what, std::function<HRESULT()> fn, DWORD timeout_ms) {
    if (!timeout_ms) timeout_ms = CreateTimeoutMs();

    xrl_gfx_host_services host{};
    bool have_run = false;
    {
        std::lock_guard<std::mutex> l(G().mu);
        if (!G().inited) return E_ILLEGAL_METHOD_CALL;
        if (G().have_host && G().host.run_on_gfx_thread) {
            host = G().host;
            have_run = true;
        }
    }

    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);

    if (have_run) {
        if (host.on_gfx_thread && host.on_gfx_thread()) {  // already there: run inline
            RunJob(job);
            return job->hr;
        }
        auto* box = new std::shared_ptr<Job>(job);  // freed by HostTramp whenever it runs
        int rc = host.run_on_gfx_thread(what, &HostTramp, box, timeout_ms);
        if (rc != 0) {
            Log(LOG_ERROR,
                "%s: host gfx thread did not complete within %lu ms — probable driver/thread-affinity "
                "hang (docs/decisions.md: UI-thread D3D create blocks). Job abandoned.",
                what, (unsigned long)timeout_ms);
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        return job->hr;
    }

    auto ws = WorkerGet();
    if (!ws) return E_ILLEGAL_METHOD_CALL;
    if (ws->tid.load() == GetCurrentThreadId()) {  // re-entrant
        RunJob(job);
        return job->hr;
    }
    {
        std::lock_guard<std::mutex> l(ws->m);
        ws->q.push_back(job);
    }
    ws->cv.notify_one();
    std::unique_lock<std::mutex> l(job->m);
    if (!job->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return job->done; })) {
        Log(LOG_ERROR,
            "%s: library gfx worker (tid %lu) did not complete within %lu ms — probable driver/thread-"
            "affinity hang. Job abandoned.",
            what, (unsigned long)ws->tid.load(), (unsigned long)timeout_ms);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return job->hr;
}

}  // namespace xrl

// ================================================================ C API
using namespace xrl;

static void CopyHost(State& s, const xrl_gfx_host_services* h) {
    s.host = xrl_gfx_host_services{};
    s.have_host = false;
    if (!h) return;
    size_t n = h->struct_size < sizeof s.host ? h->struct_size : sizeof s.host;
    if (n < sizeof(uint32_t)) return;
    memcpy(&s.host, h, n);
    s.host.struct_size = (uint32_t)sizeof s.host;
    s.have_host = true;
}

extern "C" XRL_GFX_API xrl_status xrl_gfx_init(const xrl_gfx_config* cfg) {
    if (!cfg || cfg->struct_size < sizeof(uint32_t) * 2) return XRL_ERR_GENERIC;
    State& s = G();
    bool need_worker;
    {
        std::lock_guard<std::mutex> l(s.mu);
        if (s.inited) return XRL_ERR_GENERIC;
        s.cfg = xrl_gfx_config{};
        size_t n = cfg->struct_size < sizeof(xrl_gfx_config) ? cfg->struct_size : sizeof(xrl_gfx_config);
        memcpy(&s.cfg, cfg, n);
        s.cfg.struct_size = sizeof(xrl_gfx_config);
        CopyHost(s, s.cfg.host);
        s.cfg.host = nullptr;  // never keep the caller's pointer
        s.d11 = xrl_gfx_d3d11_decision{};
        s.d11.struct_size = sizeof s.d11;
        s.preflight_done = false;
        s.inited = true;
        xrl_log_fn lf = (s.have_host && s.host.log) ? s.host.log : s.cfg.log;
        g_log.store(lf, std::memory_order_release);
        need_worker = !(s.have_host && s.host.run_on_gfx_thread);
    }
    ResetOps();
    if (need_worker) WorkerStart();
    Log(LOG_INFO,
        "xrl_gfx init: api=%u abi=%u gfx_thread=%s core_window=%p test_hwnd=%p ui_tid=%u timeout=%lums flags=0x%X "
        "caller_tid=%lu",
        XRL_GFX_API_VERSION, XRL_ABI_VERSION, need_worker ? "library-worker" : "host", s.cfg.core_window,
        s.cfg.test_hwnd, s.cfg.ui_thread_id, (unsigned long)CreateTimeoutMs(), s.cfg.flags,
        (unsigned long)GetCurrentThreadId());
    return XRL_OK;
}

extern "C" XRL_GFX_API const xrl_gfx_ops* xrl_gfx_ops_get(const xrl_gfx_host_services* host) {
    State& s = G();
    bool already;
    {
        std::lock_guard<std::mutex> l(s.mu);
        already = s.inited;
        if (already) {  // re-bind services, keep everything else
            CopyHost(s, host);
            if (s.have_host && s.host.log) g_log.store(s.host.log, std::memory_order_release);
        }
    }
    if (!already) {
        xrl_gfx_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.host = host;
        if (xrl_gfx_init(&cfg) != XRL_OK) return nullptr;
    }
    return MutableOps();
}

extern "C" XRL_GFX_API void xrl_gfx_shutdown(void) {
    State& s = G();
    {
        std::lock_guard<std::mutex> l(s.mu);
        if (!s.inited) return;
    }
    Log(LOG_INFO, "xrl_gfx shutdown");
    WorkerStop();
    {
        std::lock_guard<std::mutex> l(s.mu);
        s.have_host = false;
        s.host = xrl_gfx_host_services{};
        s.inited = false;
        g_log.store(nullptr, std::memory_order_release);
    }
}

extern "C" XRL_GFX_API int32_t xrl_gfx_last_hresult(void) { return (int32_t)xrl::t_last_hr; }

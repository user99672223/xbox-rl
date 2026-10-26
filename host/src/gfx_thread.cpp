// gfx_thread.cpp - the dedicated MTA thread all native D3D/DXGI device and
// swapchain creation is marshalled onto.
//
// WHY (docs/decisions.md, "Launch/activation facts"): D3D11CreateDevice /
// D3D12CreateDevice on the CoreWindow UI thread of a Game-bucket AppContainer app
// blocks indefinitely on Xbox. The earlier probe host had to go headless to get
// anywhere. So: UI thread = CoreWindow + message pump ONLY; every create goes
// through gfx_thread_invoke().
#include "pch.h"
#include "xrl_host.h"

#include <windows.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {

struct Job {
    std::function<void()> fn;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

struct GfxThread {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Job>> q;
    std::thread th;
    std::atomic<DWORD> tid{0};
    bool started = false;
    bool stop = false;

    void run() {
        tid.store(GetCurrentThreadId());
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
        XLOGI("gfxthread", "gfx thread running (tid 0x%lx, MTA)", tid.load());
        for (;;) {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop && q.empty()) break;
                j = std::move(q.front());
                q.pop_front();
            }
            try { j->fn(); }
            catch (const winrt::hresult_error& e) { XLOGE("gfxthread", "job threw hresult_error 0x%08x", (unsigned)e.code().value); }
            catch (const std::exception& e)       { XLOGE("gfxthread", "job threw std::exception: %s", e.what()); }
            catch (...)                           { XLOGE("gfxthread", "job threw unknown exception"); }
            {
                std::lock_guard<std::mutex> lk(j->m);
                j->done = true;
            }
            j->cv.notify_all();
        }
    }
};

GfxThread& G() { static GfxThread* g = new GfxThread(); return *g; }   // leaked on purpose

}  // namespace

namespace xrl {

bool gfx_thread_start() {
    auto& g = G();
    std::lock_guard<std::mutex> lk(g.m);
    if (g.started) return true;
    try {
        g.th = std::thread([] { G().run(); });
        g.th.detach();
        g.started = true;
    } catch (...) {
        XLOGE("gfxthread", "failed to start gfx thread");
        return false;
    }
    return true;
}

bool gfx_thread_is_current() { return G().tid.load() == GetCurrentThreadId(); }

bool gfx_thread_invoke(const char* what, std::function<void()> fn, uint32_t timeout_ms) {
    if (gfx_thread_is_current()) { fn(); return true; }
    if (!gfx_thread_start()) return false;
    auto& g = G();
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.q.push_back(job);
    }
    g.cv.notify_one();

    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        std::unique_lock<std::mutex> lk(job->m);
        if (job->cv.wait_for(lk, std::chrono::seconds(5), [&] { return job->done; })) return true;
        lk.unlock();
        ULONGLONG waited = GetTickCount64() - t0;
        XLOGW("gfxthread", "still waiting on '%s' after %llu ms (a D3D create that never returns is the known Xbox hang)",
              what, (unsigned long long)waited);
        if (timeout_ms != 0xFFFFFFFFu && waited >= timeout_ms) {
            XLOGE("gfxthread", "TIMEOUT waiting on '%s' after %llu ms", what, (unsigned long long)waited);
            return false;
        }
    }
}

// --- window info store (tiny; lives here to keep the TU count down) ---------
static SRWLOCK g_wlock = SRWLOCK_INIT;
static WindowInfo g_win;
void window_info_set(const WindowInfo& w) {
    AcquireSRWLockExclusive(&g_wlock); g_win = w; ReleaseSRWLockExclusive(&g_wlock);
}
WindowInfo window_info_get() {
    AcquireSRWLockShared(&g_wlock); WindowInfo w = g_win; ReleaseSRWLockShared(&g_wlock); return w;
}

}  // namespace xrl

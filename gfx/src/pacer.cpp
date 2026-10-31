// pacer.cpp - see pacer.h.
#include "pacer.h"

#include <algorithm>
#include <cstring>

namespace xrl {

bool Pacer120::Start(ID3D11Device* game_dev, uint32_t w, uint32_t h, uint32_t dxgi_format, double early_ms) {
    Stop();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    qf_ = f.QuadPart;
    f120_ = qf_ * 1000 / 120000;
    early_ = (int64_t)((double)qf_ * early_ms / 1000.0);
    // the presenter's device, on the game's adapter
    ComPtr<IDXGIDevice> gd;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(game_dev->QueryInterface(IID_PPV_ARGS(&gd))) || FAILED(gd->GetAdapter(&ad))) return false;
    HRESULT hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pdev_, nullptr, &pctx_);
    if (FAILED(hr) || FAILED(pctx_.As(&pctx4_))) { Log(LOG_WARN, "pacer: presenter device 0x%08lX", (unsigned long)hr); return false; }
    if (HostSwapchainCreate11(pdev_.Get(), w, h, 3, dxgi_format, host_, &host_format_, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != XRL_OK ||
        FAILED(host_->GetBuffer(0, IID_PPV_ARGS(&hostbuf_)))) { Log(LOG_WARN, "pacer: host swapchain"); return false; }
    host_->GetContainingOutput(&out_);
    if (!out_) { Log(LOG_WARN, "pacer: no output to time the vblank on"); return false; }
    // When a frame is done: the game's context signals an event once the GPU has finished what it flushed
    // (Flush1). A shared ID3D11Fence would do it too, but creating one on the game's device makes Rocket League's
    // render thread crash on the console (bisected 2026-10-30), so no fence.
    ComPtr<ID3D11DeviceContext> gctx;
    ComPtr<ID3D11Device1> p1;
    game_dev->GetImmediateContext(&gctx);
    if (FAILED(gctx.As(&gctx3_)) || FAILED(pdev_.As(&p1))) { Log(LOG_WARN, "pacer: no ID3D11DeviceContext3 (Flush1)"); return false; }
    for (HANDLE& e : slot_ev_)
        if (!(e = CreateEventW(nullptr, TRUE, FALSE, nullptr))) return false;
    // the ring: shared textures at the output size, drawn into by the game's Blit (copy or FSR), copied by the presenter
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = host_format_; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (int i = 0; i < kSlots; i++) {
        ComPtr<IDXGIResource1> r;
        HANDLE th = nullptr;
        if (FAILED(game_dev->CreateTexture2D(&td, nullptr, &ring_g_[i])) || FAILED(ring_g_[i].As(&r)) ||
            FAILED(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th)) ||
            FAILED(p1->OpenSharedResource1(th, IID_PPV_ARGS(&ring_p_[i])))) {
            if (th) CloseHandle(th);
            Log(LOG_WARN, "pacer: shared texture %d unavailable", i);
            return false;
        }
        CloseHandle(th);
        slot_value_[i] = 0; slot_seq_[i] = 0;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(pdev_->CreateQuery(&qd, &copied_))) return false;
    seq_ = 0; writing_ = -1; reading_ = -1; prev_[0] = prev_[1] = -1; tick_ = 0;
    {
        char v[16] = "";
        GetEnvironmentVariableA("XRL_PACE_LOCK", v, sizeof v);
        lock_mode_ = !strcmp(v, "60") ? 60 : !strcmp(v, "120") ? 120 : 0;
        lock60_ = lock_mode_ == 60;
        if (!shown_ev_) shown_ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        shown_ = 0; work_ema_ = 0; acq_ret_ = 0; grid0_ = 0;
        Log(LOG_INFO, "pacer: cadence %s (XRL_PACE_LOCK)", lock_mode_ == 60 ? "60" : lock_mode_ == 120 ? "120" : "auto 60/120");
    }
    st_flips_ = st_new_ = st_skipped_ = st_late_copy_ = 0; st_t0_ = Now();
    running_ = true;
    // XRL_PACE_DEBUG (bisecting): nothreads = no watcher/presenter (nothing reaches the screen); normalprio = both
    // threads at normal priority
    char dbg[32] = "";
    GetEnvironmentVariableA("XRL_PACE_DEBUG", dbg, sizeof dbg);
    normal_prio_ = strcmp(dbg, "normalprio") == 0;
    if (dbg[0]) Log(LOG_WARN, "pacer: XRL_PACE_DEBUG=%s", dbg);
    if (strcmp(dbg, "nothreads") != 0) {
        watcher_ = std::thread([this] { Watch(); });
        presenter_ = std::thread([this] { Present(); });
    }
    Log(LOG_INFO, "pacer: 120 Hz pacing on, %ux%u fmt %u, flips %.2f ms before each vblank", w, h, (unsigned)host_format_, early_ms);
    return true;
}

void Pacer120::Stop() {
    if (running_.exchange(false)) {
        if (presenter_.joinable()) presenter_.join();
        if (watcher_.joinable()) watcher_.join();   // returns at the next vblank
    }
    if (pctx_) { pctx_->ClearState(); pctx_->Flush(); }
    copied_.Reset();
    for (int i = 0; i < kSlots; i++) { ring_g_[i].Reset(); ring_p_[i].Reset(); }
    gctx3_.Reset();
    hostbuf_.Reset(); host_.Reset(); out_.Reset();
    pctx4_.Reset(); pctx_.Reset(); pdev_.Reset();
    for (HANDLE& e : slot_ev_)
        if (e) { CloseHandle(e); e = nullptr; }
}

void Pacer120::WaitUntil(int64_t t) {
    // the timer floor is 1 ms and it can run late: sleep to ~1.5 ms before, then spin
    while (t - Now() > qf_ * 3 / 2000) Sleep(1);
    while (Now() < t) YieldProcessor();
}

// The window's 60 Hz vblank tick: stamped when WaitForVBlank returns (late by a varying wake-up delay). The period is
// the long-run average; the phase the earliest stamp of the last 32 against it (the delay only ever adds).
void Pacer120::Watch() {
    if (!normal_prio_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t t0 = 0, n = 0, res[32];
    for (int64_t& r : res) r = INT64_MAX;
    while (running_.load()) {
        if (FAILED(out_->WaitForVBlank())) { Sleep(1); continue; }
        int64_t t = Now();
        if (!t0) { t0 = t; continue; }
        ++n;
        double per = (double)(t - t0) / (double)n;
        res[n % 32] = t - t0 - (int64_t)(per * (double)n);
        int64_t best = res[n % 32];
        for (int i = 0; i < 32 && i < n; ++i) best = std::min(best, res[i]);
        if (n >= 16) tick_.store(t0 + (int64_t)(per * (double)n) + best);
    }
}

void Pacer120::Present() {
    if (!normal_prio_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint64_t shown_seq = 0, copied_seq = 0, pending_seq = 0;
    int64_t copy_t0 = 0, copy_est = qf_ / 1000;   // how long a copy takes (rises at once, falls slowly); 1 ms to start
    while (running_.load()) {
        int64_t tk = tick_.load();
        if (!tk) { Sleep(2); continue; }
        // the next flip time on the grid (tick + k * 8.333 ms - early) at least 2.5 ms away: room for the copy
        int64_t base = tk - early_, diff = Now() + qf_ * 5 / 2000 - base;
        if (!grid0_) grid0_ = base;
        int64_t at = base + (diff <= 0 ? 0 : (diff + f120_ - 1) / f120_) * f120_;
        // Until 0.1 ms before the flip: copy each frame as soon as the GPU has finished it (the copy queues behind
        // whatever the GPU is doing, so it starts early), and a newer one over a copied one only while there is time for
        // that copy. One copy at a time; the game keeps off the slot being copied.
        const int64_t limit = at - qf_ / 10000, margin = qf_ / 2000;
        while (Now() < limit) {
            if (pending_seq) {
                BOOL ok = FALSE;
                if (pctx_->GetData(copied_.Get(), &ok, sizeof ok, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && ok) {
                    copied_seq = pending_seq; pending_seq = 0; reading_ = -1; st_new_++;
                    int64_t d = Now() - copy_t0;
                    copy_est = d > copy_est ? d : copy_est - copy_est / 16 + d / 16;
                }
            }
            // a newer frame over a copied one that is not up yet: only at 120 (at 60 each frame is shown, in order)
            if (!pending_seq && (copied_seq == shown_seq || (!lock60_.load() && limit - Now() > copy_est + margin))) {
                // the newest slot whose frame the GPU has finished and that is newer than the copied one
                int best = -1;
                const bool fifo = lock60_.load();   // at 60 every frame is shown, in order (vsync); at 120 the newest
                for (int i = 0; i < kSlots; i++) {
                    uint64_t v = slot_value_[i].load(), s = slot_seq_[i].load();
                    if (v && s > copied_seq && WaitForSingleObject(slot_ev_[i], 0) == WAIT_OBJECT_0 &&
                        (best < 0 || (fifo ? s < slot_seq_[best].load() : s > slot_seq_[best].load()))) best = i;
                }
                if (best >= 0) {
                    reading_ = best;
                    pending_seq = slot_seq_[best].load();
                    copy_t0 = Now();
                    pctx_->CopyResource(hostbuf_.Get(), ring_p_[best].Get());
                    pctx_->End(copied_.Get());
                    pctx_->Flush();
                    continue;
                }
            }
            if (limit - Now() > qf_ * 3 / 2000) Sleep(1);   // the timer floor is 1 ms and it can run late
            else YieldProcessor();
        }
        if (pending_seq) {
            st_late_copy_++;   // a copy still in flight: flipping now would show it half done; it goes up at a later slot
            WaitUntil(at);
        } else if (copied_seq > shown_seq && lock60_.load() && ((at - grid0_ + f120_ / 2) / f120_) % 2) {
            st_odd_++;   // at 60 only every second refresh flips: this one keeps the last frame up
            WaitUntil(at);
        } else if (copied_seq > shown_seq) {
            {
                WaitUntil(at);
                HRESULT hr = host_->Present(0, DXGI_PRESENT_ALLOW_TEARING);
                if (FAILED(hr)) { Log(LOG_ERROR, "pacer: Present 0x%08lX; pacing stops", (unsigned long)hr); running_ = false; break; }
                shown_seq = copied_seq;
                shown_ = shown_seq;
                SetEvent(shown_ev_);
                st_flips_++;
                ComPtr<ID3D11Texture2D> b0;   // a native flip swapchain's buffer 0 is always the next one
                hostbuf_.Reset();
                host_->GetBuffer(0, IID_PPV_ARGS(&hostbuf_));
            }
        } else {
            st_skipped_++;   // no new frame for this slot: the last one stays up
            WaitUntil(at);
        }
        int64_t now = Now();
        if (now - st_t0_ >= qf_ * PerfSeconds()) {
            double secs = (double)(now - st_t0_) / (double)qf_;
            Log(LOG_INFO, "perf: pacer %.1f flips/s (new frames %.1f/s), %.1f slots/s with no new frame, %llu copies late; a copy takes %.2f ms; "
                "cadence %s (game %.1f ms/frame), %llu odd slots held",
                st_flips_ / secs, st_new_ / secs, st_skipped_ / secs, (unsigned long long)st_late_copy_,
                (double)copy_est * 1000.0 / (double)qf_, lock60_.load() ? "60" : "120", work_ema_, (unsigned long long)st_odd_);
            st_flips_ = st_new_ = st_skipped_ = st_late_copy_ = st_odd_ = 0;
            st_t0_ = now;
        }
    }
}

ID3D11Texture2D* Pacer120::Acquire() {
    {   // the game's own time for the frame it is presenting, and the 60/120 decision
        const int64_t now = Now();
        if (acq_ret_) {
            double ms = (double)(now - acq_ret_) * 1000.0 / (double)qf_;
            work_ema_ = work_ema_ ? work_ema_ + (ms - work_ema_) / 16.0 : ms;
        }
        if (!lock_mode_) {
            bool l = lock60_.load(), n = l ? work_ema_ > 7.4 : work_ema_ > 8.6;
            if (n != l) {
                lock60_ = n;
                static int said;
                if (said++ < 100) Log(LOG_INFO, "pacer: cadence %s (the game's own time %.1f ms per frame)", n ? "60" : "120", work_ema_);
            }
        }
        // at 60 (vsync): the game waits until every frame but the one it just submitted is on screen, so it can never run
        // ahead of the display (each frame is shown once, in order: shown_ moves one frame per flip)
        if (lock60_.load())
            for (int i = 0; i < 8 && seq_ >= 1 && shown_.load() + 1 < seq_; i++) WaitForSingleObject(shown_ev_, 25);
    }
    // at most two frames on the GPU: wait for the one before the last
    const int64_t w0 = Now();
    if (prev_[1] >= 0) WaitForSingleObject(slot_ev_[prev_[1]], 100);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); last_wait_ms_ = (double)(Now() - w0) * 1000.0 / (double)f.QuadPart; }
    // a slot that is neither being copied by the presenter nor the newest finished one it may still take
    uint64_t newest = 0;
    int newest_i = -1;
    for (int i = 0; i < kSlots; i++) if (slot_seq_[i].load() > newest) { newest = slot_seq_[i].load(); newest_i = i; }
    for (int k = 1; k <= kSlots; k++) {
        int i = (writing_ + k + kSlots) % kSlots;
        if (i != reading_.load() && i != newest_i) { writing_ = i; break; }
    }
    slot_value_[writing_] = 0;   // not ready until Submit
    acq_ret_ = Now();
    return ring_g_[writing_].Get();
}

void Pacer120::Submit(ID3D11DeviceContext*) {
    ResetEvent(slot_ev_[writing_]);
    slot_seq_[writing_] = ++seq_;
    slot_value_[writing_] = 1;
    gctx3_->Flush1(D3D11_CONTEXT_TYPE_ALL, slot_ev_[writing_]);   // the event is set once the GPU is past this frame
    prev_[1] = prev_[0];
    prev_[0] = writing_;
}

}  // namespace xrl

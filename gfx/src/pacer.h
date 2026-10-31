// pacer.h - XRL_PACE120=1: frames onto the console's real 120 Hz scanout (native D3D11 mode, d3d11_swapchain.cpp).
#pragma once
#include "common.h"

#include <d3d11_4.h>

#include <atomic>
#include <thread>

namespace xrl {

// The console composes an app's window at 60 Hz; only ALLOW_TEARING flips reach the 120 Hz scanout, and they are
// tear-free when each lands just before a real vblank (measured: 0.30 ms early). The window's 60 Hz vblank tick is
// phase-locked to the real 120 Hz one, so a watcher thread stamps it and the flips go on a 120 Hz grid derived from it.
//
// The game never waits for the grid: at its Present the frame is copied into one of three shared textures and the
// game's context is flushed with an event the GPU sets when it is done (Flush1). A presenter thread with its own D3D11 device (its own GPU queue, so its copy is never queued
// behind the game's next frame) takes, for each 120 Hz slot, the newest frame the GPU has finished, copies it to the
// host swapchain and flips it at the slot's time. A slot with no new frame is skipped: the last image stays up. The
// game is held to two frames in flight on the GPU, as a normal swapchain would.
class Pacer120 {
public:
    ~Pacer120() { Stop(); }
    // false: the pieces are not available (the caller presents the usual way)
    bool Start(ID3D11Device* game_dev, uint32_t w, uint32_t h, uint32_t dxgi_format, double early_ms);
    void Stop();
    bool Running() const { return running_.load(); }
    IDXGISwapChain3* Host() const { return host_.Get(); }
    DXGI_FORMAT HostFormat() const { return host_format_; }
    // game thread: the texture to put this frame into (waits while two frames are still on the GPU)
    ID3D11Texture2D* Acquire();
    double last_wait_ms() const { return last_wait_ms_; }   // how long the latest Acquire waited for the GPU
    // game thread: the frame is in the texture Acquire gave; hand it to the presenter
    void Submit(ID3D11DeviceContext* game_ctx);

private:
    static constexpr int kSlots = 3;
    void Present();   // the presenter thread
    void Watch();     // the vblank watcher thread
    int64_t Now() const { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
    void WaitUntil(int64_t t);

    ComPtr<ID3D11Device> pdev_;            // the presenter's own device
    ComPtr<ID3D11DeviceContext> pctx_;
    ComPtr<ID3D11DeviceContext4> pctx4_;
    ComPtr<IDXGISwapChain3> host_;         // made on pdev_, ALLOW_TEARING
    ComPtr<ID3D11Texture2D> hostbuf_;      // its buffer 0
    DXGI_FORMAT host_format_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<IDXGIOutput> out_;
    ComPtr<ID3D11Texture2D> ring_g_[kSlots], ring_p_[kSlots];   // the same textures on the game's / presenter's device
    ComPtr<ID3D11DeviceContext3> gctx3_;   // the game's context: Flush1 with an event marks a frame done
    HANDLE slot_ev_[kSlots] = {};          // set once the GPU has finished the slot's frame
    int prev_[2] = {-1, -1};               // the slots of the last two frames (the game is held to two on the GPU)
    ComPtr<ID3D11Query> copied_;           // presenter: its copy reached the GPU's end
    std::atomic<uint64_t> slot_value_[kSlots] = {};   // 1: a submitted frame is in the slot (done once slot_ev_ is set)
    std::atomic<uint64_t> slot_seq_[kSlots] = {};     // frame number in the slot
    std::atomic<int> reading_{-1};                    // the slot the presenter copies from
    int writing_ = -1;
    double last_wait_ms_ = 0;
    uint64_t seq_ = 0;
    // XRL_PACE_LOCK=auto (default) | 60 | 120: without VRR only an even cadence looks smooth, so a game that cannot hold
    // 120 (its own time per frame over ~8.6 ms) is held to 60: a flip on every second refresh only, and the game waits
    // until the frame before its last one is on screen. Back to 120 below ~7.4 ms.
    int lock_mode_ = 0;                      // 0 auto, 60, 120
    std::atomic<bool> lock60_{false};
    double work_ema_ = 0;                    // ms of the game's own time per frame (from Acquire's return to the next)
    int64_t acq_ret_ = 0;
    std::atomic<uint64_t> shown_{0};         // the newest frame number flipped
    HANDLE shown_ev_ = nullptr;              // set on each flip
    int64_t grid0_ = 0;                      // a flip time all flips are a multiple of 8.33 ms from (even/odd)
    std::atomic<bool> running_{false};
    bool normal_prio_ = false;
    std::atomic<int64_t> tick_{0};                    // QPC time of the latest 60 Hz window tick (estimated)
    std::thread presenter_, watcher_;
    int64_t qf_ = 0, f120_ = 0, early_ = 0;
    // stats (presenter thread), logged every 10 s
    uint64_t st_flips_ = 0, st_new_ = 0, st_skipped_ = 0, st_late_copy_ = 0, st_odd_ = 0;
    int64_t st_t0_ = 0;
};

}  // namespace xrl

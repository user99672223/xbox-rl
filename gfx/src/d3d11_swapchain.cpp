// d3d11_swapchain.cpp — the swapchain a D3D11 game sees, on top of D3D11On12.
//
// A D3D11 program calls IDXGISwapChain::GetBuffer(0, IID_ID3D11Texture2D) once, keeps a render
// target view of that texture for the life of the swapchain, and calls Present. The host's real
// swapchain (swapchain.cpp) is a flip-model D3D12 one: its buffers are ID3D12Resources that rotate
// on every Present. This object bridges the two the way every D3D11-on-D3D12 layer does:
//
//   * GetBuffer(0) returns ONE stable D3D11 texture (`back_`), made on the game's own device, so
//     the game's views stay valid, MSAA and sRGB formats work, and discard/sequential/flip
//     semantics are all satisfied (the contents simply persist).
//   * Present wraps the host swapchain's current buffer with ID3D11On12Device::CreateWrappedResource
//     (done once per buffer), copies/resolves `back_` into it, releases it back to D3D12 in the
//     PRESENT state, flushes, and presents on the host swapchain.
//
// XRL_PACE120=1 (native D3D11 mode): the host swapchain belongs to a Pacer120 (pacer.h), which flips the newest
// finished frame onto the console's 120 Hz scanout; Present here only puts the frame into the pacer's ring.
// XRL_PACE_EARLY_US=<us>: how long before each vblank the flip goes (default 300, the measured tear-free point).
//
// XRL_FSR=<w>x<h>: a frame smaller than that goes to a host swapchain of that size through FSR 1 (fsr.cpp)
// instead of the copy; the game still sees its own size everywhere.
//
// Native D3D11 mode (XRL_D3D11=native, the device has no ID3D11On12Device): the same object, but the
// host swapchain is a D3D11 flip swapchain made from the game's own device, and Present copies/resolves
// `back_` into its buffer 0 on that device (no wrapped resources, no D3D12 queue).
//
// The object is an IDXGISwapChain1 with a native vtable, handed to the guest as a handle like every
// other host COM object (the layer routes the guest's calls through the host-view thunk).
// It is deliberately not IDXGISwapChain2+: GetFrameLatencyWaitableObject would return a native
// handle the guest cannot wait on, so the latency-waitable flag is refused too.
#include "common.h"
#include "fsr.h"
#include "pacer.h"

#include <dxgi1_4.h>

#include <atomic>

namespace xrl {
namespace {

// Flags of DXGI_SWAP_CHAIN_FLAG we can honour: the rest changes how the OS composes the window.
constexpr UINT kAcceptedFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                                DXGI_SWAP_CHAIN_FLAG_NONPREROTATED | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;

class D3D11SwapChain final : public IDXGISwapChain1 {
public:
    D3D11SwapChain(ID3D11Device* dev, ID3D12CommandQueue* queue, const xrl_swapchain_desc& d)
        : dev_(dev), queue_(queue), app_(d) {}

    ~D3D11SwapChain() {
        Drain();
        host_.Reset();
        pacer_.Stop();
        wrapped_.clear();
        bufs_.clear();
        hostbuf_.Reset();
        fsr_.Reset();
        back_.Reset();
        host_.Reset();
        Log(LOG_INFO, "d3d11 swapchain %p destroyed (%llu frames presented)", static_cast<void*>(this),
            (unsigned long long)presents_);
    }

    HRESULT Init() {
        Log(LOG_TRACE, "d3d11 swapchain: Init");
        // Native D3D11 mode: the queue the guest passed is the stand-in of stub12.cpp and unused. Decided by
        // the mode, not by asking the device for ID3D11On12Device: the console's native D3D11 devices answer
        // that QueryInterface too (laptop ones do not), and the stand-in queue then reached DXGI.
        native_ = NativeD3D11();
        if (!native_ && FAILED(dev_.As(&on12_))) return E_FAIL;
        Log(LOG_TRACE, "d3d11 swapchain: %s device, on12 %p", native_ ? "native" : "D3D11On12", (void*)on12_.Get());
        dev_->GetImmediateContext(&ctx_);
        Log(LOG_TRACE, "d3d11 swapchain: ctx %p", (void*)ctx_.Get());
        if (!native_ && FAILED(queue_->GetDevice(IID_PPV_ARGS(&dev12_)))) return E_FAIL;
        FsrConfig();
        if (app_.flags & ~kAcceptedFlags) {
            Log(LOG_ERROR, "d3d11 swapchain: unsupported swapchain flags 0x%X", app_.flags & ~kAcceptedFlags);
            return DXGI_ERROR_UNSUPPORTED;
        }
        if (app_.sample_count == 0) app_.sample_count = 1;
        if (app_.buffer_count == 0) app_.buffer_count = 1;
        // A swapchain created fullscreen reports fullscreen until SetFullscreenState says otherwise: engines
        // compare GetFullscreenState with what they asked for, and on a mismatch treat the swapchain as lost
        // (Unreal minimises the window and stops presenting). The host shows it in its window either way.
        fullscreen_ = !app_.windowed;
        // Host window size for a 0x0 request, as DXGI takes the client rect of the HWND.
        if (!app_.width || !app_.height) {
            uint32_t w = 0, h = 0;
            GetWindowSize(&w, &h);
            if (!app_.width) app_.width = w ? w : 1920;
            if (!app_.height) app_.height = h ? h : 1080;
        }
        Log(LOG_TRACE, "d3d11 swapchain: dev12 %p", (void*)dev12_.Get());
        xrl_status st = native_ && PaceWanted() ? StartPacer()
                      : native_ ? HostSwapchainCreate11(dev_.Get(), HostW(app_), HostH(app_), app_.buffer_count,
                                                        app_.dxgi_format, host_, &host_format_)
                                : HostSwapchainCreate(queue_.Get(), HostW(app_), HostH(app_), app_.buffer_count,
                                                      app_.dxgi_format, host_, &host_format_);
        if (st != XRL_OK) return st == XRL_ERR_NOTAVAIL ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
        Log(LOG_TRACE, "d3d11 swapchain: host swapchain %p", (void*)host_.Get());
        HRESULT hr = BuildBuffers();
        if (FAILED(hr)) return hr;
        Log(LOG_INFO, "d3d11 swapchain %p: %ux%u fmt=%u msaa=%u buffers=%u effect=%u %s hwnd=%llx (%s)", static_cast<void*>(this),
            app_.width, app_.height, app_.dxgi_format, app_.sample_count, app_.buffer_count, app_.swap_effect,
            fullscreen_ ? "fullscreen" : "windowed", (unsigned long long)app_.hwnd, native_ ? "native D3D11" : "D3D11On12");
        return S_OK;
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1)) {
            *ppv = static_cast<IDXGISwapChain1*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        static std::atomic<int> misses;
        if (misses++ < 16)
            Log(LOG_INFO, "d3d11 swapchain: QueryInterface({%08lX-%04X-%04X-...}) -> E_NOINTERFACE", (unsigned long)riid.Data1,
                riid.Data2, riid.Data3);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }

    // ---- IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT size, const void* d) override {
        return host_->SetPrivateData(n, size, d);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override {
        return host_->SetPrivateDataInterface(n, u);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* size, void* d) override {
        return host_->GetPrivateData(n, size, d);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return host_->GetParent(riid, parent); }

    // ---- IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override {
        if (!device) return DXGI_ERROR_INVALID_CALL;
        return dev_->QueryInterface(riid, device);
    }

    // ---- IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override { return DoPresent(sync, flags); }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (!surface) return DXGI_ERROR_INVALID_CALL;
        *surface = nullptr;
        // D3D11 only ever sees buffer 0, flip model included (the runtime rotates behind it).
        if (index != 0) {
            Log(LOG_WARN, "d3d11 swapchain: GetBuffer(%u) refused; D3D11 swapchains expose buffer 0 only", index);
            return DXGI_ERROR_INVALID_CALL;
        }
        std::lock_guard<std::mutex> l(mu_);
        return back_->QueryInterface(riid, surface);
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput*) override {
        // The host window is the host's: nothing to switch. Remember the state so the game's
        // GetFullscreenState and ResizeTarget handshakes stay consistent.
        fullscreen_ = fullscreen != FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
        if (fullscreen) *fullscreen = fullscreen_;
        if (target) *target = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override {
        if (!d) return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> l(mu_);
        *d = {};
        d->BufferDesc.Width = app_.width;
        d->BufferDesc.Height = app_.height;
        d->BufferDesc.RefreshRate = {60, 1};
        d->BufferDesc.Format = (DXGI_FORMAT)app_.dxgi_format;
        d->SampleDesc = {app_.sample_count, app_.sample_quality};
        d->BufferUsage = app_.buffer_usage;
        d->BufferCount = app_.buffer_count;
        d->OutputWindow = (HWND)(uintptr_t)app_.hwnd;
        d->Windowed = app_.windowed && !fullscreen_;
        d->SwapEffect = (DXGI_SWAP_EFFECT)app_.swap_effect;
        d->Flags = app_.flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override {
        return DoResize(count, w, h, fmt, flags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override {
        return mode ? S_OK : DXGI_ERROR_INVALID_CALL;   // the host owns the window size
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override {
        // Wine's dxgi owns the outputs the game enumerated; the host's would be foreign objects.
        if (output) *output = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return host_->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n) override {
        if (!n) return DXGI_ERROR_INVALID_CALL;
        *n = (UINT)presents_;
        return S_OK;
    }

    // ---- IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override {
        if (!d) return DXGI_ERROR_INVALID_CALL;
        std::lock_guard<std::mutex> l(mu_);
        *d = {};
        d->Width = app_.width;
        d->Height = app_.height;
        d->Format = (DXGI_FORMAT)app_.dxgi_format;
        d->SampleDesc = {app_.sample_count, app_.sample_quality};
        d->BufferUsage = app_.buffer_usage;
        d->BufferCount = app_.buffer_count;
        d->Scaling = DXGI_SCALING_STRETCH;
        d->SwapEffect = (DXGI_SWAP_EFFECT)app_.swap_effect;
        d->AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d->Flags = app_.flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override {
        if (!d) return DXGI_ERROR_INVALID_CALL;
        *d = {};
        d->RefreshRate = {60, 1};
        d->Windowed = app_.windowed && !fullscreen_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override {
        if (!hwnd) return DXGI_ERROR_INVALID_CALL;
        *hwnd = (HWND)(uintptr_t)app_.hwnd;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** w) override {
        if (w) *w = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return DoPresent(sync, flags);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override {
        if (o) *o = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override {
        if (c) *c = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override {
        return r == DXGI_MODE_ROTATION_IDENTITY || r == DXGI_MODE_ROTATION_UNSPECIFIED ? S_OK : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override {
        if (r) *r = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
    }

private:
    // Everything the host swapchain's buffers need on the D3D11 side, and the stable game texture.
    HRESULT BuildBuffers() {
        DXGI_SWAP_CHAIN_DESC1 hd{};
        HRESULT hr = host_->GetDesc1(&hd);
        if (FAILED(hr)) return hr;
        if (native_ && pacer_.Running()) {
            // the pacer's swapchain lives on its own device; the game draws into the pacer's ring instead
        } else if (native_) {
            // A D3D11 flip swapchain: buffer 0 is the one the runtime rotates behind; it stays the
            // copy target for the life of the swapchain (or until ResizeBuffers).
            if (FAILED(hr = host_->GetBuffer(0, IID_PPV_ARGS(&hostbuf_)))) return hr;
        } else {
            bufs_.assign(hd.BufferCount, nullptr);
            wrapped_.assign(hd.BufferCount, nullptr);
            for (UINT i = 0; i < hd.BufferCount; i++) {
                if (FAILED(hr = host_->GetBuffer(i, IID_PPV_ARGS(&bufs_[i])))) return hr;
                D3D11_RESOURCE_FLAGS rf{};
                rf.BindFlags = D3D11_BIND_RENDER_TARGET;
                // The wrapped buffer is only a copy destination here; it rests in PRESENT between frames.
                if (FAILED(hr = on12_->CreateWrappedResource(bufs_[i].Get(), &rf, D3D12_RESOURCE_STATE_PRESENT,
                                                             D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped_[i]))))
                    return hr;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = app_.width;
        td.Height = app_.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = (DXGI_FORMAT)app_.dxgi_format;
        td.SampleDesc.Count = app_.sample_count;
        td.SampleDesc.Quality = app_.sample_quality;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (((app_.buffer_usage & DXGI_USAGE_SHADER_INPUT) || Upscaling(app_)) && app_.sample_count == 1)
            td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;   // FSR samples the frame
        if ((app_.buffer_usage & DXGI_USAGE_UNORDERED_ACCESS) && app_.sample_count == 1)
            td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> t;
        hr = dev_->CreateTexture2D(&td, nullptr, &t);
        if (FAILED(hr)) {
            Log(LOG_ERROR, "d3d11 swapchain: cannot create the %ux%u fmt=%u back buffer: hr=0x%08lX", td.Width, td.Height,
                (unsigned)td.Format, (unsigned long)hr);
            return hr;
        }
        back_ = t;
        resolved_.Reset();
        if (Upscaling(app_) && app_.sample_count > 1) {   // FSR samples a resolved copy of an MSAA frame
            td.SampleDesc = {1, 0};
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(hr = dev_->CreateTexture2D(&td, nullptr, &resolved_))) return hr;
        }
        if (Upscaling(app_))
            Log(LOG_INFO, "fsr: upscaling the game's %ux%u to %ux%u with FSR 1 (RCAS sharpness %.2f stops)", app_.width, app_.height,
                HostW(app_), HostH(app_), fsr_sharp_);
        return S_OK;
    }

    // XRL_FSR=<w>x<h>: frames smaller than that are upscaled to it with FSR 1 (the host swapchain is that big; the
    // game keeps its own size). XRL_FSR_SHARP=<stops>: RCAS sharpness, 0 sharpest, default 0.2.
    void FsrConfig() {
        char v[32];
        unsigned w = 0, h = 0;
        if (!GetEnvironmentVariableA("XRL_FSR", v, sizeof v) || sscanf_s(v, "%ux%u", &w, &h) != 2 || !w || !h) return;
        if (GetEnvironmentVariableA("XRL_FSR_SHARP", v, sizeof v)) fsr_sharp_ = (float)atof(v);
        if (fsr_.Init(dev_.Get(), fsr_sharp_)) { fsr_w_ = w; fsr_h_ = h; }
    }
    // Upscale only a smaller UNORM frame (an sRGB view would make FSR work on linear values)
    bool Upscaling(const xrl_swapchain_desc& d) const {
        if (!fsr_w_ || !fsr_.Ready() || (d.width >= fsr_w_ && d.height >= fsr_h_)) return false;
        DXGI_FORMAT f = (DXGI_FORMAT)d.dxgi_format;
        return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    bool PaceWanted() {
        char v[8];
        return GetEnvironmentVariableA("XRL_PACE120", v, sizeof v) && v[0] == '1';
    }
    xrl_status StartPacer() {
        char v[16];
        double early_ms = GetEnvironmentVariableA("XRL_PACE_EARLY_US", v, sizeof v) ? atof(v) / 1000.0 : 0.30;
        if (!pacer_.Start(dev_.Get(), HostW(app_), HostH(app_), app_.dxgi_format, early_ms)) {
            Log(LOG_WARN, "pacer: unavailable; presenting the usual way");
            pacer_.Stop();
            return HostSwapchainCreate11(dev_.Get(), HostW(app_), HostH(app_), app_.buffer_count, app_.dxgi_format, host_, &host_format_);
        }
        host_ = pacer_.Host();
        host_format_ = pacer_.HostFormat();
        return XRL_OK;
    }
    UINT HostW(const xrl_swapchain_desc& d) const { return Upscaling(d) ? fsr_w_ : d.width; }
    UINT HostH(const xrl_swapchain_desc& d) const { return Upscaling(d) ? fsr_h_ : d.height; }
    // The frame onto the host buffer: FSR when upscaling, else a copy (or MSAA resolve)
    void Blit(ID3D11Resource* dst) {
        if (Upscaling(app_)) {
            ID3D11Texture2D* src = back_.Get();
            if (resolved_) { ctx_->ResolveSubresource(resolved_.Get(), 0, back_.Get(), 0, (DXGI_FORMAT)app_.dxgi_format); src = resolved_.Get(); }
            if (SUCCEEDED(fsr_.Run(ctx_.Get(), src, app_.width, app_.height, dst, host_format_, HostW(app_), HostH(app_)))) return;
            static bool said;
            if (!said) { said = true; Log(LOG_WARN, "fsr: a pass failed; this frame is not upscaled"); }
            return;
        }
        if (app_.sample_count > 1)
            ctx_->ResolveSubresource(dst, 0, back_.Get(), 0, host_format_);
        else
            ctx_->CopyResource(dst, back_.Get());
    }

    // Frame pacing numbers, logged every 10 s: frames per second, the average and the worst time
    // between two Presents, and the time this Present (copy + host Present) took.
    void Pace(LARGE_INTEGER q0, UINT sync) {
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        if (!qf_) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf_ = f.QuadPart; }
        if (!win_start_) win_start_ = q1.QuadPart;
        if (last_present_) {
            int64_t gap = q0.QuadPart - last_present_;
            if (gap > win_max_gap_) win_max_gap_ = gap;
        }
        last_present_ = q0.QuadPart;
        win_frames_++;
        win_present_ += q1.QuadPart - q0.QuadPart;
        int64_t span = q1.QuadPart - win_start_;
        if (span < 10 * qf_) return;
        double secs = (double)span / (double)qf_;
        Log(LOG_INFO, "perf: frames %.1f fps (%llu in %.1f s), worst gap %.1f ms, present %.2f ms avg; %ux%u sync %u; "
            "%llu frames so far", (double)win_frames_ / secs, (unsigned long long)win_frames_, secs,
            (double)win_max_gap_ * 1000.0 / (double)qf_, (double)win_present_ * 1000.0 / (double)qf_ / (double)win_frames_,
            app_.width, app_.height, sync, (unsigned long long)presents_);
        if (gpu_n_)
            Log(LOG_INFO, "perf: gpu frame %.2f ms avg, %.2f ms worst (%llu frames timed, %llu disjoint): the GPU's own time from "
                "the start of a frame's commands to the end of them", gpu_sum_ / (double)gpu_n_, gpu_max_,
                (unsigned long long)gpu_n_, (unsigned long long)gpu_disjoint_);
        if (lag_n_)
            Log(LOG_INFO, "perf: gpu backlog %.2f ms avg, %.2f ms worst at a frame's start; frame end reached %.2f ms after the CPU "
                "presented (avg); clocks lined up %.3f ms apart (%s): a backlog near 0 = the GPU waits for the CPU threads, "
                "near a frame or more = the GPU is the limit", lag0_sum_ / (double)lag_n_, lag0_max_, lag1_sum_ / (double)lag_n_,
                cal_spread_ms_, native_ ? "native D3D11" : "D3D11On12");
        cal_due_ = true;
        if (GpuTimingOn()) {   // the GPU's memory: what the OS budgets for this process and what it uses now
            ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIAdapter3> ad3;
            DXGI_QUERY_VIDEO_MEMORY_INFO li{}, ni{};
            if (SUCCEEDED(dev_.As(&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad.As(&ad3)) &&
                SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li)))
            {
                ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni);
                Log(LOG_INFO, "perf: gpu memory local %llu of %llu MiB budget, non-local %llu of %llu MiB",
                    (unsigned long long)(li.CurrentUsage >> 20), (unsigned long long)(li.Budget >> 20),
                    (unsigned long long)(ni.CurrentUsage >> 20), (unsigned long long)(ni.Budget >> 20));
            }
        }
        win_start_ = q1.QuadPart; win_frames_ = 0; win_max_gap_ = 0; win_present_ = 0;
        gpu_sum_ = 0; gpu_max_ = 0; gpu_n_ = 0; gpu_disjoint_ = 0;
        lag0_sum_ = 0; lag0_max_ = 0; lag1_sum_ = 0; lag_n_ = 0;
    }

    // XRL_GPU_TIMING=1: each frame's GPU time from timestamp queries. A frame is bracketed from just after the
    // previous Present to just before this one (the game's commands for it); results are read a few frames later,
    // never waited for. On a GPU-bound frame this is the GPU's work; on a CPU-bound one it also holds the gaps
    // where the GPU waited for commands (then "present" above stays short).
    //
    // The backlog: the frame's start timestamp is flushed to the GPU right away, so the GPU reaches it as soon as
    // it has finished what was queued before. Once per log window, with the GPU idle, a flushed timestamp lines
    // the GPU clock up with QueryPerformanceCounter; after that each start timestamp minus the CPU time it was
    // issued at is the work the GPU still had queued. A GPU-bound game always has a backlog (the CPU runs ahead
    // until Present throttles it); a game held back by its CPU threads has none.
    struct GpuQ { ComPtr<ID3D11Query> dj, t0, t1; bool pending = false; int64_t cpu0 = 0, cpu1 = 0; };
    static constexpr int kGpuQ = 8;
    GpuQ gq_[kGpuQ];
    int gq_i_ = 0;
    bool gq_open_ = false;
    int gpu_on_ = -1;
    double gpu_sum_ = 0, gpu_max_ = 0;
    uint64_t gpu_n_ = 0, gpu_disjoint_ = 0;
    bool cal_due_ = true, cal_ok_ = false;
    double cal_off_ = 0, cal_spread_ms_ = 0;   // GPU seconds minus QPC seconds; how wide the QPC bracket was
    double lag0_sum_ = 0, lag0_max_ = 0, lag1_sum_ = 0;
    uint64_t lag_n_ = 0;

    double QpcSec(int64_t q) {
        if (!qf_) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf_ = f.QuadPart; }
        return (double)q / (double)qf_;
    }
    // Waits for the GPU to go idle, then times one flushed timestamp against QPC. Costs one GPU drain per window.
    void Calibrate() {
        cal_due_ = false;
        D3D11_QUERY_DESC ed{D3D11_QUERY_EVENT, 0}, dd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, td{D3D11_QUERY_TIMESTAMP, 0};
        ComPtr<ID3D11Query> ev, dj, ts;
        if (FAILED(dev_->CreateQuery(&ed, &ev)) || FAILED(dev_->CreateQuery(&dd, &dj)) || FAILED(dev_->CreateQuery(&td, &ts))) return;
        ctx_->End(ev.Get());
        ctx_->Flush();
        const ULONGLONG t_end = GetTickCount64() + 200;
        BOOL done = FALSE;
        while (ctx_->GetData(ev.Get(), &done, sizeof done, 0) == S_FALSE || !done)
            if (GetTickCount64() > t_end) return;
        LARGE_INTEGER a, b;
        ctx_->Begin(dj.Get());
        QueryPerformanceCounter(&a);
        ctx_->End(ts.Get());
        ctx_->End(dj.Get());
        ctx_->Flush();
        QueryPerformanceCounter(&b);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        while (ctx_->GetData(dj.Get(), &d, sizeof d, 0) != S_OK)
            if (GetTickCount64() > t_end) return;
        UINT64 g = 0;
        if (ctx_->GetData(ts.Get(), &g, sizeof g, 0) != S_OK || d.Disjoint || !d.Frequency) return;
        cal_off_ = (double)g / (double)d.Frequency - (QpcSec(a.QuadPart) + QpcSec(b.QuadPart)) / 2.0;
        cal_spread_ms_ = (QpcSec(b.QuadPart) - QpcSec(a.QuadPart)) * 1000.0;
        cal_ok_ = true;
    }

    bool GpuTimingOn() {
        if (gpu_on_ < 0) { char v[4]; gpu_on_ = GetEnvironmentVariableA("XRL_GPU_TIMING", v, sizeof v) && v[0] == '1'; }
        return gpu_on_ == 1 && ctx_ && dev_;
    }
    void GpuFrameEnd() {   // before this frame's copy and Present
        if (!GpuTimingOn() || !gq_open_) return;
        GpuQ& q = gq_[gq_i_];
        ctx_->End(q.t1.Get());
        ctx_->End(q.dj.Get());
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        q.cpu1 = c.QuadPart;
        q.pending = true;
        gq_open_ = false;
        gq_i_ = (gq_i_ + 1) % kGpuQ;
    }
    void GpuCollect() {
        for (GpuQ& q : gq_) {
            if (!q.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx_->GetData(q.dj.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            if (ctx_->GetData(q.t0.Get(), &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx_->GetData(q.t1.Get(), &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            q.pending = false;
            if (dj.Disjoint || !dj.Frequency || b < a) { gpu_disjoint_++; continue; }
            double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
            gpu_sum_ += ms; gpu_n_++;
            if (ms > gpu_max_) gpu_max_ = ms;
            if (cal_ok_ && q.cpu0 && q.cpu1) {
                double l0 = ((double)a / (double)dj.Frequency - cal_off_ - QpcSec(q.cpu0)) * 1000.0;
                double l1 = ((double)b / (double)dj.Frequency - cal_off_ - QpcSec(q.cpu1)) * 1000.0;
                lag0_sum_ += l0; lag1_sum_ += l1; lag_n_++;
                if (l0 > lag0_max_) lag0_max_ = l0;
            }
        }
    }
    void GpuFrameBegin() {   // after Present: the next frame's commands start here
        if (!GpuTimingOn()) return;
        GpuCollect();
        GpuQ& q = gq_[gq_i_];
        if (q.pending) return;   // ring full (results late): this frame goes untimed
        if (!q.dj) {
            D3D11_QUERY_DESC dd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, td{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(dev_->CreateQuery(&dd, &q.dj)) || FAILED(dev_->CreateQuery(&td, &q.t0)) || FAILED(dev_->CreateQuery(&td, &q.t1))) {
                Log(LOG_WARN, "d3d11 swapchain: timestamp queries unavailable; XRL_GPU_TIMING off");
                gpu_on_ = 0;
                return;
            }
        }
        if (cal_due_) Calibrate();
        ctx_->Begin(q.dj.Get());
        ctx_->End(q.t0.Get());
        ctx_->Flush();   // the start timestamp goes to the GPU now, not with the game's first batch
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        q.cpu0 = c.QuadPart;
        gq_open_ = true;
    }

    // Wait until the GPU is done with everything submitted so far (buffers are about to go away).
    HRESULT Drain() {
        if (native_) return DrainNative();
        if (!ctx_ || !queue_ || !dev12_) return S_OK;
        ctx_->Flush();
        return WaitQueueIdle(dev12_.Get(), queue_.Get(), 10000);
    }

    // Native: no queue to signal; an event query tells when the GPU has reached this point.
    HRESULT DrainNative() {
        if (!ctx_ || !dev_) return S_OK;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        if (FAILED(dev_->CreateQuery(&qd, &q))) { ctx_->Flush(); return S_OK; }
        ctx_->End(q.Get());
        ctx_->Flush();
        const ULONGLONG t0 = GetTickCount64();
        for (;;) {
            BOOL done = FALSE;
            HRESULT hr = ctx_->GetData(q.Get(), &done, sizeof done, 0);
            if (hr == S_OK && done) return S_OK;
            if (FAILED(hr)) return hr;
            if (GetTickCount64() - t0 > 10000) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            Sleep(0);
        }
    }

    HRESULT DoPresent(UINT sync, UINT flags) {
        if (sync > 4) sync = 4;
        // Only flags valid for a flip-model swapchain on the host window (no tearing).
        UINT hf = flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE | DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT);
        if (flags & DXGI_PRESENT_TEST) {
            if (pacer_.Running()) return S_OK;   // the presenter thread owns the host swapchain's Present
            HRESULT thr = host_->Present(sync, hf);
            if (tests_++ % 300 == 0)
                Log(LOG_INFO, "d3d11 swapchain %p: Present(TEST) #%llu -> 0x%08lX", static_cast<void*>(this),
                    (unsigned long long)tests_, (unsigned long)thr);
            return thr;
        }
        std::lock_guard<std::mutex> l(mu_);
        LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
        GpuFrameEnd();
        if (native_ && pacer_.Running()) {
            Blit(pacer_.Acquire());
            pacer_.Submit(ctx_.Get());
            presents_++;
            GpuFrameBegin();
            Pace(q0, sync);
            return S_OK;
        }
        if (native_) {
            // buffer 0 of the host swapchain is always the next one to present; Present flushes
            Blit(hostbuf_.Get());
        } else {
            UINT idx = host_->GetCurrentBackBufferIndex();
            ID3D11Resource* dst = wrapped_[idx].Get();
            on12_->AcquireWrappedResources(&dst, 1);
            Blit(dst);
            on12_->ReleaseWrappedResources(&dst, 1);
            ctx_->Flush();   // submits the copy to the queue the host Present below is ordered after
        }
        HRESULT hr = host_->Present(sync, hf);
        if (FAILED(hr)) {
            Log(LOG_ERROR, "d3d11 swapchain: host Present failed: hr=0x%08lX", (unsigned long)hr);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                Log(LOG_ERROR, "d3d11 swapchain: device removed, reason 0x%08lX",
                    (unsigned long)(native_ ? dev_->GetDeviceRemovedReason() : dev12_->GetDeviceRemovedReason()));
            return hr;
        }
        presents_++;
        GpuFrameBegin();
        Pace(q0, sync);
        return hr;
    }

    HRESULT DoResize(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
        std::lock_guard<std::mutex> l(mu_);
        if (flags & ~kAcceptedFlags) return DXGI_ERROR_UNSUPPORTED;
        xrl_swapchain_desc nd = app_;
        if (count) nd.buffer_count = count;
        if (w) nd.width = w;
        if (h) nd.height = h;
        if (fmt != DXGI_FORMAT_UNKNOWN) nd.dxgi_format = (uint32_t)fmt;
        nd.flags = flags;
        HRESULT hr = Drain();
        if (FAILED(hr)) return hr;
        if (native_ && pacer_.Running()) {
            fsr_.Reset();
            host_.Reset();
            pacer_.Stop();
            app_ = nd;
            if (StartPacer() != XRL_OK) return E_FAIL;
            hr = BuildBuffers();
            if (SUCCEEDED(hr)) Log(LOG_INFO, "d3d11 swapchain %p resized (pacer): %ux%u", static_cast<void*>(this), app_.width, app_.height);
            return hr;
        }
        // The host swapchain may only be resized with no reference left to its buffers: drop our
        // wrapped views, flush D3D11On12's deferred destruction, and wait again.
        wrapped_.clear();
        bufs_.clear();
        hostbuf_.Reset();
        fsr_.Reset();
        ctx_->Flush();
        Drain();
        DXGI_FORMAT hf = host_format_;
        if (nd.dxgi_format != app_.dxgi_format) {
            // let HostSwapchainCreate's mapping decide: sRGB and typed formats map to flip formats
            hf = (DXGI_FORMAT)nd.dxgi_format;
            if (hf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) hf = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (hf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) hf = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        UINT hc = nd.buffer_count < 2 ? 2 : nd.buffer_count;
        hr = host_->ResizeBuffers(hc, HostW(nd), HostH(nd), hf, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            Log(LOG_ERROR, "d3d11 swapchain: device removed at ResizeBuffers(%ux%u), reason 0x%08lX", nd.width, nd.height,
                (unsigned long)(native_ ? dev_->GetDeviceRemovedReason() : dev12_->GetDeviceRemovedReason()));
        if (FAILED(hr) && native_) {
            // Native D3D11 on the console: ResizeBuffers answers DXGI_ERROR_INVALID_CALL although we hold
            // no buffer (the laptop's HWND swapchain resizes). Say who else holds buffer 0, then make a
            // new host swapchain at the new size instead (the old one must be gone first: one per window).
            ComPtr<ID3D11Texture2D> b0;
            ULONG refs = 0;
            if (SUCCEEDED(host_->GetBuffer(0, IID_PPV_ARGS(&b0)))) { refs = b0->AddRef(); b0->Release(); refs -= 2; }
            b0.Reset();
            Log(LOG_WARN, "d3d11 swapchain: host ResizeBuffers(%u, %ux%u) failed hr=0x%08lX (native D3D11; %lu other "
                "references to buffer 0): making a new host swapchain", hc, nd.width, nd.height, (unsigned long)hr,
                (unsigned long)refs);
            host_.Reset();
            ctx_->Flush();   // D3D11 destroys lazily: the old swapchain goes on the flush
            xrl_status st = HostSwapchainCreate11(dev_.Get(), HostW(nd), HostH(nd), nd.buffer_count, nd.dxgi_format,
                                                  host_, &hf);
            hr = st == XRL_OK ? S_OK : E_FAIL;
        }
        if (FAILED(hr)) {
            Log(LOG_ERROR, "d3d11 swapchain: host ResizeBuffers(%u, %ux%u, fmt %u) failed: hr=0x%08lX (does the game still hold "
                "references to the buffer or its views?)", hc, nd.width, nd.height, (unsigned)hf, (unsigned long)hr);
            // try to come back to the old size so the swapchain stays usable
            if (host_) (void)BuildBuffers();
            return hr;
        }
        host_format_ = hf;
        app_ = nd;
        hr = BuildBuffers();
        if (SUCCEEDED(hr))
            Log(LOG_INFO, "d3d11 swapchain %p resized: %ux%u fmt=%u buffers=%u", static_cast<void*>(this), app_.width,
                app_.height, app_.dxgi_format, app_.buffer_count);
        return hr;
    }

    std::atomic<ULONG> refs_{1};
    std::mutex mu_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11On12Device> on12_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Device> dev12_;
    ComPtr<IDXGISwapChain3> host_;
    DXGI_FORMAT host_format_ = DXGI_FORMAT_UNKNOWN;
    std::vector<ComPtr<ID3D12Resource>> bufs_;
    std::vector<ComPtr<ID3D11Resource>> wrapped_;
    ComPtr<ID3D11Texture2D> hostbuf_;   // native D3D11 mode: buffer 0 of host_
    ComPtr<ID3D11Texture2D> back_;
    ComPtr<ID3D11Texture2D> resolved_;   // FSR's input when the frame is MSAA
    Fsr1 fsr_;
    Pacer120 pacer_;   // XRL_PACE120: owns the host swapchain, flips onto the 120 Hz scanout
    UINT fsr_w_ = 0, fsr_h_ = 0;   // XRL_FSR output size (0: off)
    float fsr_sharp_ = 0.2f;
    xrl_swapchain_desc app_;
    bool native_ = false;   // the device is a native D3D11 one (no ID3D11On12Device, no D3D12 queue)
    bool fullscreen_ = false;
    uint64_t presents_ = 0;
    int64_t qf_ = 0, win_start_ = 0, last_present_ = 0, win_max_gap_ = 0, win_present_ = 0;
    uint64_t win_frames_ = 0;
    uint64_t tests_ = 0;   // Present(DXGI_PRESENT_TEST) calls (occlusion checks)
};

}  // namespace

xrl_status D3D11SwapchainCreateOp(xrl_handle d3d11_device, xrl_handle d3d12_queue, const xrl_swapchain_desc* desc,
                                  xrl_handle* out) {
    if (!out) return XRL_ERR_GENERIC;
    *out = XRL_INVALID_HANDLE;
    SetLastHr(S_OK);
    if (!d3d11_device || !d3d12_queue || !desc || desc->struct_size < sizeof(xrl_swapchain_desc)) {
        Log(LOG_ERROR, "d3d11_create_swapchain: bad arguments (device %llx queue %llx desc %p)",
            (unsigned long long)d3d11_device, (unsigned long long)d3d12_queue, (const void*)desc);
        return XRL_ERR_GENERIC;
    }
    Log(LOG_INFO, "d3d11_create_swapchain: device %llx queue %llx %ux%u", (unsigned long long)d3d11_device, (unsigned long long)d3d12_queue, desc->width, desc->height);
    // The guest may pass any interface pointer of the device (ID3D11Device*, IDXGIDevice*...).
    ComPtr<ID3D11Device> dev11;
    HRESULT hr = FromHandle<IUnknown>(d3d11_device)->QueryInterface(IID_PPV_ARGS(&dev11));
    if (FAILED(hr)) return Fail(hr, "d3d11_create_swapchain: device is not an ID3D11Device");
    auto* sc = new (std::nothrow) D3D11SwapChain(dev11.Get(), FromHandle<ID3D12CommandQueue>(d3d12_queue), *desc);
    if (!sc) return XRL_ERR_GENERIC;
    hr = sc->Init();
    if (FAILED(hr)) {
        sc->Release();
        return Fail(hr, "d3d11_create_swapchain");
    }
    *out = (xrl_handle)(uintptr_t)static_cast<IUnknown*>(sc);
    return XRL_OK;
}

}  // namespace xrl

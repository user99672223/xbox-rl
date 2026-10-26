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
// The object is an IDXGISwapChain1 with a native vtable, handed to the guest as a handle like every
// other host COM object (the layer routes the guest's calls through the host-view thunk).
// It is deliberately not IDXGISwapChain2+: GetFrameLatencyWaitableObject would return a native
// handle the guest cannot wait on, so the latency-waitable flag is refused too.
#include "common.h"

#include <dxgi1_2.h>

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
        wrapped_.clear();
        bufs_.clear();
        back_.Reset();
        host_.Reset();
        Log(LOG_INFO, "d3d11 swapchain %p destroyed (%llu frames presented)", static_cast<void*>(this),
            (unsigned long long)presents_);
    }

    HRESULT Init() {
        Log(LOG_TRACE, "d3d11 swapchain: Init");
        if (FAILED(dev_.As(&on12_))) {
            Log(LOG_ERROR, "d3d11 swapchain: the device is not a D3D11On12 device (no ID3D11On12Device)");
            return E_NOINTERFACE;
        }
        Log(LOG_TRACE, "d3d11 swapchain: have on12 %p", (void*)on12_.Get());
        dev_->GetImmediateContext(&ctx_);
        Log(LOG_TRACE, "d3d11 swapchain: ctx %p", (void*)ctx_.Get());
        if (FAILED(queue_->GetDevice(IID_PPV_ARGS(&dev12_)))) return E_FAIL;
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
        xrl_status st = HostSwapchainCreate(queue_.Get(), app_.width, app_.height, app_.buffer_count,
                                            app_.dxgi_format, host_, &host_format_);
        if (st != XRL_OK) return st == XRL_ERR_NOTAVAIL ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
        Log(LOG_TRACE, "d3d11 swapchain: host swapchain %p", (void*)host_.Get());
        HRESULT hr = BuildBuffers();
        if (FAILED(hr)) return hr;
        Log(LOG_INFO, "d3d11 swapchain %p: %ux%u fmt=%u msaa=%u buffers=%u effect=%u %s hwnd=%llx", static_cast<void*>(this),
            app_.width, app_.height, app_.dxgi_format, app_.sample_count, app_.buffer_count, app_.swap_effect,
            fullscreen_ ? "fullscreen" : "windowed", (unsigned long long)app_.hwnd);
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
        if ((app_.buffer_usage & DXGI_USAGE_SHADER_INPUT) && app_.sample_count == 1) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
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
        return S_OK;
    }

    // Frame pacing numbers, logged every 10 s: frames per second, the average and the worst time
    // between two Presents, and the time this Present (copy + host Present) took.
    void Pace(LARGE_INTEGER q0, UINT sync) {
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        if (!qf_) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf_ = f.QuadPart; win_start_ = q1.QuadPart; }
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
        win_start_ = q1.QuadPart; win_frames_ = 0; win_max_gap_ = 0; win_present_ = 0;
    }

    // Wait until the GPU is done with everything submitted so far (buffers are about to go away).
    HRESULT Drain() {
        if (!ctx_ || !queue_ || !dev12_) return S_OK;
        ctx_->Flush();
        return WaitQueueIdle(dev12_.Get(), queue_.Get(), 10000);
    }

    HRESULT DoPresent(UINT sync, UINT flags) {
        if (sync > 4) sync = 4;
        // Only flags valid for a flip-model swapchain on the host window (no tearing).
        UINT hf = flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE | DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT);
        if (flags & DXGI_PRESENT_TEST) {
            HRESULT thr = host_->Present(sync, hf);
            if (tests_++ % 300 == 0)
                Log(LOG_INFO, "d3d11 swapchain %p: Present(TEST) #%llu -> 0x%08lX", static_cast<void*>(this),
                    (unsigned long long)tests_, (unsigned long)thr);
            return thr;
        }
        std::lock_guard<std::mutex> l(mu_);
        LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
        UINT idx = host_->GetCurrentBackBufferIndex();
        ID3D11Resource* dst = wrapped_[idx].Get();
        on12_->AcquireWrappedResources(&dst, 1);
        if (app_.sample_count > 1)
            ctx_->ResolveSubresource(dst, 0, back_.Get(), 0, host_format_);
        else
            ctx_->CopyResource(dst, back_.Get());
        on12_->ReleaseWrappedResources(&dst, 1);
        ctx_->Flush();   // submits the copy to the queue the host Present below is ordered after
        HRESULT hr = host_->Present(sync, hf);
        if (FAILED(hr)) {
            Log(LOG_ERROR, "d3d11 swapchain: host Present failed: hr=0x%08lX", (unsigned long)hr);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                Log(LOG_ERROR, "d3d11 swapchain: device removed, reason 0x%08lX",
                    (unsigned long)dev12_->GetDeviceRemovedReason());
            return hr;
        }
        presents_++;
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
        // The host swapchain may only be resized with no reference left to its buffers: drop our
        // wrapped views, flush D3D11On12's deferred destruction, and wait again.
        wrapped_.clear();
        bufs_.clear();
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
        hr = host_->ResizeBuffers(hc, nd.width, nd.height, hf, 0);
        if (FAILED(hr)) {
            Log(LOG_ERROR, "d3d11 swapchain: host ResizeBuffers(%u, %ux%u, fmt %u) failed: hr=0x%08lX (does the game still hold "
                "references to the buffer or its views?)", hc, nd.width, nd.height, (unsigned)hf, (unsigned long)hr);
            // try to come back to the old size so the swapchain stays usable
            (void)BuildBuffers();
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
    ComPtr<ID3D11Texture2D> back_;
    xrl_swapchain_desc app_;
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

// fbpresent.cpp — xrl_gfx_ops::present_framebuffer (ABI v3): show a guest framebuffer in the host window.
//
// The layer's /dev/fb0 (and anything else that has a BGRX picture in memory) hands over pixels; this
// uploads them to a dynamic texture on a native D3D11 device and draws them, aspect-fitted with black bars,
// into the flip-model swapchain of the host window (CoreWindow on the console, the test HWND on the
// desktop: HostSwapchainCreate11, the same swapchain code the guest's own D3D11 path uses). Present(1)
// waits for vblank, so a caller that presents in a loop runs at the display's refresh rate at most.
//
// The device and the swapchain belong to this file, not to the guest: the window has ONE swapchain, so a
// guest that has made its own (a Windows game under Wine) and this must not be used together; the second
// CreateSwapChain fails and the error says why. Everything here is serialized by one mutex (D3D11's
// immediate context is single-threaded); only device creation runs on the gfx thread.
#include "common.h"
#include "fb_shaders.h"

namespace xrl {

namespace {

struct Fb {
    std::mutex mu;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t tw = 0, th = 0;   // texture size
    uint32_t sw = 0, sh = 0;   // swapchain size
    uint64_t frames = 0;
};
Fb& F() {
    static Fb* f = new Fb();
    return *f;
}

// The window's client size in pixels: the real HWND when there is one (it can be resized), else the host's report.
void WindowPixels(uint32_t* w, uint32_t* h) {
    *w = *h = 0;
#if XRL_GFX_HAVE_HWND
    if (HWND hwnd = (HWND)GetTestHwnd()) {
        RECT r{};
        if (GetClientRect(hwnd, &r)) {
            *w = (uint32_t)(r.right - r.left);
            *h = (uint32_t)(r.bottom - r.top);
        }
        return;
    }
#endif
    GetWindowSize(w, h);
}

void ResetAll(Fb& f) {
    f.rtv.Reset();
    f.srv.Reset();
    f.tex.Reset();
    f.sc.Reset();
    f.ctx.Reset();
    f.dev.Reset();
    f.tw = f.th = f.sw = f.sh = 0;
}

HRESULT EnsureDevice(Fb& f) {
    if (f.dev) return S_OK;
    struct Res {
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
    };
    auto res = std::make_shared<Res>();
    HRESULT hr = RunOnGfxThread("fb device", [res]() { return CreateNativeD3D11Impl(res->dev, res->ctx); });
    if (FAILED(hr)) return hr;
    ID3D11Device* d = res->dev.Get();
    hr = d->CreateVertexShader(g_fb_vs, sizeof g_fb_vs, nullptr, f.vs.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = d->CreatePixelShader(g_fb_ps, sizeof g_fb_ps, nullptr, f.ps.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sd, f.smp.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = d->CreateRasterizerState(&rd, f.rs.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) return hr;
    f.dev = res->dev;
    f.ctx = res->ctx;
    return S_OK;
}

HRESULT MakeRtv(Fb& f) {
    ComPtr<ID3D11Texture2D> buf;
    HRESULT hr = f.sc->GetBuffer(0, IID_PPV_ARGS(&buf));
    if (SUCCEEDED(hr)) hr = f.dev->CreateRenderTargetView(buf.Get(), nullptr, f.rtv.ReleaseAndGetAddressOf());
    return hr;
}

}  // namespace

xrl_status FramebufferPresentOp(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t flags) {
    if (!pixels || w == 0 || h == 0 || w > 16384 || h > 16384 || stride < w * 4) return XRL_ERR_GENERIC;
    Fb& f = F();
    std::lock_guard<std::mutex> lk(f.mu);
    SetLastHr(S_OK);

    HRESULT hr = EnsureDevice(f);
    if (FAILED(hr)) return Fail(hr, "present_framebuffer: native D3D11 device");

    uint32_t ww = 0, wh = 0;
    WindowPixels(&ww, &wh);
    if (!ww || !wh) {   // no window yet (or minimized)
        ww = w;
        wh = h;
    }
    if (!f.sc) {
        xrl_status st = HostSwapchainCreate11(f.dev.Get(), ww, wh, 2, DXGI_FORMAT_B8G8R8A8_UNORM, f.sc, nullptr);
        if (st != XRL_OK) return st;
        f.sw = ww;
        f.sh = wh;
        hr = MakeRtv(f);
        if (FAILED(hr)) { f.sc.Reset(); return Fail(hr, "present_framebuffer: backbuffer view"); }
        Log(LOG_INFO, "present_framebuffer: swapchain %ux%u for the window; first picture %ux%u stride %u", ww, wh, w, h, stride);
    } else if (ww != f.sw || wh != f.sh) {
        f.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        f.rtv.Reset();
        hr = f.sc->ResizeBuffers(0, ww, wh, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr)) hr = MakeRtv(f);
        if (FAILED(hr)) { ResetAll(f); return Fail(hr, "present_framebuffer: ResizeBuffers"); }
        f.sw = ww;
        f.sh = wh;
        Log(LOG_INFO, "present_framebuffer: window now %ux%u", ww, wh);
    }

    if (!f.tex || f.tw != w || f.th != h) {
        f.srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = f.dev->CreateTexture2D(&td, nullptr, f.tex.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = f.dev->CreateShaderResourceView(f.tex.Get(), nullptr, f.srv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { f.tex.Reset(); return Fail(hr, "present_framebuffer: picture texture"); }
        f.tw = w;
        f.th = h;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    hr = f.ctx->Map(f.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) return Fail(hr, "present_framebuffer: Map");
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dst = static_cast<uint8_t*>(m.pData);
    if (m.RowPitch == stride) {
        memcpy(dst, src, (size_t)stride * h);
    } else {
        for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * stride, (size_t)w * 4);
    }
    f.ctx->Unmap(f.tex.Get(), 0);

    // aspect-fit: the largest centred rectangle of the picture's shape inside the window
    double s = (double)f.sw / w < (double)f.sh / h ? (double)f.sw / w : (double)f.sh / h;
    D3D11_VIEWPORT vp{};
    vp.Width = (float)(UINT)(w * s + 0.5);
    vp.Height = (float)(UINT)(h * s + 0.5);
    vp.TopLeftX = (float)(UINT)((f.sw - vp.Width) / 2);
    vp.TopLeftY = (float)(UINT)((f.sh - vp.Height) / 2);
    vp.MaxDepth = 1.f;

    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    ID3D11RenderTargetView* rtv = f.rtv.Get();
    f.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    f.ctx->ClearRenderTargetView(rtv, black);
    f.ctx->RSSetViewports(1, &vp);
    f.ctx->RSSetState(f.rs.Get());
    f.ctx->IASetInputLayout(nullptr);
    f.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    f.ctx->VSSetShader(f.vs.Get(), nullptr, 0);
    f.ctx->PSSetShader(f.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = f.srv.Get();
    f.ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = f.smp.Get();
    f.ctx->PSSetSamplers(0, 1, &smp);
    f.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    f.ctx->PSSetShaderResources(0, 1, &none);   // the texture is mapped again next frame

    hr = f.sc->Present((flags & XRL_PRESENT_NOWAIT) ? 0 : 1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Log(LOG_ERROR, "present_framebuffer: device removed (0x%08lX); will rebuild on the next picture", (unsigned long)hr);
        ResetAll(f);
        return Fail(hr, "present_framebuffer: Present");
    }
    if (FAILED(hr)) return Fail(hr, "present_framebuffer: Present");
    if ((++f.frames & 0x3ff) == 1) Log(LOG_INFO, "present_framebuffer: %llu pictures presented", (unsigned long long)f.frames);
    return XRL_OK;
}

}  // namespace xrl

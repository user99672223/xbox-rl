// fsr.cpp - AMD FSR 1 in the D3D11 present path; see fsr.h. Shaders: src/shaders/fsr1.hlsl (precompiled headers).
#include "fsr.h"

// FSR's constant setup, on the CPU (ffx_a.h's portable C path)
#define A_CPU 1
#include "../third_party/fsr1/ffx_a.h"
#include "../third_party/fsr1/ffx_fsr1.h"

namespace xrl {
namespace {
#include "shaders/fsr1_vs.h"
#include "shaders/fsr1_easu_ps.h"
#include "shaders/fsr1_rcas_ps.h"

// Everything the two passes change, taken with the Get* calls (which AddRef) and given back with Set*.
struct SavedState {
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ComPtr<ID3D11InputLayout> il;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> ps_srv;
    ComPtr<ID3D11SamplerState> ps_smp;
    ComPtr<ID3D11Buffer> ps_cb;
    ComPtr<ID3D11RasterizerState> rs;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ComPtr<ID3D11RenderTargetView> rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11BlendState> blend;
    FLOAT blend_factor[4]{};
    UINT sample_mask = 0;
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencil_ref = 0;

    void Save(ID3D11DeviceContext* c) {
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetInputLayout(&il);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &ps_srv);
        c->PSGetSamplers(0, 1, &ps_smp);
        c->PSGetConstantBuffers(0, 1, &ps_cb);
        c->RSGetState(&rs);
        c->RSGetViewports(&nvp, vp);
        ID3D11RenderTargetView* r[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, r, &dsv);
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) rtv[i].Attach(r[i]);
        c->OMGetBlendState(&blend, blend_factor, &sample_mask);
        c->OMGetDepthStencilState(&dss, &stencil_ref);
    }
    void Restore(ID3D11DeviceContext* c) {
        ID3D11ShaderResourceView* none = nullptr;
        c->PSSetShaderResources(0, 1, &none);   // our input off before the game's targets come back
        c->IASetPrimitiveTopology(topo);
        c->IASetInputLayout(il.Get());
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->HSSetShader(hs.Get(), nullptr, 0);
        c->DSSetShader(ds.Get(), nullptr, 0);
        c->GSSetShader(gs.Get(), nullptr, 0);
        c->PSSetShader(ps.Get(), nullptr, 0);
        c->RSSetState(rs.Get());
        c->RSSetViewports(nvp, vp);
        ID3D11RenderTargetView* r[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) r[i] = rtv[i].Get();
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, r, dsv.Get());
        c->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
        c->OMSetDepthStencilState(dss.Get(), stencil_ref);
        c->PSSetShaderResources(0, 1, ps_srv.GetAddressOf());
        c->PSSetSamplers(0, 1, ps_smp.GetAddressOf());
        c->PSSetConstantBuffers(0, 1, ps_cb.GetAddressOf());
    }
};

}  // namespace

bool Fsr1::Init(ID3D11Device* dev, float sharpness) {
    dev_ = dev;
    sharp_ = sharpness;
    HRESULT hr = dev->CreateVertexShader(g_fsr1_vs, sizeof g_fsr1_vs, nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_fsr1_easu_ps, sizeof g_fsr1_easu_ps, nullptr, &easu_);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_fsr1_rcas_ps, sizeof g_fsr1_rcas_ps, nullptr, &rcas_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &lin_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &rs_);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bd, &blend_);
    D3D11_DEPTH_STENCIL_DESC dd{};   // depth and stencil off
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &ds_);
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 4 * 16;   // con0..con3
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&cd, nullptr, &cb_easu_);
    if (SUCCEEDED(hr)) {
        AU1 con[16] = {};
        FsrRcasCon(con, sharp_);
        D3D11_SUBRESOURCE_DATA init{con, 0, 0};
        hr = dev->CreateBuffer(&cd, &init, &cb_rcas_);
    }
    ok_ = SUCCEEDED(hr);
    if (!ok_) Log(LOG_WARN, "fsr: cannot set up (0x%08lX); frames are copied without upscaling", (unsigned long)hr);
    return ok_;
}

void Fsr1::Reset() {
    srvs_.clear();
    rtvs_.clear();
    mid_rtv_.Reset();
    mid_srv_.Reset();
    mid_.Reset();
    mid_w_ = mid_h_ = 0;
}

ID3D11ShaderResourceView* Fsr1::SrvFor(ID3D11Texture2D* t) {
    for (auto& s : srvs_) if (s.t == t) return s.v.Get();
    Srv s{t, nullptr};
    if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &s.v))) return nullptr;
    srvs_.push_back(s);
    return srvs_.back().v.Get();
}

ID3D11RenderTargetView* Fsr1::RtvFor(ID3D11Resource* r, DXGI_FORMAT fmt) {
    for (auto& v : rtvs_) if (v.r == r && v.f == fmt) return v.v.Get();
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Rtv v{r, fmt, nullptr};
    if (FAILED(dev_->CreateRenderTargetView(r, &d, &v.v))) return nullptr;
    rtvs_.push_back(v);
    return rtvs_.back().v.Get();
}

HRESULT Fsr1::Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT sw, UINT sh, ID3D11Resource* dst, DXGI_FORMAT dst_fmt,
                  UINT dw, UINT dh) {
    if (!ok_) return E_FAIL;
    if (!mid_ || mid_w_ != dw || mid_h_ != dh || mid_fmt_ != dst_fmt) {
        mid_rtv_.Reset(); mid_srv_.Reset(); mid_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = dw; td.Height = dh; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = dst_fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &mid_);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(mid_.Get(), nullptr, &mid_rtv_);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(mid_.Get(), nullptr, &mid_srv_);
        if (FAILED(hr)) { mid_.Reset(); return hr; }
        mid_w_ = dw; mid_h_ = dh; mid_fmt_ = dst_fmt;
    }
    if (con_sw_ != sw || con_sh_ != sh || con_dw_ != dw || con_dh_ != dh) {
        AU1 con[16] = {};
        FsrEasuCon(con, con + 4, con + 8, con + 12, (AF1)sw, (AF1)sh, (AF1)sw, (AF1)sh, (AF1)dw, (AF1)dh);
        ctx->UpdateSubresource(cb_easu_.Get(), 0, nullptr, con, 0, 0);
        con_sw_ = sw; con_sh_ = sh; con_dw_ = dw; con_dh_ = dh;
    }
    ID3D11ShaderResourceView* in = SrvFor(src);
    ID3D11RenderTargetView* out = RtvFor(dst, dst_fmt);
    if (!in || !out) return E_FAIL;

    SavedState s;
    s.Save(ctx);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(rs_.Get());
    D3D11_VIEWPORT vp{0, 0, (FLOAT)dw, (FLOAT)dh, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(ds_.Get(), 0);
    ctx->PSSetSamplers(0, 1, lin_.GetAddressOf());
    // EASU: the frame -> mid, at the output size
    ctx->OMSetRenderTargets(1, mid_rtv_.GetAddressOf(), nullptr);
    ctx->PSSetShader(easu_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &in);
    ctx->PSSetConstantBuffers(0, 1, cb_easu_.GetAddressOf());
    ctx->Draw(3, 0);
    // RCAS: mid -> the output
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(1, &out, nullptr);
    ctx->PSSetShader(rcas_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, mid_srv_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, cb_rcas_.GetAddressOf());
    ctx->Draw(3, 0);
    s.Restore(ctx);
    return S_OK;
}

}  // namespace xrl

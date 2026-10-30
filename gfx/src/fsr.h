// fsr.h - AMD FSR 1 (EASU upscale + RCAS sharpen) in the D3D11 present path (d3d11_swapchain.cpp).
#pragma once
#include "common.h"

namespace xrl {

// Upscales a frame onto a bigger render target with two full-screen passes on the game's immediate context.
// Every piece of pipeline state the passes touch is saved first and put back after, so the game never sees them.
class Fsr1 {
public:
    // sharpness: RCAS in stops (0 = sharpest, each +1 halves it); false when the device cannot run it
    bool Init(ID3D11Device* dev, float sharpness);
    bool Ready() const { return ok_; }
    // Drops the cached views and the intermediate texture (the swapchain's buffers are about to change).
    void Reset();
    // src: the frame (sw x sh, sampled through an SRV); dst: the output (dw x dh, drawn through an RTV of dst_fmt)
    HRESULT Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT sw, UINT sh, ID3D11Resource* dst, DXGI_FORMAT dst_fmt,
                UINT dw, UINT dh);

private:
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* t);
    ID3D11RenderTargetView* RtvFor(ID3D11Resource* r, DXGI_FORMAT fmt);

    bool ok_ = false;
    float sharp_ = 0.2f;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> easu_, rcas_;
    ComPtr<ID3D11SamplerState> lin_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> ds_;
    ComPtr<ID3D11Buffer> cb_easu_, cb_rcas_;
    ComPtr<ID3D11Texture2D> mid_;   // EASU's output, RCAS's input (dw x dh)
    ComPtr<ID3D11RenderTargetView> mid_rtv_;
    ComPtr<ID3D11ShaderResourceView> mid_srv_;
    UINT mid_w_ = 0, mid_h_ = 0;
    DXGI_FORMAT mid_fmt_ = DXGI_FORMAT_UNKNOWN;
    UINT con_sw_ = 0, con_sh_ = 0, con_dw_ = 0, con_dh_ = 0;   // the sizes cb_easu_ holds constants for
    struct Srv { ID3D11Texture2D* t; ComPtr<ID3D11ShaderResourceView> v; };
    struct Rtv { ID3D11Resource* r; DXGI_FORMAT f; ComPtr<ID3D11RenderTargetView> v; };
    std::vector<Srv> srvs_;
    std::vector<Rtv> rtvs_;
};

}  // namespace xrl

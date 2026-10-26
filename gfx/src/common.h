// common.h — internal declarations shared by the gfx/ translation units.
// Not part of any public/ABI surface.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winapifamily.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "seam_mirror.h"
#include "xrl_gfx.h"

// The desktop-only HWND swapchain path is compiled only when the desktop
// partition exists. Under a UWP-family build this is 0 and CoreWindow is the
// only window source.
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#define XRL_GFX_HAVE_HWND 1
#else
#define XRL_GFX_HAVE_HWND 0
#endif

// Layout guards for the seams we mirror/consume (x64).
static_assert(sizeof(void*) != 8 || sizeof(xrl_gfx_host_services) == 48,
              "xrl_gfx_host_services layout drifted from host/src/gfx_seam.h");
// v2 appended four slots after release (56 -> 88), then d3d11_create_swapchain (96), v3 present_framebuffer (104); v1 members keep their offsets.
static_assert(sizeof(void*) != 8 || sizeof(xrl_gfx_ops) == 104, "xrl_gfx_ops (ABI v3) layout changed");
static_assert(sizeof(void*) != 8 || offsetof(xrl_gfx_ops, release) == 48, "xrl_gfx_ops v1 prefix moved");

namespace xrl {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- handles
// HANDLE CONVENTION (include/xrl_abi.h: "a d3d12_device is really a native
// ID3D12Device*", and host/src/gfx_ops_default.cpp does the same): an xrl_handle
// IS the raw COM interface pointer, owning exactly ONE reference, dropped by
// xrl_gfx_ops::release(). That is what lets the layer's thunks call native vtable
// methods on a handle with no lookup. No wrapper objects, no tables.
//   device        -> ID3D12Device*
//   queue         -> ID3D12CommandQueue*      (created by the guest, via thunk)
//   swapchain     -> IDXGISwapChain3*
//   buffer        -> ID3D12Resource*
//   d3d11 device  -> ID3D11Device*,  context -> ID3D11DeviceContext*
template <class T>
xrl_handle ToHandle(ComPtr<T>& p) {
    return (xrl_handle)(uintptr_t)static_cast<IUnknown*>(p.Detach());
}
template <class T>
T* FromHandle(xrl_handle h) {
    return reinterpret_cast<T*>((uintptr_t)h);
}

// ---------------------------------------------------------------- logging
enum { LOG_TRACE = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };
void Log(int level, const char* fmt, ...);

// ---------------------------------------------------------------- errors
// Record `hr` as this thread's last HRESULT, log it, and map to xrl_status.
xrl_status Fail(HRESULT hr, const char* what);
xrl_status HrToStatus(HRESULT hr);
void SetLastHr(HRESULT hr);

// ---------------------------------------------------------------- state
struct State {
    std::mutex mu;
    bool inited = false;
    xrl_gfx_config cfg{};
    xrl_gfx_host_services host{};  // copy; host.struct_size==0 when none given
    bool have_host = false;
    xrl_gfx_d3d11_decision d11{};
    bool preflight_done = false;
    bool native_d3d11 = false;  // XRL_D3D11=native in the host environment (read at init)
};
State& G();

// Window source, resolved lazily at swapchain-creation time (the host's window
// may not exist when we are initialised).
void* GetCoreWindow();                       // IUnknown* or NULL; NOT AddRef'd
void GetWindowSize(uint32_t* w, uint32_t* h);
void* GetTestHwnd();                         // desktop builds only, else NULL

DWORD CreateTimeoutMs();
// XRL_D3D11=native: the guest's D3D11 device is a native one (d3d11on12.cpp), not D3D11On12.
bool NativeD3D11();
bool DebugLayerRequested();
void WarnIfUiThread(const char* what);       // warns if caller is cfg.ui_thread_id

// ---------------------------------------------------------------- gfx thread
// Runs `fn` on the gfx thread — the host's (host->run_on_gfx_thread) when given,
// else a library-owned MTA worker (standalone harness) — and waits up to
// timeout_ms. Returns fn's HRESULT, or HRESULT_FROM_WIN32(ERROR_TIMEOUT) if it did
// not finish; the job is then abandoned: its captured state outlives the caller
// (shared_ptr) and is never touched by the caller again. fn may still run later.
HRESULT RunOnGfxThread(const char* what, std::function<HRESULT()> fn, DWORD timeout_ms = 0);

// ---------------------------------------------------------------- dxgi/device.cpp
// Process-wide DXGI factory, created lazily ON THE GFX THREAD.
HRESULT GetFactory(ComPtr<IDXGIFactory4>& out);
// Hardware adapter the guest's devices are made on (gfx-thread side). `d3d12` selects the probe: an
// adapter that can make a FL11_0 D3D12 device, or (native D3D11 mode) a FL11_0 D3D11 one.
HRESULT PickAdapter(bool d3d12, ComPtr<IDXGIAdapter1>& out, DXGI_ADAPTER_DESC1* desc_out);
// Adapter picked + device created at FL 11_0 (gfx-thread side).
HRESULT CreateDeviceImpl(ComPtr<ID3D12Device>& out, DXGI_ADAPTER_DESC1* desc_out);
xrl_status DeviceCreateOp(xrl_handle* out_device);
// ABI v2 get_adapter_info: identity + max FL of the adapter d3d12_create_device uses.
xrl_status AdapterInfoOp(xrl_adapter_info* out);
std::string Narrow(const wchar_t* w);

// Wait for the queue to drain via a fresh fence. S_OK, or failure / timeout /
// device-removed reason.
HRESULT WaitQueueIdle(ID3D12Device* dev, ID3D12CommandQueue* q, DWORD timeout_ms);

// Native D3D11 mode: stand-ins for the D3D12 device and queue the guest's d3d11.dll insists on
// creating first (stub12.cpp): no D3D12 device exists, every method but CreateCommandQueue fails.
HRESULT CreateStubD3D12Device(ComPtr<ID3D12Device>& out);

// ---------------------------------------------------------------- caps.cpp
void FillAndLogCaps(ID3D12Device* dev, xrl_gfx_caps* out);

// ---------------------------------------------------------------- d3d11on12.cpp
HRESULT CreateD3D11On12Impl(ID3D12Device* dev, ID3D12CommandQueue* q, ComPtr<ID3D11Device>& d11,
                            ComPtr<ID3D11DeviceContext>& ctx);
xrl_status D3D11On12CreateOp(xrl_handle d3d12_device, xrl_handle d3d12_queue, xrl_handle* out_dev,
                             xrl_handle* out_ctx);
void ApplyD3D11Decision(const xrl_gfx_d3d11_decision& d);
// Native D3D11 mode (gfx-thread side): an ID3D11Device + immediate context made straight on the
// adapter at FL11_0 with BGRA support.
HRESULT CreateNativeD3D11Impl(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx);

// ---------------------------------------------------------------- swapchain.cpp
xrl_status SwapchainCreateOp(xrl_handle device, xrl_handle queue, uint32_t w, uint32_t h, uint32_t buffer_count,
                             uint32_t dxgi_format, xrl_handle* out);
xrl_status SwapchainGetBufferOp(xrl_handle sc, uint32_t index, xrl_handle* out);
xrl_status SwapchainPresentOp(xrl_handle sc, uint32_t sync, uint32_t flags);
// The host-window flip swapchain both swapchain kinds are built on (see swapchain.cpp).
xrl_status HostSwapchainCreate(ID3D12CommandQueue* queue, uint32_t w, uint32_t h, uint32_t buffer_count,
                               uint32_t dxgi_format, ComPtr<IDXGISwapChain3>& out, DXGI_FORMAT* out_format);

// Same for a native D3D11 device (native D3D11 mode): a flip-model swapchain of the host window made
// from the device itself; the D3D11 buffer 0 of it is the copy target.
xrl_status HostSwapchainCreate11(ID3D11Device* dev, uint32_t w, uint32_t h, uint32_t buffer_count,
                                 uint32_t dxgi_format, ComPtr<IDXGISwapChain3>& out, DXGI_FORMAT* out_format);

// ---------------------------------------------------------------- d3d11_swapchain.cpp
xrl_status D3D11SwapchainCreateOp(xrl_handle d3d11_device, xrl_handle d3d12_queue, const xrl_swapchain_desc* desc,
                                  xrl_handle* out);

// ---------------------------------------------------------------- fbpresent.cpp
xrl_status FramebufferPresentOp(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t flags);

// ---------------------------------------------------------------- ops.cpp
xrl_gfx_ops* MutableOps();
void ResetOps();  // re-arm d3d11on12_create (called from init)

}  // namespace xrl

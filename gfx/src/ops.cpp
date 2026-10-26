// ops.cpp — the xrl_gfx_ops vtable (ABI v3) and its thunks.
//
// Each slot is a thin function that forwards to the implementation in
// device.cpp / swapchain.cpp / d3d11on12.cpp. The table is mutable only so that
// a failed D3D11On12 preflight can NULL d3d11on12_create (ABI contract); the host
// must run the preflight before installing the table / starting the guest.
#include "common.h"

namespace xrl {
namespace {

xrl_status Op_d3d12_create_device(xrl_handle* out) { return DeviceCreateOp(out); }

xrl_status Op_create_swapchain(xrl_handle device, xrl_handle queue, uint32_t w, uint32_t h, uint32_t n,
                               uint32_t fmt, xrl_handle* out) {
    return SwapchainCreateOp(device, queue, w, h, n, fmt, out);
}

xrl_status Op_swapchain_get_buffer(xrl_handle sc, uint32_t i, xrl_handle* out) { return SwapchainGetBufferOp(sc, i, out); }

xrl_status Op_swapchain_present(xrl_handle sc, uint32_t sync, uint32_t flags) {
    return SwapchainPresentOp(sc, sync, flags);
}

xrl_status Op_d3d11on12_create(xrl_handle dev, xrl_handle q, xrl_handle* od, xrl_handle* oc) {
    return D3D11On12CreateOp(dev, q, od, oc);
}

// Handle == raw COM interface pointer owning one reference (see common.h).
void Op_release(xrl_handle h) {
    if (h) FromHandle<IUnknown>(h)->Release();
}

// ---- ABI v2 slots (the former "extras", now in the table) ----
xrl_status Op_swapchain_get_current_index(xrl_handle sc, uint32_t* out) { return xrl_gfx_swapchain_current_index(sc, out); }
xrl_status Op_swapchain_resize(xrl_handle sc, uint32_t n, uint32_t w, uint32_t h, uint32_t fmt) {
    return xrl_gfx_swapchain_resize(sc, n, w, h, fmt);
}
int32_t Op_last_hresult(void) { return xrl_gfx_last_hresult(); }
xrl_status Op_get_adapter_info(xrl_adapter_info* out) { return AdapterInfoOp(out); }
xrl_status Op_d3d11_create_swapchain(xrl_handle dev, xrl_handle q, const xrl_swapchain_desc* desc, xrl_handle* out) {
    return D3D11SwapchainCreateOp(dev, q, desc, out);
}

xrl_status Op_present_framebuffer(const void* px, uint32_t w, uint32_t h, uint32_t stride, uint32_t flags) {
    return FramebufferPresentOp(px, w, h, stride, flags);
}

xrl_gfx_ops g_ops = {
    sizeof(xrl_gfx_ops),
    Op_d3d12_create_device,
    Op_create_swapchain,
    Op_swapchain_get_buffer,
    Op_swapchain_present,
    Op_d3d11on12_create,
    Op_release,
    Op_swapchain_get_current_index,
    Op_swapchain_resize,
    Op_last_hresult,
    Op_get_adapter_info,
    Op_d3d11_create_swapchain,
    Op_present_framebuffer,
};

}  // namespace

xrl_gfx_ops* MutableOps() { return &g_ops; }

void ResetOps() { g_ops.d3d11on12_create = Op_d3d11on12_create; }

}  // namespace xrl

extern "C" XRL_GFX_API const xrl_gfx_ops* xrl_gfx_get_ops(void) { return xrl::MutableOps(); }

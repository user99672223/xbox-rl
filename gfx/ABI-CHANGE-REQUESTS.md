# ABI change requests from Workstream D (gfx/)

Against `include/xrl_abi.h` (ABI v1). I have NOT edited the header. Everything
below is a request for the integrator. Items are ordered by how much they block.

Two things first, because they are the questions the brief asked:

- **Does `create_swapchain_for_corewindow` need a CoreWindow parameter?  No.**
  Only `host/` and `gfx/` ever see the CoreWindow; the Wine side must not. The
  host already exposes it through `xrl_gfx_host_services::get_core_window()`
  (host/src/gfx_seam.h) and gfx/ calls that at swapchain-creation time. Adding a
  window argument to the vtable would push a WinRT pointer across the seam for
  no benefit. Keep the signature. (The op is merely *misnamed*: it is the
  swapchain-for-the-host-window op. Not worth a rename.)
- **How does the guest get the command queue?** It creates its own, by calling
  `ID3D12Device::CreateCommandQueue` through a native-vtable thunk on the device
  handle (see R1), then passes that queue handle to `create_swapchain...` and
  `d3d11on12_create`. gfx/ does not create or own a queue; it requires a non-NULL
  queue handle (`queue == XRL_INVALID_HANDLE` is an error, and says so in the log).

---

## R1 (BLOCKER for Wine-side d3d12.dll): handle == native COM pointer must be a *contract*, and the layer needs vtable thunks

xrl_abi.h says "a d3d12_device is really a native ID3D12Device*", and
host/src/gfx_ops_default.cpp + gfx/ both implement exactly that: **an `xrl_handle`
is the raw COM interface pointer, owning one reference, dropped by
`xrl_gfx_ops::release()`**. But the vtable only covers *creation, swapchain
present and release*. Everything else the game does (CreateCommittedResource,
CreateCommandList, ExecuteCommandLists, Signal, ... ~300 ID3D12*/ID3D11* methods)
has no op. The only way that works is for the layer (C) to expose generic
SysV->MS-x64 thunks that call a vtable slot on a handle: `native_call(handle,
vtable_index, args...)` (plus callbacks for the few COM out-interfaces).

Request: state in xrl_abi.h that handles are raw interface pointers (promote the
current prose to a guarantee, per interface type listed in `gfx/include/xrl_gfx.h`),
and have the integrator assign who builds the thunk generator (B/C). Nothing in
gfx/ depends on how that is done; gfx/ only guarantees the handle convention.
If the integrator would rather keep explicit ops, then every method the game
uses needs one, which is not realistic.

## R2: append these ops to `xrl_gfx_ops` (append-only, versioned by `struct_size`; no `XRL_ABI_VERSION` bump needed)

gfx/ already implements all of them as plain C entry points (`xrl_gfx.h`); the
request is to expose them in the vtable so the Wine side does not have to link gfx/:

```c
    /* IDXGISwapChain3::GetCurrentBackBufferIndex. Flip-model swapchains rotate
     * buffers; the guest must know which one to draw into. */
    xrl_status (*swapchain_get_current_index)(xrl_handle swapchain, uint32_t *out_index);

    /* IDXGISwapChain::ResizeBuffers. 0 = keep current buffer_count/width/height/
     * format. Fails unless every buffer handle from swapchain_get_buffer has been
     * release()d. RL changes display mode at startup. */
    xrl_status (*swapchain_resize)(xrl_handle swapchain, uint32_t buffer_count,
                                   uint32_t width, uint32_t height,
                                   uint32_t dxgi_format);

    /* Native HRESULT behind the last failing call on the calling thread, so the
     * Wine DLL can return an accurate DXGI_ERROR_* instead of a generic failure.
     * (xrl_status deliberately loses this; DXGI_ERROR_DEVICE_REMOVED vs
     * _INVALID_CALL vs _UNSUPPORTED matter to games.) */
    int32_t    (*last_hresult)(void);
```

Fallback if R1 is *not* accepted (no vtable thunks): additionally append

```c
    xrl_status (*d3d12_create_command_queue)(xrl_handle device, uint32_t type /*D3D12_COMMAND_LIST_TYPE*/,
                                             int32_t priority, uint32_t flags,
                                             xrl_handle *out_queue);
```

(gfx/ has the underlying code trivially; it was dropped from the library
because R1's handle convention makes it unnecessary.)

## R3: adapter info for Wine's dxgi (EnumAdapters / GetDesc / QueryVideoMemoryInfo)

Wine's `dxgi.dll` has to answer `IDXGIAdapter::GetDesc`, `IDXGIAdapter3::
QueryVideoMemoryInfo` and "max feature level" without native types. Nothing in
v1 gives it that. gfx/ already produces all of it (`xrl_gfx_caps`, logged on the
console too). Request one more op, with a plain struct:

```c
typedef struct xrl_gfx_adapter_info {
    uint32_t struct_size;
    char     name[128];                 /* UTF-8, e.g. "SraKmd_arden"          */
    uint32_t vendor_id, device_id, revision;
    uint64_t dedicated_video_bytes, dedicated_system_bytes, shared_system_bytes;
    uint32_t max_feature_level;         /* D3D_FEATURE_LEVEL value, 0xb000=11_0 */
    uint64_t budget_local, usage_local, budget_nonlocal, usage_nonlocal;
} xrl_gfx_adapter_info;

    xrl_status (*get_adapter_info)(xrl_gfx_adapter_info *out);   /* live budget each call */
```

Also decide the policy: game memory behaviour will key off the 128 MiB
"dedicated" / 1280 MiB "shared" the adapter reports. Whether Wine should report
those literally or lie upward (e.g. report the UMA total) is an integrator
decision; gfx/ reports the truth.

## R4 (documentation-only, no field change): clarify existing ops

- `create_swapchain_for_corewindow`: `queue` is the guest-created direct queue
  (raw `ID3D12CommandQueue*`), must be non-NULL, and must be the **same** queue
  later passed to `d3d11on12_create` (D3D11On12 contract). `out_swapchain` is an
  `IDXGISwapChain3*`; `out_resource` from `swapchain_get_buffer` is an
  `ID3D12Resource*`. `dxgi_format` sRGB variants are accepted but the buffers are
  created as the matching UNORM format (flip model forbids sRGB buffers); the
  guest must create an sRGB RTV view itself. Formats outside {B8G8R8A8,
  R8G8B8A8, R10G10B10A2, R16G16B16A16F} +sRGB fail with XRL_ERR_GENERIC.
- `d3d12_create_device`: always created at **minimum FL 11_0** (the console's
  AppContainer cap). A guest `D3D12CreateDevice(..., MinimumFeatureLevel > 11_0)`
  must be failed by the Wine side using `max_feature_level` from R3; gfx/ will
  not fake a higher level.
- `d3d11on12_create`: the Wine-side `d3d11.dll`/`dxgi.dll` pair is responsible
  for wrapping each swapchain buffer with `ID3D11On12Device::CreateWrappedResource`
  and bracketing draws with `AcquireWrappedResources`/`ReleaseWrappedResources`
  before `Present`. `D3D11On12CreateDevice` does NOT do this for you. All
  reachable through the R1 thunks (QI the d3d11 device handle for
  `ID3D11On12Device`); gfx/'s `xrl_gfx_selftest` demonstrates the exact sequence.

## R5 (for A, not the ABI): `host/src/gfx_seam.h` and gfx/src/seam_mirror.h must stay identical

gfx/ cannot include `host/` headers (it must build standalone), so it carries a
byte-identical copy of `xrl_gfx_host_services` in `gfx/src/seam_mirror.h`. If A
changes the struct, that file must change in the same commit. Suggest A adds a
`static_assert(sizeof(xrl_gfx_host_services) == 48)` (x64) next to the struct;
D's copy is also 48 bytes (checked by the build).

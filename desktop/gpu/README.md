# OpenGL (and later Vulkan) on the console's D3D12

Linux programs on the layer get OpenGL from Mesa's **d3d12** gallium driver, the driver WSLg uses: it translates
OpenGL into D3D12 command lists and DXIL shaders. On WSL it talks to Microsoft's Linux build of the D3D12 runtime;
here it talks to **the console's own D3D12** (a Windows DLL in the host app) directly, through Win64 COM.

```
glxgears / kwin_x11 / Firefox (Linux, glibc)
  -> libGLX_mesa (glvnd) -> DRI software winsys (XPutImage / MIT-SHM to the X server)
  -> Mesa d3d12 driver, built with XRL_WIN64_ABI: every D3D12 method call is a Win64 (ms_abi) COM call
       -> native ID3D12Device / queue / resources of the console        <- layer/src/com.c makes each call safe
  -> libd3d12.so + libdxcore.so = libxrl-d3d12.so (xrl-d3d12/): the few non-method entry points
       D3D12CreateDevice      -> XRL_SYS_GET_GFX_OPS -> xrl_gfx_ops.d3d12_create_device (the host's device)
       DXCore adapter         -> xrl_gfx_ops.get_adapter_info
       root signature serializer, DXIL/RTS0 container signing (written here, checked against fxc's output)
```

## Pieces

| File | What |
|---|---|
| `build-mesa.sh <out.tar.gz>` | Builds everything for Debian 13 into `/opt/xrl-gpu` (in docker `debian:trixie` or a debootstrap chroot); the tarball unpacks at the rootfs root. Inputs pinned: Mesa 25.0.7 (sha256), DirectX-Headers v1.614.1 (commit). |
| `patches/mesa-25.0.7-xrl-win64-abi.patch` | Mesa: MSVC struct returns (`d3d12_common.h`), fences without eventfds (`d3d12_fence.*`), DXIL signed after `nir_to_dxil` (`d3d12_compiler.cpp`), `xrl_dxil_hash.c` in the build, llvmpipe fallback when `GALLIUM_DRIVER=d3d12` cannot start (`sw_helper.h`). |
| `xrl-d3d12/xrl_d3d12.cpp` | The shim library (exports: `D3D12CreateDevice`, `D3D12SerializeVersionedRootSignature`, `D3D12SerializeRootSignature`, `D3D12EnableExperimentalFeatures` (no-op), `D3D12GetDebugInterface` (E_NOINTERFACE), `DXCoreCreateAdapterFactory`). |
| `xrl-d3d12/xrl_dxil_hash.c` | The container digest D3D12 checks (DXC's "retail" MD5 variant). |
| `tests/rootsig-test.sh` | Serializer + digest against Microsoft-compiled blobs (vkd3d's test data, pinned): 24 root signatures byte-identical, 66 shader digests equal. Runs in `build-mesa.sh`. |
| `/opt/xrl-gpu/bin/xrl-gpu <prog>` | Runs a program with this Mesa (LD_LIBRARY_PATH, the glvnd EGL vendor file, `GALLIUM_DRIVER=d3d12` as on WSLg). The patch lets a d3d12 screen that cannot start fall back to llvmpipe (stock Mesa gives up when GALLIUM_DRIVER names a driver); `XRL_GPU_DRIVER=llvmpipe` picks the CPU. |

## The ABI decisions

- **Calling convention.** DirectX-Headers' WSL stubs define `STDMETHODCALLTYPE`, `WINAPI`, `__stdcall` as nothing
  (SysV, for Microsoft's SysV-built Linux runtime). With `-DXRL_WIN64_ABI` they are `__attribute__((ms_abi))`, so
  GCC emits Win64 calls (this in rcx, 32-byte shadow space, xmm6-15 preserved by the callee) for every COM method and
  function pointer. The vtable layout of single-inheritance, non-overloaded MIDL interfaces is the same in GCC and MSVC.
- **Methods that return a struct** (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`, `GetAdapterLuid`, ...): MSVC
  returns them through a hidden pointer passed after `this`, even when small. DirectX-Headers already spells that
  out for MinGW (`T *GetDesc(T *RetVal)`); `XRL_WIN64_ABI` selects those declarations, and Mesa's own wrappers in
  `d3d12_common.h` follow.
- **Types.** WSL's headers use Windows sizes for `LONG`/`ULONG`/`HRESULT`/`BOOL` (32-bit), so the structs match.
  `WCHAR` stays `wchar_t` (4 bytes): only `SetName`/`OpenSharedHandleByName` take wide strings, and the GL driver
  calls neither on this path.
- **Fences.** Windows D3D12's `SetEventOnCompletion` takes a Win32 event handle, not an eventfd. The patch uses no
  event: a finite wait polls `GetCompletedValue` (20 us doubling to 1 ms), an infinite one calls
  `SetEventOnCompletion(value, NULL)`, which blocks inside the runtime until the fence is reached.
- **Shader signing.** Mesa on Linux turns on experimental shader models so unsigned DXIL is accepted; on Windows
  that needs developer mode, and the console's runtime may refuse it. Instead the patch writes the container digest
  that `dxil.dll` would (`xrl_dxil_hash.c`, verified against fxc), and the shim's `D3D12EnableExperimentalFeatures`
  succeeds without doing anything. Same digest on the shim's root signature containers.
- **No DXCore, no DXGI.** The device is the host's (`xrl_gfx_ops.d3d12_create_device`, created once per process
  and shared by every GL screen); the "adapter" Mesa enumerates is a guest object describing it from
  `get_adapter_info`. Adapter memory budget: the adapter's sizes, usage 0 (no host query yet).

## Presenting

First step: the DRI software winsys. Each frame Mesa copies the back buffer from the GPU into a readback buffer and
the winsys puts it into the X server (MIT-SHM when available). Then the X picture reaches the TV through the bridge
(Xvfb -> xbridge -> /dev/fb0 -> the host's `present_framebuffer`). Zero-copy presentation (the GL back buffer as a
D3D12 texture handed to the host's swapchain) is step 3, with A.

## Testing

- Local (any Linux): `bash desktop/gpu/build-mesa.sh out/xrl-gpu.tar.gz` (runs `tests/rootsig-test.sh`). Off the
  layer, `xrl-gpu glxinfo -B` reports llvmpipe (the shim finds no host op table and the d3d12 screen is skipped).
- CI (`tools/ci/probes-gpu.list`, milestones 8 and 9): the test host with `XRL_REAL_GFX=1 XRL_GFX_WARP=1` gives the
  guest gfx/'s real op table on WARP, the D3D12 software rasterizer of the Windows runner. `XRL_D3D12_DEBUG=1` in the
  guest logs every shim call.
- Xbox: the desktop app's gfx op table (hardware D3D12; not in `XRL_D3D11=native` mode, which hands out a stand-in
  device).

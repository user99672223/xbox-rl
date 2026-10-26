# Workstream D (gfx/) — design notes, seams, status

Integrator: merge decisions into `docs/decisions.md`; this file is the working
record. Ordered: what exists, the seams (exactly how the host passes things),
how to wire/run, what is verified vs not, the DXVK->Dozen fallback, open issues.

## 1. What exists (all under `gfx/`)

| Path | What |
|------|------|
| `include/xrl_gfx.h` | Host-facing C API (init, `xrl_gfx_ops_get`, caps, preflight/decision, selftest, probe). No D3D/WinRT types. |
| `src/core.cpp` | Logging, HRESULT->`xrl_status`, host-services plumbing, gfx-thread dispatch (host's thread or library worker), init/shutdown, **`xrl_gfx_ops_get`** |
| `src/device.cpp` | DXGI factory, adapter pick, `d3d12_create_device`, fence drain helper |
| `src/swapchain.cpp` | Flip-model `CreateSwapChainForCoreWindow` (+ desktop HWND test path), get_buffer, present, current-index, resize |
| `src/d3d11on12.cpp` | `d3d11on12_create` (native path) + **the fallback decision point** (`xrl_gfx_preflight`) |
| `src/caps.cpp` | `xrl_gfx_report_caps` — the console caps log |
| `src/ops.cpp` | The `xrl_gfx_ops` table |
| `src/selftest.cpp` | `xrl_gfx_selftest` (clear + present + GPU-readback, via D3D12 and via D3D11On12-wrapped backbuffers, then resize) and `xrl_gfx_run_probe` |
| `src/seam_mirror.h` | Private byte-identical copy of A's `xrl_gfx_host_services` (see ABI-CHANGE-REQUESTS R5) |
| `harness/desktop_harness.cpp` | Dev-PC test exe (real GPU, HWND swapchain, fake host services) |
| `CMakeLists.txt`, `build.ps1` | Desktop build + harness; UWP-family build + UWP link check |
| `ABI-CHANGE-REQUESTS.md` | What ABI v1 can't express (read this) |

## 2. Seams — exactly how things cross

### 2.1 Entry point (A's existing seam, adopted unchanged)
`host/src/gfx_seam.h` already defines it: gfx.lib exports ONE strong symbol

```c
const xrl_gfx_ops *xrl_gfx_ops_get(const xrl_gfx_host_services *host);
```

which overrides A's `/alternatename:xrl_gfx_ops_get=xrl_gfx_ops_get_default`.
Verified by a scratch link test: a TU with the alternatename default linked
against `xrl_gfx.lib` gets gfx/'s function, not the default. A passes
`/p:XrlGfxLib=<gfx\build-uwp\Release\xrl_gfx.lib>` and adds
`d3d12.lib;dxgi.lib;d3d11.lib` (A's code already links them). The lib is built
with `/MD` (CMake default) so it matches a normal UWP project's CRT; the UWP
build imports `MSVCP140_APP/VCRUNTIME140_APP` (A's package needs the VCLibs
framework dependency, which a C++/WinRT UWP vcxproj already has).

`xrl_gfx_ops_get` does not touch D3D (no device creation), so calling it from
anywhere is safe. It is idempotent (re-binds the services).

### 2.2 CoreWindow — how the host hands it over
**Through `xrl_gfx_host_services::get_core_window()`** (already implemented by A
as `svc_get_core_window`). It returns the `IUnknown*` ABI pointer of the
`CoreWindow`; gfx/ calls it **lazily, at swapchain-creation time**, because the
window may not exist when `xrl_gfx_ops_get` runs. gfx/ never AddRefs it, never
calls a method or property on it (those are UI-thread affine), and only passes
it as `IUnknown*` to `IDXGIFactory2::CreateSwapChainForCoreWindow`. A's note
says the pointer is agile and may be used from the gfx thread; **that is the
single biggest assumption gfx/ inherits and is unverified on the console** (§5).
Static alternative for hosts not using services: `xrl_gfx_config.core_window`.
Window size: `get_window_size()` (physical pixels) is used when the guest asks
for width/height 0; fallback 1920x1080 with a warning. No ABI change needed (see the intro of ABI-CHANGE-REQUESTS.md).

### 2.3 Command queue — how it reaches us
gfx/ creates **no** queue. The guest creates its direct queue through a native
vtable call on the device handle and passes the queue handle to
`create_swapchain_for_corewindow` and `d3d11on12_create` (the same one to both:
D3D11On12 contract). Handles are **raw COM pointers** (A's convention, and what
xrl_abi.h says in prose):

| handle | native type |
|---|---|
| device | `ID3D12Device*` |
| queue | `ID3D12CommandQueue*` |
| swapchain | `IDXGISwapChain3*` |
| swapchain buffer | `ID3D12Resource*` |
| d3d11 device / context | `ID3D11Device*` / `ID3D11DeviceContext*` |

Every handle returned owns exactly one reference; `ops->release(h)` is
`((IUnknown*)h)->Release()`. Handles passed *in* are borrowed (gfx/ takes its own
reference when it hands work to another thread). The gap this exposes (the vtable
has no way to *call* those objects) is **R1 in ABI-CHANGE-REQUESTS.md** and is
the real Wine-side blocker, not a gfx/ one.

### 2.4 Threading
- Every create (`d3d12_create_device`, swapchain, `d3d11on12_create`, preflight,
  caps' temp device) runs on **the gfx thread**: the host's, via
  `host->run_on_gfx_thread` (A's MTA thread); if no host service is given
  (standalone harness) a library-owned MTA worker thread is used instead. The
  caller may be any non-UI thread and is never required to be the gfx thread
  (`host->on_gfx_thread()==1` -> run inline).
- Waits have a timeout (`create_timeout_ms`, default 60 s, same as A's). On
  timeout the call fails with `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` mapped to
  `XRL_ERR_GENERIC`, logs "probable driver/thread-affinity hang", and the job is
  *abandoned*: its state is shared_ptr-owned so a late completion can never write
  into a dead stack frame (tested: `--timeout-ms 1`, both thread modes).
- `Present`, `GetBuffer`, `release`, `current_index`, `resize` run on the caller's
  thread (A: "free-threaded, need not be marshalled").
- `xrl_gfx_config.ui_thread_id` makes gfx/ WARN when a blocking call is made from
  the UI thread (it still works; it just blocks that thread).
- `XRL_GFX_FLAG_SWAPCHAIN_INLINE`: create the swapchain on the caller's thread
  instead — escape hatch if the CoreWindow-agility assumption is wrong.

### 2.5 Swapchain policy (differs from A's default; deliberate, please sanity-check)
`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `DXGI_USAGE_RENDER_TARGET_OUTPUT`, alpha IGNORE,
buffer count clamped to 2..16, **`DXGI_SCALING_STRETCH`** (A's default uses
`NONE`): if the guest asks for 1080p and the console output is 4K, STRETCH fills
the screen; NONE would leave a corner quad. sRGB formats are created as the UNORM
sibling (flip model forbids sRGB buffers) and logged. Present flags are masked to
those valid for CoreWindow swapchains (no tearing). `DXGI_STATUS_OCCLUDED`
counts as success. Unsupported formats fail loudly.

## 3. Wiring / running it (for A and the integrator)

### Production wiring (nothing to change except the lib + one call)
`xrl_host_gfx_ops()` already calls `xrl_gfx_ops_get(&g_services)`. Add, off the UI
thread, **before `xrl_layer_boot`**:

```cpp
xrl_gfx_d3d11_decision dec;
xrl_gfx_preflight(&dec);      // D3D11On12 smoke test; NULLs ops->d3d11on12_create on failure
xrl_gfx_report_caps(0, nullptr); // writes "gfx caps: ..." lines to log.txt
```

(`preflight` mutates the same `xrl_gfx_ops` table `ctx.gfx` points to; do it
before the guest starts so the table is not racing.)

### Probe-build wiring (the "re-confirm on this app's manifest" run)
After the window exists (swapchain stages are skipped with a clear log line
otherwise), on a **non-UI** thread:

```cpp
xrl_host_gfx_ops();                    // = xrl_gfx_ops_get(&g_services)
int rc = xrl_gfx_run_probe(nullptr);   // caps + preflight + selftest(60) + PASS/FAIL line
```

`xrl_gfx_run_probe` ends with one summary line to grep for:
`=== xrl_gfx probe RESULT: PASS | adapter="..." vendor=0x.... device=0x.... FL=0x.... bind_tier=N heap_tier=N d3d11on12=OK present_avg=..ms swapchain_stages=run ===`

### What the console log should say if the 2026-10-21 probe holds
Grep `gfx caps:`; expected: `adapter="SraKmd_arden" vendor=0x1414 device=0xD000`,
`dedicated_video=128 MiB ... shared_system=1280 MiB`, `max_feature_level=11_0`,
`resource_binding_tier=3 resource_heap_tier=2`. Also `D3D11 DECISION: native
D3D11On12 viable`, `selftest: D3D12 phase -> 0 ... pixel=0xFFFF0000`,
`selftest: D3D11On12 phase -> 0 ... pixel=0xFF00FF00`, `selftest: resize ... : 0`.
Any `ERROR` / `did not complete within` line is a finding to paste back.
`present_avg` should be ~16.7 ms (60 Hz vsync) or ~8.3/33.3 for other modes.

### Dev-PC build/test
```powershell
.\gfx\build.ps1 -Run            # desktop lib + harness (runs it), UWP-family lib + link check
.\gfx\build-desktop\Release\xrl_gfx_harness.exe [--host-svc|--ops-get|--headless|--force-no-on12|--timeout-ms N|--warp|--debug-layer]
```
Exit code 0 == PASS.

## 4. Status: real vs. verified vs. needs the console

| Item | Status |
|---|---|
| Library builds, desktop x64 `/W4` | **Built, zero warnings** |
| Library builds with `WINAPI_FAMILY_APP` (CMake `WindowsStore`, VS2022 + SDK 10.0.26100) | **Built, zero warnings** — header-level proof no non-UWP Win32 API is used |
| UWP **link** check (all sources into a DLL via the UWP linker, `/APPCONTAINER`) | **Linked**; imports are only d3d12/dxgi/d3d11 + api-ms-win-core-* + `*_APP` CRT |
| Strong `xrl_gfx_ops_get` overrides A's `/alternatename` default | **Verified** (scratch link test) |
| device -> D3D11On12 -> flip swapchain -> present -> readback -> resize on a real GPU | **Ran and PASSED on this dev PC** (Intel integrated GPU 0x8086:0x7D41, desktop Win11, HWND swapchain): 60 vsync'd frames, D3D12-cleared pixel `0xFFFF0000` and D3D11On12-wrapped-cleared pixel `0xFF00FF00` read back correctly, ResizeBuffers 1280x720->640x360 OK. Modes: library worker, host-services thread, `xrl_gfx_ops_get`, headless, forced-no-On12 (FAIL path), 1 ms timeout (abandon path) |
| `CreateSwapChainForCoreWindow` itself | **Compiled, never executed.** Needs a UWP/AppContainer run. (Tip for A: a UWP probe run on this Win11 dev PC would validate it before touching the console.) |
| Anything on Xbox Series S (SraKmd_arden, FL 11_0, 128 MiB dedicated, D3D11On12 on that driver, AppContainer behaviour, "create off the UI thread doesn't hang") | **Not validated. Cannot be from here.** |
| Debug layers (`XRL_GFX_FLAG_DEBUG_LAYER`) | Ran on the dev PC (layer enabled, probe PASS). Expected absent on the console; failure to enable is logged and ignored |
| DXVK->Dozen | **Not built** (by instruction). Detection only (§6) |

The desktop result says nothing about whether the console's driver supports the
D3D11On12 *behaviour* we rely on; that is exactly what preflight + selftest exist
to answer on the first console run.

## 5. Risks / open questions (all need the console or a decision)

1. **CoreWindow agility.** A's seam says the pointer may be used on the gfx thread;
   if `CreateSwapChainForCoreWindow` fails/hangs off the UI thread, set
   `XRL_GFX_FLAG_SWAPCHAIN_INLINE` and re-test; if it needs the CoreWindow's own
   thread, the host must dispatch swapchain creation to the UI thread via
   `CoreDispatcher.RunAsync` (not implemented; host-side). The UI thread must be
   *pumping events* meanwhile or the app is declared hung.
2. **FL 11_0 content.** Everything is created at FL 11_0 (probe: the AppContainer
   cap). If Rocket League needs FL 11_1 features, it will fail at device creation
   in the guest; gfx/ will not lie about the level (R4/R3).
3. **128 MiB "dedicated" / 1280 MiB "shared".** The game and Wine's dxgi will see
   whatever `xrl_gfx_caps` reports; whether to report them literally is a policy
   call (R3). gfx/ logs the live DXGI budget (`budget LOCAL/NON_LOCAL`) so the
   real ceiling is measurable on the first run. UMA flag is logged too.
4. **D3D11On12 performance.** It adds state-tracking overhead per resource and a
   wrapped-resource acquire/release around every present. Correctness is
   verified here on a PC GPU; throughput on the console GPU is unknown. A real
   RL frame will tell. `present_avg` is only a vsync sanity number, not a benchmark.
5. **Who wraps the swapchain buffers for the guest's D3D11** — Wine's `dxgi`/
   `d3d11` pair (ABI R4). `xrl_gfx_selftest` shows the working call sequence.
6. **Adapter / device-lost.** Present maps device-removed to `XRL_ERR_DEVICELOST`
   and logs `GetDeviceRemovedReason`. There is no recovery logic: RL has none to
   exercise offline; add when seen.
7. **HDR, tearing, fullscreen modes, multi-adapter, DXGI_FORMAT_R10G10B10A2/FP16
   colour spaces** — flip formats are accepted; no colour-space handling
   (`SetColorSpace1`) is done. Not needed for SDR 8-bit RL.

## 6. DXVK -> Dozen fallback — scaffolding and what it would take

**Implemented: only the decision point.** `xrl_gfx_preflight()` builds a
throwaway device + queue + `D3D11On12CreateDevice`, then really exercises it
(plain d3d11 clear+flush; a wrapped D3D12 render-target clear via
`CreateWrappedResource`/Acquire/Release; fence drain on the queue; device-removed
check). It records `xrl_gfx_d3d11_decision{backend, reason, hresult, detail}`.
On failure it sets `xrl_gfx_ops::d3d11on12_create = NULL` — the contract already
written in xrl_abi.h ("the host leaves this NULL and the Wine side falls back") —
and logs `D3D11 DECISION: native D3D11On12 NOT viable (...)`. A late failure of a
real `d3d11on12_create` call flips the same decision and the same slot, and later
calls return `XRL_ERR_NOTAVAIL`. `XRL_GFX_FLAG_FORCE_NO_D3D11ON12` exercises the
plumbing (tested).

### How we detect D3D11On12 failure
| Signal | Where | Reason code |
|---|---|---|
| No `D3D11On12CreateDevice` export / load failure | process start (static import — the app wouldn't even launch; the probe saw it exported) | n/a |
| `D3D11On12CreateDevice` returns failure | preflight / first create | `CREATE_FAILED` (+hr) |
| It does not return (the UI-thread-style hang) | timeout on gfx thread | `CREATE_TIMEOUT` |
| Device lacks `ID3D11On12Device` | after create | `NO_ON12_INTERFACE` |
| Resource/wrap/clear/flush fails | smoke test | `SMOKE_FAILED` |
| Queue reports device removed/hung after work | fence drain | `DEVICE_REMOVED` |
| **Not auto-detectable:** wrong pixels, missing features RL uses, perf collapse | needs a human / a real RL frame | — (the selftest *does* verify pixels through the wrapped path, which catches the gross case) |

No D3D12-device => verdict stays `UNDECIDED` (not evidence about On12).

### What the fallback would entail (NOT started; large and unproven)
Dozen (Mesa `dzn`) is a Vulkan driver implemented on D3D12; DXVK is D3D11->Vulkan.
Chain: RL D3D11 -> DXVK (Wine builtin d3d11/dxgi) -> Vulkan -> dzn -> **our** D3D12
device on the console. To run it we would have to build and ship all of:
1. **A Vulkan ICD for Windows/UWP**: Mesa `dzn` built as a DLL for the UWP family
   (it targets desktop Windows; D3D12-only deps, but never built for AppContainer
   here), plus a **Vulkan loader** (`vulkan-1.dll` is absent on the console) or a
   direct-ICD path.
2. **Shader pipeline**: dzn compiles SPIR-V -> DXIL in-process (`spirv_to_dxil`)
   and Xbox D3D12 may require **signed** DXIL, which normally comes from
   `dxil.dll` (a validator not known to exist in the AppContainer). Unknown;
   could be a hard blocker.
3. **Vulkan capability gap**: DXVK 2.x requires Vulkan 1.3 + many extensions
   (transform feedback, robustness2, geometry shaders, ...) dzn does not or only
   partly exposes, and the device would be capped at FL 11_0 / D3D12 tier set
   measured here. Older DXVK (1.10.x, Vulkan 1.1) is closer to dzn but ancient
   for RL. RL feature use vs dzn feature set is untested.
4. **Vulkan across the Wine seam**: Wine's `winevulkan` is a unix-side library
   that expects `libvulkan.so`; in our single-process ELF world there is none. We
   would add a new `xrl_vk_ops` ABI (instance/device/queue/memory/command buffer/
   pipelines: ~hundreds of entry points, handle + memory-mapping rules, a
   Windows-native<->SysV thunk table) — i.e. an ABI v2, and a presentation path
   from a Vulkan swapchain (dzn WSI) to the same CoreWindow.
5. **Perf + memory**: two translation layers on a 5 GB machine with a
   128 MiB/1280 MiB GPU view.

Recommendation: do not start it. Spend the first console run on the preflight +
selftest; if D3D11On12 passes, this section is dead weight. If it fails, the first
move is to read the `DECISION:` line and fix the cheap reason (queue type,
format flags, creation flags) before considering Dozen.

## 7. Small decisions made (for decisions.md)
- gfx/ implements `xrl_gfx_ops_get` per A's existing seam; handles are raw COM
  pointers (A + xrl_abi.h convention); no queue is created by gfx/.
- No CoreWindow param in the ABI; host exposes it via `get_core_window()`.
- All creates marshalled to the gfx thread with a timeout + abandon semantics.
- Device is always FL 11_0 min; adapter = first non-software D3D12-capable one.
- Swapchain: flip-discard, STRETCH, sRGB->UNORM mapping, count 2..16.
- Fallback = decision point + log, not code.

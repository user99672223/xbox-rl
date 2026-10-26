# ABI change requests from Workstream B (wine/)

`include/xrl_abi.h` is frozen and is **not** edited here. Everything below is a request to
the integrator. Items 1-2 block the graphics/audio/input forwarders from ever reaching real
hardware; 3-5 are smaller. "Wine side today" says what the code in `patches/wine/` does until
the ABI answers. Read together with `gfx/ABI-CHANGE-REQUESTS.md` (D): **B's #2 is D's R1** and
B's #3 is the Wine half of D's R2/R4 - same contract, written from the guest's side.

Severity: **BLOCKER** = nothing works without it. **NEEDED** = works degraded / only on paper.
**CLARIFY** = no header change, needs a sentence in `docs/syscall-abi.md`.

---

## 1. BLOCKER - how does the guest obtain the op-tables?

The header gives the host a `xrl_boot_context` with `gfx/audio/input` pointers, but the
Wine side is a guest ELF: nothing hands it those pointers, and they are host (MS-ABI)
function pointers it could not call anyway ("the layer owns the thunk").

**Request:** reserve a range of pseudo-syscall numbers outside the Linux table, answered by
the layer inside `xrl_syscall`:

| nr        | name                  | returns                                                        |
|-----------|-----------------------|----------------------------------------------------------------|
| `0x10001` | `XRL_SYS_GET_GFX_OPS`   | pointer to a **guest-callable** (SysV ABI) copy of `xrl_gfx_ops`, or `-ENOSYS` |
| `0x10002` | `XRL_SYS_GET_AUDIO_OPS` | same for `xrl_audio_ops`                                       |
| `0x10003` | `XRL_SYS_GET_INPUT_OPS` | same for `xrl_input_ops`                                       |
| `0x10004` | `XRL_SYS_GET_LOG_FN`    | guest-callable `xrl_log_fn` (so Wine's `ERR/WARN` land in `LocalState/log.txt` in order) |

The returned tables are the layer's thunk tables (each slot a guest-callable trampoline that
switches GS/TEB/stack and calls the real host function). `struct_size` is the host's.
**Wine side today:** `dlls/xrlhost/unixlib.c` calls exactly these numbers and treats `-ENOSYS`
as "host has no such ops" (returns `XRL_ERR_NOTAVAIL`). Tested against a fake host in
`wine/tests/musl-smoke/host_ops.c` (`wine/tests/run-pe-test.sh`: passes).

## 2. BLOCKER - handles must be *directly callable native COM objects* (= gfx R1)

`xrl_gfx_ops` can create a device, a swapchain, fetch swapchain buffers and present. A game
then calls ~600 `ID3D11*`/`ID3D12*` methods on the objects it got back. B adopts D's reading:
**every `xrl_handle` for a COM object is the raw interface pointer, and the host/layer
guarantees it can be called from guest context** (vtable trampolines that swap GS to the host
TEB, put the call on a host-valid stack, and swap back; MS-x64 ABI from the PE side). Out-parameters
that yield new COM objects (`CreateBuffer(..., &p)`) are produced by native code and are equally
callable - nothing is wrapped in Wine. Please write this into the header comment as a guarantee.
Needed in the other direction too: native -> guest callbacks (`ID3DInclude`, debug-message
callbacks, `IXAudio2VoiceCallback`) must be thunked or declared unsupported.

**Wine side today:** `d3d11.dll`/`d3d12.dll` return the host pointer to the game; `d3d11.dll`
itself makes **one direct COM call** on it - `ID3D12Device::CreateCommandQueue` - because gfx/
creates no queue (D: "the guest creates its direct queue"). The test host models this with a
fake MS-ABI vtable.

## 3. NEEDED - the Wine half of gfx's R2/R3/R4 (what B does, what B needs)

* **Queue (D2.3):** B creates the one direct queue (see #2) and passes the same handle to
  `d3d11on12_create` and `create_swapchain_for_corewindow`. B keeps a (d3d11 device -> d3d12
  device, queue) table in `xrlhost.dll` so `dxgi` can find the pair when the game creates a
  swapchain with its `ID3D11Device`; for D3D12 games the swapchain's `device` argument is the
  game's `ID3D12CommandQueue` and B asks it `GetDevice`. (Earlier drafts of this file proposed
  "queue == 0 means host picks" - **withdrawn**, D requires non-NULL.)
* **Swapchain buffers for D3D11 (D R4): NOT DONE.** A D3D11 game calls
  `GetBuffer(0, IID_ID3D11Texture2D)`; the host swapchain only has D3D12 resources. B must add a
  Wine-side `IDXGISwapChain1..3` wrapper in `dxgi.dll` that forwards everything to the inner native
  swapchain (direct COM calls) except `GetBuffer`/`Present`/`ResizeBuffers`/`GetCurrentBackBufferIndex`,
  which use `ID3D11On12Device::CreateWrappedResource` and Acquire/ReleaseWrappedResources
  (gfx `xrl_gfx_selftest` has the exact sequence). Until then `dxgi` returns the raw host swapchain.
* **R2 ops** (`swapchain_get_current_index`, `swapchain_resize`, `last_hresult`) - B will use
  them; please append them to `xrl_gfx_ops` as D asks.
* **R3 `get_adapter_info`**: B's `dxgi` currently enumerates adapters/outputs through Wine's own
  code (wined3d "no3d" renderer + registry identity). With `get_adapter_info` B can answer
  `IDXGIAdapter::GetDesc`, `QueryVideoMemoryInfo`, and cap `D3D12CreateDevice`'s minimum feature
  level from the real value (today: hard-coded 11_0 for D3D11, 12_0 ceiling for D3D12).

## 4. NEEDED - audio pacing (`xrl_audio_ops`)

There is no way to ask a voice how much it has consumed (`BuffersQueued`, `SamplesPlayed`,
or a buffer-end callback). A WASAPI/XAudio2 driver built on Wine's mixer needs that to know
how many frames it may write next. **Request:** `xrl_status (*voice_get_state)(xrl_handle
voice, uint64_t *samples_played, uint32_t *buffers_queued)`.
Without it a `winexrl.drv` would have to pace by wall clock and hope the host never starves.

Design note (why not forward XAudio2 COM): Wine's xaudio2_7/8/9 are FAudio on top of WASAPI.
Implementing one mmdevapi driver (`winexrl.drv`) that feeds the 7 existing ops covers WASAPI,
DirectSound, winmm and XAudio2 at once and needs no callback thunking. The driver itself is
**not written yet** (see `notes-B.md`).

## 5. CLARIFY - gateway contracts that are not 1:1 Linux syscalls

Wine+musl cannot work through a plain C call for four syscalls. The patched musl does this
(`patches/musl/0001`, tested by `tests/musl-smoke` with a pass-through kernel);
`docs/syscall-abi.md` should state it for the layer (C):

* **`clone` (56)** - `xrl_syscall(56, flags, child_stack, ptid, ctid, tls, resume_pc)`.
  Parent returns the tid or `-errno`. The child is a new thread that **starts executing at
  `resume_pc`** (musl's `__xrl_clone_child`) with `rsp = child_stack`, `rax = 0`,
  FS base = `tls` when `CLONE_SETTLS`; other registers undefined. It never returns through the
  gateway. (A `syscall` instruction can return twice; a call cannot, hence the explicit entry.)
  `a6 == 0` means a plain fork-style `clone` and should fail with `-ENOSYS`.
* **`exit` (60)** - `xrl_syscall(60, status, base, size)`: `base/size` is an optional stack
  region (musl's `__unmapself`) the layer must unmap *after* it has left that stack. Ignoring it
  only leaks a detached thread's stack.
* **`rt_sigreturn` (15)** - `xrl_syscall(15, ucontext_ptr)`: `a1` is the address of the
  `ucontext_t` (= `rsp` at entry of `__restore_rt`, the same place a kernel would find it). The
  layer restores that context and never returns.
* **`arch_prctl(ARCH_SET_FS / ARCH_SET_GS)` (158)** must really set the guest FS/GS base.
  Wine puts the Windows TEB in **GS** (PE code reads `gs:[0x30]`, `gs:[0x58]`...) and musl TLS in
  **FS**. See open question Q1 (GS vs the host's own TEB).
* **`__xrl_gateway`** - libc.so exports an 8-byte data symbol `__xrl_gateway` (default
  visibility, `.dynsym`). The ELF loader must store the address of the layer's SysV `xrl_syscall`
  thunk there **before any guest instruction runs**: musl's own start-up issues syscalls.
  (Alternative: an auxv entry; the symbol is simpler and was tested: `libc.so` as interpreter,
  `dlopen`, threads, Wine itself.)
* **no vDSO.** Do not provide `AT_SYSINFO_EHDR`; musl's vDSO fast paths are compiled out and
  `clock_gettime` goes through `xrl_syscall(228)`.
* **7th argument.** `xrl_syscall` is a 7-argument C function; `a6` is on the stack. Every musl
  stub zeroes it, so the layer may always read it.
* **Unix side of Wine also needs (all through plain syscalls):** `socketpair(AF_UNIX)` with
  `SCM_RIGHTS` over `sendmsg/recvmsg` (the wineserver protocol passes pipes and file fds that way;
  in-process this is "dup into the same fd table"), `pipe2`, `poll`, `fcntl`, `openat/unlinkat/
  mkdirat/getdents64/renameat`, `mmap(MAP_SHARED)` of one file **mapped twice coherently in the same
  process** (server and client both map the session/temp-section files), `/proc/self/exe`,
  `/proc/cpuinfo`, `/proc/meminfo`, `/proc/uptime`, `/proc/stat`. `getuid()` must agree with
  `stat().st_uid`.

## 6. CLARIFY - Windows-side fixed addresses Wine needs

* `0x7ffe0000`: Wine reads `KUSER_SHARED_DATA` there. On the console the real one is mapped
  read-only and is used as is (`XRL_HOST_USD=1`); Wine then never `mmap`s over it.
* `0x7ffe1000` is **no longer used** (the PE syscall stubs call through an in-image pointer).
* PE images load at their preferred bases (`ntdll.dll` `0x170000000` - or relocated when taken,
  the game at `0x140000000`, system DLLs below `0x7b000000`...). Wine reserves its own areas
  (there is no preloader); the layer's mmap arena must keep clear of anything Wine maps `MAP_FIXED`.

---

### Nothing here changes `XRL_ABI_VERSION` by itself
Items 1-4 add pseudo-syscalls and optional table members and would be ABI v2 (append-only).
Items 5-6 document what the existing v1 syscall gateway must do for Wine.

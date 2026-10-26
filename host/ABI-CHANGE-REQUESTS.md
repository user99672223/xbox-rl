# ABI change requests from Workstream A (host)

`include/xrl_abi.h` is frozen and owned by the integrator; nothing here has been applied.
Each item says what host/ does TODAY so the integrator can decide whether it is a header
change, a doc clarification, or ignorable. Ordered by how likely they are to block bring-up.

## 1. Calling convention + COM method calls across the gfx seam  (BLOCKER-class design gap)

*Observation.* The header says calls use "System V on the unix side as Wine sees it; the layer
owns the thunk to the host's native convention", and that an `xrl_handle` for a D3D12 device
"is really a native `ID3D12Device*`". But the op table only covers create / swapchain /
present / D3D11On12 / release. To do anything with a device (CreateCommandQueue,
CreateCommittedResource, ExecuteCommandLists, ...) Wine's `d3d12.dll` must call COM methods
through the vtable of that raw pointer - a Microsoft-x64-ABI call made from SysV-ABI guest
code (Wine's unix side is musl/SysV running under Blink).

*Today's host behaviour.* `host/src/gfx_ops_default.cpp` returns **raw COM interface pointers**
as handles (one reference each) and `release()` is `IUnknown::Release`.

*Request.* Add to the header's prose (or to a new op) exactly one of:
 (a) "handles are raw COM pointers; the layer provides a generic MS-x64 call thunk
     (`xrl_call_ms64(fn, a0..a5)` or similar) that Wine's d3d12/dxgi builtins use for every
     vtable call", **or**
 (b) a method-level op set.
Either way state it, and state that **every function pointer in `xrl_*_ops` and `xrl_log_fn`
is a Microsoft x64 function** (host/ is MSVC). Threading: all ops except the four create ops are
free-threaded and may be called from any guest thread; the create ops block on the host's gfx
thread (they are marshalled off the UI thread).

## 2. Audio ops are under-specified

Host implements what exists, with these interpretations (`host/src/audio_ops.cpp`):
 * `voice_submit_buffer(.., uint32_t flags)`: `flags` is undefined in the header; passed through as
   `XAUDIO2_BUFFER::Flags` (only `XAUDIO2_END_OF_STREAM`=0x40 honoured).
 * `create_source_voice(.., bits_per_sample)`: 32 is interpreted as IEEE float, 8/16/24 as PCM.
   Needs an explicit format tag (int32 PCM vs float, plus `WAVEFORMATEXTENSIBLE` channel mask).
 * Buffers are copied; no loop count / loop region / start-offset / context parameter.
Missing and needed by Wine's `xaudio2`: `voice_get_state` (BuffersQueued, SamplesPlayed) and a
buffer-end notification (the guest's `IXAudio2VoiceCallback::OnBufferEnd`), `voice_set_volume`,
`voice_set_frequency_ratio`, `voice_flush_source_buffers`, `voice_exit_loop`,
`set_channel_volumes`/output matrix, `mastering voice` query of channel mask / device details.

## 3. Input: keyboard, mouse, XInput packet number

 * No keyboard/mouse op. The full (SDL) host already tracks key state (`xrl_host_key_down(sdl_scancode)`
   in `host/src/sdl_main.cpp`, host-internal, NOT in the ABI). Request an op such as
   `get_key_state(uint32_t scancode)` / an event ring, plus mouse position/buttons/wheel.
 * `xrl_pad_state` has no `packet` (XInput `dwPacketNumber`). Wine's `XInputGetState` relies on it
   for change detection - add `uint32_t packet` (host can derive it from the WGI reading timestamp).
 * `set_pad_vibration` has no trigger motors (Xbox pads have impulse triggers); add
   `left_trigger_motor/right_trigger_motor`. WGI supports them.

## 4. Lifecycle + entropy are missing from the boot context

 * No suspend/resume/exit signal into the layer except `xrl_layer_request_exit`. UWP apps get
   `Suspending`/`Resuming` (and are killed if a suspend deferral is held > ~5 s). Request e.g.
   `void xrl_layer_notify(int event)` with SUSPEND/RESUME/FOCUS_LOST. Today the host only logs them.
 * `docs/syscall-abi.md` says `/dev/urandom` is "seeded by host, since console has no RDRAND", but
   `xrl_boot_context` carries no seed. Request `uint8_t entropy[32]` (host fills from `BCryptGenRandom`,
   which works in the AppContainer) or a `fill_random` callback.
 * No guest->host "fatal error / user message" channel (e.g. show an error on screen before exit).
   Today the guest can only log and exit.

## 5. Window / swapchain surface

 * No op to query window size / DPI / display mode / HDR, no resize notification, no
   `swapchain_resize_buffers`, no `swapchain_get_current_back_buffer_index`
   (`IDXGISwapChain3` is reachable through the raw pointer, but only if item 1(a) holds).
 * `create_swapchain_for_corewindow` takes `queue` as a handle: the host treats it as a raw
   `ID3D12CommandQueue*`; document that.
 * `xrl_status` loses the HRESULT. Request a `get_last_hresult()` op (or return it via an out
   param) so Wine can hand the guest the *exact* DXGI/D3D error instead of a synthesized one.

## 6. Documentation gaps (no code change)

 * `include/xrl_abi.h` (`guest_mem_budget`) says "See docs/decisions.md on the address-space hole" -
   there is no such section in `docs/decisions.md`. The host probe now logs a user-VA free-region
   map (`vamap` step) and an allocation ceiling (`ceiling` step); once pulled, that data is the
   missing section.
 * Default `guest_mem_budget` is a host PLACEHOLDER of 4096 MiB (`HostConfig` in `host/src/xrl_host.h`; override with `mem_budget_mb=` in launch.cfg); the console
   shows ~5107 MiB available to the app. The integrator should set the real number.
 * `xrl_log_fn` has no user-data pointer and no printf-style variant; fine, but note the host
   flattens embedded newlines to `|` so each call is one physical log line.

# host/ — Workstream A (UWP app)

C++/WinRT UWP app. Owns the window, gamepad, keyboard, audio device, the manifest
(caps: `codeGeneration`, `expandedResources`), logging to `LocalState/log.txt`, and the
host-side `xrl_gfx_ops` / `xrl_audio_ops` / `xrl_input_ops`; calls `xrl_layer_boot()`.
Design decisions, verification status and open questions: **`notes-A.md`**. Requested
ABI changes: **`ABI-CHANGE-REQUESTS.md`**.

It is a real UWP `.vcxproj` with a real activation class (`App.idl` -> `XboxRL.winmd`), because
the Xbox launcher refuses an exe without matching WinRT metadata before any process starts.
All native device/swapchain creation is marshalled onto a dedicated gfx thread, never the
CoreWindow UI thread (UI-thread D3D create hangs on Xbox).

## Build / run loop

```powershell
cp .env.example .env            # fill XBOX_IP / XBOX_USER / XBOX_PASS (+ GAME_SRC)
pwsh tools/build.ps1            # PROBE build (default): fetches pinned deps, makes the dev cert, builds, signs
pwsh tools/deploy.ps1           # upload + register over Device Portal (in-place update; keeps LocalState)
pwsh tools/run.ps1              # prompts: press the "xbox-rl" tile in Dev Home; follows the log live
pwsh tools/logs.ps1             # (re)pull LocalState/log.txt -> results/log-latest.txt
```

Look for `PROBE_RESULT ... | required=PASS` in the log. The probe build logs: D3D12 device (adapter, feature
level, caps), D3D11On12 device, RWX `VirtualAlloc`, a 4 GiB allocation, plus a VA map, a CoreWindow
swapchain test, XAudio2, Gaming.Input and an allocation-ceiling scan.

Validate on the dev laptop before spending a console round-trip (no UWP registration / Developer Mode needed):

```powershell
pwsh tools/test-desktop.ps1     # builds + runs the probe + boot-context path as a desktop exe
pwsh tools/test/test-tools.ps1  # exercises deploy/run/logs/push-* against a local mock Device Portal
```

Full host (SDL2 WinRT front-end + boot context + layer seam):

```powershell
pwsh tools/build.ps1 -Mode Full [-LayerLib path\to\layer.lib] [-GfxLib path\to\gfx.lib]
```

## Layout

| Path | What |
|------|------|
| `XboxRL.vcxproj`, `Package.appxmanifest`, `App.idl/.h`, `pch.*` | UWP project, manifest, activation class |
| `src/log.cpp` | `LocalState/log.txt` sink = the `xrl_log_fn` for host + layer + wine; crash recorder |
| `src/config.cpp` | optional `LocalState/launch.cfg` (see `launch.cfg.example`) |
| `src/gfx_thread.cpp`, `src/gfx_ops_default.cpp`, `src/gfx_seam.h` | gfx thread + default `xrl_gfx_ops` + **the seam D plugs into** |
| `src/audio_ops.cpp` | `xrl_audio_ops` over native XAudio2 |
| `src/input_ops.cpp` | `xrl_input_ops` over Windows.Gaming.Input |
| `src/probe.cpp` | the first-milestone probe |
| `src/boot.cpp`, `src/layer_stub.cpp` | builds `xrl_boot_context`, calls `xrl_layer_boot` (stub until C's lib is linked) |
| `src/app.cpp` | entry + probe-build front-end (C++/WinRT view) |
| `src/sdl_main.cpp` | full-build front-end (SDL2 WinRT) |
| `test/desktop_probe_main.cpp` | desktop harness for `tools/test-desktop.ps1` |
| `Assets/` | placeholder tile art (regenerate with `tools/make-assets.ps1`) |
| `build/`, `signing/` | git-ignored outputs / dev signing key |

SDL2 is vendored at a pin (`vendor/PINS.md`, fetched by `tools/fetch-deps.ps1`); upstream is never edited -
changes go in `patches/sdl2-*.patch` (currently none).

# xbox-rl

A compatibility layer that runs the **Windows build of Rocket League** inside a
**UWP app on an Xbox Series S in Developer Mode**, offline (anti-cheat disabled
via the game's launch option), at native CPU/GPU speed.

Think "Proton for a locked-down Windows host": the game is unmodified; we
provide the environment under it.

## How it fits together

```
  Xbox Series S (Dev Mode, AppContainer, ~5 GB)
  +-----------------------------------------------------------+
  |  host/   UWP C++ app (SDL2 WinRT): window, pad, kbd, audio|
  |          installs xrl_gfx/audio/input ops, logs to        |
  |          LocalState/log.txt, calls xrl_layer_boot()       |
  |            |                         ^                     |
  |            v  boot ctx               | gfx/audio/input ops |
  |  layer/  Blink ELF loader + Cosmo-derived syscall backend |
  |          + VFS over LocalState + threads/futex/mmap        |
  |            ^  xrl_syscall                                  |
  |            |                                               |
  |  wine/   Proton-GE Wine, musl unix side, no `syscall`     |
  |          instrs; wineserver-as-thread; d3d12/dxgi/d3d11/  |
  |          xaudio2/xinput forward to native via the ops     |
  |            |                                               |
  |            v                                               |
  |  gfx/    DXGI/CoreWindow swapchain, D3D11On12, caps        |
  |          (DXVK->Dozen only if D3D11On12 fails)            |
  +-----------------------------------------------------------+
            Rocket League (unmodified Win64 build)
```

The frozen seam between all four is **`include/xrl_abi.h`**. The syscall ABI in
prose is **`docs/syscall-abi.md`**. Every non-obvious decision and the measured
facts about the console are in **`docs/decisions.md`** — read that first.

## Layout

| Dir        | Workstream | Contents                                             |
|------------|------------|------------------------------------------------------|
| `host/`    | A          | UWP app, SDL2 WinRT, manifest, Device Portal scripts |
| `wine/`    | B          | Proton-GE Wine vendored + patches, `scripts/build-wine.sh` |
| `layer/`   | C          | Blink loader, Cosmo syscall backend, VFS, threads    |
| `gfx/`     | D          | DXGI/CoreWindow swapchain, caps, D3D11On12, Dozen branch |
| `include/` | (shared)   | `xrl_abi.h` — the frozen ABI. Owned by the integrator |
| `vendor/`  | —          | upstream pinned at commits (submodules/tarballs)     |
| `patches/` | —          | `*.patch` applied by scripts; no in-tree upstream edits |
| `tools/`   | A          | `deploy/run/kill/logs/push-game` PowerShell scripts  |
| `docs/`    | —          | `decisions.md`, `syscall-abi.md`                     |

## Build/run loop

`build -> package -> deploy -> run -> read log`. Never guess what happened on the
console — read `LocalState/log.txt` via `tools/logs.ps1`. When a step needs a
physical action on the console (press the tile in Dev Home), the tooling asks
once and continues when told "done".

See `tools/README` for the `.env` the scripts read (`XBOX_IP`, `XBOX_USER`,
`XBOX_PASS`); copy `.env.example` to `.env`.

## Done

Rocket League launches inside the UWP app with anti-cheat off and an offline
match plays at native speed.

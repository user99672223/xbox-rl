# tools/ — build, Device Portal and payload scripts (PowerShell 7)

Read `XBOX_IP` / `XBOX_USER` / `XBOX_PASS` (and `GAME_SRC`) from `../.env` (copy `../.env.example`).
All scripts need **PowerShell 7+** (`pwsh`). Device Portal is `https://$XBOX_IP:11443` (self-signed cert,
HTTP Basic auth). Optional `.env` key `XBOX_URL` redirects the tools to a mock (tests only).

| Script | Purpose |
|--------|---------|
| `build.ps1 [-Mode Probe\|Full] [-Config Release]` | fetch deps, make dev cert, (Full: build SDL2), version-stamp, build + sign the host package. Writes `host/build/package/latest.json` |
| `deploy.ps1` | upload cert + `.msix` (+ VCLibs dependency) and register it over Device Portal; polls until installed. In-place update, keeps LocalState. `-Uninstall` wipes the app + its LocalState |
| `run.ps1` | Device Portal app-launch returns **403** on the console, so it prints the "press the xbox-rl tile in Dev Home" prompt, then follows `log.txt` until the run ends (`-Manual` = wait for Enter) |
| `kill.ps1` | try Device Portal terminate; on 403 prints the manual procedure (exit 2) |
| `logs.ps1 [-Prev] [-Follow] [-Tail N]` | pull `LocalState/log.txt` -> `results/log-<ts>.txt` + `results/log-latest.txt`, print key lines |
| `push-game.ps1` | copy the `GAME_SRC` Rocket League install into `LocalState\rootfs\game` (= guest `/game`); resumable, parallel, canary-first. `-DryRun` first |
| `push-file.ps1 -Local f [-Remote dir]` | upload one file into LocalState (e.g. `launch.cfg`) |
| `fetch-deps.ps1`, `apply-patches.ps1` | pinned upstream fetch (`vendor/PINS.md`) and `patches/sdl2-*.patch` application |
| `new-cert.ps1`, `make-assets.ps1` | dev signing cert (git-ignored), placeholder tile art |
| `test-desktop.ps1` | build + run the probe as a desktop exe on the laptop |
| `test/test-tools.ps1` | run the Device Portal scripts against `test/mock_device_portal.py` |

Only `LocalState` and `TEMP` are writable on the console, so everything the app needs at run time
(wine prefix, game, `launch.cfg`) goes under `LocalState` via Device Portal's file API.

## First console run (probe)

```powershell
pwsh tools/build.ps1
pwsh tools/deploy.ps1
pwsh tools/run.ps1        # <- press the tile when asked
```

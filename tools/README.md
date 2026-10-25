# tools/ — Device Portal + payload scripts (PowerShell)

Read XBOX_IP/XBOX_USER/XBOX_PASS from ../.env (copy ../.env.example).

- deploy.ps1    package + register the appx on the console via Device Portal
- run.ps1       launch (note: Device Portal app-launch returns 403; prints the
                "press the tile in Dev Home" prompt and waits for the run)
- kill.ps1      terminate the running app
- logs.ps1      pull LocalState/log.txt off the console
- push-game.ps1 copy the Rocket League install (GAME_SRC) into the package's
                LocalState rootfs

PROBE BUILD FIRST: before the full stack, ship a probe that reports the D3D12
device, the D3D11On12 device, an RWX VirtualAlloc, and a 4 GB allocation, and
read it back through logs.ps1. (The capability probe in Documents/win10xbox
already has 2026-10-21 measurements for all of these — see docs/decisions.md —
but re-confirm on this app's own manifest/caps.)

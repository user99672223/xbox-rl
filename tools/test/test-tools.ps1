<#
.SYNOPSIS
  Drive deploy/run/logs/push-file/push-game/kill against tools/test/mock_device_portal.py.
  Catches scripting bugs on the laptop. It does NOT prove the real console's Device Portal
  behaves like the mock - it only encodes our assumptions (see the mock's docstring).
  Requires a prior `pwsh tools/build.ps1` (uses host/build/package/latest.json).
#>
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'xrl-tooltest'
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null

# fake game
$game = Join-Path $tmp 'game'
New-Item -ItemType Directory -Force "$game\Binaries\Win64", "$game\TAGame\CookedPCConsole" | Out-Null
Set-Content "$game\Binaries\Win64\RocketLeague.exe" 'MZ-fake'
Set-Content "$game\TAGame\CookedPCConsole\a.upk" ('x' * 3000)
Set-Content "$game\TAGame\CookedPCConsole\b.upk" ('y' * 2000)
Set-Content "$game\readme.txt" 'hi'

$envFile = Join-Path $tmp '.env'
@"
XBOX_IP=127.0.0.1
XBOX_USER=willy
XBOX_PASS=mock
XBOX_URL=http://127.0.0.1:11444
GAME_SRC=$game
"@ | Set-Content $envFile
$env:XRL_ENV_FILE = $envFile

$py = Start-Process python -ArgumentList '-I', (Join-Path $PSScriptRoot 'mock_device_portal.py'), '11444' -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds 2
    $t = Join-Path $repo 'tools'
    function Step($n, [scriptblock]$b) { Write-Host "`n===== $n ====="; & $b; if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { Write-Host "(exit $LASTEXITCODE)" } }

    Step 'deploy'       { & pwsh -NoProfile -File "$t\deploy.ps1" }
    Step 'push-file'    { Set-Content "$tmp\launch.cfg" "probe=quick`nlinger_s=1"; & pwsh -NoProfile -File "$t\push-file.ps1" -Local "$tmp\launch.cfg" }
    Step 'push-game dry'{ & pwsh -NoProfile -File "$t\push-game.ps1" -DryRun }
    Step 'push-game'    { & pwsh -NoProfile -File "$t\push-game.ps1" -Yes -Throttle 2 }
    Step 'push-game again (should skip all)' { & pwsh -NoProfile -File "$t\push-game.ps1" -Yes }
    Step 'kill (expect 403 -> exit 2)' { & pwsh -NoProfile -File "$t\kill.ps1" }
    Step 'run + logs'   {
        $job = Start-Job { Start-Sleep 6; Invoke-RestMethod -Method POST http://127.0.0.1:11444/mock/run | Out-Null }
        & pwsh -NoProfile -File "$t\run.ps1" -TimeoutSec 60
        Remove-Job $job -Force
    }
    Step 'logs'         { & pwsh -NoProfile -File "$t\logs.ps1" -Tail 5 }
    Write-Host "`n===== mock saw ====="
    (Invoke-RestMethod http://127.0.0.1:11444/mock/state) | ConvertTo-Json -Depth 5
} finally {
    Stop-Process -Id $py.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:\XRL_ENV_FILE -ErrorAction SilentlyContinue
}

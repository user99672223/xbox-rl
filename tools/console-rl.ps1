<#
.SYNOPSIS
  Rocket League on the console, end to end: install the host package, upload the guest tree
  and the game, log in with a fresh Epic exchange code, then (after the tile press) follow the log.

.DESCRIPTION
  1. tools/deploy.ps1                      install/update the latest built package (keeps LocalState)
  2. tools/push-game.ps1 -Source <rootfs>  the staged guest tree -> LocalState\rootfs (incremental;
                                           the laptop's 'game' junction is not followed), then
                                           ~\xrl-work\console\rootfs-overrides\* over it (console-only files)
  3. tools/push-game.ps1                   the Rocket League install (GAME_SRC) -> LocalState\rootfs\game
                                           (~42 GB the first time; resumable, later runs upload changes only)
  4. tools/epic-capture.ps1                a fresh exchange code from the Epic Games Launcher (codes
                                           expire within minutes, so this runs last), written with
                                           tools/configs/rl.cfg to C:\Users\<you>\xrl-work\console\launch.cfg
                                           (outside the repo; never printed) and uploaded
  5. tools/run.ps1                         asks you to press the xbox-rl tile in Dev Home, follows log.txt

.PARAMETER Rootfs     Staged guest tree (default: ~\xrl-work\rootfs, the one tools/run-rl.sh uses).
.PARAMETER SkipGame   Do not (re)check the game upload (it is already there).
.PARAMETER NoLogin    Start Rocket League without Epic arguments.
.PARAMETER SkipDeploy Keep the installed app (the loose-file install keeps the game in its install folder).
.EXAMPLE
  pwsh tools/build.ps1 -Mode Full -LayerLib layer/build/uwp/xrl_layer.lib -GfxLib gfx/build-uwp/Release/xrl_gfx.lib
  pwsh tools/console-rl.ps1
#>
[CmdletBinding()]
param([string]$Rootfs = (Join-Path $HOME 'xrl-work\rootfs'), [switch]$SkipGame, [switch]$NoLogin, [switch]$SkipDeploy)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

function Step($n, $what) { Write-Host "`n[console-rl] ($n/5) $what" -ForegroundColor Cyan; $global:LASTEXITCODE = 0 }

if ($SkipDeploy) { Step 1 'install skipped (-SkipDeploy: e.g. the loose install from tools/console-loose.ps1)' }
else {
    Step 1 'install the host package'
    & "$PSScriptRoot\deploy.ps1"
    if ($LASTEXITCODE) { throw "deploy failed ($LASTEXITCODE)" }
}

# The laptop's Rocket League runs (tools/run-rl.sh, agents too) share the guest tree and capture
# logins through the launcher: hold their lock from the guest-tree upload to the login capture, so
# no run writes the prefix mid-upload or competes for the launcher.
$lock = Join-Path $HOME 'xrl-work\rl-run.lock'
while ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 20) {
    Write-Host '[console-rl] a laptop Rocket League run holds the lock; waiting...'; Start-Sleep 15
}
"console-rl.ps1 $(Get-Date -Format s)" | Set-Content $lock
try {

Step 2 "upload the guest tree $Rootfs"
# Rocket League's own logs stay where they are written (the console writes its own)
& "$PSScriptRoot\push-game.ps1" -Source $Rootfs -Dest 'rootfs' -Exclude 'game', 'game\*', 'tmp\*', '*\TAGame\Logs\*' -Yes
if ($LASTEXITCODE) { throw "guest tree upload failed ($LASTEXITCODE)" }
# console-only files over the laptop's (e.g. Rocket League's TASystemSettings.ini at the console's
# resolution): ~\xrl-work\console\rootfs-overrides\<path in the guest tree>, uploaded every run
$ovr = Join-Path $HOME 'xrl-work\console\rootfs-overrides'
if (Test-Path $ovr) {
    foreach ($o in Get-ChildItem $ovr -Recurse -File) {
        $rel = [IO.Path]::GetDirectoryName($o.FullName.Substring($ovr.Length).TrimStart('\'))
        $global:LASTEXITCODE = 0
        & "$PSScriptRoot\push-file.ps1" -Local $o.FullName -Remote ('rootfs\' + $rel)
        if ($LASTEXITCODE) { throw "override upload failed: $($o.FullName)" }
    }
}

if (-not $SkipGame) {
    Step 3 'upload the Rocket League install (resumable)'
    & "$PSScriptRoot\push-game.ps1" -Yes
    if ($LASTEXITCODE) { throw "game upload failed ($LASTEXITCODE)" }
} else { Step 3 'game upload skipped (-SkipGame)' }

Step 4 'launch.cfg with a fresh Epic login'
$dir = Join-Path $HOME 'xrl-work\console'
New-Item -ItemType Directory -Force $dir | Out-Null
$cfg = Join-Path $dir 'launch.cfg'
$lines = [Collections.Generic.List[string]]::new()
$lines.AddRange([string[]](Get-Content (Join-Path $PSScriptRoot 'configs\rl.cfg')))
if (-not $NoLogin) {
    $argsFile = Join-Path $HOME 'xrl-work\console\launch-args.json'
    $global:LASTEXITCODE = 0
    & "$PSScriptRoot\epic-capture.ps1" -Out $argsFile
    if ($LASTEXITCODE) { throw "Epic capture failed ($LASTEXITCODE)" }
    foreach ($a in (Get-Content -Raw $argsFile | ConvertFrom-Json).args) { $lines.Add("argv=$a") }
    Write-Host "[console-rl] $(@((Get-Content -Raw $argsFile | ConvertFrom-Json).args).Count) Epic arguments added (not shown)"
}
} finally { Remove-Item $lock -ErrorAction SilentlyContinue }
Set-Content -Path $cfg -Value $lines -Encoding utf8NoBOM
$global:LASTEXITCODE = 0
& "$PSScriptRoot\push-file.ps1" -Local $cfg
if ($LASTEXITCODE) { throw "launch.cfg upload failed ($LASTEXITCODE)" }

Step 5 'run: press the xbox-rl tile in Dev Home now (the login code expires within minutes)'
& "$PSScriptRoot\run.ps1"

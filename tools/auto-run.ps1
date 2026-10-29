<#
.SYNOPSIS
  Autonomous console run: push a launch.cfg, launch the app via Device Portal,
  follow LocalState\log.txt until it ends (or a deadline), terminate, save the log.
  No tile press needed (the launch REST call is accepted on this console).

.PARAMETER Cfg        Local launch.cfg to push (default: the current console out\launch.cfg).
.PARAMETER TimeoutSec How long to follow before giving up and terminating (default 120).
.PARAMETER Tag        Short name for the saved log under results\ (default: timestamp).
#>
[CmdletBinding()]
param(
    [string]$Cfg = (Join-Path $HOME 'xrl-work\console\out\launch.cfg'),
    [int]$TimeoutSec = 120,
    [string]$Tag = (Get-Date -Format 'HHmmss')
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
$pkg  = Get-RequiredPackage $xbox
$b64  = { param($s) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
$appidB   = [uri]::EscapeDataString((& $b64 $pkg.PackageRelativeId))
$packageB = [uri]::EscapeDataString((& $b64 $pkg.PackageFullName))

# 1. push the launch.cfg (the remote name is the local basename, so stage it AS launch.cfg)
Write-Host "[auto] push $Cfg -> LocalState\launch.cfg"
$staged = Join-Path ([IO.Path]::GetTempPath()) 'launch.cfg'
Copy-Item -Force $Cfg $staged
& "$PSScriptRoot\push-file.ps1" -Local $staged | Out-Null

# 2. make sure nothing is running, snapshot the current log
try { Invoke-Dp $xbox "/api/taskmanager/app?package=$packageB" -Method DELETE | Out-Null } catch {}
Start-Sleep 2
$results = Join-Path $RepoRoot 'results'
New-Item -ItemType Directory -Force $results | Out-Null
$tmp = Join-Path $results "auto-$Tag.tmp"
$before = $null
try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp; $before = (Get-FileHash $tmp).Hash } catch { $before = $null }

# 3. launch
Write-Host "[auto] launch v$($pkg.VersionString)"
Invoke-Dp $xbox "/api/taskmanager/app?appid=$appidB&package=$packageB" -Method POST | Out-Null

# 4. follow log.txt until an end marker or the deadline
$doneRx = 'exiting application|guest/layer finished|UNHANDLED EXCEPTION|guest exiting with code|exec handover|successfully loaded in-game|Easy Anti-Cheat'
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$started = $false; $shown = 0; $finished = $false
while ((Get-Date) -lt $deadline -and -not $finished) {
    Start-Sleep -Seconds 2
    try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp } catch { continue }
    $h = (Get-FileHash $tmp).Hash
    if (-not $started) { if ($h -eq $before) { continue } $started = $true; Write-Host '[auto] new run detected:' }
    $lines = @(Get-Content $tmp -Encoding utf8)
    if ($lines.Count -gt $shown) { $lines[$shown..($lines.Count - 1)] | ForEach-Object { Write-Host $_ }; $shown = $lines.Count }
    if (($lines | Select-Object -Last 20) -match $doneRx) { Start-Sleep 3; try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp } catch {}; $finished = $true }
}

# 5. terminate
Write-Host "[auto] terminate"
try { Invoke-Dp $xbox "/api/taskmanager/app?package=$packageB" -Method DELETE | Out-Null } catch {}

# 6. save
$save = Join-Path $results "auto-$Tag.log"
if (Test-Path $tmp) { Move-Item -Force $tmp $save }
Copy-Item -Force $save (Join-Path $results 'log-latest.txt') -ErrorAction SilentlyContinue
if (-not $started) { Write-Warning "[auto] no new run detected (launch may have failed). Saved: $save"; exit 2 }
if (-not $finished) { Write-Warning "[auto] no end marker before ${TimeoutSec}s (still running/hung). Saved: $save" }
Write-Host "[auto] saved: $save"

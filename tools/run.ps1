<#
.SYNOPSIS
  Start the host on the console. Device Portal's app-launch REST call returns 403
  there (docs/decisions.md), so a human presses the tile in Dev Home; this script
  prompts for that once and then follows the log until the run finishes.

.DESCRIPTION
  1. confirms the package is installed and snapshots the current remote log
  2. prints the "press the tile" instruction
  3. waits - automatically (polls log.txt for a NEW run) or, with -Manual, until you
     press Enter
  4. follows the log until "exiting application" / layer exit / a crash record, then saves
     it to results/ (same as tools/logs.ps1)

.PARAMETER Manual     Don't poll: just wait for Enter, then pull the log once.
.PARAMETER TryApi     First try Device Portal's launch endpoint anyway (expected: 403).
.PARAMETER TimeoutSec How long to wait for the run to finish (default 900).
#>
[CmdletBinding()]
param([switch]$Manual, [switch]$TryApi, [int]$TimeoutSec = 900)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
$pkg = Get-RequiredPackage $xbox
$aumid = "$($pkg.PackageFamilyName)!$script:AppId"
Write-Host "[run] installed: v$($pkg.VersionString)  $($pkg.PackageFullName)"

if ($TryApi) {
    $b64 = { param($s) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
    try {
        Invoke-Dp $xbox "/api/taskmanager/app?appid=$([uri]::EscapeDataString((& $b64 $pkg.PackageRelativeId)))&package=$([uri]::EscapeDataString((& $b64 $pkg.PackageFullName)))" -Method POST | Out-Null
        Write-Host '[run] Device Portal accepted the launch request (unexpected - good!)'
    } catch {
        Write-Host "[run] launch API refused as expected (HTTP $(Get-DpStatus $_)); falling back to the manual tile press."
    }
}

# snapshot of the log as it is NOW, so we can tell when a new run has replaced it
$results = Join-Path $RepoRoot 'results'
New-Item -ItemType Directory -Force $results | Out-Null
$tmp = Join-Path $results 'log-run.tmp'
$beforeHash = $null
try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp; $beforeHash = (Get-FileHash $tmp).Hash } catch { $beforeHash = $null }

Write-Host ''
Write-Host '  ================================================================'
Write-Host '   ON THE CONSOLE:  press the  "xbox-rl"  tile in Dev Home'
Write-Host "   (Dev Home -> Installed apps / My games & apps -> xbox-rl; AUMID $aumid)"
Write-Host '  ================================================================'
Write-Host ''

if ($Manual) {
    Read-Host 'Press Enter when the app has run (or crashed)' | Out-Null
    & "$PSScriptRoot\logs.ps1"
    return
}

Write-Host "[run] waiting for a new run to appear in log.txt (Ctrl+C to give up; or re-run with -Manual) ..."
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$started = $false; $shown = 0; $finished = $false
# NOT 'PROBE END': the full build keeps running after its quick probe.
$doneRx = 'exiting application|guest/layer finished|UNHANDLED EXCEPTION'
while ((Get-Date) -lt $deadline -and -not $finished) {
    Start-Sleep -Seconds 2
    try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp } catch { continue }
    $h = (Get-FileHash $tmp).Hash
    if (-not $started) {
        if ($h -eq $beforeHash) { continue }
        $started = $true
        Write-Host '[run] new run detected - following log:'
    }
    $lines = @(Get-Content $tmp -Encoding utf8)
    if ($lines.Count -gt $shown) {
        $lines[$shown..($lines.Count - 1)] | ForEach-Object { Write-Host $_ }
        $shown = $lines.Count
    }
    if (($lines | Select-Object -Last 15) -match $doneRx) {
        Start-Sleep -Seconds 3      # let trailing lines (exit path) land
        try { Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' 'log.txt' $tmp } catch {}
        $finished = $true
    }
}
Remove-Item $tmp -ErrorAction SilentlyContinue
if (-not $started)  { Write-Warning 'No new run was detected before the timeout. Did you press the tile? (tools/logs.ps1 still shows the last log.)'; exit 2 }
if (-not $finished) { Write-Warning 'Run started but did not reach an end marker before the timeout (still running, hung, or crashed). Pulling the log as it stands.' }
& "$PSScriptRoot\logs.ps1" -Tail 0 -Quiet
Write-Host '[run] full log saved: results/log-latest.txt'

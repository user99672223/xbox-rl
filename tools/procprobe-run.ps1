<#
.SYNOPSIS
  Console process-survival probe (host/src/probe.cpp procprobe): which kind of 2nd process
  does the Xbox let live? Each run pushes a launch.cfg with XRL_PROCPROBE=<spec>, launches the
  app over Device Portal, waits for "procprobe: DONE", pulls log.txt + every pp-child-*.log and
  log-child-*.txt into results\procprobe-<run>\, and terminates the app.

.PARAMETER Runs  Which runs (default: props, ramp, layer). See $specs below.
#>
[CmdletBinding()]
param([string[]]$Runs = @('props', 'ramp', 'layer'), [int]$TimeoutSec = 140)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"
Set-StrictMode -Off

# run -> (spec, extra launch.cfg lines)
$specs = [ordered]@{
    props = @('idle,busy,winrt,extexec,d3d11,d3d12,init', @())
    ramp  = @('memramp', @())
    layer = @('idle,layer', @('mem_budget_mb=256', 'host_env=XRL_PP_ARGV=/usr/bin/pp-guest|64'))
    late  = @('idle@10,init@10,layer@10', @('mem_budget_mb=256', 'host_env=XRL_PP_ARGV=/usr/bin/pp-guest|64'))
}

$xbox = Get-XboxConfig
$pkg  = Get-RequiredPackage $xbox
$fn   = $pkg.PackageFullName
$b64  = { param($s) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
$appidB   = [uri]::EscapeDataString((& $b64 $pkg.PackageRelativeId))
$packageB = [uri]::EscapeDataString((& $b64 $fn))
Write-Host "[pp] app v$($pkg.VersionString) on $($xbox.Ip)"

# the guest the "layer" child runs: /usr/bin/pp-guest in the loose install's rootfs (vfs_root=pkg:rootfs)
$guest = Join-Path $RepoRoot 'layer\tests\guest\pp-guest'
if ($Runs | Where-Object { $_ -in 'layer', 'late' }) {
    if (-not (Test-Path $guest)) { throw "build $guest first (see pp-guest.c)" }
    $rdir = '\LooseApps\xbox-rl\rootfs\usr\bin'
    New-DpDirectory $xbox '' $rdir 'DevelopmentFiles'
    Send-DpFile $xbox '' $rdir $guest 'DevelopmentFiles'
    Write-Host "[pp] uploaded pp-guest -> DevelopmentFiles$rdir"
}

$results = Join-Path $RepoRoot 'results'
# A LocalState\launch.cfg wins over the installed one, so keep whatever is there and put it back after.
$backup = Join-Path ([IO.Path]::GetTempPath()) 'pp-launch.cfg.bak'
Remove-Item -Force $backup -ErrorAction SilentlyContinue
try { Receive-DpFile $xbox $fn '\LocalState' 'launch.cfg' $backup; Write-Host '[pp] backed up the existing LocalState\launch.cfg' } catch { Remove-Item -Force $backup -ErrorAction SilentlyContinue }
try {
foreach ($run in $Runs) {
    $spec, $extra = $specs[$run]
    $out = Join-Path $results "procprobe-$run"
    New-Item -ItemType Directory -Force $out | Out-Null
    $cfgText = @(
        "# procprobe run '$run'",
        'vfs_root=pkg:rootfs',
        'display=auto',
        "host_env=XRL_PROCPROBE=$spec"
    ) + $extra
    $cfg = Join-Path ([IO.Path]::GetTempPath()) 'launch.cfg'
    Set-Content -Path $cfg -Value $cfgText -Encoding ascii
    Copy-Item -Force $cfg (Join-Path $out 'launch.cfg')

    try { Invoke-Dp $xbox "/api/taskmanager/app?package=$packageB" -Method DELETE | Out-Null } catch {}
    Start-Sleep 3
    New-DpDirectory $xbox $fn '\LocalState'
    Send-DpFile $xbox $fn '\LocalState' $cfg
    Write-Host "[pp] === run '$run': $spec"
    Invoke-Dp $xbox "/api/taskmanager/app?appid=$appidB&package=$packageB" -Method POST | Out-Null

    $tmp = Join-Path $out 'log.txt'
    $deadline = (Get-Date).AddSeconds($TimeoutSec); $done = $false; $shown = 0
    while ((Get-Date) -lt $deadline -and -not $done) {
        Start-Sleep 5
        try { Receive-DpFile $xbox $fn '\LocalState' 'log.txt' $tmp } catch { continue }
        $lines = @(Get-Content $tmp -Encoding utf8 | Where-Object { $_ -match 'procprobe' })
        if ($lines.Count -gt $shown) { $lines[$shown..($lines.Count - 1)] | Where-Object { $_ -notmatch ' t=\s*\d+s ' -or $_ -match 'TERM|FROZEN' } | ForEach-Object { Write-Host $_ }; $shown = $lines.Count }
        if ($lines -match 'procprobe: DONE') { $done = $true }
    }

    # pull every probe file in LocalState
    $u = "/api/filesystem/apps/files?knownfolderid=LocalAppData&packagefullname=$([uri]::EscapeDataString($fn))&path=$([uri]::EscapeDataString('\LocalState'))"
    $names = @()
    try { $names = @((Invoke-Dp $xbox $u).Items | ForEach-Object { $_.Id }) } catch { Write-Host "[pp] list failed: $_" }
    foreach ($n in $names | Where-Object { $_ -like 'pp-child-*.log' -or $_ -like 'log-child-*.txt' -or $_ -eq 'log.txt' }) {
        try { Receive-DpFile $xbox $fn '\LocalState' $n (Join-Path $out $n) } catch { Write-Host "[pp] pull $n failed" }
    }
    try { Invoke-Dp $xbox "/api/taskmanager/app?package=$packageB" -Method DELETE | Out-Null } catch {}
    $v = @(Get-Content $tmp -Encoding utf8 | Where-Object { $_ -match 'procprobe: (VERDICT|main job)|TERMINATED' })
    Write-Host "[pp] run '$run' $(if ($done) { 'finished' } else { 'TIMED OUT' }); saved $out"
    $v | ForEach-Object { Write-Host "    $_" }
}
} finally {
    try { Invoke-Dp $xbox "/api/taskmanager/app?package=$packageB" -Method DELETE | Out-Null } catch {}
    if (Test-Path $backup) {
        $restore = Join-Path ([IO.Path]::GetTempPath()) 'launch.cfg'
        Copy-Item -Force $backup $restore
        Send-DpFile $xbox $fn '\LocalState' $restore
        Write-Host '[pp] restored the original LocalState\launch.cfg'
    } else {
        try { Invoke-Dp $xbox (Get-DpFileUrl $fn '\LocalState' 'launch.cfg') -Method DELETE | Out-Null; Write-Host '[pp] removed the probe launch.cfg (the installed one applies again)' }
        catch { Write-Warning "[pp] could not remove LocalState\launch.cfg: $_" }
    }
}

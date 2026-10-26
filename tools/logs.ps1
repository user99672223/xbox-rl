<#
.SYNOPSIS
  Pull LocalState/log.txt (the single log for host + layer + wine) off the console.

.DESCRIPTION
  Saves to results/log-<UTC timestamp>.txt and results/log-latest.txt (both git-ignored),
  prints the PROBE_RESULT / error / warning lines, then the tail. Never guess what the
  console did - read this.

.PARAMETER Prev     Fetch log.prev.txt (the previous run, rotated at app start).
.PARAMETER Tail     Lines of the log to print (default 60; 0 = whole file).
.PARAMETER Follow   Keep polling and print new lines until Ctrl+C.
.PARAMETER Quiet    Only save, print the path.
#>
[CmdletBinding()]
param([switch]$Prev, [int]$Tail = 60, [switch]$Follow, [switch]$Quiet, [string]$File)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
$pkg = Get-RequiredPackage $xbox
$name = if ($File) { $File } elseif ($Prev) { 'log.prev.txt' } else { 'log.txt' }
$results = Join-Path $RepoRoot 'results'
New-Item -ItemType Directory -Force $results | Out-Null

function Get-Remote([string]$dest) {
    Receive-DpFile $xbox $pkg.PackageFullName '\LocalState' $name $dest
}

if ($Follow) {
    $tmp = Join-Path $results 'log-follow.tmp'
    $shown = 0
    Write-Host "[logs] following $name on $($xbox.Ip) (Ctrl+C to stop)"
    while ($true) {
        try {
            Get-Remote $tmp
            $lines = @(Get-Content $tmp -Encoding utf8)
            if ($lines.Count -lt $shown) { $shown = 0; Write-Host '--- log restarted (new run) ---' }
            if ($lines.Count -gt $shown) { $lines[$shown..($lines.Count - 1)] | ForEach-Object { Write-Host $_ }; $shown = $lines.Count }
        } catch { Write-Verbose $_.Exception.Message }
        Start-Sleep -Seconds 2
    }
}

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$base = [IO.Path]::GetFileNameWithoutExtension($name)
$out = Join-Path $results "$base-$stamp.txt"
try { Get-Remote $out } catch {
    if ((Get-DpStatus $_) -eq 404) { throw "$name does not exist yet on the console (the app has not been started since install, or the path is wrong)." }
    throw
}
Copy-Item $out (Join-Path $results "$base-latest.txt") -Force
$text = @(Get-Content $out -Encoding utf8)
Write-Host "[logs] saved $out ($($text.Count) lines) and results/$base-latest.txt"
if ($Quiet) { return }

$hl = $text | Where-Object { $_ -match 'PROBE_RESULT| E |UNHANDLED|PROBE END|TIMEOUT|still waiting' }
if ($hl) { Write-Host "`n--- key lines ---"; $hl | ForEach-Object { Write-Host $_ } }
Write-Host "`n--- tail ---"
if ($Tail -gt 0) { $text | Select-Object -Last $Tail | ForEach-Object { Write-Host $_ } } else { $text | ForEach-Object { Write-Host $_ } }

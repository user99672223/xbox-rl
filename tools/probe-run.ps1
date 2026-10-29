param([int]$Wait = 18)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"
$x = Get-XboxConfig; $pkg = Get-RequiredPackage $x
$b64 = { param($s) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
$package = [uri]::EscapeDataString((& $b64 $pkg.PackageFullName))
$appid = [uri]::EscapeDataString((& $b64 $pkg.PackageRelativeId))
for ($i = 0; $i -lt 3; $i++) {
    try { Invoke-Dp $x "/api/taskmanager/app?package=$package" -Method DELETE | Out-Null; Write-Host "terminate $i ok" }
    catch { Write-Host ("terminate {0}: HTTP {1}" -f $i, $_.Exception.Data['Status']) }
    Start-Sleep 2
}
Write-Host "--- relaunch once ---"
Invoke-Dp $x "/api/taskmanager/app?appid=$appid&package=$package" -Method POST | Out-Null
Write-Host "launched; sleeping $Wait s"
Start-Sleep $Wait
$tmp = Join-Path $RepoRoot 'results\probe.tmp'
Receive-DpFile $x $pkg.PackageFullName '\LocalState' 'log.txt' $tmp
$lines = Get-Content $tmp
Write-Host ("lines: {0}" -f $lines.Count)
($lines | Select-Object -Last 14) -join "`n"

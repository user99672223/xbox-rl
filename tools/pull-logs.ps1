# Pull log.txt and every log-child-*.txt from the app's LocalState into results/.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"
$x = Get-XboxConfig; $pkg = Get-RequiredPackage $x
$results = Join-Path $RepoRoot 'results'
New-Item -ItemType Directory -Force $results | Out-Null
# list LocalState
$u = "/api/filesystem/apps/files?knownfolderid=LocalAppData&packagefullname=$([uri]::EscapeDataString($pkg.PackageFullName))&path=$([uri]::EscapeDataString('\LocalState'))"
$names = @()
try { $r = Invoke-Dp $x $u; $names = @($r.Items | Where-Object { $_.Type -eq 32 -or $_.Id } | ForEach-Object { $_.Id }) } catch { Write-Host "list failed: $_" }
if (-not $names) { $names = @('log.txt') }
$want = $names | Where-Object { $_ -eq 'log.txt' -or $_ -like 'log-child-*.txt' }
if (-not $want) { $want = @('log.txt') }
foreach ($n in $want) {
    $out = Join-Path $results $n
    try { Receive-DpFile $x $pkg.PackageFullName '\LocalState' $n $out; Write-Host ("pulled {0} ({1} lines)" -f $n, (Get-Content $out).Count) }
    catch { Write-Host "pull $n failed: $_" }
}

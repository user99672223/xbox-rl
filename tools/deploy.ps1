<#
.SYNOPSIS
  Package -> console: upload + register the host appx over Device Portal
  (https://$XBOX_IP:11443), using the last build recorded by tools/build.ps1.

.DESCRIPTION
  Reads XBOX_IP / XBOX_USER / XBOX_PASS from ../.env. Steps:
    1. check Device Portal is reachable (auth works)
    2. upload the dev signing certificate (.cer) so the console trusts the package
    3. upload the .msix (+ the VCLibs dependency .appx) and poll until installed
    4. confirm via the package list and print the PackageFullName / tile name

  Installing a HIGHER version over an existing one is an in-place UPDATE: LocalState
  (log.txt, rootfs/, the multi-GB game copy) is preserved. tools/build.ps1 stamps a
  strictly increasing version, so a plain build+deploy always updates in place.
  Use -Uninstall only if you really want to wipe the app AND its LocalState.

  This registers the app. It does NOT start it: Device Portal app-launch returns 403 on
  the console, so a human presses the tile in Dev Home (tools/run.ps1 prompts for that).
#>
[CmdletBinding()]
param(
    [switch]$SkipCert,
    [switch]$SkipDependencies,
    [switch]$Uninstall,     # remove the installed package (DESTROYS LocalState, incl. the game copy)
    [int]$TimeoutSec = 300
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
Write-Host "[deploy] Device Portal: $($xbox.Base) (user $($xbox.User))"

# 1. reachability / auth ------------------------------------------------------------
$ok = $false
foreach ($ep in '/api/os/info', '/api/os/machinename', '/api/app/packagemanager/packages') {
    try { $r = Invoke-Dp $xbox $ep -TimeoutSec 20; $ok = $true; Write-Host "[deploy] reachable ($ep)"; break }
    catch { Write-Verbose $_.Exception.Message; if ((Get-DpStatus $_) -eq 401) { throw "Device Portal rejected the credentials (401). Check XBOX_USER / XBOX_PASS in .env." } }
}
if (-not $ok) { throw "Cannot reach Device Portal at $($xbox.Base). Is Remote access enabled in Dev Home, and is XBOX_IP right?" }

if ($Uninstall) {
    $p = Get-InstalledPackage $xbox
    if (-not $p) { Write-Host '[deploy] nothing to uninstall'; return }
    Write-Warning "Uninstalling $($p.PackageFullName) will DELETE its LocalState (log, rootfs, game copy)."
    if ((Read-Host 'Type YES to continue') -ne 'YES') { Write-Host 'aborted'; return }
    Invoke-Dp $xbox "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" -Method DELETE | Out-Null
    Write-Host '[deploy] uninstalled'
    return
}

$bi = Read-BuildInfo
$msix = $bi.msix; $cer = $bi.cer
if (-not (Test-Path $msix)) { throw "Package not found: $msix - run tools/build.ps1" }
Write-Host "[deploy] package: $msix  (v$($bi.version), $($bi.mode), $($bi.config))"

$before = Get-InstalledPackage $xbox
if ($before) { Write-Host "[deploy] currently installed: v$($before.VersionString)  $($before.PackageFullName)" }

# 2. certificate ---------------------------------------------------------------------
if (-not $SkipCert -and (Test-Path $cer)) {
    Write-Host "[deploy] uploading certificate $(Split-Path $cer -Leaf)"
    try {
        Invoke-Dp $xbox '/api/app/packagemanager/certificate' -Method POST -Form @{ certificate = (Get-Item $cer) } | Out-Null
    } catch {
        # The earlier probe used this endpoint; if this console build lacks it, the .cer is also
        # included in the package upload below (Device Portal installs a .cer found there).
        Write-Warning "certificate endpoint failed (continuing, .cer is also sent with the package): $($_.Exception.Message)"
    }
}

# 3. package -------------------------------------------------------------------------
$files = @($msix)
if (-not $SkipDependencies) { foreach ($d in $bi.dependencies) { if (Test-Path $d) { $files += $d } } }
if (-not $SkipCert -and (Test-Path $cer)) { $files += $cer }
Write-Host "[deploy] uploading: $(($files | ForEach-Object { Split-Path $_ -Leaf }) -join ', ')"
$q = [uri]::EscapeDataString((Split-Path $msix -Leaf))
Invoke-DpMultipart $xbox "/api/app/packagemanager/package?package=$q" -Files $files -TimeoutSec 0 | Out-Null

Write-Host '[deploy] waiting for the console to finish installing...'
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$last = ''
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    try {
        $s = Invoke-Dp $xbox '/api/app/packagemanager/state' -TimeoutSec 20
        if ($null -eq $s -or "$s" -eq '') { break }          # 204 / empty: no install in progress
        $line = "Code=$($s.Code) Success=$($s.Success) $($s.CodeText) $($s.Reason)"
        if ($line -ne $last) { Write-Host "         $line"; $last = $line }
        $hasSuccess = $s.PSObject.Properties['Success']
        $hasCode    = $s.PSObject.Properties['Code']
        if ($hasSuccess -and $s.Success -eq $true) { break }
        # Failure = a negative (HRESULT-style) code, or Success=false WITH a reason. A small
        # non-zero code with no reason is treated as "still installing" (final arbiter is the
        # package-list check below).
        $reason = if ($s.PSObject.Properties['Reason']) { "$($s.Reason)" } else { '' }
        if ($hasSuccess -and $s.Success -eq $false -and (($hasCode -and $s.Code -lt 0) -or $reason)) {
            throw "install failed: Code=$($s.Code) $($s.CodeText) $reason"
        }
        # otherwise: still in progress -> keep polling
    } catch {
        $st = Get-DpStatus $_
        if ($st -in 404, 204) { break }                        # state endpoint idle
        if ($_.Exception.Message -like 'install failed*') { throw }
        Write-Verbose $_.Exception.Message
    }
}

# 4. confirm -------------------------------------------------------------------------
$after = Get-InstalledPackage $xbox
if (-not $after -or $after.VersionString -ne $bi.version) {
    throw "install did not complete: console reports $(if ($after) { 'v' + $after.VersionString } else { 'no package' }), wanted v$($bi.version). Check Dev Home for an error."
}
Write-Host ''
Write-Host "[deploy] INSTALLED  $($after.PackageFullName)"
Write-Host "[deploy] AUMID      $($after.PackageFamilyName)!$script:AppId"
Write-Host '[deploy] next: pwsh tools/run.ps1   (then press the "xbox-rl" tile in Dev Home on the console)'

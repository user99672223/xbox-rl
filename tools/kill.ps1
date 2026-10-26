<#
.SYNOPSIS
  Terminate the running host app on the console.

.DESCRIPTION
  Device Portal's taskmanager REST API returns 403 on the console for launch, and may for
  terminate as well (docs/decisions.md). This tries DELETE /api/taskmanager/app; if the
  console refuses, it prints the manual procedure. Exit code 0 = Device Portal accepted the
  request, 2 = refused (do it by hand).
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
$pkg = Get-RequiredPackage $xbox
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pkg.PackageFullName))
try {
    Invoke-Dp $xbox "/api/taskmanager/app?package=$([uri]::EscapeDataString($b64))&forcestop=yes" -Method DELETE | Out-Null
    Write-Host "[kill] Device Portal accepted terminate for $($pkg.PackageFullName)"
} catch {
    $st = Get-DpStatus $_
    Write-Warning "Device Portal refused terminate (HTTP $st): $($_.Exception.Message)"
    Write-Host ''
    Write-Host '  ON THE CONSOLE (manual):'
    Write-Host '    press the Xbox button -> highlight the "xbox-rl" tile -> Menu button -> Quit'
    Write-Host '    (or hold B / Xbox button -> Quit; if it is hung, Dev Home -> Running apps -> Quit)'
    exit 2
}

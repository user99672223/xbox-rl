<#
 .SYNOPSIS  Build + test gfx/ (Workstream D).
 .DESCRIPTION
   Desktop : static lib + xrl_gfx_harness.exe (dev-PC test; real GPU, HWND swapchain)
   Uwp     : static lib built with WINAPI_FAMILY_APP + a DLL link-check against the
             UWP linker (WindowsApp.lib, /APPCONTAINER). Library only, no run.
   Default : both. -Run executes the desktop harness afterwards (exit code 0 == PASS).
 .EXAMPLE   .\gfx\build.ps1 -Run
#>
param([switch]$Desktop, [switch]$Uwp, [switch]$Run, [string]$Config = "Release")
$ErrorActionPreference = "Stop"
if (-not ($Desktop -or $Uwp)) { $Desktop = $true; $Uwp = $true }
$here = $PSScriptRoot

if ($Desktop) {
    cmake -S $here -B "$here\build-desktop" -G "Visual Studio 17 2022" -A x64
    cmake --build "$here\build-desktop" --config $Config
    if ($LASTEXITCODE) { throw "desktop build failed" }
}
if ($Uwp) {
    cmake -S $here -B "$here\build-uwp" -G "Visual Studio 17 2022" -A x64 `
          -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0
    cmake --build "$here\build-uwp" --config $Config
    if ($LASTEXITCODE) { throw "uwp build failed" }
}
if ($Run -and $Desktop) {
    & "$here\build-desktop\$Config\xrl_gfx_harness.exe" @args
    exit $LASTEXITCODE
}

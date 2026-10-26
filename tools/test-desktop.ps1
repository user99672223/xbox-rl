<#
.SYNOPSIS
  Build and run the host probe as a plain DESKTOP exe on this laptop (no UWP
  registration, no Developer Mode needed). Validates everything except the
  CoreWindow swapchain step before you spend a console round-trip.

.NOTES
  Allocates and touches 4 GiB of RAM and plays a 200 ms quiet beep (XAudio2 probe).
  Output: host/build/test/desktop_probe.exe ; log in %TEMP%\log.txt (+ echoed here).
#>
[CmdletBinding()]
param([switch]$NoRun)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$host_ = Join-Path $RepoRoot 'host'
$out = Join-Path $host_ 'build\test'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$src = 'log config gfx_thread gfx_ops_default audio_ops input_ops probe boot layer_stub' -split ' ' | ForEach-Object { "`"$host_\src\$_.cpp`"" }
$cl = @(
    '/nologo', '/std:c++20', '/EHa', '/O2', '/W3', '/Zi', '/permissive-',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE',
    "/I`"$host_`"", "/I`"$host_\src`"", "/I`"$RepoRoot\include`"",
    "/Fo`"$out\\`"", "/Fd`"$out\\`"", "/Fe`"$out\desktop_probe.exe`"",
    "`"$host_\test\desktop_probe_main.cpp`""
) + $src + @('/link', 'windowsapp.lib', 'd3d12.lib', 'd3d11.lib', 'dxgi.lib', 'xaudio2.lib', 'user32.lib', 'ole32.lib')

$cmd = "call `"$vcvars`" >nul && cl $($cl -join ' ')"
Write-Host '[test] compiling desktop probe...'
cmd /c $cmd
if ($LASTEXITCODE) { throw "desktop probe build failed ($LASTEXITCODE)" }
if ($NoRun) { return }

Write-Host '[test] running desktop probe...'
& "$out\desktop_probe.exe"
$rc = $LASTEXITCODE
$log = Join-Path $env:TEMP 'log.txt'
if (Test-Path $log) { Write-Host "`n---- $log ----"; Get-Content $log }
exit $rc

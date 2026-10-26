<#
.SYNOPSIS
  Build + package + sign the host UWP app. Output is a .msix ready for tools/deploy.ps1.

.PARAMETER Mode
  Probe (default) : no SDL, no layer. The first-milestone build: logs D3D12 device,
                    D3D11On12 device, RWX VirtualAlloc, 4 GiB allocation, ... to log.txt.
  Full            : SDL2 WinRT front-end + boot context + layer seam (layer is a stub
                    until -LayerLib is supplied).

.PARAMETER Config      Release (default; what the console wants) or Debug.
.PARAMETER LayerLib    Path to Workstream C's layer.lib (Full mode).
.PARAMETER GfxLib      Path to Workstream D's gfx.lib (replaces the default gfx ops).
.PARAMETER Version     Override the package version (a.b.c.d). Default: 1.<days since
                       2026-10-26>.<minute of day>.0 - strictly increasing so an update
                       install always replaces the previous build WITHOUT wiping
                       LocalState (where the multi-GB game copy lives).
.PARAMETER Rebuild     Clean rebuild.

.EXAMPLE
  pwsh tools/build.ps1                    # probe package
  pwsh tools/build.ps1 -Mode Full         # SDL host
#>
[CmdletBinding()]
param(
    [ValidateSet('Probe', 'Full')][string]$Mode = 'Probe',
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [string]$LayerLib,
    [string]$GfxLib,
    [string]$Version,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$hostDir = Join-Path $RepoRoot 'host'
$msbuild = Find-Msbuild
Write-Host "[build] MSBuild: $msbuild"

# --- prerequisites ------------------------------------------------------------
& "$PSScriptRoot\fetch-deps.ps1"
if ($Mode -eq 'Full') { & "$PSScriptRoot\apply-patches.ps1" }
& "$PSScriptRoot\new-cert.ps1"

function Invoke-Msb([string[]]$ArgList) {
    # Filter the (expected, harmless) D9025 WINAPI_FAMILY override noise; keep real output.
    & $msbuild @ArgList 2>&1 | Where-Object { $_ -notmatch 'D9025' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE) { throw "MSBuild failed ($LASTEXITCODE)" }
}

# --- SDL2 (Full) ----------------------------------------------------------------
$sdlOut = Join-Path $hostDir 'build\sdl'
if ($Mode -eq 'Full') {
    Write-Host '[build] SDL2 (WinRT backend, Release)'
    $sdlProj = Join-Path $RepoRoot 'vendor\SDL2\VisualC-WinRT\SDL-UWP.vcxproj'
    # SDL's project hardcodes v142 / SDK "10.0" / app type revision 8.2; override from the
    # command line (global properties win) instead of patching the vendored project.
    Invoke-Msb @($sdlProj, '-nologo', '-v:m', '-m',
        "-p:Configuration=Release;Platform=x64;PlatformToolset=v143;WindowsTargetPlatformVersion=10.0.26100.0;ApplicationTypeRevision=10.0;OutDir=$sdlOut\;IntDir=$hostDir\build\sdl-int\")
}

# --- version-stamped manifest ---------------------------------------------------
if (-not $Version) {
    $now = Get-Date
    $days = [int]([datetime]::new($now.Year, $now.Month, $now.Day) - [datetime]'2026-10-26').TotalDays
    if ($days -lt 0) { $days = 0 }
    $Version = '1.{0}.{1}.0' -f $days, [int]($now.TimeOfDay.TotalMinutes)
}
$mdir = Join-Path $hostDir 'build\manifest'
New-Item -ItemType Directory -Force $mdir | Out-Null
$manifest = Join-Path $mdir 'Package.appxmanifest'
$text = Get-Content (Join-Path $hostDir 'Package.appxmanifest') -Raw
$stamped = [regex]::Replace($text, '(<Identity\b[^>]*?\bVersion=")[^"]*(")', "`${1}$Version`${2}", 'Singleline')
if ($stamped -eq $text -and $Version -ne '1.0.0.0') { throw 'failed to stamp manifest version' }
Set-Content -Path $manifest -Value $stamped -NoNewline -Encoding utf8
Write-Host "[build] package version $Version"

# --- host app --------------------------------------------------------------------
$props = "Configuration=$Config;Platform=x64;XrlMode=$Mode;XrlManifest=$manifest;XrlSdlLibDir=$sdlOut\SDL2-UWP\"
if ($LayerLib) { $props += ";XrlLayerLib=$((Resolve-Path $LayerLib).Path)" }
if ($GfxLib)   { $props += ";XrlGfxLib=$((Resolve-Path $GfxLib).Path)" }
$args_ = @((Join-Path $hostDir 'XboxRL.vcxproj'), '-nologo', '-v:m', "-p:$props")
if ($Rebuild) { $args_ += '-t:Rebuild' }
Write-Host "[build] host app ($Mode, $Config)"
Invoke-Msb $args_

# --- locate + record the package ----------------------------------------------------
$pkgRoot = Join-Path $hostDir 'build\package'
$msix = Get-ChildItem $pkgRoot -Recurse -Filter "XboxRL_${Version}_x64.msix" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $msix) { throw "expected package XboxRL_${Version}_x64.msix not found under $pkgRoot" }
$dir = $msix.Directory.FullName
$cer = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($msix.Name) + '.cer')
$deps = @(Get-ChildItem (Join-Path $dir 'Dependencies\x64') -Filter '*.appx' -ErrorAction SilentlyContinue | ForEach-Object FullName)
$info = [ordered]@{
    packageName = $PackageName
    version     = $Version
    mode        = $Mode
    config      = $Config
    msix        = $msix.FullName
    cer         = $(if (Test-Path $cer) { $cer } else { (Join-Path $hostDir 'signing\xbox-rl-dev.cer') })
    dependencies = $deps
    builtUtc    = (Get-Date).ToUniversalTime().ToString('o')
}
$info | ConvertTo-Json | Set-Content (Join-Path $pkgRoot 'latest.json') -Encoding utf8

Write-Host ''
Write-Host "[build] OK  $($msix.FullName)  ($([math]::Round($msix.Length/1KB)) KiB)"
Write-Host "[build]     cert: $($info.cer)"
foreach ($d in $deps) { Write-Host "[build]     dep : $d" }
Write-Host '[build] next: pwsh tools/deploy.ps1'

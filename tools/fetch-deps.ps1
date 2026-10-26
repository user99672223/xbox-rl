<#
.SYNOPSIS
  Fetch the pinned third-party dependencies Workstream A needs into vendor/.

.DESCRIPTION
  vendor/<tree>/ is git-ignored (see /.gitignore); what we version are the PINS
  (vendor/PINS.md + the constants below) and patches/*.patch. This script is the
  single place the pins live in executable form. Re-running is idempotent.

    * SDL2  - libsdl-org/SDL at tag $SdlTag (commit $SdlCommit), WinRT backend.
    * Microsoft.Windows.CppWinRT NuGet package $CppWinRtVersion - provides the
      MSBuild integration (midlrt /winrt, cppwinrt projection, winmd generation
      for our own runtimeclass) that the VS UWP workload alone does not.

  Upstream trees are NEVER edited in place. Source changes are patches in
  patches/sdl2-*.patch applied by tools/apply-patches.ps1.
#>
[CmdletBinding()]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

# ---- PINS (keep in sync with vendor/PINS.md) --------------------------------
$SdlTag          = 'release-2.32.10'
$SdlCommit       = '5d249570393f7a37e037abf22cd6012a4cc56a71'
$SdlUrl          = 'https://github.com/libsdl-org/SDL.git'
$CppWinRtVersion = '2.0.250303.1'
# SHA256 of the .nupkg as downloaded from nuget.org on 2026-10-26. If this
# mismatches, STOP and investigate before building (supply-chain check).
$CppWinRtSha256  = '955E3051B35DB1C00177ED14AB6B7B995C3B32A53FDB6C184EA53B1DBA66E439'  # nuget.org, fetched 2026-10-26; build/ dir byte-identical to the copy used by the earlier probe project
# -----------------------------------------------------------------------------

$vendor = Join-Path $RepoRoot 'vendor'

# ---- SDL2 -------------------------------------------------------------------
$sdlDir = Join-Path $vendor 'SDL2'
if ($Force -and (Test-Path $sdlDir)) { Remove-Item -Recurse -Force $sdlDir }
if (-not (Test-Path (Join-Path $sdlDir '.git'))) {
    Write-Host "[fetch] SDL2 $SdlTag"
    git clone --depth 1 --branch $SdlTag $SdlUrl $sdlDir
    if ($LASTEXITCODE) { throw 'git clone SDL failed' }
}
$head = (git -C $sdlDir rev-parse HEAD).Trim()
if ($head -ne $SdlCommit) {
    throw "SDL2 HEAD is $head, pin is $SdlCommit. Run with -Force (this discards vendor/SDL2) or fix the pin."
}
Write-Host "[fetch] SDL2 at pinned commit $head"

# ---- Microsoft.Windows.CppWinRT ----------------------------------------------
$pkgDir = Join-Path $vendor "packages\Microsoft.Windows.CppWinRT.$CppWinRtVersion"
if ($Force -and (Test-Path $pkgDir)) { Remove-Item -Recurse -Force $pkgDir }
if (-not (Test-Path (Join-Path $pkgDir 'build\native\Microsoft.Windows.CppWinRT.props'))) {
    Write-Host "[fetch] Microsoft.Windows.CppWinRT $CppWinRtVersion"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "cppwinrt-$CppWinRtVersion"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $nupkg = Join-Path $tmp "cppwinrt.$CppWinRtVersion.nupkg"
    Invoke-WebRequest -UseBasicParsing "https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/$CppWinRtVersion" -OutFile $nupkg
    $sha = (Get-FileHash $nupkg -Algorithm SHA256).Hash
    Write-Host "[fetch]   sha256 = $sha"
    if ($CppWinRtSha256 -and $sha -ne $CppWinRtSha256) { throw "CppWinRT nupkg sha256 mismatch (got $sha, pinned $CppWinRtSha256)" }
    New-Item -ItemType Directory -Force $pkgDir | Out-Null
    Expand-Archive -Path $nupkg -DestinationPath $pkgDir -Force
}
Write-Host "[fetch] CppWinRT at $pkgDir"

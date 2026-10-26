<#
.SYNOPSIS
  Apply patches/sdl2-*.patch to the pinned vendor/SDL2 tree (idempotent).

.DESCRIPTION
  The vendored upstream tree is never edited by hand. Every source change is a
  patches/sdl2-NNNN-*.patch (git-format, `git apply`-able from the SDL root). This script
  applies the ones not applied yet and records them in vendor/SDL2/.xrl-patches-applied.
  Currently there are NO SDL patches: the pinned release builds and works unmodified on the
  WinRT backend with command-line toolset overrides (see host/notes-A.md).
#>
[CmdletBinding()]
param([switch]$Reset)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$sdl = Join-Path $RepoRoot 'vendor\SDL2'
if (-not (Test-Path (Join-Path $sdl '.git'))) { throw 'vendor/SDL2 missing - run tools/fetch-deps.ps1 first' }
$marker = Join-Path $sdl '.xrl-patches-applied'

if ($Reset) {
    git -C $sdl checkout -- . ; git -C $sdl clean -fdq
    Remove-Item $marker -ErrorAction SilentlyContinue
    Write-Host '[patch] SDL2 tree reset to the pin'
}
$applied = @(); if (Test-Path $marker) { $applied = @(Get-Content $marker) }

$patches = @(Get-ChildItem (Join-Path $RepoRoot 'patches') -Filter 'sdl2-*.patch' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($patches.Count -eq 0) { Write-Host '[patch] no sdl2-*.patch files (SDL2 builds unmodified)'; return }
foreach ($p in $patches) {
    if ($applied -contains $p.Name) { Write-Host "[patch] already applied: $($p.Name)"; continue }
    git -C $sdl apply --check $p.FullName
    if ($LASTEXITCODE) { throw "patch does not apply cleanly: $($p.Name)" }
    git -C $sdl apply $p.FullName
    if ($LASTEXITCODE) { throw "git apply failed: $($p.Name)" }
    Add-Content $marker $p.Name
    Write-Host "[patch] applied $($p.Name)"
}

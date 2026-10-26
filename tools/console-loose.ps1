<#
.SYNOPSIS
  Install the host as a LOOSE-FILE app on the console with the game inside its install folder.

.DESCRIPTION
  The console's LocalState data area is too small for the 41 GB game (it filled at ~28.7 GB) and
  it also holds TEMP, where the layer swaps. So the game ships in the app's install folder, which
  on Dev Mode lives in the development storage (D:\DevelopmentFiles\LooseApps\<name>), and the
  layer mounts it at /game (launch.cfg mount=/game=pkg:game; layer/src/vfs.c XRL_MOUNTS).

  1. unpack the latest built .msix into a staging folder (drop the signature/block map: a loose
     registration has neither)
  2. -Uninstall: remove the installed msix package (DELETES its LocalState) - needed once, when
     switching from the msix install to the loose one
  3. copy the app files, then the game (GAME_SRC) into \\<console>\DevelopmentFiles\LooseApps\<name>
     with robocopy (resumable; only changed files go again). Needs an SMB session to the share:
     open \\<console>\DevelopmentFiles in File Explorer once and log in (Device Portal shows the
     credentials at /ext/smb/developerfolder)
  4. register the folder (POST /api/app/packagemanager/register?folder=<name>) and wait until installed
  Then upload the guest tree + launch.cfg and run: pwsh tools/console-rl.ps1 -SkipGame

.PARAMETER Name        Folder under LooseApps (default xbox-rl).
.PARAMETER Uninstall   Remove the msix-installed package first (wipes its LocalState).
.PARAMETER SkipGame    Do not (re)copy the game.
.PARAMETER ViaPortal   Upload the app files through Device Portal (no SMB session needed); with -SkipGame.
#>
[CmdletBinding()]
param([string]$Name = 'xbox-rl', [switch]$Uninstall, [switch]$SkipGame, [switch]$ViaPortal)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"
$xbox = Get-XboxConfig
$env1 = Import-DotEnv
$share = "\\$($xbox.Ip)\DevelopmentFiles\LooseApps\$Name"

# 1. staging
$bi = Read-BuildInfo
$stage = Join-Path $HOME "xrl-work\console\loose-$Name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($bi.msix, $stage)
foreach ($x in 'AppxSignature.p7x', 'AppxBlockMap.xml', '[Content_Types].xml', 'AppxMetadata') {
    $p = Join-Path $stage $x; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
Write-Host "[loose] staged $($bi.version) from $($bi.msix)"

# 2. uninstall the msix install (its LocalState goes with it)
if ($Uninstall) {
    $p = Get-InstalledPackage $xbox
    if ($p) {
        Write-Host "[loose] uninstalling $($p.PackageFullName) (and its LocalState)"
        Invoke-Dp $xbox "/api/app/packagemanager/package?package=$([uri]::EscapeDataString($p.PackageFullName))" -Method DELETE | Out-Null
        for ($i = 0; $i -lt 60 -and (Get-InstalledPackage $xbox); $i++) { Start-Sleep 2 }
    }
    # push-game's memory of what LocalState held is void now
    $sf = Join-Path $RepoRoot 'host\build\push-state.json'
    if (Test-Path $sf) {
        $j = Get-Content $sf -Raw | ConvertFrom-Json -AsHashtable
        foreach ($k in @($j.Keys)) { if ($k -like "$($xbox.Ip)|*") { $j.Remove($k) } }
        $j | ConvertTo-Json -Depth 4 | Set-Content $sf -Encoding utf8
    }
}

# 3. copy over SMB (-ViaPortal: the app files through Device Portal's DevelopmentFiles folder instead)
if ($ViaPortal) {
    if (-not $SkipGame) { throw '-ViaPortal copies the app files only: add -SkipGame (the game goes over SMB)' }
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $rel = [IO.Path]::GetDirectoryName($f.FullName.Substring($stage.Length).TrimStart('\'))
        $dir = "\LooseApps\$Name" + $(if ($rel) { "\$rel" } else { '' })
        New-DpDirectory $xbox '' $dir -KnownFolder DevelopmentFiles
        Send-DpFile $xbox '' $dir $f.FullName -KnownFolder DevelopmentFiles
    }
    Write-Host "[loose] app files uploaded through Device Portal"
}
else {
    if (-not (Test-Path "\\$($xbox.Ip)\DevelopmentFiles")) {
        throw "No SMB session to \\$($xbox.Ip)\DevelopmentFiles: open it in File Explorer once and log in, then re-run (or use -ViaPortal -SkipGame)."
    }
    robocopy $stage $share /E /R:2 /W:2 /NP /NFL /NDL /XD game | Out-Host; if ($LASTEXITCODE -ge 8) { throw "app copy failed (robocopy $LASTEXITCODE)" }
}
if (-not $SkipGame) {
    $game = $env1['GAME_SRC']; if (-not $game) { throw 'GAME_SRC is not set in .env' }
    Write-Host "[loose] copying the game ($game) -> $share\game"
    robocopy $game "$share\game" /E /MT:16 /R:3 /W:5 /NP /NFL /NDL /BYTES | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "game copy failed (robocopy $LASTEXITCODE): re-run to resume" }
}

# 4. register
$p0 = Get-InstalledPackage $xbox
Write-Host "[loose] registering LooseApps\$Name"
# the console wants the folder name base64-encoded (plain: HTTP 400 "Failed to find the AppXManifest.xml")
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Name))
Invoke-Dp $xbox "/api/app/packagemanager/register?folder=$([uri]::EscapeDataString($b64))" -Method POST -TimeoutSec 600 | Out-Null
for ($i = 0; $i -lt 90; $i++) {
    $p = Get-InstalledPackage $xbox
    if ($p -and (-not $p0 -or $p.VersionString -ne $p0.VersionString -or $p.PackageFullName -ne $p0.PackageFullName)) { break }
    if ($p -and $i -ge 5) { break }
    Start-Sleep 2
}
if (-not $p) { throw 'registration did not show up in the package list' }
Write-Host "[loose] installed: $($p.PackageFullName) v$($p.VersionString)"

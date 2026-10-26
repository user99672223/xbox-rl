<#
.SYNOPSIS
  Upload one local file into the installed app's LocalState (the only writable place
  besides TEMP).

.EXAMPLE
  pwsh tools/push-file.ps1 -Local .\launch.cfg                     # -> LocalState\launch.cfg
  pwsh tools/push-file.ps1 -Local .\wineprefix.tar -Remote rootfs\  # -> LocalState\rootfs\wineprefix.tar

  launch.cfg (key=value, see host/src/config.cpp) changes argv/env/memory budget/probe
  selection without rebuilding the package.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Local,
    [string]$Remote = ''          # LocalState-relative directory; '' = LocalState itself
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$f = (Resolve-Path $Local).Path
$xbox = Get-XboxConfig
$pkg = Get-RequiredPackage $xbox
$dir = ('\LocalState\' + $Remote.Trim('\', '/').Replace('/', '\')).TrimEnd('\')
if ($dir -eq '') { $dir = '\LocalState' }
Write-Host "[push-file] $f -> $dir\$(Split-Path $f -Leaf)  ($([math]::Round((Get-Item $f).Length/1KB)) KiB)"
New-DpDirectory $xbox $pkg.PackageFullName $dir
Send-DpFile $xbox $pkg.PackageFullName $dir $f
Write-Host '[push-file] done'

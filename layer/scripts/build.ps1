<#
 build.ps1 - build the layer static library.
   pwsh layer/scripts/build.ps1                 # desktop flavour + test host
   pwsh layer/scripts/build.ps1 -Flavor uwp     # UWP/Xbox flavour (xrl_layer.lib for host/)
   pwsh layer/scripts/build.ps1 -Flavor both
 Outputs: layer/build/<flavor>/xrl_layer.lib (and xrl_test_host.exe for desktop).
#>
[CmdletBinding()]
param([ValidateSet("desktop","uwp","both")][string]$Flavor = "desktop", [string]$Config = "RelWithDebInfo", [switch]$Clean)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot/vsenv.ps1" 2>$null
$layer = Split-Path -Parent $PSScriptRoot
foreach ($f in @(if ($Flavor -eq "both") { "desktop","uwp" } else { $Flavor })) {
  $b = Join-Path $layer "build/$f"
  if ($Clean -and (Test-Path $b)) { Remove-Item -Recurse -Force $b }
  $uwp = if ($f -eq "uwp") { "ON" } else { "OFF" }
  cmake -S $layer -B $b -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DXRL_UWP=$uwp"
  if ($LASTEXITCODE) { throw "cmake configure failed ($f)" }
  cmake --build $b
  if ($LASTEXITCODE) { throw "build failed ($f)" }
  Write-Host "[layer] $f OK -> $b"
}

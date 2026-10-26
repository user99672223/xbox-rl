<#
.SYNOPSIS
  Get Rocket League's Epic login arguments from the (logged-in) Epic Games
  Launcher, for a run of the game on the layer.

.DESCRIPTION
  Starts the game through the launcher (com.epicgames.launcher://apps/Sugar),
  waits for RocketLeague_EAC.exe / RocketLeague.exe to appear with
  -AUTH_PASSWORD (a one-time exchange code) on its command line, stops that
  native process at once (before it uses the code) and writes the arguments
  to <Out> (JSON: captured, exe, args[]). The code is short-lived and single
  use: capture right before each run.

  The file holds a login secret: it lives outside the repo (default
  C:\Users\<you>\xrl-work\epic-launch) and nothing here prints its values.

.PARAMETER Out   Output JSON path.
#>
param([string]$Out = (Join-Path $HOME 'xrl-work\epic-launch\launch-args.json'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$t0 = Get-Date
Start-Process 'com.epicgames.launcher://apps/Sugar?action=launch&silent=true'
$found = $null
while (((Get-Date) - $t0).TotalSeconds -lt 120 -and -not $found) {
    $found = Get-CimInstance Win32_Process -Filter "Name LIKE 'RocketLeague%'" |
        Where-Object { $_.CommandLine -match 'AUTH_PASSWORD' } | Select-Object -First 1
    if (-not $found) { Start-Sleep -Milliseconds 200 }
}
Get-CimInstance Win32_Process -Filter "Name LIKE 'RocketLeague%'" |
    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch {} }
if (-not $found) { Write-Error 'no Rocket League process with a login code within 120 s (is the launcher running and logged in?)'; exit 1 }
# the arguments after the exe path, split the way the launcher wrote them (-name=value, no quoting needed)
$cl = $found.CommandLine
$rest = if ($cl.StartsWith('"')) { $cl.Substring($cl.IndexOf('"', 1) + 1) } else { $cl.Substring($cl.IndexOf(' ') + 1) }
$args = @($rest.Trim() -split '\s+(?=-)' | Where-Object { $_ })
@{ captured = (Get-Date -Format o); exe = $found.Name; args = $args } | ConvertTo-Json | Set-Content -Encoding utf8 $Out
"[epic-capture] $($args.Count) arguments from $($found.Name) after $([int]((Get-Date) - $t0).TotalSeconds) s -> $Out (native game stopped)"

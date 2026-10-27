<#
.SYNOPSIS
  Copy files to the Xbox via Device Portal.  By default uploads the Rocket League install
  (GAME_SRC) into the package's install folder (LooseApps) at pkg:game, matching the
  mount=/game=pkg:game directive in launch.cfg.

.DESCRIPTION
  * Resumable: a local state file (host/build/push-state.json) records each file's size +
    mtime once uploaded OK; re-runs skip unchanged files. -Force re-uploads everything.
  * Parallel: -Throttle concurrent uploads (default 4); each file is streamed, not buffered.
  * Retries each file 3x. Failures are listed at the end and the exit code is non-zero.
  * A canary upload into a nested directory runs first so a "Device Portal does not create
    missing directories" surprise fails fast instead of 40 GB in.

  -ToPackage (default for game): uploads into the package install folder via DevelopmentFiles
  (D:\DevelopmentFiles\LooseApps\xbox-rl\<Dest>). Use for large read-only data (the game).

  Without -ToPackage: uploads into LocalState\<Dest>. Use for rootfs and other writable data.

.PARAMETER Source    Defaults to GAME_SRC in .env.
.PARAMETER Dest      Target subfolder (default 'game' with -ToPackage, 'rootfs\game' without).
.PARAMETER ToPackage Upload to the package install dir (LooseApps) instead of LocalState.
.PARAMETER Exclude   Wildcard patterns on the relative path to skip (e.g. '*.pdb','Logs\*').
.PARAMETER Yes       Don't ask for confirmation.
.PARAMETER DryRun    List what would be uploaded and the total size, then stop.
#>
[CmdletBinding()]
param(
    [string]$Source,
    [string]$Dest,
    [switch]$ToPackage,
    [string[]]$Exclude = @(),
    [int]$Throttle = 4,
    [switch]$Force,
    [switch]$Yes,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$xbox = Get-XboxConfig
if (-not $Source) { $Source = $xbox.GameSrc }
if (-not $Source) { throw 'No source: pass -Source or set GAME_SRC in .env' }
if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "GAME_SRC not found: $Source" }
$Source = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
if (-not $Dest) { $Dest = if ($ToPackage) { 'game' } else { 'rootfs\game' } }
$Dest = $Dest.Trim('\', '/').Replace('/', '\')
$pkg = Get-RequiredPackage $xbox
$full = $pkg.PackageFullName

# Package dir (LooseApps) vs LocalState
$kf = if ($ToPackage) { 'DevelopmentFiles' } else { 'LocalAppData' }
$looseName = ($full -split '_')[0..1] -join '_' -replace '_.*', ''
$looseName = 'xbox-rl'
$baseDir = if ($ToPackage) { "\LooseApps\$looseName\$Dest" } else { "\LocalState\$Dest" }
$dpFull = if ($ToPackage) { '' } else { $full }

if (-not (Test-Path (Join-Path $Source 'Binaries\Win64\RocketLeague.exe'))) {
    Write-Warning "Binaries\Win64\RocketLeague.exe not found under $Source - is GAME_SRC the Rocket League install root?"
}

Write-Host "[push-game] scanning $Source ..."
$all = Get-ChildItem -LiteralPath $Source -Recurse -File -Force
$items = foreach ($f in $all) {
    $rel = $f.FullName.Substring($Source.Length + 1)
    $skip = $false
    foreach ($x in $Exclude) { if ($rel -like $x) { $skip = $true; break } }
    if (-not $skip) { [pscustomobject]@{ Rel = $rel; Full = $f.FullName; Size = $f.Length; Stamp = "$($f.Length)|$($f.LastWriteTimeUtc.Ticks)" } }
}
$items = @($items)

$stateFile = Join-Path $RepoRoot 'host\build\push-state.json'
New-Item -ItemType Directory -Force (Split-Path $stateFile -Parent) | Out-Null
$stateKey = "$($xbox.Ip)|$Dest"
$state = @{}
if ((Test-Path $stateFile) -and -not $Force) {
    $j = Get-Content $stateFile -Raw | ConvertFrom-Json -AsHashtable
    if ($j.ContainsKey($stateKey)) { $state = $j[$stateKey] }
}
$todo = @($items | Where-Object { $state[$_.Rel] -ne $_.Stamp })
$totalAll = [long]0; foreach ($i in $items) { $totalAll += $i.Size }
$totalTodo = [long]0; foreach ($i in $todo) { $totalTodo += $i.Size }
Write-Host ("[push-game] {0} files / {1:N1} GiB total; {2} files / {3:N1} GiB to upload (rest already pushed)" -f $items.Count, ($totalAll / 1GB), $todo.Count, ($totalTodo / 1GB))
Write-Host "[push-game] target: $($xbox.Ip)  $full  $baseDir"
if ($DryRun) { $todo | Select-Object -First 50 | ForEach-Object { Write-Host "   $($_.Rel)  ($([math]::Round($_.Size/1MB,1)) MiB)" }; if ($todo.Count -gt 50) { Write-Host "   ... and $($todo.Count-50) more" }; return }
if ($todo.Count -eq 0) { Write-Host '[push-game] nothing to do'; return }
if (-not $Yes) { if ((Read-Host 'Upload now? (y/N)') -notmatch '^[yY]') { Write-Host 'aborted'; return } }

function Save-State($st) {
    $j = @{}
    if (Test-Path $stateFile) { try { $j = Get-Content $stateFile -Raw | ConvertFrom-Json -AsHashtable } catch { $j = @{} } }
    $j[$stateKey] = $st
    $j | ConvertTo-Json -Depth 4 | Set-Content $stateFile -Encoding utf8
}

# 1. directories: the upload endpoint does not create them (verified on the console), so create
#    every level once, parents first, before any upload.
$dirs = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$stopDir = if ($ToPackage) { '\LooseApps' } else { '\LocalState' }
foreach ($f in $todo) {
    $d = $baseDir + $(if ($f.Rel.Contains('\')) { '\' + (Split-Path $f.Rel -Parent) } else { '' })
    while ($d -and $d -ne $stopDir) { [void]$dirs.Add($d); $d = Split-Path $d -Parent }
}
$made = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($d in ($dirs | Sort-Object { ($_ -split '\\').Count })) {
    $parent = Split-Path $d -Parent; $name = Split-Path $d -Leaf
    try { Invoke-Dp $xbox ((Get-DpFileUrl $dpFull $parent '' 'folder' $kf) + "&newfoldername=$([uri]::EscapeDataString($name))") -Method POST | Out-Null }
    catch {
        $list = Invoke-Dp $xbox (Get-DpFileUrl $dpFull $parent '' 'files' $kf)
        if (-not ($list.Items | Where-Object { $_.Name -eq $name -and $_.Type -eq 16 })) { throw "cannot create $d : $($_.Exception.Message)" }
    }
    [void]$made.Add($d)
}
Write-Host "[push-game] $($made.Count) directories ready"

# 2. canary: smallest file first, so a format/size surprise fails before the big files
$canary = $todo | Sort-Object Size | Select-Object -First 1
$canaryDir = $baseDir + $(if ($canary.Rel.Contains('\')) { '\' + (Split-Path $canary.Rel -Parent) } else { '' })
Write-Host "[push-game] canary upload: $($canary.Rel) -> $canaryDir"
try { Send-DpFile $xbox $dpFull $canaryDir $canary.Full -KnownFolder $kf }
catch { throw "Canary upload failed. Details: $($_.Exception.Message)" }
$state[$canary.Rel] = $canary.Stamp
Write-Host '[push-game] canary ok'

$common = Join-Path $PSScriptRoot '_common.ps1'
$done =[System.Collections.Concurrent.ConcurrentDictionary[string, string]]::new()
$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$counter = [hashtable]::Synchronized(@{ n = 0; bytes = 0L; last = [datetime]::UtcNow; t0 = [datetime]::UtcNow })
$total = $todo.Count; $totalBytes = $totalTodo

try {
    $todo | Where-Object { $_.Rel -ne $canary.Rel } | ForEach-Object -ThrottleLimit $Throttle -Parallel {
        $f = $_
        . $using:common
        if (-not (Get-Variable -Name xbx -Scope Script -ErrorAction SilentlyContinue)) { $script:xbx = Get-XboxConfig }
        $dir = $using:baseDir + $(if ($f.Rel.Contains('\')) { '\' + (Split-Path $f.Rel -Parent) } else { '' })
        $ok = $false
        for ($try = 1; $try -le 3 -and -not $ok; $try++) {
            try {
                Send-DpFile $script:xbx $using:dpFull $dir $f.Full -KnownFolder $using:kf
                $ok = $true
            } catch {
                if ($try -eq 3) { ($using:failed).Add("$($f.Rel): $($_.Exception.Message)") } else { Start-Sleep -Seconds (2 * $try) }
            }
        }
        if ($ok) { ($using:done)[$f.Rel] = $f.Stamp }
        $c = $using:counter
        $c.n++; if ($ok) { $c.bytes += $f.Size }
        if (([datetime]::UtcNow - $c.last).TotalSeconds -ge 5) {
            $c.last = [datetime]::UtcNow
            $el = ([datetime]::UtcNow - $c.t0).TotalSeconds
            Write-Host ("[push-game] {0}/{1} files, {2:N2} GiB, {3:N1} MiB/s" -f $c.n, $using:total, ($c.bytes / 1GB), (($c.bytes / 1MB) / [math]::Max($el, 1)))
        }
    }
} finally {
    foreach ($k in $done.Keys) { $state[$k] = $done[$k] }
    Save-State $state
}

Write-Host ''
if ($failed.Count) {
    Write-Warning "$($failed.Count) file(s) failed:"
    $failed | Select-Object -First 25 | ForEach-Object { Write-Host "   $_" }
    Write-Host '[push-game] re-run to retry only the failures (successful files are remembered).'
    exit 1
}
Write-Host "[push-game] OK - $($todo.Count) file(s) pushed to $baseDir"

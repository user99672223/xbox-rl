<#
.SYNOPSIS
  Copy the Rocket League install (GAME_SRC from .env) into the package's rootfs under
  LocalState via Device Portal.  Only LocalState and TEMP are writable on the console
  (docs/decisions.md), so the game lives at

      <LocalState>\rootfs\game\...        (guest path /game, xrl_boot_context::game_path)

.DESCRIPTION
  * Resumable: a local state file (host/build/push-state.json) records each file's size +
    mtime once uploaded OK; re-runs skip unchanged files. -Force re-uploads everything.
    (The console's directory *listing* reports stale sizes, so it is not used for resume.)
  * Parallel: -Throttle concurrent uploads (default 4); each file is streamed, not buffered.
  * Retries each file 3x. Failures are listed at the end and the exit code is non-zero.
  * A canary upload into a nested directory runs first so a "Device Portal does not create
    missing directories" surprise fails fast instead of 40 GB in.

  Needs: the host package installed (tools/deploy.ps1) so its LocalState exists.
  UNVERIFIED on the console (needs the physical box): nested-directory creation, the max
  single-upload size Device Portal accepts (RL has multi-GB .upk/.bulk files), and sustained
  throughput. See host/notes-A.md "Open questions".

.PARAMETER Source    Defaults to GAME_SRC in .env.
.PARAMETER Dest      LocalState-relative target (default rootfs\game).
.PARAMETER Exclude   Wildcard patterns on the relative path to skip (e.g. '*.pdb','Logs\*').
.PARAMETER Yes       Don't ask for confirmation.
.PARAMETER DryRun    List what would be uploaded and the total size, then stop.
#>
[CmdletBinding()]
param(
    [string]$Source,
    [string]$Dest = 'rootfs\game',
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
$Dest = $Dest.Trim('\', '/').Replace('/', '\')
$pkg = Get-RequiredPackage $xbox
$full = $pkg.PackageFullName

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
Write-Host "[push-game] target: $($xbox.Ip)  $full  \LocalState\$Dest"
if ($DryRun) { $todo | Select-Object -First 50 | ForEach-Object { Write-Host "   $($_.Rel)  ($([math]::Round($_.Size/1MB,1)) MiB)" }; if ($todo.Count -gt 50) { Write-Host "   ... and $($todo.Count-50) more" }; return }
if ($todo.Count -eq 0) { Write-Host '[push-game] nothing to do'; return }
if (-not $Yes) { if ((Read-Host 'Upload now? (y/N)') -notmatch '^[yY]') { Write-Host 'aborted'; return } }

function Save-State($st) {
    $j = @{}
    if (Test-Path $stateFile) { try { $j = Get-Content $stateFile -Raw | ConvertFrom-Json -AsHashtable } catch { $j = @{} } }
    $j[$stateKey] = $st
    $j | ConvertTo-Json -Depth 4 | Set-Content $stateFile -Encoding utf8
}

# canary: smallest file first, into its real nested directory
$canary = $todo | Sort-Object Size | Select-Object -First 1
$canaryDir = '\LocalState\' + $Dest + $(if ($canary.Rel.Contains('\')) { '\' + (Split-Path $canary.Rel -Parent) } else { '' })
Write-Host "[push-game] canary upload: $($canary.Rel) -> $canaryDir"
try { Send-DpFile $xbox $full $canaryDir $canary.Full }
catch { throw "Canary upload failed - Device Portal may not create missing directories, or the path form is wrong. Details: $($_.Exception.Message)" }
$state[$canary.Rel] = $canary.Stamp
Write-Host '[push-game] canary ok'

$cred = $xbox.Cred; $base = $xbox.Base; $insecure = $xbox.Base.StartsWith('http://')
$done = [System.Collections.Concurrent.ConcurrentDictionary[string, string]]::new()
$failed = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$counter = [hashtable]::Synchronized(@{ n = 0; bytes = 0L; last = [datetime]::UtcNow; t0 = [datetime]::UtcNow })
$total = $todo.Count; $totalBytes = $totalTodo

try {
    $todo | Where-Object { $_.Rel -ne $canary.Rel } | ForEach-Object -ThrottleLimit $Throttle -Parallel {
        $f = $_
        $dir = '\LocalState\' + $using:Dest + $(if ($f.Rel.Contains('\')) { '\' + (Split-Path $f.Rel -Parent) } else { '' })
        $uri = "$($using:base)/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$([uri]::EscapeDataString($using:full))&path=$([uri]::EscapeDataString($dir))"
        $extra = @{}; if ($using:insecure) { $extra['AllowUnencryptedAuthentication'] = $true }
        $ok = $false
        for ($try = 1; $try -le 3 -and -not $ok; $try++) {
            try {
                Invoke-RestMethod -Uri $uri -Method POST -Credential $using:cred -Authentication Basic -SkipCertificateCheck @extra `
                    -Form @{ ($f.Rel | Split-Path -Leaf) = (Get-Item -LiteralPath $f.Full) } -TimeoutSec 0 | Out-Null
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
Write-Host "[push-game] OK - $($todo.Count) file(s) pushed to LocalState\$Dest (guest path /game)"

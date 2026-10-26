<#
 vendor.ps1 - fetch Blink + Cosmopolitan at the commits pinned in layer/vendor.pins
 into layer/vendor/<name>, verify the commit, then apply layer/patches/<name>/*.patch.

   pwsh layer/scripts/vendor.ps1            # fetch (if missing) + verify + patch
   pwsh layer/scripts/vendor.ps1 -Clean     # delete layer/vendor/<name> first
   pwsh layer/scripts/vendor.ps1 -Only blink

 Rules: vendor/ trees are never edited by hand. All changes are patches under
 layer/patches/<name>/NNNN-*.patch (git-format, applied with `git apply`).
 Cosmopolitan is fetched with a sparse checkout (only the dirs named in the pins
 file) because the full tree is >1 GB.
#>
[CmdletBinding()]
param([switch]$Clean, [string]$Only = "")
$ErrorActionPreference = "Stop"
$layer  = Split-Path -Parent $PSScriptRoot
$pins   = Join-Path $layer "vendor.pins"
$vendor = Join-Path $layer "vendor"
New-Item -ItemType Directory -Force $vendor | Out-Null

function Invoke-Git { & git @args; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed ($LASTEXITCODE)" } }

foreach ($line in Get-Content $pins) {
  if ($line -match '^\s*(#|$)') { continue }
  $name, $url, $ref, $commit, $sparse = $line.Split('|')
  if ($Only -and $Only -ne $name) { continue }
  $dir = Join-Path $vendor $name
  if ($Clean -and (Test-Path $dir)) { Remove-Item -Recurse -Force $dir }
  if (-not (Test-Path (Join-Path $dir ".git"))) {
    Write-Host "[vendor] fetching $name @ $ref ($commit)"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Invoke-Git -C $dir init -q
    Invoke-Git -C $dir remote add origin $url
    if ($sparse -ne '*') {
      Invoke-Git -C $dir config core.sparseCheckout true
      Invoke-Git -C $dir sparse-checkout init --no-cone
      Set-Content -Encoding ascii (Join-Path $dir ".git/info/sparse-checkout") ($sparse.Split(',') | ForEach-Object { "/$_" })
    }
    $free = (Get-PSDrive ($dir.Substring(0,1))).Free
    if ($free -lt 1GB) { throw "[vendor] only $([int]($free/1MB)) MB free; refusing to fetch $name" }
    if ($sparse -ne '*') { Invoke-Git -C $dir fetch -q --depth 1 --filter=blob:none origin $commit }
    else { Invoke-Git -C $dir fetch -q --depth 1 origin $commit }
    Invoke-Git -C $dir checkout -q --detach FETCH_HEAD
  }
  $head = (& git -C $dir rev-parse HEAD).Trim()
  if ($head -ne $commit) { throw "[vendor] $name is at $head, pin says $commit (run with -Clean)" }

  # patches: apply only those not yet applied (marker file lists applied names)
  $marker = Join-Path $dir ".xrl-applied-patches"
  $applied = @(); if (Test-Path $marker) { $applied = Get-Content $marker }
  $pdir = Join-Path $layer "patches/$name"
  if (Test-Path $pdir) {
    foreach ($p in Get-ChildItem $pdir -Filter *.patch | Sort-Object Name) {
      if ($applied -contains $p.Name) { continue }
      Write-Host "[vendor] applying $name/$($p.Name)"
      Invoke-Git -C $dir apply --whitespace=nowarn $p.FullName
      Add-Content $marker $p.Name
    }
  }
  Write-Host "[vendor] $name OK @ $head"
}

<#
.SYNOPSIS
  Regenerates the placeholder tile/logo PNGs in host/Assets (committed). Run only
  if you want to change the artwork; the build does not need this.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\host\Assets'
New-Item -ItemType Directory -Force $out | Out-Null

function New-Png([string]$name, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $bg = [System.Drawing.Color]::FromArgb(255, 0x1b, 0x1b, 0x2f)
    $g.Clear($bg)
    $accent = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x10, 0x7c, 0x10))   # Xbox green
    $d = [Math]::Min($w, $h) * 0.62
    $g.FillEllipse($accent, ($w - $d) / 2, ($h - $d) / 2, $d, $d)
    $fs = [Math]::Max(6, [int]($d * 0.34))
    $font = New-Object System.Drawing.Font 'Segoe UI', $fs, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $g.DrawString('XRL', $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, 0, $w, $h), $sf)
    $g.Dispose()
    $bmp.Save((Join-Path $out $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
New-Png 'StoreLogo.png' 50 50
New-Png 'Square44x44Logo.png' 44 44
New-Png 'Square150x150Logo.png' 150 150
New-Png 'Wide310x150Logo.png' 310 150
New-Png 'SplashScreen.png' 620 300
Get-ChildItem $out | Select-Object Name, Length

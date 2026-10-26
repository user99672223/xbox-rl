<#
 .SYNOPSIS  Screenshot a top-level window of this desktop and check one pixel against an expected colour.
 .DESCRIPTION
   Used to prove that frames presented by the guest reach the screen: the desktop test host's window is
   brought to the front, its client area is copied from the screen (what the user sees, through DWM),
   saved as PNG, and the centre pixel is compared with -Expect "R,G,B" (tolerance -Tolerance per channel).
   Exit code 0 = match, 1 = mismatch, 2 = window not found.
 .EXAMPLE
   pwsh tools/capture-window.ps1 -Out results/present-1.png -Expect 51,153,255 -CommandLineMatch rootfs-av
#>
param(
    [string]$Title = "xbox-rl (desktop host)",
    [string]$CommandLineMatch,     # pick the xrl_test_host whose command line contains this (several may run)
    [Parameter(Mandatory)][string]$Out,
    [string]$Expect,               # "R,G,B"
    [int]$Tolerance = 3,
    [int]$WaitSeconds = 20
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class XrlWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][XrlWin]::SetProcessDPIAware()
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$h = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    if ($CommandLineMatch) {
        $procs = Get-CimInstance Win32_Process -Filter "Name='xrl_test_host.exe'" | Where-Object { $_.CommandLine -like "*$CommandLineMatch*" }
        foreach ($pr in $procs) {
            $mh = (Get-Process -Id $pr.ProcessId -ErrorAction SilentlyContinue).MainWindowHandle
            if ($mh -and $mh -ne [IntPtr]::Zero) { $h = $mh; break }
        }
    } else {
        $h = [XrlWin]::FindWindowW($null, $Title)
    }
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 300
}
if ($h -eq [IntPtr]::Zero) { Write-Host "window '$Title' not found"; exit 2 }

# HWND_TOPMOST (-1), SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW
[void][XrlWin]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
[void][XrlWin]::SetForegroundWindow($h)

$want = if ($Expect) { @($Expect -split '[,\s]+' | ForEach-Object { [int]$_ }) } else { $null }
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
# The program is still starting up when the window appears: capture until the colour shows (or time runs out).
$matched = $false
do {
    Start-Sleep -Milliseconds 700   # let DWM compose a few presented frames
    $r = New-Object XrlWin+RECT
    [void][XrlWin]::GetClientRect($h, [ref]$r)
    $p = New-Object XrlWin+POINT
    [void][XrlWin]::ClientToScreen($h, [ref]$p)
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
    $g.Dispose()
    $px = $bmp.GetPixel([int]($w / 2), [int]($ht / 2))
    if ($want) {
        $d = @([math]::Abs($px.R - $want[0]), [math]::Abs($px.G - $want[1]), [math]::Abs($px.B - $want[2]))
        $matched = (($d | Measure-Object -Maximum).Maximum -le $Tolerance)
    }
    if ($matched -or -not $want -or (Get-Date) -ge $deadline) { $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
} while ($want -and -not $matched -and (Get-Date) -lt $deadline)
Write-Host ("client {0}x{1} at ({2},{3}); centre pixel R={4} G={5} B={6}" -f $w, $ht, $p.X, $p.Y, $px.R, $px.G, $px.B)
if ($want) {
    if ($matched) { Write-Host "MATCH expected $($want -join ',')"; exit 0 }
    Write-Host "MISMATCH expected $($want -join ',')"; exit 1
}
exit 0

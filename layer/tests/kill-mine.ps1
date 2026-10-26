# kill-mine.ps1 [-List]: end the xrl_test_host.exe processes that belong to THIS worktree or to the EAC guest tree
# (rootfs-eac), nothing else. Other agents' runs use other executables and trees and are never touched: the match
# is on the process's own path / command line, not on "processes that were not there before".
param([switch]$List)
$mine = @('worktrees\eac-launch', 'rootfs-eac')
Get-CimInstance Win32_Process -Filter "Name='xrl_test_host.exe'" | Where-Object {
    $t = "$($_.ExecutablePath) $($_.CommandLine)"
    ($mine | Where-Object { $t -like "*$_*" }).Count -gt 0
} | ForEach-Object {
    if (-not $List) { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    $_.ProcessId
}

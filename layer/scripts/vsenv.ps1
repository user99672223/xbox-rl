<# Dot-source to import the VS2022 x64 build environment into the current PowerShell:
     . layer/scripts/vsenv.ps1
#>
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
if (-not (Test-Path $vs)) { $vs = (& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath) }
$bat = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$tmp = [IO.Path]::GetTempFileName()
cmd /c "`"$bat`" >nul && set > `"$tmp`""
Get-Content $tmp | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
Remove-Item $tmp

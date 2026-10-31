#!/usr/bin/env bash
# Compiles gfx/src/shaders/*.hlsl into the bytecode headers next to them (checked in, so builds need no fxc).
# Needs the Windows SDK's fxc.exe; run from Git Bash: bash gfx/src/shaders/gen-shaders.sh
set -euo pipefail
cd "$(dirname "$0")"
fxc=$(ls "/c/Program Files (x86)/Windows Kits/10/bin/"*/x64/fxc.exe | sort -V | tail -1)
build() {   # entry, profile, defines, output header, array name
    MSYS_NO_PATHCONV=1 "$fxc" /nologo /O3 /T "$2" /E "$1" $3 /Fh "$4" /Vn "$5" fsr1.hlsl
}
build vs vs_5_0 "" fsr1_vs.h g_fsr1_vs
build ps ps_5_0 "/D EASU=1" fsr1_easu_ps.h g_fsr1_easu_ps
build ps ps_5_0 "/D RCAS=1" fsr1_rcas_ps.h g_fsr1_rcas_ps

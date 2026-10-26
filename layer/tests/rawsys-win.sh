#!/bin/sh
# Raw Linux system calls from Windows code on Wine (Git Bash): a PE program (rawsys-win/rawsys_eac.c) issues
# `syscall` instructions like the EAC bootstrapper does (in its image, and in a VirtualAlloc'd RWX page);
# they must be answered by the layer as Linux system calls. Needs rawsys-win/build.sh (WSL) and a staged
# Wine guest tree (XRL_ROOTFS).
cd "$(dirname "$0")/../.."
root="${XRL_ROOTFS:-$(cygpath -w "$HOME/xrl-work/rootfs")}"
rootu=$(cygpath -u "$root")
mkdir -p "$rootu/exectest" results
cp layer/tests/rawsys-win/out/rawsys_eac.exe layer/tests/rawsys-win/out/xrl_mod.so "$rootu/exectest"/
WINEDEBUG="${WINEDEBUG:-err+all}" XRL_ROOTFS="$root" XRL_TIMEOUT="${XRL_TIMEOUT:-170}" \
    tools/run-desktop-wine.sh 'Z:\exectest\rawsys_eac.exe' > results/rawsys-win.txt 2>&1
rc=$?
grep -E "rawsys:|dbt:|FAULT|fault" results/rawsys-win.txt | cut -c1-200
ok=1
grep -q "RAWSYS-OK" results/rawsys-win.txt || { echo "FAIL: the program did not report RAWSYS-OK"; ok=0; }
grep -q "rawsys: FAIL" results/rawsys-win.txt && { echo "FAIL: a check failed"; ok=0; }
[ "$(grep -c 'case ' results/rawsys-win.txt)" -ge 2 ] || { echo "FAIL: not both cases ran"; ok=0; }
[ $ok = 1 ] && echo "rawsys-win: OK (rc=$rc)"
[ $ok = 1 ]

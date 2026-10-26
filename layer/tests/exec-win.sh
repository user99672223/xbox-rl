#!/bin/sh
# Exec handover on Wine (Git Bash): a Windows program CreateProcess()es a second one and exits; the second one
# must run in the same OS process. Needs layer/tests/exec-win/build.sh (WSL) and a staged Wine guest tree (XRL_ROOTFS).
cd "$(dirname "$0")/../.."
root="${XRL_ROOTFS:-$(cygpath -w "$HOME/xrl-work/rootfs")}"
rootu=$(cygpath -u "$root")
mkdir -p "$rootu/exectest"
cp layer/tests/exec-win/out/*.exe "$rootu/exectest"/
WINEDEBUG="${WINEDEBUG:-err+all}" XRL_ROOTFS="$root" XRL_TIMEOUT="${XRL_TIMEOUT:-170}" \
    tools/run-desktop-wine.sh 'Z:\exectest\parent.exe' 'Z:\exectest\child.exe' > results/exec-win.txt 2>&1
rc=$?
grep -E "guest.out\]|exec:|exec handover|boot: |exited|fault|FAULT" results/exec-win.txt | cut -c1-220
n_boot=$(grep -c "layer .*booting" results/exec-win.txt)
ok=1
grep -q "parent: pid" results/exec-win.txt || { echo "FAIL: parent did not run"; ok=0; }
grep -q "child: EXEC-HANDOVER-OK" results/exec-win.txt || { echo "FAIL: child did not run correctly"; ok=0; }
grep -q "parent: CreateProcess returned" results/exec-win.txt && { echo "FAIL: parent continued after CreateProcess"; ok=0; }
[ "$n_boot" = 1 ] || { echo "FAIL: $n_boot layer boots"; ok=0; }
grep -q "layer .*exec: .*old image kept" results/exec-win.txt || { echo "FAIL: no exec in the layer log"; ok=0; }
# two handovers in a row: parent -> chain(first) -> chain(hop2)
WINEDEBUG="${WINEDEBUG:-err+all}" XRL_ROOTFS="$root" XRL_TIMEOUT="${XRL_TIMEOUT:-170}"     tools/run-desktop-wine.sh 'Z:\exectest\parent.exe' 'Z:\exectest\chain.exe' > results/exec-win-chain.txt 2>&1
grep -E "chain:|exec: .*kept" results/exec-win-chain.txt | cut -c1-200
grep -q "hop 2 .*CHAIN-OK" results/exec-win-chain.txt || { echo "FAIL: the chain did not reach hop 2"; ok=0; }
grep -q "hop 1 continued" results/exec-win-chain.txt && { echo "FAIL: hop 1 continued after CreateProcess"; ok=0; }
[ "$(grep -c 'exec: .*old image kept' results/exec-win-chain.txt)" = 2 ] || { echo "FAIL: expected two execs"; ok=0; }
[ $ok = 1 ] && echo "exec-win: OK (rc=$rc)"
[ $ok = 1 ]

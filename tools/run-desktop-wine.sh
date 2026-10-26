#!/bin/sh
# run-desktop-wine.sh [wine args...]   (Git Bash on the laptop)
#
# Run the console build of Wine on the laptop through the real layer (the
# desktop test host), with the console's conditions emulated: no FSGSBASE,
# the layer's commit budget, and Wine's xbox-rl environment from wine/notes-B.md.
# Seconds per try instead of a console deploy. Stage the guest tree first:
#   wsl -- bash wine/scripts/stage-rootfs.sh /mnt/c/Users/<you>/xrl-work/rootfs
#
#   tools/run-desktop-wine.sh 'C:\windows\system32\cmd.exe' /c ver
#
# XRL_ROOTFS overrides the staged tree; the log is printed and kept in
# results/desktop-wine-latest.txt.
cd "$(dirname "$0")/.." || exit 2
root="${XRL_ROOTFS:-$(cygpath -w "$HOME/xrl-work/rootfs")}"
host=layer/build/desktop/xrl_test_host.exe
[ -x "$host" ] || { echo "build the layer first: pwsh layer/scripts/build.ps1 -Flavor desktop" >&2; exit 2; }
export XRL_NO_FSGSBASE=1 XRL_TEST_BUDGET_MB="${XRL_TEST_BUDGET_MB:-3584}"
export XRL_ENV_WINEPREFIX=/prefix XRL_ENV_WINEARCH=win64 XRL_ENV_USER=steamuser \
       XRL_ENV_XRL_HOST_USD=1 XRL_ENV_XRL_NO_SPAWN=1 XRL_ENV_XRL_SKIP_WINEBOOT=1 \
       XRL_ENV_WINEDEBUG="${WINEDEBUG:-err+all}" XRL_ENV_WINEDLLOVERRIDES=winemenubuilder.exe=d
mkdir -p results
# XRL_CATCH=1: run under layer/tests/catch.c to see faults the VEH never gets
catch=; [ "${XRL_CATCH:-0}" = 1 ] && { catch=layer/build/desktop/xrl_catch.exe; host=$(cygpath -w "$host"); }
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-120}" $catch "$host" "$root" /usr/lib/wine/x86_64-unix/wine64 "$@" \
    > results/desktop-wine-latest.txt 2>&1
rc=$?
cat results/desktop-wine-latest.txt
echo "[run-desktop-wine] exit $rc"
exit $rc

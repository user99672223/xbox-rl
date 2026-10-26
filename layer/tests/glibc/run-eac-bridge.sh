#!/bin/sh
# Run the EAC bridge chain on the layer with a stand-in client module (Git Bash).
#   needs: build-standin.sh run in WSL; a staged Wine guest tree (wine/scripts/stage-rootfs.sh);
#          the Proton EAC runtime (v2/lib64/easyanticheat_x64.{dll,so}).
#   XRL_ROOTFS         the staged tree (default: $HOME/xrl-work/rootfs)
#   EAC_RUNTIME        host folder of the Proton EasyAntiCheat Runtime (default: $HOME/xrl-work/eac-runtime)
cd "$(dirname "$0")/../../.."
src=layer/tests/glibc/out/eactest
[ -f "$src/eac_bridge_test.exe" ] || { echo "run layer/tests/glibc/build-standin.sh in WSL first" >&2; exit 2; }
root="${XRL_ROOTFS:-$(cygpath -w "$HOME/xrl-work/rootfs")}"
rt="${EAC_RUNTIME:-$HOME/xrl-work/eac-runtime}"
[ -d "$rt/v2/lib64" ] || { echo "no Proton EAC runtime at $rt (set EAC_RUNTIME)" >&2; exit 2; }
rootu=$(cygpath -u "$root")
mkdir -p "$rootu/eactest" "$rootu/eac-runtime"
cp -r "$rt"/. "$rootu/eac-runtime"/
cp "$src"/* "$rootu/eactest"/
export XRL_ENV_PROTON_EAC_RUNTIME=/eac-runtime
WINEDEBUG="${WINEDEBUG:-err+all}" XRL_ROOTFS="$root" XRL_TIMEOUT="${XRL_TIMEOUT:-120}" \
    tools/run-desktop-wine.sh 'Z:\eactest\eac_bridge_test.exe' 'Z:\eactest\easyanticheat_x64.dll'

#!/usr/bin/env bash
# run-pe-test.sh -- end-to-end test of the Wine forwarders on a PC.
#
# Needs: the hosttest libc (with the fake host op-tables), the hosttest Wine build and a baked
# prefix.  Builds tests/pe/xrl_pe_test.c with mingw and runs it as the main program of the musl
# Wine under the in-process model (wineserver thread, XRL_NO_SPAWN):
#
#   XRL_HOSTTEST=1 wine/scripts/build-wine.sh musl tools configure build
#   wine/scripts/bake-prefix.sh
#   wine/tests/run-pe-test.sh
. "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"
here="$(dirname "${BASH_SOURCE[0]}")"
out="$WORK/build/pe-test"; mkdir -p "$out"
x86_64-w64-mingw32-gcc -O1 -o "$out/xrl_pe_test.exe" "$here/pe/xrl_pe_test.c" || die "mingw build failed"
export WINEPREFIX="${WINEPREFIX:-$WORK/out/prefix}"
[ -f "$WINEPREFIX/system.reg" ] || die "no baked prefix at $WINEPREFIX (run bake-prefix.sh)"
cp "$out/xrl_pe_test.exe" "$WINEPREFIX/drive_c/xrl_pe_test.exe"
exec "$here/run-wine-hosttest.sh" 'C:\xrl_pe_test.exe'

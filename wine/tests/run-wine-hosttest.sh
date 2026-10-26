#!/usr/bin/env bash
# run-wine-hosttest.sh <wine args...>
#
# Run the *musl-built, gateway-patched* Wine under WSL, against the laptop-only pass-through
# libc (sysroot-hosttest).  This exercises everything except the Xbox-specific parts: the musl
# port, every syscall going through xrl_syscall, the in-process wineserver thread, the patched
# PE syscall stubs, and (with XRL_NO_SPAWN=1) the in-process CreateProcess stubs.
#
#   XRL_HOSTTEST=1 wine/scripts/build-wine.sh musl tools configure build     # once
#   wine/tests/run-wine-hosttest.sh cmd /c ver
#
# Defaults mimic what the layer will set on the console, except XRL_HOST_USD (a Linux box has no
# KUSER_SHARED_DATA at 0x7ffe0000, so Wine's own is used).
. "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"
SYS="$WORK/sysroot-hosttest"; BLD="$WORK/build/wine-hosttest"
[ -x "$BLD/loader/wine" ] || die "no hosttest build at $BLD (see header of this script)"

export WINEPREFIX="${WINEPREFIX:-$WORK/prefix-hosttest}"
export WINEDEBUG="${WINEDEBUG:--all,err+all,fixme-all}"
export WINEARCH=win64
export XRL_NO_SPAWN="${XRL_NO_SPAWN:-1}"
export XRL_SKIP_WINEBOOT="${XRL_SKIP_WINEBOOT:-1}"
export XRL_HOST_USD="${XRL_HOST_USD:-0}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-winemenubuilder.exe=d;mscoree=d;mshtml=d}"
mkdir -p "$WINEPREFIX"

# Build-tree layout puts each .so in its own directory, and musl only resolves DT_NEEDED
# ("ntdll.so") through the library search path.
export LD_LIBRARY_PATH="$(find "$BLD" -name '*.so' -printf '%h:' | tr ':' '
' | sort -u | tr '
' ':')${LD_LIBRARY_PATH:-}"

# `libc.so prog args` is musl's way of running a program with an explicit interpreter, which
# avoids needing /lib/ld-musl-x86_64.so.1 on the host.
exec "$SYS/usr/lib/libc.so" "$BLD/loader/wine" "$@"

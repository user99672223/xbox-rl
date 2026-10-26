#!/usr/bin/env bash
# glibc-compat-check.sh MODULE.so [GUEST_LIB_DIR]   (WSL/Linux)
#
# Will a glibc-built module `dlopen` in Wine's musl process? Prints what it needs that the guest
# cannot provide: undefined symbols (not weak) that neither musl's libc.so nor the glibc-compat
# shim (wine/glibc-compat) exports, and DT_NEEDED libraries missing from GUEST_LIB_DIR. Also
# reports what the code needs from the layer's rewriter: `syscall` instructions and %fs uses.
#   XRL_WORK   build area holding sysroot/usr/lib/libc.so (default ~/xrl-build)
# GUEST_LIB_DIR defaults to a directory with the freshly built shim (build-glibc-compat.sh).
set -euo pipefail
mod=${1:?usage: glibc-compat-check.sh MODULE.so [GUEST_LIB_DIR]}
here=$(cd "$(dirname "$0")" && pwd)
work=${XRL_WORK:-$HOME/xrl-build}
musl=$work/sysroot/usr/lib/libc.so
[ -f "$musl" ] || { echo "no $musl (build wine/scripts/build-musl.sh)" >&2; exit 2; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
libdir=${2:-}
if [ -z "$libdir" ]; then libdir=$tmp/lib; XRL_WORK=$work "$here/../wine/scripts/build-glibc-compat.sh" "$libdir" >/dev/null 2>&1; fi

nm -D --defined-only "$musl" "$libdir"/*.so* 2>/dev/null | awk 'NF>=3 {print $3}' | sed 's/@.*//' | sort -u > "$tmp/have"
nm -D --undefined-only "$mod" | awk '$1 != "w" && $1 != "v" && $1 == "U" {print $2}' | sed 's/@.*//' | sort -u > "$tmp/want"
missing=$(comm -23 "$tmp/want" "$tmp/have" || true)
echo "module: $mod"
echo "undefined symbols: $(wc -l < "$tmp/want"), provided by musl or the shim: $(comm -12 "$tmp/want" "$tmp/have" | wc -l)"
if [ -n "$missing" ]; then echo "NOT provided (dlopen will fail naming the first of these):"; echo "$missing" | sed 's/^/  /'; rc=1; else echo "all undefined symbols are provided"; rc=0; fi

echo "needed libraries:"
readelf -d "$mod" | awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); print $5}' | while read -r n; do
  case "$n" in
    libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libutil.so*|libxnet.so*) echo "  $n: musl (its loader aliases these names to itself)" ;;
    *) if [ -e "$libdir/$n" ]; then echo "  $n: empty stub in the shim directory"; else echo "  $n: MISSING from $libdir (not provided by musl or the shim)"; fi ;;
  esac
done
echo "needs rewriting by the layer:"
od=$(objdump -d -w "$mod")
echo "  syscall instructions: $(grep -cE '^\s+[0-9a-f]+:\s+(0f 05)\s+syscall' <<<"$od" || true)"
echo "  %fs memory operands:  $(grep -c '%fs:' <<<"$od" || true)"
echo "  sysenter / int \$0x80: $(grep -cE '\s(sysenter|int\s+\$0x80)\b' <<<"$od" || true)"
exit $rc

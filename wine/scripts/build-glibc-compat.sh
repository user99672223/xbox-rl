#!/usr/bin/env bash
# build-glibc-compat.sh DESTLIB
#
# Build the glibc-ABI shim (wine/glibc-compat/glibc_compat.c) with the patched musl and install
# it into DESTLIB (a guest /lib) as libgcompat.so.0, plus two empty libraries (ld-linux-x86-64.so.2,
# libresolv.so.2) that glibc-built modules list as DT_NEEDED and musl cannot alias to itself. (musl's
# loader turns a DT_NEEDED of libc.so.N / libm / libdl / libpthread / librt / libutil into itself, so a
# shim named libc.so.6 would never be loaded: Wine patch 0017 dlopen()s libgcompat.so.0 RTLD_GLOBAL
# before the first native unix library instead.)
# Why: docs/decisions.md, "Easy Anti-Cheat" entry. Needs `wine/scripts/build-musl.sh` to have run.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need gcc python3 readelf
dest=${1:?usage: build-glibc-compat.sh DESTLIB}
cc="$SYSROOT/bin/xrl-musl-gcc"
[ -x "$cc" ] || die "no $cc -- run build-musl.sh first"
src="$WINE_DIR/glibc-compat/glibc_compat.c"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
# same code-generation rules as the rest of the guest: no stack-protector TLS guard, no direct %fs forms
flags="-O2 -fPIC -fno-stack-protector -fcf-protection=none -mno-tls-direct-seg-refs -Wall -Wno-unused-function"

"$cc" $flags -shared -Wl,-soname,libgcompat.so.0 -o "$tmp/libgcompat.so.0" "$src"
echo 'int xrl_glibc_compat_stub;' > "$tmp/stub.c"
for l in libresolv.so.2 ld-linux-x86-64.so.2; do
  "$cc" $flags -shared -Wl,-soname,"$l" -o "$tmp/$l" "$tmp/stub.c"
done
python3 "$ROOT/tools/xrl-tpfix.py" "$tmp"/*.so* >/dev/null
python3 "$ROOT/tools/xrl-tpfix.py" --check --stamp "$tmp"/*.so* >/dev/null
mkdir -p "$dest"
cp "$tmp"/*.so* "$dest"/
log "glibc-compat: installed libgcompat.so.0 (+2 empty libs) into $dest ($(nm -D --defined-only "$dest/libgcompat.so.0" | wc -l) symbols)"
readelf -d "$dest/libgcompat.so.0" | grep -E 'NEEDED|SONAME' >&2 || true

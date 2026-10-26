#!/usr/bin/env bash
# build-libgcc-s.sh DESTLIB
#
# Build a libgcc_s.so.1 for the guest (musl) from the toolchain's static GCC runtime (libgcc_eh.a + libgcc.a,
# the same sources as the shared one) and install it into DESTLIB (a guest /usr/lib).
# Why not the build machine's glibc-built libgcc_s.so.1: it defines some of its own symbols with a non-default
# version (`__cpu_indicator_init@GCC_4.8.0`) and relocates against them; musl's loader never matches a symbol
# whose version is hidden, so it fails with "Error relocating libgcc_s.so.1: __cpu_indicator_init: symbol not
# found" (measured). Linked from the archives the symbols are plain, which is also what musl systems get when
# GCC is configured for musl. A glibc-built module that lists libgcc_s.so.1 (the EAC module does) needs the
# unwinder (_Unwind_*) and the 128-bit helpers (__udivti3...) from it. The unwinder asks for the object that
# holds a pc through _dl_find_object (glibc 2.35+); wine/glibc-compat/libgcc_s_extra.c answers it (why there).
# Needs `wine/scripts/build-musl.sh` to have run (xrl-musl-gcc); same code rules as the rest of the guest.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need gcc python3 readelf
dest=${1:?usage: build-libgcc-s.sh DESTLIB}
cc="$SYSROOT/bin/xrl-musl-gcc"
[ -x "$cc" ] || die "no $cc -- run build-musl.sh first"
eh=$(gcc -print-file-name=libgcc_eh.a); gc=$(gcc -print-file-name=libgcc.a)
[ -f "$eh" ] && [ -f "$gc" ] || die "no libgcc_eh.a / libgcc.a from $(gcc --version | head -1)"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
# members of both archives, except the split-stack ones (morestack*.o read %fs:0x70: a glibc TCB field that
# means nothing here and that no code of ours uses; xrl-tpfix refuses direct %fs forms)
mkdir "$tmp/eh" "$tmp/gc"
( cd "$tmp/eh" && ar x "$eh" ) && ( cd "$tmp/gc" && ar x "$gc" )
rm -f "$tmp"/gc/*morestack*.o "$tmp"/eh/*morestack*.o
# the archives are built with hidden visibility (HIDE_EXPORTS), which a shared object cannot export
python3 "$ROOT/tools/xrl-elf-unhide.py" "$tmp"/eh/*.o "$tmp"/gc/*.o >/dev/null
"$cc" -O2 -fPIC -fno-stack-protector -fcf-protection=none -mno-tls-direct-seg-refs -c "$WINE_DIR/glibc-compat/libgcc_s_extra.c" -o "$tmp/extra.o"
"$cc" -shared -nostdlib -fPIC -Wl,-Bsymbolic -Wl,--eh-frame-hdr -Wl,-s -Wl,-soname,libgcc_s.so.1 "$tmp/extra.o" "$tmp"/eh/*.o "$tmp"/gc/*.o -o "$tmp/libgcc_s.so.1"
python3 "$ROOT/tools/xrl-tpfix.py" "$tmp/libgcc_s.so.1" >/dev/null
python3 "$ROOT/tools/xrl-tpfix.py" --check --stamp "$tmp/libgcc_s.so.1" >/dev/null
mkdir -p "$dest"
cp "$tmp/libgcc_s.so.1" "$dest/libgcc_s.so.1"
log "libgcc_s: installed libgcc_s.so.1 into $dest ($(nm -D --defined-only "$dest/libgcc_s.so.1" | grep -c ' T _Unwind_') _Unwind_ entry points, from $eh)"

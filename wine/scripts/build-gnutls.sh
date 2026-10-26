#!/usr/bin/env bash
# build-gnutls.sh -- gnutls (+ nettle) built against our musl, into $WORK/deps.
#
# Why: Wine's schannel (secur32) and bcrypt use gnutls, loaded with dlopen at run time and only if
# Wine was configured --with-gnutls. Without it every Windows TLS client that uses SSPI/schannel
# fails ("no schannel support"). Epic's Easy Anti-Cheat bootstrapper is one: its libcurl has only
# the schannel backend, so it cannot download the anti-cheat module (docs/decisions.md, "Exec handover").
#
#   XRL_WORK=~/xrl-build-eac wine/scripts/build-gnutls.sh
#
# Output: $WORK/deps/{include,lib}: libgnutls.so.30, libnettle.so.8, libhogweed.so.6 (shared, musl),
# which build-wine.sh configures Wine against and installs next to ntdll.so's rootfs (usr/lib).
# nettle uses its own mini-gmp (no libgmp); libtasn1 and libunistring are the copies inside gnutls;
# no p11-kit, no zlib, no assembler (the console has no AES-NI proof: plain C is enough here).
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need curl tar make gcc sha256sum pkg-config m4

NETTLE=nettle-3.10.2;  NETTLE_SHA=fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
GNUTLS=gnutls-3.8.12;  GNUTLS_SHA=a7b341421bfd459acf7a374ca4af3b9e06608dcd7bd792b2bf470bea012b8e51
# (hashes recorded when first fetched, 2026-10-26, from ftp.gnu.org/gnu/nettle and www.gnupg.org/ftp/gcrypt/gnutls/v3.8)

DEPS="$WORK/deps"; SRCD="$WORK/deps-src"
CC="$SYSROOT/bin/xrl-musl-gcc"
[ -x "$CC" ] || die "no $CC: run wine/scripts/build-musl.sh first"
mkdir -p "$SRCD"; cd "$SRCD"

if [ -f "$DEPS/lib/libgnutls.so" ] && [ -f "$DEPS/.xrl-built" ] && [ "$(cat "$DEPS/.xrl-built")" = "$NETTLE $GNUTLS" ] && [ "${1:-}" != "--force" ]; then
  log "gnutls: already built ($(cat "$DEPS/.xrl-built"))"; exit 0
fi

fetch() {  # name url sha
  [ -f "$1" ] || { log "fetch $1"; curl -fsSL -m 300 -o "$1.part" "$2" && mv "$1.part" "$1"; }
  echo "$3  $1" | sha256sum -c --status || die "$1: sha256 mismatch"
}
fetch $NETTLE.tar.gz https://ftp.gnu.org/gnu/nettle/$NETTLE.tar.gz $NETTLE_SHA
fetch $GNUTLS.tar.xz https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/$GNUTLS.tar.xz $GNUTLS_SHA

cap_jobs
rm -rf "$DEPS" "$SRCD/$NETTLE" "$SRCD/$GNUTLS"; mkdir -p "$DEPS"
export PATH="$SYSROOT/bin:$PATH"
# the flags Wine itself is built with: no stack protector or TLS segment forms the console cannot run
CF="-O2 -g0 -fcf-protection=none -fno-stack-protector -mno-tls-direct-seg-refs"

log "nettle: configure + build (-j$JOBS)"
tar xf $NETTLE.tar.gz && cd $NETTLE
CC="$CC" CFLAGS="$CF" ./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu \
    --prefix="$DEPS" --disable-static --enable-shared --enable-mini-gmp --disable-assembler \
    --disable-documentation --disable-openssl >configure.log 2>&1 || { tail -20 configure.log; die "nettle configure failed"; }
make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; die "nettle build failed"; }
cd "$SRCD"

log "gnutls: configure + build (-j$JOBS)"
tar xf $GNUTLS.tar.xz && cd $GNUTLS
PKG_CONFIG_PATH="$DEPS/lib/pkgconfig" CC="$CC" CFLAGS="$CF -I$DEPS/include" LDFLAGS="-L$DEPS/lib" \
./configure --host=x86_64-linux-musl --build=x86_64-linux-gnu --prefix="$DEPS" --disable-static --enable-shared --disable-doc --disable-tests --disable-tools \
    --disable-cxx --disable-guile --disable-nls --disable-libdane --disable-openssl-compatibility \
    --disable-hardware-acceleration --disable-ktls --disable-rpath --disable-full-test-suite --disable-gtk-doc \
    --disable-non-suiteb-curves --without-p11-kit --without-tpm --without-tpm2 --without-idn --without-zlib \
    --without-brotli --without-zstd --with-included-libtasn1 --with-included-unistring --with-nettle-mini \
    --disable-year2038 >configure.log 2>&1 || { tail -30 configure.log; die "gnutls configure failed"; }
make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { grep -n 'error' make.log | head; tail -20 make.log; die "gnutls build failed"; }

echo "$NETTLE $GNUTLS" > "$DEPS/.xrl-built"
rm -rf "$SRCD/$NETTLE" "$SRCD/$GNUTLS"
log "gnutls: built into $DEPS ($(du -sh "$DEPS" | cut -f1))"
ls "$DEPS/lib" | grep -E '\.so' | head

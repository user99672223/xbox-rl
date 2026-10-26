#!/bin/sh
# Build the gnutls test (run in WSL): out/tls_test + the gnutls libraries + the CA bundle, as a guest root.
#   XRL_WORK  build area with the musl sysroot and deps/ (wine/scripts/build-gnutls.sh), default ~/xrl-build
set -e
cd "$(dirname "$0")"
work=${XRL_WORK:-$HOME/xrl-build}
rm -rf out; mkdir -p out/bin out/lib out/etc/ssl/certs out/tmp
cp "$work/sysroot/usr/lib/libc.so" out/lib/ld-musl-x86_64.so.1
"$work/sysroot/bin/xrl-musl-gcc" -O1 -fno-stack-protector -fcf-protection=none -mno-tls-direct-seg-refs \
    -I"$work/deps/include" -o out/bin/tls_test tls_test.c -L"$work/deps/lib" -lgnutls -Wl,-rpath,/lib
for l in libgnutls.so.30 libnettle.so.8 libhogweed.so.6; do cp -L "$work/deps/lib/$l" out/lib/$l; done   # real files: the test root is on NTFS
cp /etc/ssl/certs/ca-certificates.crt out/etc/ssl/certs/
printf 'nameserver 1.1.1.1\n' > out/etc/resolv.conf
python3 ../../../tools/xrl-tpfix.py out/lib/ld-musl-x86_64.so.1 out/bin/tls_test out/lib/lib*.so* 2>&1 | tail -2
python3 ../../../tools/xrl-tpfix.py --check --stamp out/lib/ld-musl-x86_64.so.1 out/bin/tls_test out/lib/lib*.so* 2>&1 | tail -3
echo built out

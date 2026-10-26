#!/bin/sh
# Build the stand-in EAC test tree (run in WSL): out/standin/ = a guest root with the patched
# musl as the interpreter, the glibc-compat shim, dlopen_host (musl) and libstandin_eac.so (glibc).
#   XRL_WORK  build area with the patched musl sysroot (default ~/xrl-build)
set -e
cd "$(dirname "$0")"
work=${XRL_WORK:-$HOME/xrl-build}
sys=$work/sysroot
cc=$sys/bin/xrl-musl-gcc
[ -x "$cc" ] || { echo "no $cc: run wine/scripts/build-musl.sh" >&2; exit 1; }
rm -rf out/standin; mkdir -p out/standin/bin out/standin/lib
cp "$sys/usr/lib/libc.so" out/standin/lib/ld-musl-x86_64.so.1
XRL_WORK=$work ../../../wine/scripts/build-glibc-compat.sh out/standin/lib
$cc -O1 -fno-stack-protector -fcf-protection=none -mno-tls-direct-seg-refs -o out/standin/bin/dlopen_host dlopen_host.c
python3 ../../../tools/xrl-tpfix.py out/standin/lib/ld-musl-x86_64.so.1 out/standin/bin/dlopen_host
python3 ../../../tools/xrl-tpfix.py --check --stamp out/standin/lib/ld-musl-x86_64.so.1 out/standin/bin/dlopen_host
# the module: plain gcc against glibc with the distro's default hardening
gcc -O2 -fPIC -shared -o out/standin/lib/libstandin_eac.so standin_eac.c -ldl -lpthread
# the Wine-side test: the module under the name the EAC bridge dlopen()s, next to a PE test program
rm -rf out/eactest; mkdir -p out/eactest
cp out/standin/lib/libstandin_eac.so out/eactest/easyanticheat_x64.so
x86_64-w64-mingw32-gcc -O1 -o out/eactest/eac_bridge_test.exe eac_bridge_test.c || echo "no mingw: eac_bridge_test.exe not built"
# a stand-in for the game's own Windows easyanticheat_x64.dll: any valid PE (Wine only looks at its name and the .so next to it)
echo 'int xrl_standin_pe;' > out/eactest/placeholder.c
x86_64-w64-mingw32-gcc -shared -o out/eactest/easyanticheat_x64.dll out/eactest/placeholder.c && rm out/eactest/placeholder.c
readelf -d out/standin/lib/libstandin_eac.so | grep NEEDED
echo built out/standin

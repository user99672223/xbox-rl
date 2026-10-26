#!/bin/sh
# Build the raw-syscall test program (run in WSL): out/rawsys_eac.exe
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-gcc -O1 -Wall -o out/rawsys_eac.exe rawsys_eac.c
# the stand-in module: glibc ABI, no DT_NEEDED, own raw syscall, a glibc-only import
gcc -O1 -fPIC -shared -nostdlib -fno-stack-protector -fcf-protection=none -Wl,-z,noexecstack -Wl,--hash-style=gnu -o out/xrl_mod.so xrl_mod.c -Wl,--no-as-needed /lib/x86_64-linux-gnu/libgcc_s.so.1
echo built out/rawsys_eac.exe out/xrl_mod.so

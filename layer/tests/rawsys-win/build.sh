#!/bin/sh
# Build the raw-syscall test program (run in WSL): out/rawsys_eac.exe
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-gcc -O1 -Wall -o out/rawsys_eac.exe rawsys_eac.c
echo built out/rawsys_eac.exe

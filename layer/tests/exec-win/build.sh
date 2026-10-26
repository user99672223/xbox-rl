#!/bin/sh
# Build the exec-handover test programs (run in WSL): out/parent.exe out/child.exe
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-gcc -O1 -o out/parent.exe parent.c
x86_64-w64-mingw32-gcc -O1 -o out/child.exe child.c
x86_64-w64-mingw32-gcc -O1 -o out/chain.exe chain.c
echo built out/parent.exe out/child.exe out/chain.exe

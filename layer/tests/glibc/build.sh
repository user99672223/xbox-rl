#!/bin/sh
# Build the glibc test program (run in WSL): produces out/glibc_test (static-PIE, stack protector on).
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -O1 -g0 -static-pie -fPIE -fstack-protector-all -pthread -o out/glibc_test glibc_test.c
echo built out/glibc_test

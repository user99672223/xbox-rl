#!/bin/sh
# Build and run the rewriter's host tests (WSL / Linux). Usage: layer/tests/rewrite_test.sh [ELF files for --elf]
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/xrl-rewrite-test
mkdir -p "$out"
gcc -O1 -g -Wall -Wno-unused-function -o "$out/rewrite_test" rewrite_test.c ../src/rewrite_core.c ../src/x86dec.c
gcc -O2 -Wall -o "$out/x86dec_test" x86dec_test.c ../src/x86dec.c
"$out/rewrite_test" --diff
for f in /lib/x86_64-linux-gnu/libc.so.6 /lib64/ld-linux-x86-64.so.2 "$@"; do
  [ -f "$f" ] || continue
  "$out/rewrite_test" --elf "$f"
  objdump -d -w "$f" | "$out/x86dec_test" | tail -1
done

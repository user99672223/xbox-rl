#!/bin/sh
# Build the freestanding test guest (run in WSL): produces out/guest.elf
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -O1 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie \
    -mno-tls-direct-seg-refs \
    -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Wall -Wno-unused-function \
    -o out/guest.elf guest.c
# Thread pointer: %fs -> host TEB slot (the console has no FSGSBASE; see
# XRL_TP_GS_OFFSET in include/xrl_abi.h). Same step the Wine build runs.
python3 ../../../tools/xrl-tpfix.py out/guest.elf
python3 ../../../tools/xrl-tpfix.py --check out/guest.elf
# the execve test: one program in two roles (layer/tests/exec-test.sh)
gcc -O1 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie     -mno-tls-direct-seg-refs -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Wall     -o out/exec_test.elf exec_test.c
python3 ../../../tools/xrl-tpfix.py out/exec_test.elf
# the modes/owners test (layer/tests/meta-test.sh)
gcc -O1 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie     -mno-tls-direct-seg-refs -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Wall     -o out/meta_test.elf meta_test.c
python3 ../../../tools/xrl-tpfix.py out/meta_test.elf
echo built out/guest.elf out/exec_test.elf out/meta_test.elf

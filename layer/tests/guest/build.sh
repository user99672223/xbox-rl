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
# guest processes (layer/tests/procs-test.sh): a static PIE (own load address per process) and a non-PIE copy
gcc -O1 -static-pie -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fPIE -mno-tls-direct-seg-refs     -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fno-jump-tables -Wall -o out/procs_test.elf procs_test.c
gcc -O1 -DNOPIE -static -no-pie -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -mno-tls-direct-seg-refs     -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Wall -o out/procs_nopie.elf procs_test.c
echo built out/guest.elf out/exec_test.elf out/procs_test.elf out/procs_nopie.elf

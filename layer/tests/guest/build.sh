#!/bin/sh
# Build the freestanding test guest (run in WSL): produces out/guest.elf
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -O1 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie \
    -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Wall -Wno-unused-function \
    -o out/guest.elf guest.c
echo built out/guest.elf

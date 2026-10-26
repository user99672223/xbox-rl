#!/bin/sh
# Build the glibc test programs (run in WSL): out/glibc_test (static-PIE, stack protector on) and
# out/cxx_test (static-PIE C++ with exceptions; layer/tests/dbt-test.sh runs both translated).
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -O1 -g0 -static-pie -fPIE -fstack-protector-all -pthread -o out/glibc_test glibc_test.c
echo built out/glibc_test
g++ -O1 -g0 -static-pie -fPIE -pthread -o out/cxx_test cxx_test.cpp
echo built out/cxx_test

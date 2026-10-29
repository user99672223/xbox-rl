#!/bin/sh
# Userland objects between guest processes (Git Bash). Needs layer/tests/guest/build.sh run in WSL, and fork: the proc.c
# branch (on a single-process layer the test says so and passes).
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin" "$R/tmp"
cp tests/guest/out/xproc_test.elf "$R/bin/xproc_test.elf"
o=$(XRL_NO_AUDIO=1 MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-90}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/xproc_test.elf --fake-host 2>&1)
echo "$o" | grep -E "^\[I guest\.out\]" | sed 's/^\[I guest.out\] //'
rm -rf "$R"
echo "$o" | grep -q "xproc: all checks passed"

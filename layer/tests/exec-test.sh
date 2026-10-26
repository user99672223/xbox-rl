#!/bin/sh
# execve in the layer (Git Bash). Needs layer/tests/guest/build.sh run in WSL.
# One process runs two programs one after the other; the child checks what an exec must and must not keep.
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin"
cp tests/guest/out/exec_test.elf "$R/bin/exec_test.elf"
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-60}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/exec_test.elf --fake-host 2>&1
rc=$?
rm -rf "$R"
exit $rc

#!/bin/sh
# Run the test guest under the desktop test host (Git Bash). Usage: tests/run.sh [extra env]
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin"
cp tests/guest/out/guest.elf "$R/bin/guest.elf"
MSYS_NO_PATHCONV=1 ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/guest.elf --fake-host "$@"
rc=$?
rm -rf "$R"
exit $rc

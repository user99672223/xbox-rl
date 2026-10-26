#!/bin/sh
# Guest processes in the layer (Git Bash). Needs layer/tests/guest/build.sh run in WSL.
# One driver program forks, vforks, execs and waits for copies of itself (see tests/guest/procs_test.c).
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin"
cp tests/guest/out/procs_test.elf "$R/bin/procs_test.elf"
cp tests/guest/out/procs_nopie.elf "$R/bin/procs_nopie.elf"
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-120}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/procs_test.elf --fake-host 2>&1
rc=$?
rm -rf "$R"
exit $rc

#!/bin/sh
# Run the glibc test program on the layer (Git Bash). The desktop host is told to behave like the
# console (no FSGSBASE), so glibc's %fs only works through the loader's rewriting.
# Usage: layer/tests/glibc/run.sh [program [args]]   (default: glibc_test)
cd "$(dirname "$0")/../.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin" "$R/tmp"
prog=${1:-glibc_test}; [ $# -gt 0 ] && shift
cp "tests/glibc/out/$prog" "$R/bin/$prog"
XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1 ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" "/bin/$prog" "$@"
rc=$?
rm -rf "$R"
exit $rc

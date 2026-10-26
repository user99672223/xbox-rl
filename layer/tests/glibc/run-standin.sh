#!/bin/sh
# Run the stand-in EAC module through a musl dlopen on the layer (Git Bash); build it first with
# build-standin.sh. FSGSBASE is hidden so the module's %fs works only through the rewriter.
cd "$(dirname "$0")/../.."
src="tests/glibc/out/standin"
[ -d "$src" ] || { echo "run tests/glibc/build-standin.sh in WSL first" >&2; exit 2; }
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/tmp"
cp -r "$src"/. "$R"/
XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1 ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/dlopen_host /lib/libstandin_eac.so standin_run
rc=$?
rm -rf "$R"
exit $rc

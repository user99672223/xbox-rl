#!/bin/sh
# gnutls on the layer: a real TLS handshake with certificate verification and an HTTPS request (Git Bash).
# Needs layer/tests/tls/build.sh run in WSL. Uses the network (modules-cdn.eac-prod.on.epicgames.com by default).
cd "$(dirname "$0")/.."
src=tests/tls/out
[ -d "$src" ] || { echo "run tests/tls/build.sh in WSL first" >&2; exit 2; }
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
cp -r "$src"/. "$R"/
XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-60}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/tls_test "$@" 2>&1 | grep -E "tls_test|FAULT|rip="
rc=$?
rm -rf "$R"
exit $rc

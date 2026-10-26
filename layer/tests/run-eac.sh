#!/bin/sh
# Everything added for EAC / foreign-binary support, in one go (Git Bash). Needs, built in WSL beforehand:
#   layer/tests/guest/build.sh, layer/tests/glibc/build.sh, layer/tests/glibc/build-standin.sh
# and, for the last step only, a staged Wine guest tree (XRL_ROOTFS) plus the Proton EAC runtime (EAC_RUNTIME).
cd "$(dirname "$0")/.."
fail=0
echo "== layer guest suite (incl. network)";   sh tests/run.sh 2>&1 | grep -E "FAIL|TOTAL FAILS" ; sh tests/run.sh >/dev/null 2>&1 || fail=1
echo "== unmodified glibc program";            sh tests/glibc/run.sh 2>&1 | grep -E "FAIL|failure" ; sh tests/glibc/run.sh >/dev/null 2>&1 || fail=1
echo "== glibc module dlopen'ed from musl";    sh tests/glibc/run-standin.sh 2>&1 | grep -E "FAIL|failure" ; sh tests/glibc/run-standin.sh >/dev/null 2>&1 || fail=1
if [ -n "$XRL_ROOTFS" ] && [ -d "${EAC_RUNTIME:-$HOME/xrl-work/eac-runtime}" ]; then
  echo "== EAC bridge chain on Wine";          sh tests/glibc/run-eac-bridge.sh 2>&1 | grep -E "FAIL|OK|exit"
else echo "== EAC bridge chain: skipped (set XRL_ROOTFS and EAC_RUNTIME)"; fi
echo "(host tests for the rewriter: wsl -- sh layer/tests/rewrite_test.sh)"
exit $fail

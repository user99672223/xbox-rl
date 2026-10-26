#!/bin/sh
# Byte-identical execution (layer/src/dbt.c), Git Bash. Needs, built in WSL: layer/tests/glibc/build.sh,
# build-standin.sh, and (for the Wine step) a staged guest tree in XRL_ROOTFS and the Proton EAC runtime.
#   1. the stand-in EAC module measures its own mapped code against its file:
#        XRL_DBT unset: the rewriter's patches are seen (> 0 bytes differ)   [control]
#        XRL_DBT=standin: 0 bytes differ, and its syscall / %fs / thread checks still pass
#   2. an unmodified static glibc program and a C++ program with exceptions run translated: 0 failures
#   3. the whole Proton EAC bridge chain on Wine, the real Valve bridge .so and the stand-in module both translated
cd "$(dirname "$0")/.."
fail=0
standin() { sh tests/glibc/run-standin.sh 2>&1; }
echo "== stand-in module, rewritten (control)"
n=$(standin | sed -n 's/.*differs from the file in \([0-9]*\) byte.*/\1/p')
echo "   bytes changed in its own code: $n"
[ -n "$n" ] && [ "$n" -gt 0 ] || { echo "   FAIL: expected the rewriter's patches to be visible"; fail=1; }
echo "== stand-in module, translated (XRL_DBT=standin)"
out=$(XRL_DBT=standin standin)
n=$(echo "$out" | sed -n 's/.*differs from the file in \([0-9]*\) byte.*/\1/p')
echo "   bytes changed in its own code: $n"
echo "$out" | grep -q "dbt: .*translated, not executed" || { echo "   FAIL: not translated"; fail=1; }
echo "$out" | grep -q "standin_eac: 0 failure" || { echo "   FAIL: module checks"; fail=1; }
[ "$n" = 0 ] || { echo "   FAIL: code changed"; fail=1; }
for p in glibc_test cxx_test; do
  echo "== $p, translated"
  out=$(XRL_DBT=1 sh tests/glibc/run.sh $p 2>&1)
  echo "$out" | grep -E "failure\(s\)"
  echo "$out" | grep -q "dbt: .*translated" || { echo "   FAIL: not translated"; fail=1; }
  echo "$out" | grep -q ": 0 failure(s)" || { echo "   FAIL"; fail=1; }
done
if [ -n "$XRL_ROOTFS" ] && [ -d "${EAC_RUNTIME:-$HOME/xrl-work/eac-runtime}" ]; then
  echo "== EAC bridge chain on Wine, translated"
  out=$(XRL_DBT=easyanticheat_x64.so sh tests/glibc/run-eac-bridge.sh 2>&1)
  echo "$out" | grep -E "dbt:|differs|eac_bridge_test" | cut -c1-160
  echo "$out" | grep -q "eac_bridge_test: OK" || { echo "   FAIL"; fail=1; }
  echo "$out" | grep -q "differs from the file in 0 byte" || { echo "   FAIL: code changed"; fail=1; }
else echo "== EAC bridge chain: skipped (set XRL_ROOTFS and EAC_RUNTIME)"; fi
[ $fail = 0 ] && echo "dbt-test: OK"
exit $fail

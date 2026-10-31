#!/bin/bash
# ltp-run.sh [regex] - runs INSIDE the layer (guest): every entry of LTP's runtest/syscalls, one after another in this one
# guest session, each under `timeout`. One line per test in /tmp/ltp-results.txt (exit code and LTP's TPASS / TFAIL /
# TBROK / TCONF counts); the tail of every failing test's output in /tmp/ltp-fails.txt. A regex limits the run.
# LTP exit codes: 0 pass, 32 TCONF only (not applicable here), 124 our timeout, others failed or broken.
cd /opt/ltp/testcases/bin || exit 1
export LTPROOT=/opt/ltp TMPDIR=/tmp PATH=/opt/ltp/testcases/bin:$PATH LTP_TIMEOUT_MUL=2
[ -f /opt/ltp/kconfig ] && export KCONFIG_PATH=/opt/ltp/kconfig   # tst_kconfig: "Cannot parse kernel .config" without it
res=/tmp/ltp-results.txt fails=/tmp/ltp-fails.txt tconf=/tmp/ltp-tconf.txt   # tconf: each TCONF-only test's first reason
: > "$res"; : > "$fails"; : > "$tconf"
filter=${1:-}
while read -r name cmd; do
    [ -z "$name" ] || [ "${name:0:1}" = "#" ] && continue
    [ -n "$filter" ] && ! [[ $name =~ $filter ]] && continue
    bin=${cmd%% *}
    if [ ! -x "$bin" ]; then echo "$name rc=- MISSING" >> "$res"; continue; fi
    t0=$SECONDS
    out=$(timeout -k 5 60 bash -c "$cmd" 2>&1 < /dev/null)
    rc=$?
    printf '%s rc=%d pass=%d fail=%d brok=%d conf=%d %ds\n' "$name" "$rc" "$(grep -c TPASS <<< "$out")" \
        "$(grep -c TFAIL <<< "$out")" "$(grep -c TBROK <<< "$out")" "$(grep -c TCONF <<< "$out")" $((SECONDS - t0)) >> "$res"
    [ "$rc" -eq 32 ] && echo "$name $(grep -m1 TCONF <<< "$out" | sed 's/^[^:]*:[0-9]*: //' | cut -c1-160)" >> "$tconf"
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 32 ]; then
        { echo "=== $name (rc=$rc): $cmd"; tail -n 12 <<< "$out"; } >> "$fails"
    fi
done < /opt/ltp/runtest/syscalls
echo "ltp-run: done, $(wc -l < "$res") entries"

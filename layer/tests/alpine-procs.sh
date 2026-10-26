#!/bin/sh
# Alpine's busybox sh on the layer's guest processes (Git Bash). Needs the Alpine minirootfs the userland work makes
# (XRL_ALPINE_ROOT, default C:\Users\10harbad\xrl-work\alpine\rootfs); prints SKIP and exits 0 without it.
# Each case runs `busybox sh -c <script>` and compares the guest's stdout lines and the exit code.
cd "$(dirname "$0")/.."
ROOT="${XRL_ALPINE_ROOT:-/c/Users/10harbad/xrl-work/alpine/rootfs}"
[ -x "$ROOT/bin/busybox" ] || [ -f "$ROOT/bin/busybox" ] || { echo "SKIP: no Alpine rootfs at $ROOT"; exit 0; }
HOST=./build/desktop/xrl_test_host.exe
fails=0

# case NAME EXPECTED_RC EXPECTED_STDOUT SCRIPT
case_() {
    name=$1; erc=$2; want=$3; script=$4
    got=$(MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-60}" "$HOST" "$(cygpath -w "$ROOT")" /bin/busybox sh -c "$script" 2>&1)
    rc=$?
    out=$(printf '%s\n' "$got" | sed -n 's/^\[I guest.out\] //p')
    if [ "$rc" = "$erc" ] && [ "$out" = "$want" ]; then echo "  ok   $name"
    else
        echo "  FAIL $name (rc=$rc, want $erc)"; echo "--- got"; printf '%s\n' "$out"; echo "--- want"; printf '%s\n' "$want"; fails=$((fails+1))
        [ -n "$XRL_KEEP_LOG" ] && printf '%s\n' "$got" > "$XRL_KEEP_LOG"   # the whole log of a failing case
    fi
}

case_ "pipe, count, background subshell, wait, exit 3" 3 "b
N
done" 'echo a | tr a b; ls /bin | wc -l | sed "s/[0-9][0-9]*/N/"; (sleep 1; echo done) & wait; exit 3'

case_ "3-command pipeline, command substitution, background job" 0 "sub=HELLO
2
before
bg-done
after wait rc=0
y=1
3
rc=7" 'x=$(echo hello | tr a-z A-Z); echo "sub=$x"
echo one two three | tr " " "\n" | sort | head -2 | wc -l
(sleep 1; echo bg-done) &
echo before
wait
echo "after wait rc=$?"
y=1; (y=2); echo "y=$y"
i=0; for f in a b c; do i=$((i+1)); done; echo $i
sh -c "exit 7"; echo "rc=$?"'

case_ "kill, wait status, trap, subshell status, jobs" 3 "bg started
killed rc=143
got-usr1
subshell rc=5
3
job 1
job 2
job 3
all-jobs-done
read:hello
n rc=9" 'sleep 30 &
bg=$!
echo "bg started"
kill $bg; wait $bg; echo "killed rc=$?"
trap "echo got-usr1" USR1; kill -USR1 $$
( exit 5 ); echo "subshell rc=$?"
out=$(echo a; echo b; echo c); echo "$out" | wc -l
for i in 1 2 3; do (echo "job $i"; sleep 0.2) & sleep 0.05; done
wait
echo all-jobs-done
echo hello | (read x; echo "read:$x")
sh -c "sh -c \"exit 9\"; echo \"n rc=\$?\""
exit 3'

case_ "orphaned background job still runs; \$! and /proc/<pid>" 0 "ok
busybox
S" '(sleep 1; echo late) >/dev/null 2>&1 &
p=$!
echo ok
cat /proc/$p/comm 2>/dev/null | head -1
grep "^State:" /proc/$p/status | cut -f2 | cut -c1
kill $p; wait $p 2>/dev/null; true'

case_ "#! scripts are run by execve itself (env, a nested interpreter)" 0 "shebang /tmp/xrl-sb.sh a b
shebang2 /tmp/xrl-sb2.sh c
cleaned" 'printf "#!/bin/sh
echo shebang \$0 \$1 \$2
" > /tmp/xrl-sb.sh
printf "#!/tmp/xrl-sb.sh
" > /tmp/xrl-sb-nested.sh
printf "#! /bin/sh -e
echo shebang2 \$0 \$1
" > /tmp/xrl-sb2.sh
chmod +x /tmp/xrl-sb.sh /tmp/xrl-sb2.sh /tmp/xrl-sb-nested.sh
env /tmp/xrl-sb.sh a b
env /tmp/xrl-sb2.sh c
rm -f /tmp/xrl-sb.sh /tmp/xrl-sb2.sh /tmp/xrl-sb-nested.sh
echo cleaned'

echo "TOTAL FAILS=$fails"
exit $fails

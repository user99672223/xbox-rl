#!/bin/bash
# desktop-run.sh <rootfs dir> <out dir> [command...] - run Linux programs from the CI rootfs on the layer (Windows,
# Git Bash, the desktop test host). With a command: just that one. Without: the probe list below (the current
# milestone's checks, one per line). Each probe gets its own log in <out>/; a summary line per probe goes to stdout,
# and <out>/enosys.txt counts every syscall the layer answered ENOSYS, across all probes (the gap list).
# A probe passes when it exits 0 and its output matches its pattern (grep -E); the last lines are the milestones' verdicts.
# Owned by the desktop work (docs/desktop-brief.md): change the probes as milestones advance.
root=$(cygpath -w "$1"); rootu=$1; out=$2; shift 2
host=layer/build/desktop/xrl_test_host.exe
# XRL_TEST_BUDGET_MB: the guest's commit budget, as the Xbox app gives it (host/src/xrl_host.h guest_mem_budget)
# XRL_PROFILE=1: the perf lines name each busy thread's most frequent syscalls (busy loops, stalls)
# XRL_FORK_SNAPSHOT_MB: a fork of a process with more writable memory than this falls back to vfork semantics (the
# child's writes stay in the parent); Firefox's parent has more than the 256 MiB default
export XRL_UID=0 XRL_GID=0 XRL_NO_AUDIO=1 XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1 XRL_TEST_BUDGET_MB=4096 XRL_PROFILE=1 XRL_FORK_SNAPSHOT_MB=2048
# One probe per line: milestone|timeout (s)|the pattern its output must match (no '|' in it)|the command line, which
# may start with VAR=value words for the test host (layer switches such as XRL_TRACE). A milestone
# passes when all its probes do; earlier milestones stay in the list as regression checks.
names=([1]="bash, coreutils, ps, dpkg -l" [2]="apt-get update + install" [3]="Xvfb + xterm" [4]="XFCE session" [5]="Firefox opens a local page" [6]="TV: the session on /dev/fb0")
list=(
    "1|120|guest.out\] hello from bash|/bin/bash -c 'echo hello from bash; uname -a'"
    "1|120|guest.out\] d.* usr$|/usr/bin/ls -la /"
    "1|120|guest.out\] uid=0\(root\) gid=0\(root\)|/usr/bin/id"
    "1|120|guest.out\] 42$|/bin/bash -c 'echo \$(( 6 * 7 )) | cat; ls /usr/bin | wc -l'"
    "1|120|guest.out\] sub 3$|/bin/bash -c 'x=\$(echo sub); (exit 3); echo \$x \$?; for i in 1 2 3; do /usr/bin/true; done; echo loop ok'"
    "1|120|guest.out\] USER +PID|/usr/bin/ps aux"
    "1|120|guest.out\] ii +bash |/usr/bin/dpkg -l"
    "1|120|guest.out\] apt [0-9]|/usr/bin/apt-get --version"
    "2|600|guest.out\] Reading package lists|XRL_TEST_BUDGET_MB=1024 XRL_TRACE=2 XRL_TRACE_FROM=all XRL_TRACE_COMM=store /usr/bin/apt-get update"
    "2|600|guest.out\] Setting up hello|XRL_TEST_BUDGET_MB=1024 /usr/bin/apt-get install -y --no-install-recommends hello"
    "2|60|guest.out\] Hello, world!|/usr/bin/hello"
    "3|240|\\(\"xterm\" \"XTerm\"\\)|/bin/bash /tmp/xrl-xsession.sh xterm"
    "4|600|found /\"\\(Desktop|/bin/bash /tmp/xrl-xsession.sh xfce"
    "5|900|\"xbox-rl page .*Mozilla Firefox\"|/bin/bash /tmp/xrl-xsession.sh firefox"
    "6|400|dumped the framebuffer|/bin/bash /tmp/xrl-xsession.sh tv"
)
# X sessions (milestones 3-5): tools/ci/xsession.sh in the guest; each saves /tmp/xrl-<mode>.xwd, which becomes
# <out>/<mode>.png; a session probe passes only with its screenshot
cp tools/ci/xsession.sh "$rootu/tmp/xrl-xsession.sh"
# the X -> /dev/fb0 bridge (layer/tests/guest/xbridge.c), executable in the rootfs's mode table
if [ -f layer/tests/guest/out/xbridge.elf ]; then
    mkdir -p "$rootu/usr/local/bin" && cp layer/tests/guest/out/xbridge.elf "$rootu/usr/local/bin/xbridge"
    grep -q "^/usr/local/bin/xbridge	" "$rootu/.xrl-meta" 2>/dev/null || printf '/usr/local/bin/xbridge\t755\t0\t0\n' >> "$rootu/.xrl-meta"
fi
[ $# -gt 0 ] && { list=("0|1500|.|$*"); names=([0]="command"); }
# the guest's environment (layer/tests/test_host.c build_envp): a root login's
export XRL_ENV_XRL_SESSION_KILLALL=1 XRL_ENV_HOME=/root XRL_ENV_PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
       XRL_ENV_DEBIAN_FRONTEND=noninteractive XRL_ENV_LANG=C.UTF-8 XRL_ENV_TERM=dumb
# the layer's own process tests (fork groups, exec, wait: proc.c), which the guest suite does not run
if [ -f layer/tests/guest/out/procs_test.elf ]; then
    XRL_TIMEOUT=300 sh layer/tests/procs-test.sh > "$out/procs-test.txt" 2>&1
    echo "procs-test: exit $? ($(grep -ao 'TOTAL FAILS=[0-9]*' "$out/procs-test.txt" | tail -1))"
fi
n=0
declare -A fails seen
for e in "${list[@]}"; do
    n=$((n + 1))
    m=${e%%|*}; e=${e#*|}; t=${e%%|*}; e=${e#*|}
    pat=${e%%|*}; p=${e#*|}
    seen[$m]=1
    log="$out/probe-$n.txt"
    echo "\$ $p" > "$log"
    shot=; [[ $p == *xrl-xsession.sh* ]] && { shot=${p##* }; rm -f "$rootu/tmp/xrl-$shot.xwd"; }
    s0=$SECONDS
    envs=; while [[ $p =~ ^([A-Z_]+=[^ ]*)\ (.*)$ ]]; do envs="$envs ${BASH_REMATCH[1]}"; p=${BASH_REMATCH[2]}; done   # VAR=v ...: for the host
    eval "$envs timeout $t $host \"$root\" $p" >> "$log" 2>&1
    rc=$?
    echo "exit $rc" >> "$log"
    v=PASS
    [ $rc -eq 0 ] && tr -d '\r' < "$log" | grep -qE "$pat" || v=FAIL
    # what the guest printed, and the layer's errors and warnings: small enough never to hit the publisher's 2 MB cap
    grep -aE "^\[(I guest|W guest|E )|UNHANDLED|not restored|mem-fail\[|ENOSYS|exit [0-9]+$" "$log" | grep -av "MAP " > "$out/probe-$n-short.txt"
    if [ -n "$shot" ]; then
        for f in "$rootu"/tmp/xrl-*.log; do [ -f "$f" ] && cp "$f" "$out/$shot-$(basename "$f")"; done
        if [ -s "$rootu/tmp/xrl-$shot.xwd" ]; then
            cp "$rootu/tmp/xrl-$shot.xwd" "$out/$shot.xwd"
            if [ "$shot" = tv ]; then   # what the bridge put on /dev/fb0
                if [ -s "$rootu/tmp/xrl-fb0.raw" ]; then python -I tools/raw2png.py "$rootu/tmp/xrl-fb0.raw" 1280 720 "$out/tv-fb0.png" >> "$log" 2>&1 || v=FAIL
                else echo "no fb0 dump" >> "$log"; v=FAIL; fi
            fi
            python -I tools/xwd2png.py "$out/$shot.xwd" "$out/$shot.png" >> "$log" 2>&1 || v=FAIL
            rm -f "$out/$shot.xwd"
        else
            echo "no screenshot" >> "$log"; v=FAIL
        fi
    fi
    [ $v = FAIL ] && fails[$m]=$((${fails[$m]:-0} + 1))
    hw=$(grep -aoE "committed [0-9]+ MiB" "$log" | awk '{print $2}' | sort -n | tail -1)   # peak committed
    printf 'probe %d (m%s): %s exit %3d %4ds %5s MiB  %s\n' "$n" "$m" "$v" "$rc" $((SECONDS - s0)) "${hw:--}" "$p"
done
grep -ho "ENOSYS [0-9]* [a-z0-9_]*" "$out"/probe-*.txt 2>/dev/null | sort | uniq -c | sort -rn > "$out/enosys.txt"
echo "--- syscalls answered ENOSYS (count, nr, name):"
cat "$out/enosys.txt"
for m in $(printf '%s\n' "${!seen[@]}" | sort -n); do
    f=${fails[$m]:-0}
    [ "$f" -eq 0 ] && echo "milestone $m (${names[$m]}): PASS" || echo "milestone $m (${names[$m]}): FAIL ($f probe(s))"
done

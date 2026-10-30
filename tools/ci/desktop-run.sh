#!/bin/bash
# desktop-run.sh <rootfs dir> <out dir> [command...] - run Linux programs from the CI rootfs on the layer (Windows,
# Git Bash, the desktop test host). With a command: just that one. Without: the probe list below (the current
# milestone's checks, one per line). Each probe gets its own log in <out>/; a summary line per probe goes to stdout,
# and <out>/enosys.txt counts every syscall the layer answered ENOSYS, across all probes (the gap list).
# A probe passes when it exits 0 and its output matches its pattern (grep -E); the last line is the milestone's verdict.
# Owned by the desktop work (docs/desktop-brief.md): change the probes as milestones advance.
root=$(cygpath -w "$1"); out=$2; shift 2
host=layer/build/desktop/xrl_test_host.exe
export XRL_UID=0 XRL_GID=0 XRL_NO_AUDIO=1 XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1
milestone="milestone 1 (bash, coreutils, ps, dpkg -l)"
probes=(
    "/bin/bash -c 'echo hello from bash; uname -a'"
    "/usr/bin/ls -la /"
    "/usr/bin/id"
    "/bin/bash -c 'echo \$(( 6 * 7 )) | cat; ls /usr/bin | wc -l'"
    "/bin/bash -c 'x=\$(echo sub); (exit 3); echo \$x \$?; for i in 1 2 3; do /usr/bin/true; done; echo loop ok'"
    "/usr/bin/ps aux"
    "/usr/bin/dpkg -l"
    "/usr/bin/apt-get --version"
)
expect=(
    "guest.out\] hello from bash"
    "guest.out\] .* usr -> |guest.out\] d.* usr$"
    "guest.out\] uid=0\(root\) gid=0\(root\)"
    "guest.out\] 42$"
    "guest.out\] sub 3$"
    "guest.out\] USER +PID"
    "guest.out\] ii +bash "
    "guest.out\] apt [0-9]"
)
[ $# -gt 0 ] && { probes=("$*"); expect=("."); milestone="command"; }
n=0 fails=0
for p in "${probes[@]}"; do
    n=$((n + 1))
    log="$out/probe-$n.txt"
    echo "\$ $p" > "$log"
    eval "timeout 120 $host \"$root\" $p" >> "$log" 2>&1
    rc=$?
    echo "exit $rc" >> "$log"
    v=PASS
    [ $rc -eq 0 ] && tr -d '\r' < "$log" | grep -qE "${expect[$((n - 1))]}" || { v=FAIL; fails=$((fails + 1)); }
    printf 'probe %d: %s exit %3d  %s\n' "$n" "$v" "$rc" "$p"
done
grep -ho "ENOSYS [0-9]* [a-z0-9_]*" "$out"/probe-*.txt 2>/dev/null | sort | uniq -c | sort -rn > "$out/enosys.txt"
echo "--- syscalls answered ENOSYS (count, nr, name):"
cat "$out/enosys.txt"
[ $fails -eq 0 ] && echo "$milestone: PASS" || echo "$milestone: FAIL ($fails of $n probes)"

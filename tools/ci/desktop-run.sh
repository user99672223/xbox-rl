#!/bin/bash
# desktop-run.sh <rootfs dir> <out dir> [command...] - run Linux programs from the CI rootfs on the layer (Windows,
# Git Bash, the desktop test host). With a command: just that one. Without: the probe list below (the current
# milestone's checks, one per line). Each probe gets its own log in <out>/; a summary line per probe goes to stdout,
# and <out>/enosys.txt counts every syscall the layer answered ENOSYS, across all probes (the gap list).
# Owned by the desktop work (docs/desktop-brief.md): change the probes as milestones advance.
root=$(cygpath -w "$1"); out=$2; shift 2
host=layer/build/desktop/xrl_test_host.exe
export XRL_UID=0 XRL_GID=0 XRL_NO_AUDIO=1 XRL_NO_FSGSBASE=1 MSYS_NO_PATHCONV=1
probes=(
    "/bin/bash -c 'echo hello from bash; uname -a'"
    "/usr/bin/ls -la /"
    "/usr/bin/id"
    "/bin/bash -c 'echo \$(( 6 * 7 )) | cat; ls /usr/bin | wc -l'"
    "/usr/bin/ps aux"
    "/usr/bin/dpkg -l"
    "/usr/bin/apt-get --version"
)
[ $# -gt 0 ] && probes=("$*")
n=0
for p in "${probes[@]}"; do
    n=$((n + 1))
    log="$out/probe-$n.txt"
    echo "\$ $p" > "$log"
    eval "timeout 120 $host \"$root\" $p" >> "$log" 2>&1
    rc=$?
    echo "exit $rc" >> "$log"
    printf 'probe %d: exit %3d  %s\n' "$n" "$rc" "$p"
done
grep -ho "ENOSYS [0-9]* [a-z0-9_]*" "$out"/probe-*.txt 2>/dev/null | sort | uniq -c | sort -rn > "$out/enosys.txt"
echo "--- syscalls answered ENOSYS (count, nr, name):"
cat "$out/enosys.txt"

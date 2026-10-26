#!/bin/sh
# The guest suite with fork()/ptrace()/exec-spawn tasks switched on (XRL_FORK_TASKS=1; layer/src/task.c), Git Bash.
# Exec in a task starts a second layer process, so this script is careful: a hard timeout, at most 2 live
# children (XRL_MAX_CHILDREN), the whole process group killed when the first process ends
# (XRL_SPAWN_KILL_ON_EXIT), and afterwards a census: any xrl_test_host.exe of THIS worktree (tests/kill-mine.ps1: matched by its own path, never by
# "not there before") is killed and reported as a failure. Needs 3 GB free on C: (skipped otherwise).
cd "$(dirname "$0")/.."
free=$(powershell -NoProfile -Command "[int]((Get-PSDrive C).Free/1GB)")
[ "${free:-0}" -ge 3 ] || { echo "fork-tasks: only ${free} GB free on C:, skipped"; exit 0; }
export XRL_FORK_TASKS=1 XRL_MAX_CHILDREN=2 XRL_SPAWN_KILL_ON_EXIT=1 XRL_NO_FSGSBASE=1 XRL_TEST_BUDGET_MB="${XRL_TEST_BUDGET_MB:-1024}"
out=$(timeout 300 sh tests/run.sh --no-swap 2>&1); rc=$?
echo "$out" | grep -E "FAIL|TOTAL|skip"
left=$(powershell -NoProfile -File "$(cygpath -w tests/kill-mine.ps1)" | tr -d '\r' | tr '\n' ' ')
if [ -n "$left" ]; then echo "fork-tasks: FAIL: leftover xrl_test_host.exe processes killed: $left"; rc=1; fi
echo "$out" | grep -q "TOTAL FAILS=0" || rc=1
echo "fork-tasks: rc=$rc"
exit $rc

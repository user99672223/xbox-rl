#!/bin/bash
# run-rl.sh [extra wine args...]   (Git Bash on the laptop)
#
# Rocket League on the layer with a fresh Epic login: takes the shared run
# lock (one Rocket League at a time on this laptop), captures the launcher's
# arguments (tools/epic-capture.ps1; the login code stays in a file outside the
# repo and is never printed), then runs RocketLeague.exe through
# tools/run-desktop-wine.sh with them. Environment as for run-desktop-wine.sh
# (XRL_TEST_BUDGET_MB, XRL_TIMEOUT, XRL_REAL_GFX, WINEDEBUG, ...);
# XRL_RL_EXE picks the exe (default RocketLeague.exe; RocketLeague_EAC.exe for EAC).
cd "$(dirname "$0")/.." || exit 2
lock="$HOME/xrl-work/rl-run.lock"
if [ -f "$lock" ] && [ $(( $(date +%s) - $(stat -c %Y "$lock") )) -lt 1200 ]; then
    echo "[run-rl] another Rocket League run holds $lock: $(cat "$lock")" >&2; exit 3
fi
echo "run-rl.sh (main checkout) $(date -Iseconds)" > "$lock"
trap 'rm -f "$lock"' EXIT INT TERM
args="$HOME/xrl-work/epic-launch/launch-args.json"
pwsh -NoProfile -File tools/epic-capture.ps1 -Out "$(cygpath -w "$args")" || exit 1
exe="${XRL_RL_EXE:-RocketLeague.exe}"
# the arguments as a NUL-separated list, read back without echoing them
tmp="$(mktemp)"
python3 -I -c 'import json,sys; sys.stdout.write("\0".join(json.load(open(sys.argv[1], encoding="utf-8-sig"))["args"]))' \
    "$(cygpath -w "$args")" > "$tmp" || exit 1
set --  "Z:\\game\\Binaries\\Win64\\$exe" "$@"
while IFS= read -r -d '' a; do set -- "$@" "$a"; done < <(cat "$tmp"; printf '\0')
rm -f "$tmp"
echo "[run-rl] $exe with $(( $# - 1 )) arguments"
tools/run-desktop-wine.sh "$@"

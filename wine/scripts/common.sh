#!/usr/bin/env bash
# wine/scripts/common.sh -- sourced by the other scripts.  Runs inside WSL.
#
# Layout it defines:
#   $ROOT      repo root (on the Windows drive, /mnt/c/...)           [read mostly]
#   $VENDOR    $ROOT/vendor      bare, shallow, pinned git repos: wine.git musl.git
#   $PATCHES   $ROOT/patches     patches/<component>/NNNN-*.patch
#   $WORK      WSL-native build area (default ~/xrl-build).  Never on /mnt/c:
#              building Wine over 9p is 10x slower and breaks exec bits.
#   $WORK/src/<component>        exported pristine tree + patches applied
#   $WORK/sysroot                patched musl install (headers, libc.so, crt)
#   $WORK/build/wine             Wine out-of-tree build dir
#   $WORK/out                    final artifacts (rootfs/ tree to ship)
set -euo pipefail

_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WINE_DIR="$(cd "$_here/.." && pwd)"
ROOT="$(cd "$WINE_DIR/.." && pwd)"
VENDOR="${XRL_VENDOR:-$ROOT/vendor}"   # XRL_VENDOR: a worktree can use the main checkout's vendor/
PATCHES="$ROOT/patches"
WORK="${XRL_WORK:-$HOME/xrl-build}"
SYSROOT="$WORK/sysroot"
JOBS="${XRL_JOBS:-$(nproc)}"

# shellcheck disable=SC1091
. "$WINE_DIR/pins.env"

log()  { printf '\033[1;34m[xrl-wine]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[xrl-wine] WARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[xrl-wine] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

need() { for t in "$@"; do command -v "$t" >/dev/null 2>&1 || die "missing tool '$t' (see wine/notes-B.md 'WSL prerequisites')"; done; }

# Memory guard: this WSL VM is small.  Each cc1 on Wine's big files can hit
# ~600 MB, so never run more than one job per 700 MB of RAM.
cap_jobs() {
  local mem_mb; mem_mb=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
  local cap=$(( mem_mb / 700 )); [ "$cap" -lt 1 ] && cap=1
  [ "$JOBS" -gt "$cap" ] && JOBS=$cap
  return 0
}

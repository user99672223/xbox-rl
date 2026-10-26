#!/usr/bin/env bash
# verify-patches.sh [wine|musl ...]   (default: both)
#
# Clean-clone check: export each pinned tree into a throwaway directory and apply
# patches/<name>/*.patch in order, exactly as apply-patches.sh does, without
# touching $WORK/src (whose file times the incremental build depends on).
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need git tar
names=("$@"); [ ${#names[@]} = 0 ] && names=(wine musl)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
shopt -s nullglob
for name in "${names[@]}"; do
  case "$name" in wine) pin=$WINE_COMMIT ;; musl) pin=$MUSL_COMMIT ;; *) die "unknown component '$name'" ;; esac
  d="$tmp/$name"; mkdir -p "$d"
  git -C "$VENDOR/$name.git" archive --format=tar "$pin" | tar -x -C "$d"
  git -C "$d" init -q
  git -C "$d" -c user.name=xrl -c user.email=xrl@localhost add -A >/dev/null
  git -C "$d" -c user.name=xrl -c user.email=xrl@localhost commit -q -m pin
  n=0
  for p in "$PATCHES/$name"/*.patch; do
    git -C "$d" apply --index --whitespace=nowarn "$p" || die "$name: $(basename "$p") does not apply on a clean export"
    n=$((n+1))
  done
  log "$name: all $n patches apply cleanly to pin $pin"
  rm -rf "$d"
done

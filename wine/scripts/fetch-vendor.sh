#!/usr/bin/env bash
# Fetch the pinned upstream commits into vendor/ as *bare shallow* repos.
#
# vendor/ is a pin, not a checkout: vendor/wine.git and vendor/musl.git hold
# exactly one commit each (the ones in pins.env), verified by hash.  Nothing
# ever edits them.  apply-patches.sh exports a tree from them with `git archive`.
#
# Idempotent.  Re-running with an unchanged pins.env is a no-op.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need git

fetch_pin() {  # name repo commit
  local name=$1 repo=$2 commit=$3 dir="$VENDOR/$1.git"
  if [ -d "$dir" ] && git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
    log "$name: already pinned at $commit"; return 0
  fi
  log "$name: fetching $commit from $repo"
  rm -rf "$dir"; mkdir -p "$VENDOR"
  git init -q --bare "$dir"
  # GitHub (and musl's cgit) allow fetching a reachable commit by full sha.
  git -C "$dir" fetch -q --depth 1 "$repo" "$commit"
  local got; got=$(git -C "$dir" rev-parse FETCH_HEAD)
  [ "$got" = "$commit" ] || die "$name: fetched $got, pin says $commit"
  git -C "$dir" update-ref refs/pins/current "$commit"
  log "$name: OK $(du -sh "$dir" | cut -f1)"
}

fetch_pin wine "$WINE_REPO" "$WINE_COMMIT"
fetch_pin musl "$MUSL_REPO" "$MUSL_COMMIT"

#!/usr/bin/env bash
# apply-patches.sh <wine|musl> [--force]
#
# Export the pinned tree from vendor/<name>.git into $WORK/src/<name>, make it a
# throwaway git repo (baseline commit = the pin), and apply patches/<name>/*.patch
# in lexical order.  The vendor repo is never touched; the patched tree is
# disposable.  To change a patch: edit in $WORK/src/<name>, `git diff` against the
# baseline commit, write the result back to patches/<name>/ (see
# wine/scripts/regen-patch.sh).
#
# A stamp file (sha256 of pin + every patch's bytes) makes re-runs a no-op when
# nothing changed, and forces a clean re-export when anything did.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need git sha256sum tar

name=${1:?usage: apply-patches.sh <wine|musl> [--force]}; shift || true
force=0; [ "${1:-}" = "--force" ] && force=1
case "$name" in
  wine) pin=$WINE_COMMIT ;;
  musl) pin=$MUSL_COMMIT ;;
  *) die "unknown component '$name'" ;;
esac

repo="$VENDOR/$name.git"
[ -d "$repo" ] || die "$repo missing -- run wine/scripts/fetch-vendor.sh"
src="$WORK/src/$name"
mkdir -p "$WORK/src"

shopt -s nullglob
patches=("$PATCHES/$name"/*.patch)

stamp=$( { echo "$pin"; for p in "${patches[@]}"; do sha256sum "$p"; done; } | sha256sum | cut -d' ' -f1)
if [ $force = 0 ] && [ -f "$src/.xrl-stamp" ] && [ "$(cat "$src/.xrl-stamp")" = "$stamp" ]; then
  log "$name: patched tree up to date (${#patches[@]} patches)"; exit 0
fi

log "$name: exporting $pin"
rm -rf "$src"; mkdir -p "$src"
git -C "$repo" archive --format=tar "$pin" | tar -x -C "$src"
git -C "$src" init -q
git -C "$src" -c user.name=xrl -c user.email=xrl@localhost add -A >/dev/null
git -C "$src" -c user.name=xrl -c user.email=xrl@localhost commit -q -m "pin $pin"
git -C "$src" tag -f xrl-base >/dev/null

for p in "${patches[@]}"; do
  log "$name: applying $(basename "$p")"
  # --index keeps the baseline diffable; -p1 patches are produced by `git diff`.
  git -C "$src" apply --index --whitespace=nowarn "$p" \
    || die "$name: $(basename "$p") does not apply to pin $pin"
done
echo "$stamp" > "$src/.xrl-stamp"
log "$name: ${#patches[@]} patches applied"

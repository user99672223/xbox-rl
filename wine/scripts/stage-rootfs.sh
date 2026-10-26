#!/usr/bin/env bash
# stage-rootfs.sh DEST
#
# Assemble the guest filesystem the layer serves from DEST (its vfs_root):
#   DEST/            <- out/rootfs  (Wine install + /lib/ld-musl-x86_64.so.1)
#   DEST/prefix/     <- out/prefix  (the baked Wine prefix; bake-prefix.sh)
#   DEST/.xrl-symlinks   every symlink of both trees, as the layer's VFS expects:
#                        "GUEST_ABS_PATH<TAB>TARGET" lines (layer/notes-C.md "VFS")
# DEST is a host directory (e.g. /mnt/c/Users/<you>/xrl-work/rootfs for the laptop
# test host, or the tree tools/push-game.ps1 uploads to the console's LocalState).
# NTFS cannot hold symlinks the layer understands, so links are never copied as
# links: they only go into the table. Dangling links (Wine's usr/bin launcher
# shortcuts) are skipped.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need find rsync
dest=${1:?usage: stage-rootfs.sh DEST}
rootfs="$WORK/out/rootfs"; prefix="$WORK/out/prefix"
[ -d "$rootfs" ] || die "no $rootfs -- run build-wine.sh install"
[ -d "$prefix" ] || die "no $prefix -- run bake-prefix.sh"

mkdir -p "$dest"
log "stage: Wine rootfs -> $dest"
rsync -a --delete --no-links --exclude=/prefix --exclude=/.xrl-symlinks --exclude=/game "$rootfs"/ "$dest"/
log "stage: prefix -> $dest/prefix"
rsync -a --delete --no-links "$prefix"/ "$dest/prefix"/

# Wine's unix side reads a stable machine identity; keep one per staged tree.
mkdir -p "$dest/etc"
[ -s "$dest/etc/machine-id" ] || { tr -dc 'a-f0-9' < /dev/urandom | head -c 32; echo; } > "$dest/etc/machine-id"

bad=$(find "$rootfs" "$prefix" -type f -name '*[:*?"<>|\\]*' | head -5)
[ -z "$bad" ] || die "file names NTFS cannot store (need the layer's U+F000 escaping): $bad"

tab=$'\t'; links="$dest/.xrl-symlinks.tmp"; : > "$links"; n=0; skipped=0
emit() {  # tree guest_prefix
  local tree=$1 gp=$2 l rel target
  while IFS= read -r -d '' l; do
    if [ ! -e "$l" ]; then skipped=$((skipped+1)); continue; fi
    rel=${l#"$tree"}; target=$(readlink "$l")
    printf '%s%s%s\n' "$gp$rel" "$tab" "$target" >> "$links"; n=$((n+1))
    # the parent directory must exist on the host even if it holds only links
    mkdir -p "$dest$gp$(dirname "$rel")"
  done < <(find "$tree" -type l -print0)
}
emit "$rootfs" ""
emit "$prefix" "/prefix"
mv "$links" "$dest/.xrl-symlinks"
log "stage: $n symlinks -> .xrl-symlinks ($skipped dangling skipped); $(du -sh "$dest" | cut -f1) total"

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
rsync -a --delete --no-links --exclude=/prefix --exclude=/.xrl-symlinks --exclude=/game --exclude=/etc/resolv.conf --exclude=/etc/hosts "$rootfs"/ "$dest"/
log "stage: prefix -> $dest/prefix"
rsync -a --delete --no-links "$prefix"/ "$dest/prefix"/

# glibc-ABI shim for native modules built against glibc (the EAC runtime); see docs/decisions.md.
# musl is already in $dest/lib/ld-musl-x86_64.so.1, so this only adds libgcompat.so.0 and two stubs.
"$WINE_DIR/scripts/build-glibc-compat.sh" "$dest/lib"

# Wine's unix side reads a stable machine identity; keep one per staged tree.
mkdir -p "$dest/etc"
# Name resolution: musl's resolver reads /etc/resolv.conf and /etc/hosts and talks plain UDP/TCP, which the
# layer's sockets carry. The files are staged once and then yours to edit (never overwritten).
[ -s "$dest/etc/resolv.conf" ] || printf '# staged by wine/scripts/stage-rootfs.sh; edit for your network
nameserver 1.1.1.1
nameserver 8.8.8.8
' > "$dest/etc/resolv.conf"
[ -s "$dest/etc/hosts" ] || printf '127.0.0.1 localhost
::1 localhost ip6-localhost
' > "$dest/etc/hosts"
# (not `tr < /dev/urandom | head`: under pipefail the SIGPIPE to tr aborted a first-time stage)
[ -s "$dest/etc/machine-id" ] || od -An -N16 -tx1 /dev/urandom | tr -d ' \n' > "$dest/etc/machine-id"
# Root CAs: Wine's crypt32 fills the Windows root store from the Linux bundle (dlls/crypt32/unixlib.c), and
# TLS clients verify servers against it (Rocket League's Epic SDK). The build machine's public Mozilla bundle.
ca=/etc/ssl/certs/ca-certificates.crt
[ -s "$ca" ] || die "no CA bundle at $ca (apt install ca-certificates)"
mkdir -p "$dest/etc/ssl/certs"
cp "$ca" "$dest/etc/ssl/certs/ca-certificates.crt"

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

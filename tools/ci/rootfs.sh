#!/bin/bash
# rootfs.sh <out.tar.gz> - the Debian root file system the layer runs in CI (run as root on Linux).
# Debian 13 (trixie), minbase, plus the packages in tools/ci/rootfs-debian.list. Owned by the desktop work
# (docs/desktop-brief.md): extend the list as milestones need; the CI caches the result by this file + the list.
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
command -v debootstrap > /dev/null || { apt-get update -qq; apt-get install -y -qq debootstrap > /dev/null; }
r=$(mktemp -d)/rootfs
script=/usr/share/debootstrap/scripts/trixie
[ -e "$script" ] || script=/usr/share/debootstrap/scripts/sid
# ROOTFS_LIST=<file>: another package list (the LTP run uses tools/ci/rootfs-ltp.list)
list=${ROOTFS_LIST:+$(realpath "$ROOTFS_LIST")}; list=${list:-$here/rootfs-debian.list}
pk=$(grep -v '^#' "$list" | tr -s ' \n' ',' | sed 's/^,//; s/,$//')
debootstrap --variant=minbase ${pk:+--include="$pk"} trixie "$r" http://deb.debian.org/debian "$script"
printf 'nameserver 1.1.1.1\nnameserver 1.0.0.1\n' > "$r/etc/resolv.conf"
# No man pages, docs or translations, now or from later apt installs: they are the bulk of the names that differ only
# in case (pam.7 vs PAM.7), which the case-insensitive host file system cannot hold side by side.
cat > "$r/etc/dpkg/dpkg.cfg.d/01-xrl-nodoc" <<'EOF'
path-exclude /usr/share/man/*
path-exclude /usr/share/info/*
path-exclude /usr/share/doc/*
path-include /usr/share/doc/*/copyright
path-exclude /usr/share/locale/*
path-include /usr/share/locale/locale.alias
EOF
find "$r/usr/share/man" "$r/usr/share/info" "$r/usr/share/locale" -mindepth 1 -maxdepth 1 ! -name locale.alias -exec rm -rf {} +
find "$r/usr/share/doc" -mindepth 1 ! -name copyright ! -type d -delete
rm -rf "$r"/var/cache/apt/archives/*.deb "$r"/var/lib/apt/lists/* "$r"/dev/*
tar -C "$r" --numeric-owner -czf "$out" .
du -h "$out"

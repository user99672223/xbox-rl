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
pk=$(grep -v '^#' "$here/rootfs-debian.list" | tr -s ' \n' ',' | sed 's/^,//; s/,$//')
debootstrap --variant=minbase ${pk:+--include="$pk"} trixie "$r" http://deb.debian.org/debian "$script"
printf 'nameserver 1.1.1.1\nnameserver 1.0.0.1\n' > "$r/etc/resolv.conf"
rm -rf "$r"/var/cache/apt/archives/*.deb "$r"/var/lib/apt/lists/* "$r"/dev/*
tar -C "$r" --numeric-owner -czf "$out" .
du -h "$out"

#!/bin/bash
# merge-rootfs.sh <rootfs.tar.gz> <out.tar.gz> [overlay.tar.gz ...] - the rootfs with overlays unpacked on top, as one
# tarball (what tools/xrl-rootfs-unpack.py reads). Overlays: desktop/build-overlay.sh's (theme, session script),
# desktop/gpu/build-mesa.sh's (/opt/xrl-gpu). Run as root on Linux (owners and modes are kept).
set -euo pipefail
in=$(realpath "$1"); out=$(realpath -m "$2"); shift 2
r=$(mktemp -d)
trap 'rm -rf "$r"' EXIT
tar -C "$r" --numeric-owner -xzf "$in"
for o in "$@"; do
    [ -s "$o" ] || { echo "merge-rootfs: no overlay $o, skipped"; continue; }
    tar -C "$r" --numeric-owner --keep-directory-symlink -xzf "$o"
    echo "merge-rootfs: + $o ($(du -h "$o" | cut -f1))"
done
tar -C "$r" --numeric-owner -czf "$out" .
du -h "$out"

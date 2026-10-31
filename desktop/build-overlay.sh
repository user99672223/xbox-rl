#!/bin/bash
# build-overlay.sh <out.tar.gz> - the xbox-rl desktop's own files for the Debian rootfs, as a tarball that unpacks at
# its root: the Plasma look (desktop/plasma/rootfs: /etc/xdg defaults, the look-and-feel package with the dock
# layout, the wallpaper, the Konsole profile) and the session script /usr/local/bin/xrl-plasma-session. Plain files
# from this repo, owned by root; no build step, so no cache needed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "$1")
tar -C "$here/plasma/rootfs" --owner=0 --group=0 --numeric-owner --mode='u+rwX,go+rX,go-w' --sort=name \
    --mtime='2026-01-01 00:00Z' -czf "$out" .
tar -tzvf "$out" | awk '{print $1, $6}' | grep -v '/$' | sed 's/^/  /'
du -h "$out"

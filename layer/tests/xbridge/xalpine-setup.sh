#!/bin/bash
# WSL, as root. An Alpine userland with Xvfb, twm, xterm in RAM (tmpfs /tmp/xa): nothing on the C: drive. Packages come from
# Alpine's official mirror through apk with the keys in the minirootfs (never --allow-untrusted); the minirootfs tarball is
# checked against its published .sha256 first. Lost when the WSL VM stops (/tmp is private to each wsl.exe session).
#   XRL_ALPINE_TAR=/mnt/c/.../alpine-minirootfs-<ver>-x86_64.tar.gz   (with its .sha256 next to it)
set -e
R=/tmp/xa
TAR=${XRL_ALPINE_TAR:-/mnt/c/Users/10harbad/xrl-work/alpine/dl/alpine-minirootfs-3.24.3-x86_64.tar.gz}
if [ ! -x $R/usr/bin/Xvfb ]; then
  rm -rf $R; mkdir -p $R; cd $R
  (cd "$(dirname "$TAR")" && sha256sum -c "$(basename "$TAR").sha256")
  tar xzf "$TAR"
  cp /etc/resolv.conf $R/etc/resolv.conf
  mount --rbind /dev $R/dev 2>/dev/null || true
  chroot $R /sbin/apk add --no-cache xvfb xdpyinfo xkbcomp xterm twm xsetroot font-misc-misc font-cursor-misc 2>&1 | tail -5
  umount -l $R/dev 2>/dev/null || true
fi
du -sm $R | tail -1
ls -la $R/usr/bin/Xvfb

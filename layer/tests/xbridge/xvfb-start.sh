#!/bin/bash
# WSL, as root: Xvfb from the Alpine userland in RAM (xalpine-setup.sh), twm + an xterm on it, its screen file copied by a loop to
# <outdir>/screen.xwd where the layer's guest reads it (Xvfb cannot mmap its -fbdir file on a Windows drive).
#   xvfb-start.sh <seconds to run> <outdir (a /mnt/c/... path)>
# Listens on TCP 6007 (display :7, no access control: -ac). Everything runs in a private mount namespace.
R=/tmp/xa
SECS=${1:-30}
T=${2:?outdir}
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$T"
[ -x $R/usr/bin/Xvfb ] || bash "$HERE/xalpine-setup.sh" > "$T/setup.log" 2>&1
exec unshare -m bash -c "
  mount --rbind /dev $R/dev; mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts $R/dev/pts 2>/dev/null; mount --bind $R/dev/pts/ptmx $R/dev/ptmx 2>/dev/null
  mkdir -p $R/tmp/xfb $R/tmp/.X11-unix
  chroot $R /usr/bin/Xvfb :7 -screen 0 1280x720x24 -fbdir /tmp/xfb -ac -listen tcp > '$T/xvfb.log' 2>&1 &
  XPID=\$!
  sleep 1.5
  chroot $R /bin/sh -c 'DISPLAY=:7 xdpyinfo | head -4' > '$T/xdpyinfo.log' 2>&1
  chroot $R /bin/sh -c 'DISPLAY=:7 xsetroot -solid steelblue; DISPLAY=:7 twm >/dev/null 2>&1 &  DISPLAY=:7 xterm -geometry 80x20+100+100 -e sh -c \"echo hello from xterm in Xvfb; uname -a; sleep 3000\" >/dev/null 2>&1 &'
  ( while true; do cp $R/tmp/xfb/Xvfb_screen0 '$T/screen.xwd' 2>/dev/null; sleep 0.04; done ) &
  CPID=\$!
  sleep $SECS
  kill \$CPID \$XPID 2>/dev/null
  pkill -f 'xterm -geometry 80x20+100+100' 2>/dev/null; pkill -x twm 2>/dev/null; pkill -f 'Xvfb :7' 2>/dev/null
  true
"

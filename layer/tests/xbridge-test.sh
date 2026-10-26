#!/bin/sh
# An X server's screen on /dev/fb0 and the keyboard/mouse into X, on the laptop (Git Bash).
#   1. Xvfb (Alpine's, from the userland agent's chroot /root/xrl-alpine/root in WSL, run in a private mount namespace so
#      nothing there changes) with twm + an xterm, listening on TCP 6007, its screen file copied to
#      xrl-work/linux-agents/display/x11tmp/screen.xwd by a loop (Xvfb cannot mmap its -fbdir file on a Windows drive);
#   2. the xbridge guest in the layer: reads screen.xwd -> /dev/fb0 -> the test host's window, and turns the events the
#      host injects (XRL_INPUT_SCRIPT: a pointer move into the xterm, then typing "hello world") into XTest requests to
#      the X server over TCP; --verify asks the server where the pointer ended up.
# Needs layer/tests/guest/build.sh run in WSL. Extra arguments go to xbridge.
D=/c/Users/10harbad/xrl-work/linux-agents/display
cd "$(dirname "$0")/.."
rm -f $D/x11tmp/screen.xwd
(wsl -u root -e bash -lc "bash /mnt/c/Users/10harbad/xrl-work/linux-agents/display/xvfb-start.sh ${XB_SECONDS:-40}" > $D/xvfb-run.log 2>&1 &)
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s $D/x11tmp/screen.xwd ] && break; sleep 1; done
sleep 3   # twm and the xterm draw
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin" "$R/tmp"
cp tests/guest/out/xbridge.elf "$R/bin/xbridge.elf"
cp $D/x11tmp/screen.xwd "$R/tmp/screen.xwd"
# the screen file the guest reads is the one the WSL loop keeps rewriting: link it by copying fresh in the background
( while [ -d "$R/tmp" ]; do cp $D/x11tmp/screen.xwd "$R/tmp/screen.xwd" 2>/dev/null; sleep 0.05; done ) &
CP=$!
XRL_REAL_GFX=1 XRL_NO_AUDIO=1 XRL_WINDOW_INPUT="${XRL_WINDOW_INPUT:-0}" XRL_INPUT_AFTER=input-ready \
XRL_INPUT_SCRIPT="0.5:abs:0:7680,0.5:abs:1:11378,1.0:key:35:1,1.05:key:35:0,1.2:key:18:1,1.25:key:18:0,1.4:key:38:1,1.45:key:38:0,1.6:key:38:1,1.65:key:38:0,1.8:key:24:1,1.85:key:24:0,2.0:key:57:1,2.05:key:57:0,2.2:key:17:1,2.25:key:17:0,2.4:key:24:1,2.45:key:24:0,2.6:key:19:1,2.65:key:19:0,2.8:key:38:1,2.85:key:38:0,3.0:key:32:1,3.05:key:32:0" \
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-90}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/xbridge.elf --xwd /tmp/screen.xwd --tcp 127.0.0.1:6007 --seconds "${XB_RUN:-14}" --verify "$@" 2>&1
rc=$?
kill $CP 2>/dev/null
rm -rf "$R"
exit $rc

#!/bin/sh
# An X server's screen on /dev/fb0 and the keyboard/mouse into X, on the laptop (Git Bash). Needs WSL.
#   1. Xvfb (Alpine's, unpacked into RAM by layer/tests/xbridge/xalpine-setup.sh: ~1-2 min on the first start of each
#      WSL session) with twm + an xterm, display :7 on TCP 6007; a loop copies its screen file to $XB_DIR/screen.xwd;
#   2. the xbridge guest in the layer: reads screen.xwd -> /dev/fb0 -> the test host's window, and turns the events the
#      host injects (XRL_INPUT_SCRIPT: a pointer move into the xterm, then typing "hello world") into XTest requests to
#      the X server over TCP; --verify asks the server where the pointer ended up; --dump writes /dev/fb0's pixels to
#      $XB_DIR/fb.raw (raw BGRX 1280x720) so the picture can be checked without a screenshot.
# Needs layer/tests/guest/build.sh run in WSL. XB_DIR (default: a temp dir), XB_RUN (guest seconds, default 14),
# XB_SECONDS (Xvfb lifetime, default 120); extra arguments go to xbridge.
# Expected: "pointer x=300 y=250", "rows written" > 700, and fb.raw shows the xterm with "hello world" typed in it.
cd "$(dirname "$0")/.."
D="${XB_DIR:-$(mktemp -d)}"
mkdir -p "$D"
DW=$(cygpath -m "$D" | sed 's#^\([A-Za-z]\):#/mnt/\L\1#')                     # the same directory as WSL sees it
HW=$(cygpath -m "$PWD/tests/xbridge" | sed 's#^\([A-Za-z]\):#/mnt/\L\1#')
rm -f "$D/screen.xwd"
(MSYS2_ARG_CONV_EXCL='*' wsl -u root -e bash "$HW/xvfb-start.sh" "${XB_SECONDS:-120}" "$DW" > "$D/xvfb-run.log" 2>&1 &)
for i in $(seq 1 360); do [ -s "$D/screen.xwd" ] && break; sleep 1; done
sleep 3   # twm and the xterm draw
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin" "$R/tmp"
cp tests/guest/out/xbridge.elf "$R/bin/xbridge.elf"
cp "$D/screen.xwd" "$R/tmp/screen.xwd"
# the guest reads $R/tmp/screen.xwd; keep refreshing it from the file the WSL loop rewrites
( while [ -d "$R/tmp" ]; do cp "$D/screen.xwd" "$R/tmp/screen.xwd" 2>/dev/null; sleep 0.05; done ) &
CP=$!
XRL_REAL_GFX=1 XRL_NO_AUDIO=1 XRL_WINDOW_INPUT="${XRL_WINDOW_INPUT:-0}" XRL_INPUT_AFTER=input-ready \
XRL_INPUT_SCRIPT="0.5:abs:0:7680,0.5:abs:1:11378,1.0:key:35:1,1.05:key:35:0,1.2:key:18:1,1.25:key:18:0,1.4:key:38:1,1.45:key:38:0,1.6:key:38:1,1.65:key:38:0,1.8:key:24:1,1.85:key:24:0,2.0:key:57:1,2.05:key:57:0,2.2:key:17:1,2.25:key:17:0,2.4:key:24:1,2.45:key:24:0,2.6:key:19:1,2.65:key:19:0,2.8:key:38:1,2.85:key:38:0,3.0:key:32:1,3.05:key:32:0" \
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-90}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/xbridge.elf --xwd /tmp/screen.xwd --tcp 127.0.0.1:6007 --seconds "${XB_RUN:-14}" --verify --dump /tmp/fb.raw "$@" 2>&1
rc=$?
kill $CP 2>/dev/null
[ -f "$R/tmp/fb.raw" ] && cp "$R/tmp/fb.raw" "$D/fb.raw"
rm -rf "$R"
echo "[xbridge-test] screen/dump files in $D"
exit $rc

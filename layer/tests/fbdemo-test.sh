#!/bin/sh
# /dev/fb0 + /dev/input/event0..2 in the layer, on the laptop (Git Bash). Needs layer/tests/guest/build.sh run in WSL.
# Opens the desktop test host's window (XRL_REAL_GFX=1), runs the fbdemo guest in its self-test mode and injects the
# keyboard/mouse events it expects through the host's input queue (XRL_INPUT_SCRIPT, 'N seconds after the guest printed
# input-ready'; the window's own mouse/keyboard are ignored unless XRL_WINDOW_INPUT=1). Exit code = number of failed checks. Extra arguments go to the guest (e.g. --hold 10 to keep the picture).
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin"
cp tests/guest/out/fbdemo.elf "$R/bin/fbdemo.elf"
XRL_REAL_GFX=1 XRL_NO_AUDIO=1 XRL_WINDOW_INPUT="${XRL_WINDOW_INPUT:-0}" XRL_INPUT_AFTER=input-ready \
XRL_INPUT_SCRIPT="0.3:key:30:1,0.4:key:30:0,0.5:key:42:1,0.55:key:48:1,0.6:key:48:0,0.65:key:42:0,0.8:key:30:1,0.85:key:30:1,0.9:key:30:0,\
1.0:abs:0:16384,1.0:abs:1:8192,1.1:key:272:1,1.2:key:272:0,1.3:rel:0:5,1.3:rel:1:-3,1.4:rel:8:1,1.5:text:0:97,1.6:key:30:1,1.7:focus:0:0" \
MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-90}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/fbdemo.elf --selftest "$@" 2>&1
rc=$?
rm -rf "$R"
exit $rc

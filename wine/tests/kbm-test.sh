#!/bin/bash
# kbm-test.sh -- host keyboard/mouse -> Windows program, through the layer and Wine, on the laptop (Git Bash).
#
#   XRL_ROOTFS=<staged guest tree with the 0027 Wine build> wine/tests/kbm-test.sh
#
# Runs wine/tests/pe/kbm_test.c (built with mingw in WSL into $XRL_ROOTFS/kbm_test.exe if missing) as the Windows
# program, injects keyboard/mouse events into the host's queue (the test host's XRL_INPUT_SCRIPT), and checks what the
# program's window received (its log is <rootfs>/tmp/kbm.log). Exit code = number of failed checks. The tree needs the
# 0027 user32.dll/xrlhost.dll/xrlhost.so AND the 0026 win32u.so (the pump is started by the in-process desktop thread).
cd "$(dirname "$0")/../.." || exit 2
root="${XRL_ROOTFS:?set XRL_ROOTFS to the staged guest tree (see tools/run-desktop-wine.sh)}"
rootu=$(cygpath -u "$root")
if [ ! -f "$rootu/kbm_test.exe" ]; then
  wsl -e bash -lc "x86_64-w64-mingw32-gcc -O1 -Wall -o '$(wslpath -a "$root")/kbm_test.exe' '$(wslpath -a "$PWD")/wine/tests/pe/kbm_test.c' -luser32 -lkernel32" || exit 2
fi
export XRL_NO_AUDIO=1 XRL_TIMEOUT="${XRL_TIMEOUT:-170}" XRL_INPUT_AFTER=kbm-ready XRL_WINDOW_INPUT=0
export XRL_INPUT_SCRIPT="0.5:abs:0:16384,0.5:abs:1:16384,1.0:key:30:1,1.05:key:30:0,1.3:key:42:1,1.35:key:48:1,1.4:key:48:0,1.45:key:42:0,1.8:rel:8:1,2.0:key:272:1,2.05:key:272:0,2.3:rel:0:5,2.3:rel:1:-3,2.6:key:105:1,2.65:key:105:0"
rm -f "$rootu/tmp/kbm.log"
bash tools/run-desktop-wine.sh 'Z:\kbm_test.exe' 8 > "${TMPDIR:-/tmp}/kbm-test.out" 2>&1
log="$rootu/tmp/kbm.log"
fails=0
check() { if grep -qE "$1" "$log" 2>/dev/null; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
check '^WM_KEYDOWN vk=0x41 scan=0x1e ext=0' "key A down: VK_A, scancode 0x1e"
check "^WM_CHAR 'a' " "...and WM_CHAR 'a' (the layout turns the key into a character)"
check '^WM_KEYUP vk=0x41 ' "key A up"
check "^WM_CHAR 'B' " "Shift+B types 'B'"
check '^WM_KEYDOWN vk=0x10 scan=0x2a' "Left Shift down"
check '^WM_MOUSEWHEEL delta=120' "wheel notch = WHEEL_DELTA 120"
check '^WM_KEYDOWN vk=0x25 scan=0x4b ext=1' "Left arrow: extended key"
check '^WM_INPUT mouse dx=5 dy=-3 flags=0x0' "relative motion 5,-3 arrives as raw input"
read -r w h < <(sed -n 's/^screen \([0-9]*\)x\([0-9]*\)/\1 \2/p' "$log")
if [ -n "$w" ]; then
  check "^WM_LBUTTONDOWN $((w/2)),$((h/2))" "left button down at the centre of ${w}x${h} (the absolute pointer at half range)"
  check "^WM_MOUSEMOVE $((w/2+5)),$((h/2-3))" "...and the relative move lands 5 right, 3 up of it"
else
  echo "  FAIL no 'screen WxH' line in $log"; fails=$((fails+1))
fi
echo "TOTAL FAILS=$fails"
exit $fails

#!/bin/sh
# An X11 desktop session on the layer (Git Bash): Xvfb + twm + xterm as guest processes on an Alpine rootfs that has
# xvfb xdpyinfo xwd xwininfo twm xterm (xrl-work/alpine/rootfs). Needs guest processes (fork/exec: the proc.c branch)
# merged with AF_UNIX sockets, ptys and the rest of the userland branch. Asserts the xterm window exists, saves a
# screenshot (tools/xwd2png.py makes the PNG).   Usage: layer/tests/xsession-test.sh [rootfs] [out.png]
cd "$(dirname "$0")/.."
R="${1:-C:/Users/10harbad/xrl-work/alpine/rootfs}"
OUT="${2:-/tmp/xsession.png}"
HOSTR=$(cygpath -u "$R")
cat > "$HOSTR/tmp/xrl-xsession.sh" <<'EOF'
#!/bin/sh
/usr/bin/Xvfb :0 -screen 0 1280x720x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
export DISPLAY=:0
n=0
until /usr/bin/xdpyinfo >/dev/null 2>&1; do n=$((n+1)); [ $n -gt 60 ] && break; sleep 1; done
echo "display up after $n tries"
/usr/bin/twm >/tmp/twm.log 2>&1 &
sleep 4
/usr/bin/xterm -geometry 60x12+40+40 -e /bin/sh -c 'echo hello from xterm on the layer; id; sleep 30' >/tmp/xterm.log 2>&1 &
sleep 10
/usr/bin/xwininfo -root -tree
/usr/bin/xwd -root -silent -out /tmp/xsession.xwd
kill %1 %2 %3
EOF
rm -f "$HOSTR/tmp/xsession.xwd"
o=$(XRL_UID=0 XRL_GID=0 XRL_NO_AUDIO=1 MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-240}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$HOSTR")" /bin/busybox sh /tmp/xrl-xsession.sh 2>&1)
rc=0
echo "$o" | grep -E "^\[I guest\.out\]" | sed 's/^\[I guest.out\] //' | head -40
echo "$o" | grep -q '"sh": ("xterm" "XTerm")' || { echo "FAIL: no xterm window in the tree"; rc=1; }
if [ -s "$HOSTR/tmp/xsession.xwd" ]; then python -I ../tools/xwd2png.py "$(cygpath -w "$HOSTR/tmp/xsession.xwd")" "$OUT" && echo "screenshot: $OUT"; else echo "FAIL: no screenshot"; rc=1; fi
rm -f "$HOSTR/tmp/xrl-xsession.sh" "$HOSTR/tmp/xsession.xwd" "$HOSTR/tmp/xvfb.log" "$HOSTR/tmp/twm.log" "$HOSTR/tmp/xterm.log"
exit $rc

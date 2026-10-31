#!/bin/bash
# xsession.sh <xterm|xfce|firefox> - an X session inside the guest (Debian rootfs), for tools/ci/desktop-run.sh, which
# copies this file to /tmp/xrl-xsession.sh in the rootfs and runs it on the layer. Same script runs in a chroot on real
# Linux for the reference. Xvfb on :0, then:
#   xterm    an xterm (milestone 3)
#   xfce     startxfce4 under dbus-launch (milestone 4)
#   firefox  the XFCE session + Firefox showing a local page (milestone 5)
#   plasma   KDE Plasma 6 (X11 session, KWin on Mesa llvmpipe) under dbus-run-session
#   tv       the XFCE session shown on /dev/fb0 (the TV on the Xbox) through /usr/local/bin/xbridge (the X picture over
#            MIT-SHM + DAMAGE, controller/keyboard/mouse from /dev/input/event* as XTest); fb0 is dumped to
#            /tmp/xrl-fb0.raw (BGRX, the X screen's size) at the end
# Prints the window tree (xwininfo -root -tree) and saves the screen to /tmp/xrl-<mode>.xwd. Logs of the background
# programs go to /tmp/xrl-*.log and are printed at the end.
mode=${1:-xterm}
poll=1; [ "$mode" = xterm ] || poll=3   # each look at the window tree is two processes: fewer of them in the logs
export DISPLAY=:0 HOME=${HOME:-/root} XDG_RUNTIME_DIR=/tmp/xrl-runtime NO_AT_BRIDGE=1
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
rm -f /tmp/.X0-lock /tmp/.X11-unix/X0 /tmp/xrl-*.log "/tmp/xrl-$mode.xwd"
t0=$SECONDS
say() { echo "[xsession +$((SECONDS - t0))s] $*"; }

# wait_for <seconds> <grep -E pattern>: until the window tree shows the pattern
wait_for() {
    local n=0
    until /usr/bin/xwininfo -root -tree 2> /dev/null | grep -qE "$2"; do
        n=$((n + poll)); [ $n -ge "$1" ] && { say "timed out waiting for /$2/"; return 1; }
        sleep "$poll"
    done
    say "found /$2/"
}

/usr/bin/Xvfb :0 -screen 0 1280x720x24 -nolisten tcp > /tmp/xrl-xvfb.log 2>&1 &
pids=$!
n=0
until /usr/bin/xdpyinfo > /dev/null 2>&1; do n=$((n + 1)); [ $n -gt 90 ] && break; sleep 1; done
say "display up after $n tries"

case $mode in
xterm)
    /usr/bin/xterm -geometry 80x20+40+40 -e /bin/bash -c 'echo hello from xterm; id; sleep 120' > /tmp/xrl-xterm.log 2>&1 &
    pids="$pids $!"
    wait_for 90 '\("xterm" "XTerm"\)'
    ;;
plasma)
    /usr/bin/dbus-run-session /usr/bin/startplasma-x11 > /tmp/xrl-plasma.log 2>&1 &
    pids="$pids $!"
    wait_for 400 '"plasmashell"' && wait_for 200 '"(Desktop|plasmashell)".*1280x'
    ;;
xfce|firefox|tv)
    if [ "$mode" = tv ]; then
        rm -f /tmp/xrl-fb0.raw
        /usr/local/bin/xbridge --x --unix /tmp/.X11-unix/X0 --seconds "${XRL_TV_SECONDS:-120}" --dump /tmp/xrl-fb0.raw \
            > /tmp/xrl-bridge.log 2>&1 &
        bridge=$!
    fi
    /usr/bin/dbus-launch --exit-with-session /usr/bin/startxfce4 > /tmp/xrl-xfce.log 2>&1 &
    pids="$pids $!"
    wait_for 240 '"xfce4-panel"' && wait_for 120 '"(Desktop|xfdesktop)"'
    if [ "$mode" = firefox ]; then
        mkdir -p /tmp/xrl-page
        cat > /tmp/xrl-page/index.html <<'EOF'
<!doctype html><html><head><title>xbox-rl page</title></head>
<body style="background:#1d6b33;color:#fff;font:32px sans-serif"><h1>Hello from Firefox on xbox-rl</h1></body></html>
EOF
        mkdir -p /tmp/xrl-ffprofile
        # fewer processes on a console with ~5 GiB for everything: two content processes, no per-site isolation, no
        # preallocated, file://, privileged, extension, socket, RDD, utility, GPU or fork-server processes
        for pr in 'dom.ipc.processCount", 2' 'fission.autostart", false' 'browser.shell.checkDefaultBrowser", false' \
                'dom.ipc.processPrelaunch.enabled", false' 'browser.tabs.remote.separateFileUriScheme", false' \
                'browser.tabs.remote.separatePrivilegedContentProcess", false' \
                'browser.tabs.remote.separatePrivilegedMozillaWebContentProcess", false' \
                'extensions.webextensions.remote", false' 'network.process.enabled", false' \
                'media.rdd-process.enabled", false' 'media.utility-process.enabled", false' \
                'layers.gpu-process.enabled", false' 'dom.ipc.forkserver.enable", false' \
                'browser.sessionstore.resume_from_crash", false' 'datareporting.policy.dataSubmissionEnabled", false'; do
            echo "user_pref(\"$pr);"
        done > /tmp/xrl-ffprofile/user.js
        # MOZ_LOG: process start-up and IPC, to see where it waits when no window comes (/tmp/xrl-firefox.log);
        # no crash reporter: a crashing process ends (and the layer logs where) instead of waiting for crashhelper
        env -u MOZ_HEADLESS MOZ_ENABLE_WAYLAND=0 MOZ_CRASHREPORTER_DISABLE=1 MOZ_LOG=IPC:4,ipc:4,ContentParent:4,ProcessChild:4,Widget:3 \
            /usr/bin/firefox-esr --no-remote --new-instance --profile /tmp/xrl-ffprofile \
            file:///tmp/xrl-page/index.html > /tmp/xrl-firefox.log 2>&1 &
        pids="$pids $!"
        wait_for 300 '"xbox-rl page'
    fi
    ;;
esac
[ "$mode" = xterm ] && sleep 5 || sleep 20   # xfdesktop fills its icons in asynchronously
if [ "$mode" = tv ]; then wait "$bridge"; say "bridge done: $(tail -n 3 /tmp/xrl-bridge.log | tr '\n' ' ')"; fi
/usr/bin/xwininfo -root -tree
/usr/bin/xwd -root -silent -out "/tmp/xrl-$mode.xwd" && say "screenshot /tmp/xrl-$mode.xwd ($(stat -c %s "/tmp/xrl-$mode.xwd") bytes)"
for f in /tmp/xrl-*.log; do echo "--- $f"; tail -n 40 "$f"; done
if [ "$mode" = xfce ]; then   # what xfdesktop's Home / File System icons are made from
    echo "--- stat $HOME / /root/Desktop"; /usr/bin/stat "$HOME" / "$HOME/Desktop" 2>&1
fi
kill $pids 2> /dev/null
sleep 1
# on the layer this script is the init process: end every other guest process (dbus-daemon, xfconfd, ...) so the
# test host exits. Never on real Linux (it would be everything on the machine).
[ "$XRL_SESSION_KILLALL" = 1 ] && kill -9 -1 2> /dev/null
exit 0

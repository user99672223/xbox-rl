#!/bin/sh
# test-present.sh [config ...]      (Git Bash on the laptop)
#
# Picture test: the Windows D3D11 program wine/tests/pe/xrl_present_test.c runs under Wine on the
# layer, presents frames to the desktop test host's window (XRL_REAL_GFX=1: gfx/ on the laptop GPU),
# and for every swapchain configuration a screenshot of that window is taken and its pixels
# compared with the picture the program was presenting. Needs the guest tree staged with the test
# built into it (wine/tests/build-pe-tests.sh <rootfs>/prefix/drive_c). Screenshots land in
# results/present-<n>.png, the logs in results/present-<n>.txt. With PATTERN=1 the program holds
# four quadrants (red, green / blue, yellow) instead of one colour and all four are checked:
# orientation and channel order (results/present-pattern-<n>.png).
#   XRL_ROOTFS='C:\Users\me\xrl-work\rootfs-av' tools/test-present.sh        # every configuration
#   XRL_ROOTFS=... PATTERN=1 tools/test-present.sh 1 5
cd "$(dirname "$0")/.." || exit 2
root="${XRL_ROOTFS:?set XRL_ROOTFS to your staged guest tree}"
export XRL_ROOTFS="$root" XRL_REAL_GFX=1
configs="$*"; [ -n "$configs" ] || configs="0 1 2 3 4 5 6 7 8"
match=$(basename "$(cygpath -u "$root")")
tag=present; extra=; mode=-Expect; want=51,153,255
if [ "${PATTERN:-0}" = 1 ]; then
    tag=present-pattern; extra=--pattern; mode=-Points
    want="0.25,0.25:255,0,0;0.75,0.25:0,255,0;0.25,0.75:0,0,255;0.75,0.75:255,255,0"
fi
fail=0
for n in $configs; do
    tools/run-desktop-wine.sh 'C:\xrl_present_test.exe' --only "$n" --hold 12 $extra > "results/$tag-$n.txt" 2>&1 &
    pid=$!
    sleep 8
    if pwsh -NoProfile -File tools/capture-window.ps1 -Out "results/$tag-$n.png" "$mode" "$want" -CommandLineMatch "$match" \
        > "results/$tag-$n.capture.txt" 2>&1; then r=ok; else r=FAIL; fail=1; fi
    wait $pid
    grep -q "RESULT: 1 of 1" "results/$tag-$n.txt" || { r="$r (program failed)"; fail=1; }
    echo "config $n: screenshot $r: $(grep -v '^client' "results/$tag-$n.capture.txt" | tr '\n' ' ' | cut -c1-300)"
done
exit $fail

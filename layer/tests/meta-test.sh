#!/bin/sh
# Unix modes/owners (vfs.c meta, .xrl-meta) in the layer (Git Bash). Needs layer/tests/guest/build.sh run in WSL.
# Two runs on one tree: as root (XRL_UID=0), then as uid 1000.
cd "$(dirname "$0")/.."
R="${XRL_TEST_ROOT:-$(mktemp -d)}"
mkdir -p "$R/bin"
cp tests/guest/out/meta_test.elf "$R/bin/meta_test.elf"
printf '#default\t0\t0\n' > "$R/.xrl-meta"
rc=0
for who in "root 0" "user 1000"; do
    set -- $who
    o=$(XRL_UID=$2 XRL_GID=$2 MSYS_NO_PATHCONV=1 timeout "${XRL_TIMEOUT:-60}" ./build/desktop/xrl_test_host.exe "$(cygpath -w "$R")" /bin/meta_test.elf $1 --fake-host 2>&1)
    echo "$o" | grep -E "^\[I guest\.out\]" | sed 's/^\[I guest.out\] //'
    echo "$o" | grep -q "meta: all checks passed" || rc=1
done
rm -rf "$R"
exit $rc

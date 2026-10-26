#!/usr/bin/env bash
# build-pe-tests.sh DEST_DIR [test ...]      (WSL)
#
# Cross-compile the Windows test programs of wine/tests/pe/ with mingw-w64 into DEST_DIR, which
# is normally <rootfs>/prefix/drive_c (the guest sees it as C:\).  Without test names it builds
# every program that takes no special setup:  xrl_present_test  xrl_audio_test.
#   wine/tests/build-pe-tests.sh /mnt/c/Users/<you>/xrl-work/rootfs-av/prefix/drive_c
. "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"
need x86_64-w64-mingw32-gcc
dest=${1:?usage: build-pe-tests.sh DEST_DIR [test ...]}; shift || true
here="$(dirname "${BASH_SOURCE[0]}")/pe"
mkdir -p "$dest"
tests=("$@"); [ ${#tests[@]} = 0 ] && tests=(xrl_present_test xrl_audio_test)
for t in "${tests[@]}"; do
  case $t in
    xrl_present_test) libs="-ld3d11 -ldxgi -luuid -lole32 -luser32" ;;
    xrl_audio_test)   libs="-lole32 -luuid -lm" ;;
    xrl_aio_test)     libs="" ;;
    *) die "unknown test $t" ;;
  esac
  [ -f "$here/$t.c" ] || { warn "$t.c does not exist yet"; continue; }
  x86_64-w64-mingw32-gcc -O1 -Wall -o "$dest/$t.exe" "$here/$t.c" $libs || die "$t: mingw build failed"
  log "built $dest/$t.exe"
done

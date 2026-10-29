#!/usr/bin/env bash
# build-wine-glibc.sh -- the same patched Wine as build-wine.sh, built against the distro's glibc instead
# of our musl, for Easy Anti-Cheat: EAC's bootstrapper and runtime are glibc programs and read the game's
# libc the way glibc lays it out (docs/decisions.md). Runs in WSL; reuses build-wine.sh's host tools
# ($WORK/build/wine-tools, stage 'tools'), but patches its OWN source tree ($WORK/gsrc/src/wine) with this
# checkout's patches/wine, so other branches' patches in the shared $WORK/src/wine never leak in.
#
#   wine/scripts/build-wine-glibc.sh [stage ...]    stages: deps patch gateway configure build install (default: all)
#
#   deps       apt packages: gnutls headers (needs root in WSL: `wsl -u root`)
#   patch      export the pin + this checkout's patches/wine into $WORK/gsrc/src/wine, ./autogen.sh
#   gateway    libxrlgw.so: xrl_syscall() as a raw syscall (wine/glibc/xrl_gateway.c)
#   configure  native configure (gcc, glibc) into $WORK/build/wine-glibc
#   build      make
#   install    make install-lib into $WORK/out/rootfs-glibc, plus glibc and every library the unix side needs
#
# Differences from the musl build: no xrl-tpfix (the layer's load-time rewriter handles glibc's %fs and
# syscall sites, as for any foreign binary), no no-syscall check (same reason), the libraries live in
# /lib/x86_64-linux-gnu as on Debian/Ubuntu.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

stages=("$@"); [ ${#stages[@]} -eq 0 ] && stages=(deps patch gateway configure build install)
want() { local s; for s in "${stages[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

SRC="$WORK/gsrc/src/wine"; TOOLS="$WORK/build/wine-tools"
BLD="$WORK/build/wine-glibc"; OUT="$WORK/out/rootfs-glibc"; GW="$WORK/glibc-gw"
cap_jobs

if want deps; then
  if ! dpkg -s libgnutls28-dev >/dev/null 2>&1; then
    [ "$(id -u)" = 0 ] || die "run the 'deps' stage as root: wsl -u root -e bash $0 deps"
    apt-get update -qq && apt-get install -y -qq libgnutls28-dev >/dev/null || die "apt-get install libgnutls28-dev failed"
  fi
  log "deps: libgnutls28-dev present"
fi

if want patch; then
  XRL_WORK="$WORK/gsrc" "$WINE_DIR/scripts/apply-patches.sh" wine
  cp "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h"
  if [ ! -f "$SRC/configure" ] || [ "$SRC/.xrl-stamp" -nt "$SRC/configure" ]; then
    log "wine-glibc: ./autogen.sh"
    ( cd "$SRC" && ./autogen.sh >"$WORK/gsrc/autogen.log" 2>&1 ) || die "autogen failed ($WORK/gsrc/autogen.log)"
  fi
fi

if want gateway; then
  mkdir -p "$GW"
  gcc -O2 -fPIC -shared -nostdlib -Wl,-soname,libxrlgw.so -o "$GW/libxrlgw.so" "$WINE_DIR/glibc/xrl_gateway.c"
  log "gateway: $GW/libxrlgw.so"
fi

if want configure; then
  [ -f "$SRC/configure" ] || die "no patched source at $SRC -- run the 'patch' stage"
  [ -x "$TOOLS/tools/winebuild/winebuild" ] || die "no host tools at $TOOLS -- run build-wine.sh tools"
  [ -f "$GW/libxrlgw.so" ] || die "no $GW/libxrlgw.so -- run the 'gateway' stage"
  mkdir -p "$BLD"; cd "$BLD"
  log "wine-glibc: configure in $BLD"
  # The same switches as the musl build (build-wine.sh 'configure'), native gcc, xrl_syscall from libxrlgw.
  CC=gcc CROSSCC=x86_64-w64-mingw32-gcc \
  CPPFLAGS="-I$ROOT/include" \
  CFLAGS="-O1 -g0 -std=gnu17 -fcf-protection=none -fno-stack-protector -mno-tls-direct-seg-refs" \
  CROSSCFLAGS="-O1" LIBS="-L$GW -lxrlgw" \
  "$SRC/configure" ac_cv_func_epoll_create=no ac_cv_func_epoll_create1=no \
      --prefix=/usr --libdir=/usr/lib \
      --with-wine-tools="$TOOLS" --enable-win64 --disable-tests --disable-amd_ags_x64 \
      --without-x --without-freetype --with-gnutls --without-gstreamer --without-pulse \
      --without-alsa --without-oss --without-cups --without-dbus --without-v4l2 --without-udev \
      --without-usb --without-sane --without-gphoto --without-pcap --without-krb5 --without-opencl \
      --without-vulkan --without-sdl --without-unwind --without-wayland --without-capi \
      --without-pcsclite --without-netapi --without-ffmpeg --without-opengl --without-gettext \
      --without-inotify >configure.log 2>&1 || { tail -30 configure.log; die "configure failed"; }
fi

if want build; then
  cmp -s "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h" || cp "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h"
  cd "$BLD"
  find . -name '*.o' -size 0 -delete 2>/dev/null || true
  log "wine-glibc: make -j$JOBS (free: $(df -BG --output=avail "$WORK" | tail -1 | tr -dc 0-9) GB in the WSL disk)"
  make -j"$JOBS" -k >make.log 2>&1 || { grep -n 'error:\|Error [0-9]' make.log | grep -v "fonts/" | head -40; die "build failed (see $BLD/make.log)"; }
  log "wine-glibc: built ($(du -sh . | cut -f1))"
fi

if want install; then
  cd "$BLD"
  rm -rf "$OUT"; mkdir -p "$OUT"
  make install-lib DESTDIR="$OUT" >install.log 2>&1 || die "install-lib failed (install.log)"
  rm -f "$OUT"/usr/bin/wine-preloader "$OUT"/usr/bin/wine64-preloader "$OUT"/usr/bin/wineserver \
        "$OUT"/usr/lib/wine/*/wine-preloader "$OUT"/usr/lib/wine/*/wine64-preloader
  install -m 755 "$BLD/server/wineserver.so" "$OUT/usr/lib/wine/x86_64-unix/wineserver.so"
  # glibc + the gateway + every shared library the unix side needs (gnutls and its deps are dlopen()ed
  # by soname, so they are named explicitly), from this distro, laid out as on Debian/Ubuntu.
  L="$OUT/lib/x86_64-linux-gnu"; mkdir -p "$L" "$OUT/lib64"
  cp "$GW/libxrlgw.so" "$L/"
  cp -L /lib64/ld-linux-x86-64.so.2 "$OUT/lib64/"
  libs=$( { find "$OUT/usr" -type f -name '*.so' -o -type f -path '*/bin/*'; } | while read -r f; do
            file -b "$f" | grep -q '^ELF' && LD_LIBRARY_PATH="$GW" ldd "$f" 2>/dev/null; done
          for s in libgnutls.so.30; do ldd "/lib/x86_64-linux-gnu/$s"; echo " $s => /lib/x86_64-linux-gnu/$s ("; done )
  echo "$libs" | grep -o '=> /[^ ]*' | cut -c4- | sort -u | while read -r so; do
    [ -f "$so" ] && cp -L "$so" "$L/"
  done
  log "wine-glibc: installed into $OUT ($(du -sh "$OUT" | cut -f1)); libraries: $(ls "$L" | tr '\n' ' ')"
fi

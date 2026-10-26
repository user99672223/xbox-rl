#!/usr/bin/env bash
# build-wine.sh -- build Proton's Wine (pinned) for the xbox-rl layer.   Runs in WSL.
#
#   wine/scripts/build-wine.sh [stage ...]       default: all stages
#
# Stages (each is idempotent and cheap to re-run):
#   vendor     fetch the pinned wine + musl commits into vendor/   (fetch-vendor.sh)
#   musl       build the patched musl sysroot + xrl-musl-gcc       (build-musl.sh)
#   patch      export the pinned wine tree, apply patches/wine/*, ./autogen.sh
#   tools      build Wine's *build-host* tools (winebuild, widl, wrc, ...) with the
#              distro gcc.  They must run on this machine; a musl-linked tool would need
#              the gateway, so it cannot.
#   configure  cross-configure: unix side = xrl-musl-gcc, PE side = mingw-w64
#   build      make
#   install    make install-lib into $WORK/out/rootfs  (+ musl libc.so as /lib/ld-musl-x86_64.so.1)
#   check      assert no `syscall` instruction in anything under out/rootfs
#
# Environment:
#   XRL_WORK        build area (default ~/xrl-build; keep it on the WSL ext4 disk)
#   XRL_JOBS        parallel jobs (default nproc, capped by free RAM)
#   XRL_HOSTTEST=1  build against the *laptop-only* pass-through libc (sysroot-hosttest) so the
#                   result can run under WSL (wine/tests/run-wine-hosttest.sh).  Output goes to
#                   out/rootfs-hosttest.  Never ship this.
#   XRL_KEEP_OBJS=0 delete $WORK/build/wine objects after `install` (disk is tight on C:)
#
# Disk: sources ~0.4 GB, build tree ~1.5-2.5 GB, rootfs ~0.5 GB.  The script prints free space
# before/after the heavy stages and refuses to start `build` below XRL_MIN_FREE_GB (default 4).
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need gcc make git autoconf flex bison perl x86_64-w64-mingw32-gcc python3 file

stages=("$@"); [ ${#stages[@]} -eq 0 ] && stages=(vendor musl patch tools configure build install check)
want() { local s; for s in "${stages[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

variant=prod; [ "${XRL_HOSTTEST:-0}" = 1 ] && variant=hosttest
if [ $variant = hosttest ]; then
  SYS="$WORK/sysroot-hosttest"; OUT="$WORK/out/rootfs-hosttest"; BLD="$WORK/build/wine-hosttest"
else
  SYS="$WORK/sysroot"; OUT="$WORK/out/rootfs"; BLD="$WORK/build/wine"
fi
SRC="$WORK/src/wine"; TOOLS="$WORK/build/wine-tools"
# gnutls (build-gnutls.sh) makes schannel/bcrypt work: configure --with-gnutls when it was built
DEPS="$WORK/deps"; gnutls_args=(--without-gnutls)
[ $variant != hosttest ] && [ -f "$DEPS/lib/libgnutls.so" ] && gnutls_args=(--with-gnutls "GNUTLS_CFLAGS=-I$DEPS/include" "GNUTLS_LIBS=-L$DEPS/lib -lgnutls")
# The WSL disk reports ~1 TB free but its VHDX lives on C:, so measure the Windows drive.
free_gb() { df -BG --output=avail /mnt/c 2>/dev/null | tail -1 | tr -dc 0-9; }
cap_jobs

if want vendor; then "$WINE_DIR/scripts/fetch-vendor.sh"; fi
if want musl;   then XRL_MUSL_VARIANT=$variant "$WINE_DIR/scripts/build-musl.sh"; fi

if want patch; then
  "$WINE_DIR/scripts/apply-patches.sh" wine
  # The frozen ABI header is single-sourced in include/ and copied (never edited) into the tree so
  # that makedep, which only scans Wine's own include path, can resolve `#include "xrl_abi.h"`.
  cp "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h"
  if [ ! -f "$SRC/configure" ] || [ "$SRC/.xrl-stamp" -nt "$SRC/configure" ]; then
    log "wine: ./autogen.sh (regenerates configure, server protocol headers, specfiles)"
    ( cd "$SRC" && ./autogen.sh >"$WORK/autogen.log" 2>&1 ) || die "autogen failed, see $WORK/autogen.log"
  fi
fi

if want tools; then
  # Host tools: plain gcc, no musl, no cross.  FreeType is wanted here so sfnt2fon can build the
  # .fon bitmap fonts that the prefix needs (the *target* is configured --without-freetype).
  mkdir -p "$TOOLS"; cd "$TOOLS"
  if [ ! -f Makefile ]; then
    log "wine tools: configure (host)"
    "$SRC/configure" --enable-win64 --disable-tests --without-x --without-gnutls --without-gstreamer \
        --without-pulse --without-alsa --without-oss --without-cups --without-dbus --without-v4l2 \
        --without-udev --without-usb --without-sane --without-gphoto --without-pcap --without-krb5 \
        --without-opencl --without-vulkan --without-sdl --without-unwind --without-wayland \
        --without-capi --without-pcsclite --without-netapi --without-ffmpeg --without-opengl \
        CFLAGS="-O1 -std=gnu17" >configure.log 2>&1 || { tail -20 configure.log; die "tools configure failed"; }
  fi
  log "wine tools: make __tooldeps__ -j$JOBS"
  make -j"$JOBS" __tooldeps__ >make.log 2>&1 || { tail -20 make.log; die "tools build failed"; }
fi

if want configure; then
  [ -x "$SYS/bin/xrl-musl-gcc" ] || die "no sysroot at $SYS -- run the 'musl' stage"
  [ -x "$TOOLS/tools/winebuild/winebuild" ] || die "no host tools at $TOOLS -- run the 'tools' stage"
  mkdir -p "$BLD"; cd "$BLD"
  log "wine: cross configure ($variant) in $BLD"
  # ac_cv_*: epoll off -> the in-process wineserver uses poll() (fewer syscalls to implement in the layer)
  CC="$SYS/bin/xrl-musl-gcc" CROSSCC=x86_64-w64-mingw32-gcc \
  CPPFLAGS="-I$ROOT/include" \
  CFLAGS="-O1 -g0 -std=gnu17 -fcf-protection=none -fno-stack-protector -mno-tls-direct-seg-refs" \
  CROSSCFLAGS="-O1" \
  "$SRC/configure" --host=x86_64-linux-musl --build=x86_64-linux-gnu \
      ac_cv_func_epoll_create=no ac_cv_func_epoll_create1=no \
      --prefix=/usr --libdir=/usr/lib \
      --with-wine-tools="$TOOLS" --enable-win64 --disable-tests --disable-amd_ags_x64 \
      --without-x --without-freetype "${gnutls_args[@]}" --without-gstreamer --without-pulse \
      --without-alsa --without-oss --without-cups --without-dbus --without-v4l2 --without-udev \
      --without-usb --without-sane --without-gphoto --without-pcap --without-krb5 --without-opencl \
      --without-vulkan --without-sdl --without-unwind --without-wayland --without-capi \
      --without-pcsclite --without-netapi --without-ffmpeg --without-opengl --without-gettext \
      --without-inotify >configure.log 2>&1 || { tail -30 configure.log; die "cross configure failed"; }
fi

if want build; then
  # Refresh the ABI header copy here too (not only at `patch`), so an incremental build
  # never compiles against a stale contract; copy only on change to keep make's timestamps.
  cmp -s "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h" || cp "$ROOT/include/xrl_abi.h" "$SRC/include/xrl_abi.h"
  cd "$BLD"
  [ "$(free_gb)" -ge "${XRL_MIN_FREE_GB:-4}" ] || die "only $(free_gb) GB free; refusing to build (XRL_MIN_FREE_GB)"
  log "wine: free before build: $(free_gb) GB"
  find . -name '*.o' -size 0 -delete 2>/dev/null || true     # leftovers from a killed compile
  make -j"$JOBS" -k >make.log 2>&1 || { grep -n 'error:\|Error [0-9]' make.log | grep -v "fonts/" | head -40; die "build failed (see $BLD/make.log)"; }
  log "wine: built. free after: $(free_gb) GB, build tree $(du -sh . | cut -f1)"
fi

if want install; then
  cd "$BLD"
  rm -rf "$OUT"; mkdir -p "$OUT"
  make install-lib DESTDIR="$OUT" >install.log 2>&1 || die "install-lib failed (install.log)"
  # wine-preloader is freestanding and issues raw `syscall`s; the layer reserves the address space
  # itself, so it is not shipped.  (There is no wineserver binary any more: it is wineserver.so.)
  rm -f "$OUT"/usr/bin/wine-preloader "$OUT"/usr/bin/wine64-preloader "$OUT"/usr/bin/wineserver \
        "$OUT"/usr/lib/wine/*/wine-preloader "$OUT"/usr/lib/wine/*/wine64-preloader
  # server/ has no MODULE, so install-lib does not know about wineserver.so (the in-process server)
  install -m 755 "$BLD/server/wineserver.so" "$OUT/usr/lib/wine/x86_64-unix/wineserver.so"
  # gnutls and its two libraries, which Wine dlopen()s by soname (secur32, bcrypt)
  if [ "${gnutls_args[0]}" = --with-gnutls ]; then
    mkdir -p "$OUT/usr/lib"
    cp -a "$DEPS"/lib/libgnutls.so* "$DEPS"/lib/libnettle.so* "$DEPS"/lib/libhogweed.so* "$OUT/usr/lib/"
    log "wine: gnutls installed into the rootfs"
  fi
  # the interpreter the layer's VFS serves at /lib/ld-musl-x86_64.so.1
  mkdir -p "$OUT/lib"; cp "$SYS/usr/lib/libc.so" "$OUT/lib/ld-musl-x86_64.so.1"
  # Thread pointer: the console has no FSGSBASE, so every `mov %fs:0,%reg` becomes a load from
  # the host TEB slot (XRL_TP_GS_OFFSET in include/xrl_abi.h). Console variant only: the hosttest
  # rootfs runs on real Linux, where FS works and the TEB slot does not exist.
  if [ $variant != hosttest ]; then
    find "$OUT" -type f -print0 | while IFS= read -r -d '' f; do
      case "$(file -b "$f")" in ELF*x86-64*) echo "$f" ;; esac
    done | xargs -r python3 "$ROOT/tools/xrl-tpfix.py" >tpfix.log 2>&1 \
      || { tail -20 tpfix.log; die "xrl-tpfix: unsupported %fs use (see $BLD/tpfix.log)"; }
    log "wine: thread-pointer loads patched ($(grep -c 'load(s)' tpfix.log) ELF files)"
  fi
  log "wine: installed into $OUT ($(du -sh "$OUT" | cut -f1))"
  [ "${XRL_KEEP_OBJS:-1}" = 0 ] && { find "$BLD" -name '*.o' -delete; log "build objects deleted"; }
fi

if want check; then
  if [ $variant = hosttest ]; then warn "hosttest rootfs contains the pass-through shim; skipping no-syscall check"
  else
    "$WINE_DIR/scripts/check-no-syscall.sh" "$OUT"
    # and no %fs use left anywhere (the console cannot set an FS base); files that pass are stamped
    # (e_flags) so the layer's load-time rewriter, which exists for foreign binaries, skips them
    find "$OUT" -type f -print0 | while IFS= read -r -d '' f; do
      case "$(file -b "$f")" in ELF*x86-64*) echo "$f" ;; esac
    done | xargs -r python3 "$ROOT/tools/xrl-tpfix.py" --check --stamp >"$BLD/tpcheck.log" 2>&1 \
      || { grep -v 'OK, no' "$BLD/tpcheck.log" | head -20; die "check: %fs use remains"; }
    log "check: no %fs use in $(wc -l <"$BLD/tpcheck.log") ELF files"
  fi
fi

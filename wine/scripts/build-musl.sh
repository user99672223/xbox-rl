#!/usr/bin/env bash
# build-musl.sh -- build the *patched* musl (xrl_syscall gateway) into $SYSROOT
# and generate the `xrl-musl-gcc` compiler wrapper.
#
# Why we build musl ourselves instead of installing a prebuilt musl cross
# toolchain: the whole point is to replace __syscall, so the libc has to be ours.
# The compiler itself is the distro's gcc (no cross needed: host and target are
# both x86_64 Linux user space); only the libc/headers/crt differ, selected by a
# musl-gcc.specs file generated with absolute paths into $SYSROOT.
#
# Outputs:
#   $SYSROOT/usr/include        musl headers + Linux kernel UAPI headers (linux/, asm/, asm-generic/)
#   $SYSROOT/usr/lib            libc.a, libc.so, crt1.o, Scrt1.o, crti.o, crtn.o, rcrt1.o, libm.a ...
#   $SYSROOT/lib/ld-musl-x86_64.so.1   (symlink to ../usr/lib/libc.so) -- the guest's /lib/ld-musl-x86_64.so.1
#   $SYSROOT/bin/xrl-musl-gcc   wrapper: gcc with -specs=musl-gcc.specs
#   $SYSROOT/xrl-musl-gcc.specs
#
# Then verifies libc.so/libc.a contain NO syscall instruction.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need gcc make git objdump

"$WINE_DIR/scripts/fetch-vendor.sh"
"$WINE_DIR/scripts/apply-patches.sh" musl

# XRL_MUSL_VARIANT=hosttest builds a *laptop-only* libc whose gateway slot is pre-wired to
# wine/tests/musl-smoke/shim.S (a pass-through to the real Linux kernel), so Wine itself can be
# run under WSL.  It is never shipped and deliberately skips the no-syscall check.
variant="${XRL_MUSL_VARIANT:-prod}"
src="$WORK/src/musl"; bld="$WORK/build/musl"
if [ "$variant" = hosttest ]; then
  SYSROOT="$WORK/sysroot-hosttest"; bld="$WORK/build/musl-hosttest"
  rm -rf "$WORK/src/musl-hosttest"; cp -r "$src" "$WORK/src/musl-hosttest"
  src="$WORK/src/musl-hosttest"
  rm -f "$src/src/thread/x86_64/xrl_gateway_slot.s"
  cp "$WINE_DIR/tests/musl-smoke/shim.S" "$src/src/thread/x86_64/xrl_hosttest_shim.S"
  # fake host op-tables (xinput/gfx) so the Wine forwarders can be tested on a PC
  cp "$WINE_DIR/tests/musl-smoke/host_ops.c" "$src/src/thread/xrl_hosttest_ops.c"
  cp "$ROOT/include/xrl_abi.h" "$src/src/thread/xrl_abi.h"
fi
extra=""
if [ "$variant" = hosttest ]; then
  extra="$(cat "$WINE_DIR"/tests/musl-smoke/shim.S "$WINE_DIR"/tests/musl-smoke/host_ops.c "$ROOT/include/xrl_abi.h" | sha256sum | cut -c1-16)"
fi
stamp="$(cat "$WORK/src/musl/.xrl-stamp")$variant$extra"
if [ -f "$SYSROOT/.xrl-musl-stamp" ] && [ "$(cat "$SYSROOT/.xrl-musl-stamp")" = "$stamp" ]; then
  log "musl: sysroot up to date"; exit 0
fi

rm -rf "$bld" "$SYSROOT"; mkdir -p "$bld" "$SYSROOT"
cd "$bld"
log "musl: configure"
# --prefix=/usr + DESTDIR: the dynamic linker path baked into binaries must be
# /lib/ld-musl-x86_64.so.1 (that is what the layer's VFS serves as the guest
# interpreter), while files land inside $SYSROOT.
"$src/configure" --prefix=/usr --syslibdir=/lib \
    --enable-shared --enable-static --disable-wrapper \
    CFLAGS="-O2 -g1 -fno-stack-protector -fcf-protection=none"
log "musl: make -j$JOBS"
make -j"$JOBS" >/dev/null
make install DESTDIR="$SYSROOT" >/dev/null

# Linux UAPI headers: musl ships none; Wine includes <linux/input.h> etc.
inc="$SYSROOT/usr/include"
cp -r /usr/include/linux /usr/include/asm-generic "$inc/"
cp -r /usr/include/x86_64-linux-gnu/asm "$inc/asm"

# Compiler wrapper driven by a specs file with absolute paths.
sh "$src/tools/musl-gcc.specs.sh" "$SYSROOT/usr/include" "$SYSROOT/usr/lib" "/lib/ld-musl-x86_64.so.1" \
    > "$SYSROOT/xrl-musl-gcc.specs"
mkdir -p "$SYSROOT/bin"
cat > "$SYSROOT/bin/xrl-musl-gcc" <<EOF
#!/bin/sh
# gcc against the xbox-rl patched musl. -std=gnu17: Ubuntu 26.04's gcc 15 defaults to gnu23.
# \$ORIGIN rpath: musl matches a DT_NEEDED name only through the library search path, so Wine's
# unix libs (win32u.so, wineserver.so, ...) must be able to find the already-loaded ntdll.so next to them.
exec "\${REALGCC:-gcc}" -specs "$SYSROOT/xrl-musl-gcc.specs" -Wl,-rpath,'\$ORIGIN' "\$@"
EOF
chmod +x "$SYSROOT/bin/xrl-musl-gcc"
ln -sf xrl-musl-gcc "$SYSROOT/bin/xrl-musl-cc"

# The acceptance check.
if [ "$variant" = prod ]; then
  "$WINE_DIR/scripts/check-no-syscall.sh" "$SYSROOT/usr/lib"
else
  warn "hosttest variant: contains the pass-through shim's syscall instructions (laptop only)"
fi

echo "$stamp" > "$SYSROOT/.xrl-musl-stamp"
log "musl: sysroot ready at $SYSROOT (wrapper: $SYSROOT/bin/xrl-musl-gcc)"

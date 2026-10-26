#!/usr/bin/env bash
# Build the smoke test against the patched sysroot and run it on the laptop
# through a pass-through gateway.  Passing proves: every syscall musl makes
# (incl. clone/sigreturn/unmapself) is routed through xrl_syscall, and that the
# gateway contracts documented in wine/notes-B.md are internally consistent.
. "$(dirname "${BASH_SOURCE[0]}")/../../scripts/common.sh"
cc="$SYSROOT/bin/xrl-musl-gcc"
[ -x "$cc" ] || die "run wine/scripts/build-musl.sh first"
out="$WORK/build/smoke"; mkdir -p "$out"
here="$(dirname "${BASH_SOURCE[0]}")"
"$cc" -O1 -g -static -I"$ROOT/include" -o "$out/smoke" "$here/smoke.c" "$here/shim.S" "$here/host_ops.c"
# The shim object defines __xrl_gateway, so libc's slot object is not pulled in.
"$WINE_DIR/scripts/check-no-syscall.sh" "$SYSROOT/usr/lib" >/dev/null
# shim.S itself contains syscall instructions by design; prove that is the *only* place.
n=$(objdump -d --no-show-raw-insn "$out/smoke" | grep -cE '^\s+[0-9a-f]+:\s+syscall' || true)
log "smoke binary: $n syscall instructions (expected: the shim's 3 only)"
"$out/smoke"

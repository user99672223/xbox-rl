#!/usr/bin/env bash
# bake-prefix.sh [out-dir]   -- produce the pre-baked Wine prefix that ships to the console.
#
# The console cannot run wineboot as a separate process (one OS process, writable FS = LocalState
# only, registry writes denied), so the prefix is created once on the laptop and shipped as data.
#
# The recipe runs the hosttest Wine (musl build + pass-through libc, tests/run-wine-hosttest.sh)
# with the SAME in-process settings the console uses (wineserver is a thread; XRL_NO_SPAWN=1 stubs
# helper processes) and a neutral identity (USER=xrl), so the prefix is made by the code that will
# consume it.  Because there are no child processes, the steps wineboot would normally delegate
# to children are run here as *main* processes, in "prefix bootstrap" mode (WINEBOOTSTRAPMODE=1:
# builtin DLLs/EXEs are loaded straight from the build tree, no stub files needed yet):
#
#   1. wineboot.exe --init                       dirs, registry hives, user profile
#   2. rundll32 setupapi,InstallHinfSection PreInstall / DefaultInstall 128 wine.inf
#                                                fake-DLL stubs in system32, fonts, inf files, more registry
#   3. reg add ...                               the bits explorer.exe would have written (see below)
#   4. trim + MANIFEST
#
# What ends up in the prefix (win64-only, measured on the first successful bake: ~280 MB, ~800
# files in system32 before trimming):
#   system.reg user.reg userdef.reg          registry hives (HKLM / HKCU / defaults)
#   dosdevices/c: -> ../drive_c              (relative symlink)    dosdevices/z: -> /  (the rootfs)
#   drive_c/windows/{system32,inf,Fonts,resources,...}   fake-DLL stubs + wine.inf-driven files
#   drive_c/users/xrl/...                    profile dirs
#   .update-timestamp                        stops wineboot from wanting to "update" the prefix
#
# Registry values written in step 3 (they are what the missing helper processes would create):
#   HKLM\System\CurrentControlSet\Control\Video\{0000...}\0000  GraphicsDriver = "null"
#        win32u's lazy driver load otherwise reports "no driver could be loaded" and the FIRST
#        top-level CreateWindow FAILS (nodrv_CreateWindow).  "null" selects the built-in null
#        user driver, whose CreateWindow succeeds: windows are pure server-side objects, the
#        host owns the real CoreWindow.
#   HKCU\Software\Wine\Direct3D\renderer = no3d
#        wined3d (still used by Wine's dxgi for adapter/output enumeration) must not try
#        Vulkan/OpenGL.
#
# The shipped layout is <LocalState>\rootfs\prefix  (== boot ctx prefix_path "/prefix").
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

OUT="${1:-$WORK/out/prefix}"
BLD="$WORK/build/wine-hosttest"
[ -d "$BLD/programs/wineboot" ] || die "no hosttest build (XRL_HOSTTEST=1 build-wine.sh ...)"
export XRL_WORK="$WORK"   # keep the work dir when HOME changes below
export WINEPREFIX="$OUT"
export USER=xrl LOGNAME=xrl HOME=/home/xrl USERNAME=xrl
rm -rf "$OUT"; mkdir -p "$OUT"
RUN="$WINE_DIR/tests/run-wine-hosttest.sh"
PE="$BLD/programs"
INF="Z:${BLD//\//\\}\\loader\\wine.inf"

step() { local name=$1; shift; log "bake: $name"; WINEBOOTSTRAPMODE=1 WINEDEBUG="${WINEDEBUG:-err+all}" "$RUN" "$@" 2>&1 \
         | grep -v 'bcrypt:key_asymmetric_create' | sed 's/^/    /' || true; }

step "wineboot --init"        "$PE/wineboot/x86_64-windows/wineboot.exe" --init
step "wine.inf PreInstall"    "$PE/rundll32/x86_64-windows/rundll32.exe" setupapi,InstallHinfSection PreInstall 128 "$INF"
step "wine.inf DefaultInstall" "$PE/rundll32/x86_64-windows/rundll32.exe" setupapi,InstallHinfSection DefaultInstall 128 "$INF"
step "GraphicsDriver=null"    "$PE/reg/x86_64-windows/reg.exe" add 'HKLM\System\CurrentControlSet\Control\Video\{00000000-0000-0000-0000-000000000000}\0000' /v GraphicsDriver /d null /f
step "renderer=no3d"          "$PE/reg/x86_64-windows/reg.exe" add 'HKCU\Software\Wine\Direct3D' /v renderer /d no3d /f

[ -f "$OUT/system.reg" ] || die "no system.reg produced -- the bake did not run"

log "bake: trimming"
# symlinks that point into the laptop's home (My Documents -> $HOME/Documents ...) become real dirs
find "$OUT/drive_c/users" -type l | while read -r l; do rm -f "$l"; mkdir -p "$l"; done
rm -rf "$OUT/.wineserver" "$OUT/drive_c/windows/temp"/* "$OUT/drive_c/windows/logs" 2>/dev/null || true
find "$OUT" -name '*.log' -delete 2>/dev/null || true
# deterministic mtimes (the registry hives carry their own timestamps inside)
find "$OUT" -exec touch -h -d '2026-01-01 00:00:00' {} + 2>/dev/null || true

log "bake: manifest"
( cd "$OUT" && find . \( -type f -o -type l \) | sort | while read -r f; do
    if [ -L "$f" ]; then printf 'L %s -> %s\n' "$f" "$(readlink "$f")"
    else printf 'F %s %s %s\n' "$f" "$(stat -c %s "$f")" "$(sha256sum "$f" | cut -c1-16)"; fi
  done ) > "$OUT.MANIFEST"
log "bake: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1); manifest $OUT.MANIFEST"

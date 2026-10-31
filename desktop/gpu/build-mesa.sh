#!/bin/bash
# build-mesa.sh <out.tar.gz> - OpenGL on the console's D3D12 for the Debian desktop (docs: desktop/gpu/README.md).
#
# Builds, for Debian 13 (the CI rootfs's release: same glibc, libX11, LLVM), into the prefix /opt/xrl-gpu:
#   - Mesa $MESA_VERSION with the gallium drivers d3d12 (GL on D3D12, the WSLg approach) and llvmpipe (fallback),
#     GLX through glvnd (libGLX_mesa) on the DRI software winsys (XPutImage / MIT-SHM), EGL on X11; compiled with
#     Dozen (Vulkan on D3D12, -Dvulkan-drivers=microsoft-experimental), all compiled with
#     -DXRL_WIN64_ABI: DirectX-Headers' STDMETHODCALLTYPE/WINAPI become __attribute__((ms_abi)) and struct-returning
#     methods take MSVC's hidden return pointer, so Mesa calls the console's Win64 D3D12 objects directly
#     (patches/mesa-*.patch: fences without eventfds, DXIL signed in nir_to_dxil, llvmpipe fallback)
#   - VA-API decode (lib/dri/d3d12_drv_video.so: H.264, HEVC, VP9) on the same d3d12 screen, X11 through the
#     software winsys (no DRM device needed)
#   - libxrl-d3d12.so (xrl-d3d12/), installed as libd3d12.so and libdxcore.so: the device from the layer's host
#     op table, a root signature serializer, one DXCore adapter
#   - bin/xrl-gpu: runs a program with this Mesa (LD_LIBRARY_PATH, the glvnd EGL vendor file, GALLIUM_DRIVER=d3d12)
# The tarball unpacks at the rootfs root (opt/xrl-gpu/...). Debian's own Mesa stays the default (llvmpipe).
#
# Where it builds: inside Debian trixie it builds in place; elsewhere it runs itself in a trixie environment:
# docker (debian:trixie) when available, else a debootstrap chroot at $XRL_BUILD_CHROOT (default
# /var/tmp/xrl-gpu-chroot, kept between runs). Needs root for the chroot. Inputs are pinned (versions + sha256),
# so the CI can cache the output by this directory's hash.
set -euo pipefail
MESA_VERSION=25.0.7
MESA_SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
DXH_TAG=v1.614.1
DXH_COMMIT=48a762973271c5a75869946bf1fdbc489a628a5c
PREFIX=/opt/xrl-gpu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(realpath -m "$1")

in_trixie() { grep -q '^VERSION_CODENAME=trixie' /etc/os-release 2>/dev/null; }

if ! in_trixie; then
    rel=${here#"$repo"/}
    outdir=$(dirname "$out")
    mkdir -p "$outdir"
    if command -v docker > /dev/null && docker info > /dev/null 2>&1; then
        exec docker run --rm -v "$repo:/src:ro" -v "$outdir:/out" debian:trixie \
            bash "/src/$rel/build-mesa.sh" "/out/$(basename "$out")"
    fi
    ch=${XRL_BUILD_CHROOT:-/var/tmp/xrl-gpu-chroot}
    if [ ! -x "$ch/usr/bin/apt-get" ]; then
        command -v debootstrap > /dev/null || { apt-get update -qq; apt-get install -y -qq debootstrap > /dev/null; }
        script=/usr/share/debootstrap/scripts/trixie
        [ -e "$script" ] || script=/usr/share/debootstrap/scripts/sid
        debootstrap --variant=minbase trixie "$ch" http://deb.debian.org/debian "$script"
    fi
    cp /etc/resolv.conf "$ch/etc/resolv.conf" 2>/dev/null || true
    mkdir -p "$ch/src" "$ch/out"
    for m in /proc /dev; do mountpoint -q "$ch$m" || mount --bind "$m" "$ch$m"; done
    mountpoint -q "$ch/src" || mount --bind "$repo" "$ch/src"
    mountpoint -q "$ch/out" || mount --bind "$outdir" "$ch/out"
    # a proxied build host (HTTPS_PROXY + its CA bundle in CURL_CA_BUNDLE): the same inside the chroot
    pass=()
    for v in HTTPS_PROXY https_proxy HTTP_PROXY http_proxy NO_PROXY no_proxy; do [ -n "${!v:-}" ] && pass+=("$v=${!v}"); done
    if [ -n "${CURL_CA_BUNDLE:-}" ] && [ -f "$CURL_CA_BUNDLE" ]; then
        cp "$CURL_CA_BUNDLE" "$ch/etc/xrl-build-ca.crt"
        pass+=(CURL_CA_BUNDLE=/etc/xrl-build-ca.crt GIT_SSL_CAINFO=/etc/xrl-build-ca.crt SSL_CERT_FILE=/etc/xrl-build-ca.crt)
    fi
    rc=0
    chroot "$ch" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root MAKEFLAGS="${MAKEFLAGS:-}" "${pass[@]}" \
        bash "/src/$rel/build-mesa.sh" "/out/$(basename "$out")" || rc=$?
    for m in /out /src /dev /proc; do umount "$ch$m" 2>/dev/null || true; done
    exit $rc
fi

# ---- inside Debian trixie ----
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ca-certificates curl git xz-utils patch gcc g++ meson ninja-build pkgconf bison flex \
    python3-mako python3-yaml python3-packaging \
    libdrm-dev libx11-dev libxext-dev libxfixes-dev libx11-xcb-dev libxcb-glx0-dev libxcb-shm0-dev libxcb-dri2-0-dev \
    libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-randr0-dev libxshmfence-dev \
    libxxf86vm-dev libxrandr-dev libexpat1-dev zlib1g-dev libzstd-dev libglvnd-dev llvm-19-dev libva-dev > /dev/null
w=${XRL_GPU_WORK:-/tmp/xrl-gpu-build}
mkdir -p "$w"
cd "$w"

# sources
if [ ! -f "mesa-$MESA_VERSION.tar.xz" ]; then
    curl -sSfLO "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
fi
echo "$MESA_SHA256  mesa-$MESA_VERSION.tar.xz" | sha256sum -c -
rm -rf "mesa-$MESA_VERSION" dxh
tar xf "mesa-$MESA_VERSION.tar.xz"
git -c advice.detachedHead=false clone -q --depth 1 -b "$DXH_TAG" https://github.com/microsoft/DirectX-Headers.git dxh
[ "$(git -C dxh rev-parse HEAD)" = "$DXH_COMMIT" ] || { echo "DirectX-Headers $DXH_TAG is not $DXH_COMMIT"; exit 1; }

# DirectX-Headers for Win64 COM from Linux code (XRL_WIN64_ABI):
#  - calling conventions: ms_abi instead of nothing (the WSL runtime is SysV-built; the console's is not)
#  - methods returning a struct: MSVC returns them through a hidden pointer after `this`, which is what the headers'
#    MinGW declarations spell out (`T *Get(T *RetVal)`); take those instead of the by-value ones
python3 -I - dxh/include/wsl/stubs/basetsd.h <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "#define __cdecl\n#define __stdcall\n#define STDMETHODCALLTYPE\n#define STDAPICALLTYPE\n#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE\n#define WINAPI\n"
new = """#ifdef XRL_WIN64_ABI
// xbox-rl: the D3D12 runtime is the console's own (Win64 COM): every COM method and API entry point uses the
// Microsoft x64 calling convention, called directly from this Linux (SysV) code.
#define __cdecl __attribute__((ms_abi))
#define __stdcall __attribute__((ms_abi))
#define STDMETHODCALLTYPE __attribute__((ms_abi))
#define STDAPICALLTYPE __attribute__((ms_abi))
#define WINAPI __attribute__((ms_abi))
#else
#define __cdecl
#define __stdcall
#define STDMETHODCALLTYPE
#define STDAPICALLTYPE
#define WINAPI
#endif
#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
"""
assert old in s, "basetsd.h calling conventions not found"
open(p, "w").write(s.replace(old, new))
PY
grep -rlE '^#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)' dxh/include | xargs sed -i -E \
    's/^#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)(\r?)$/#if (defined(_MSC_VER) || !defined(_WIN32)) \&\& !defined(XRL_WIN64_ABI)\1/'
! grep -rqE '^#if defined\(_MSC_VER\) \|\| !defined\(_WIN32\)' dxh/include
# the C interfaces (Dozen is C): MIDL writes the RetVal form for every Windows compiler there, guarded by !_WIN32
grep -rlE '^#if !defined\(_WIN32\)' dxh/include | xargs sed -i -E \
    's/^#if !defined\(_WIN32\)(\r?)$/#if !defined(_WIN32) \&\& !defined(XRL_WIN64_ABI)\1/'

# Mesa: its DirectX-Headers subproject is ours; the xbox-rl patch and the digest code
mkdir -p "mesa-$MESA_VERSION/subprojects"
rm -rf "mesa-$MESA_VERSION/subprojects/DirectX-Headers-1.0" "mesa-$MESA_VERSION/subprojects/DirectX-Headers.wrap"
cp -r dxh "mesa-$MESA_VERSION/subprojects/DirectX-Headers"
patch -d "mesa-$MESA_VERSION" -p1 < "$here/patches/mesa-$MESA_VERSION-xrl-win64-abi.patch"
cp "$here/xrl-d3d12/xrl_dxil_hash.c" "$here/xrl-d3d12/xrl_dxil_hash.h" "mesa-$MESA_VERSION/src/microsoft/compiler/"

rm -rf build dest
meson setup build "mesa-$MESA_VERSION" --wrap-mode=nodownload --force-fallback-for=DirectX-Headers \
    --prefix="$PREFIX" --libdir=lib --buildtype=release -Db_ndebug=true \
    -Dc_args=-DXRL_WIN64_ABI -Dcpp_args=-DXRL_WIN64_ABI \
    -Dplatforms=x11 -Dglx=dri -Dglvnd=enabled -Degl=enabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled \
    -Dgallium-drivers=d3d12,llvmpipe -Dvulkan-drivers=microsoft-experimental -Dllvm=enabled -Dshared-llvm=enabled \
    -Dgallium-d3d12-video=enabled -Dgallium-d3d12-graphics=enabled -Dvideo-codecs=h264dec,h265dec,vp9dec -Dgallium-va=enabled \
    -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log 2>&1 \
    || { tail -40 meson-setup.log; exit 1; }
grep -E "gallium-drivers|Gallium drivers|GLX|EGL" meson-setup.log | head -10 || true
ninja -C build > ninja.log 2>&1 || { tail -60 ninja.log; exit 1; }
DESTDIR="$w/dest" ninja -C build install > /dev/null

# the shim: the same headers, the same flags
inc=$w/dxh/include
d=$w/dest$PREFIX
g++ -std=c++17 -DXRL_WIN64_ABI -O2 -g -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter \
    -I"$inc" -I"$inc/wsl/stubs" -I"$repo/include" -I"$here/xrl-d3d12" -c "$here/xrl-d3d12/xrl_d3d12.cpp" -o shim.o
gcc -std=gnu11 -O2 -g -fPIC -fvisibility=hidden -Wall -I"$here/xrl-d3d12" -c "$here/xrl-d3d12/xrl_dxil_hash.c" -o hash.o
g++ -shared -Wl,-soname,libxrl-d3d12.so -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL \
    -o "$d/lib/libxrl-d3d12.so" shim.o hash.o -lpthread
ln -sf libxrl-d3d12.so "$d/lib/libd3d12.so"
ln -sf libxrl-d3d12.so "$d/lib/libdxcore.so"
bash "$here/tests/rootsig-test.sh" "$d/lib/libxrl-d3d12.so" "$inc"

mkdir -p "$d/bin"
cat > "$d/bin/xrl-gpu" <<'SH'
#!/bin/sh
# xrl-gpu <program> [args] - run a program with xbox-rl's Mesa: OpenGL on the console's D3D12 (Mesa d3d12 driver).
# GALLIUM_DRIVER=d3d12 (as on WSLg); with this Mesa a d3d12 screen that cannot start (off the layer, or no D3D12
# device from the host op table) falls back to llvmpipe. XRL_GPU_DRIVER=llvmpipe: the same Mesa on the CPU.
p=/opt/xrl-gpu/lib
export LD_LIBRARY_PATH="$p${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export __EGL_VENDOR_LIBRARY_FILENAMES=/opt/xrl-gpu/share/glvnd/egl_vendor.d/50_mesa.json
# VA-API: Mesa's d3d12 video driver (decode: H.264, HEVC, VP9; the console has no AV1 and no D3D12 encode)
export LIBVA_DRIVERS_PATH=/opt/xrl-gpu/lib/dri LIBVA_DRIVER_NAME=d3d12
# Vulkan: Dozen (Mesa's Vulkan on D3D12) as the only driver
export VK_DRIVER_FILES=/opt/xrl-gpu/share/vulkan/icd.d/dzn_icd.x86_64.json VK_ICD_FILENAMES=/opt/xrl-gpu/share/vulkan/icd.d/dzn_icd.x86_64.json
unset LIBGL_ALWAYS_SOFTWARE
export GALLIUM_DRIVER="${XRL_GPU_DRIVER:-d3d12}"
exec "$@"
SH
chmod 755 "$d/bin/xrl-gpu"
cat > "$d/BUILD-INFO" <<EOF
mesa $MESA_VERSION ($MESA_SHA256)
DirectX-Headers $DXH_TAG ($DXH_COMMIT)
patch $(sha256sum "$here/patches/mesa-$MESA_VERSION-xrl-win64-abi.patch" | cut -c1-16)
built $(date -u +%Y-%m-%dT%H:%MZ) on $(. /etc/os-release; echo "$PRETTY_NAME"), $(gcc --version | head -1)
EOF
# the shim's ABI check: every export ms_abi-callable, only the D3D12/DXCore names
nm -D --defined-only "$d/lib/libxrl-d3d12.so" | awk '{print $3}' | sort | tr '\n' ' '; echo
rm -rf "$d/include" "$d/lib/pkgconfig" "$d"/lib/*.a
tar -C "$w/dest" --numeric-owner -czf "$out" .
du -h "$out"

#!/bin/bash
# ltp-build.sh <out.tar.gz> - the Linux Test Project's syscall tests, built on the CI's Linux machine and installed
# under /opt/ltp (testcases/bin + runtest/syscalls), packed to be put into the layer's root file system.
# LTP_TAG picks the release (default below). Tests that fail to build are skipped (make -k): the run lists them MISSING.
set -uo pipefail
out=$(realpath -m "$1")
tag=${LTP_TAG:-20260930}
sudo apt-get update -qq && sudo apt-get install -y -qq autoconf automake m4 pkgconf libtirpc-dev libaio-dev libacl1-dev libcap-dev > /dev/null
w=$(mktemp -d)
git clone -q --depth 1 -b "$tag" https://github.com/linux-test-project/ltp.git "$w/ltp" || exit 1
cd "$w/ltp"
make autotools > /dev/null 2>&1 && ./configure --prefix=/opt/ltp > /dev/null || exit 1
j=$(nproc)
make -k -j"$j" -C lib > /dev/null 2>&1
make -k -j"$j" -C libs > /dev/null 2>&1
make -k -j"$j" -C testcases/kernel/syscalls > build.log 2>&1
sudo make -k -C testcases/kernel/syscalls install > /dev/null 2>&1
sudo mkdir -p /opt/ltp/runtest && sudo cp runtest/syscalls /opt/ltp/runtest/
echo "built $(ls /opt/ltp/testcases/bin | wc -l) programs for $(grep -vc '^#\|^$' runtest/syscalls) runtest entries (LTP $tag)"
grep -c "Error" build.log | xargs echo "make errors:"
tar -C / -czf "$out" opt/ltp
du -h "$out"

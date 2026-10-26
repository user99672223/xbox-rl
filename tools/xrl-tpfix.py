#!/usr/bin/env python3
"""xrl-tpfix: move a guest ELF's thread pointer from %fs to the host TEB slot.

The Xbox hides FSGSBASE, so the FS base cannot carry the guest thread pointer
(see XRL_TP_GS_OFFSET in include/xrl_abi.h). Guest code is compiled with
-mno-tls-direct-seg-refs and without a TLS stack-protector guard, which leaves
exactly one %fs form in the machine code:

    mov %fs:0x0, %r64          64 REX.W 8b /r(mod=00,rm=100) 25 00000000

Each is rewritten in place to the same-length

    mov %gs:XRL_TP_GS_OFFSET, %r64     65 REX.W 8b ... 25 <offset>

Any other %fs use (direct TLS offsets, the %fs:0x28 stack-protector canary,
rdfsbase/wrfsbase) is an error: the build flags are wrong, and patching around
it would hide that. Instructions are located with objdump, so bytes that merely
look like the pattern inside other instructions are never touched.

usage: xrl-tpfix.py [--check] FILE...
  --check   only verify that no %fs use remains (exit 1 if any does)
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ABI_H = os.path.join(HERE, '..', 'include', 'xrl_abi.h')


def tp_gs_offset():
    with open(ABI_H, encoding='utf-8') as f:
        m = re.search(r'#define\s+XRL_TP_GS_OFFSET\s+(0x[0-9a-fA-F]+|\d+)', f.read())
    if not m:
        sys.exit(f'xrl-tpfix: XRL_TP_GS_OFFSET not found in {ABI_H}')
    return int(m.group(1), 0)


def exec_segments(data):
    """[(vaddr, offset, filesz)] for executable PT_LOAD segments of an ELF64."""
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise ValueError('not a little-endian ELF64 file')
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    segs = []
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1 and p_flags & 1:      # PT_LOAD, PF_X
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def vaddr_to_off(segs, addr):
    for vaddr, off, size in segs:
        if vaddr <= addr < vaddr + size:
            return addr - vaddr + off
    return None


# objdump -d -w line:  "  401062:\t64 48 8b 04 25 00 00 00 00 \tmov    %fs:0x0,%rax"
LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')


def fs_uses(path):
    out = subprocess.run(['objdump', '-d', '-w', path], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(f'objdump failed on {path}: {out.stderr.strip()}')
    for line in out.stdout.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        text = m.group(3)
        if '%fs' in text or 'fsbase' in text:
            yield int(m.group(1), 16), bytes.fromhex(m.group(2).replace(' ', '')), text.strip()


def is_tp_load(b):
    return (len(b) == 9 and b[0] == 0x64 and b[1] in (0x48, 0x4c) and b[2] == 0x8b
            and (b[3] & 0xc7) == 0x04 and b[4] == 0x25 and b[5:9] == b'\0\0\0\0')


def hint(text):
    if '%fs:0x28' in text:
        return 'stack-protector canary: build with -fno-stack-protector or -mstack-protector-guard=global'
    if 'fsbase' in text:
        return 'rdfsbase/wrfsbase are unusable on the console'
    return 'direct TLS access: build with -mno-tls-direct-seg-refs'


def process(path, check_only, gs_off):
    with open(path, 'rb') as f:
        data = bytearray(f.read())
    segs = exec_segments(data)
    bad, sites = [], []
    for addr, raw, text in fs_uses(path):
        if is_tp_load(raw):
            sites.append(addr)
        else:
            bad.append((addr, text))
    for addr, text in bad:
        print(f'{path}: {addr:#x}: {text}  <- {hint(text)}', file=sys.stderr)
    if check_only:
        left = len(bad) + len(sites)
        print(f'xrl-tpfix: {path}: {"OK, no %fs use" if not left else f"{left} %fs use(s) remain"}')
        return left == 0
    if bad:
        print(f'xrl-tpfix: {path}: {len(bad)} unsupported %fs use(s); not patched', file=sys.stderr)
        return False
    for addr in sites:
        off = vaddr_to_off(segs, addr)
        if off is None or not is_tp_load(bytes(data[off:off + 9])):
            print(f'xrl-tpfix: {path}: {addr:#x} does not map to the expected bytes', file=sys.stderr)
            return False
        data[off] = 0x65
        data[off + 5:off + 9] = struct.pack('<I', gs_off)
    if sites:
        with open(path, 'wb') as f:
            f.write(data)
    print(f'xrl-tpfix: {path}: {len(sites)} thread-pointer load(s) -> %gs:{gs_off:#x}')
    return True


def main(argv):
    check = '--check' in argv
    files = [a for a in argv if a != '--check']
    if not files:
        sys.exit(__doc__)
    gs_off = tp_gs_offset()
    ok = all([process(p, check, gs_off) for p in files])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main(sys.argv[1:])

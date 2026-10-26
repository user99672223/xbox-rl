#!/usr/bin/env python3
"""xrl-elf-unhide.py FILE.o ...   -- give the global symbols of relocatable x86-64 ELF objects default visibility.

The static GCC runtime (libgcc_eh.a, libgcc.a) is built with hidden visibility (HIDE_EXPORTS): its _Unwind_*
functions cannot be exported from a shared object as they are. wine/scripts/build-libgcc-s.sh links the archive
members into a libgcc_s.so.1 for the musl guest, so it sets the visibility of every defined global symbol in them
to default (STV_DEFAULT) first. Nothing else in the objects changes."""
import struct, sys

def unhide(path):
    d = bytearray(open(path, 'rb').read())
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
        return 0
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x3a)
    n = 0
    for i in range(shnum):
        off = shoff + i * shentsize
        sh_type, = struct.unpack_from('<I', d, off + 4)
        if sh_type != 2:                      # SHT_SYMTAB
            continue
        sh_offset, sh_size = struct.unpack_from('<QQ', d, off + 0x18)
        sh_entsize, = struct.unpack_from('<Q', d, off + 0x38)
        for s in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_info, st_other = d[s + 4], d[s + 5]
            st_shndx, = struct.unpack_from('<H', d, s + 6)
            bind = st_info >> 4
            if bind in (1, 2) and st_shndx != 0 and (st_other & 3) == 2:      # global/weak, defined, STV_HIDDEN
                d[s + 5] = st_other & ~3
                n += 1
    if n:
        open(path, 'wb').write(d)
    return n

if __name__ == '__main__':
    total = sum(unhide(p) for p in sys.argv[1:])
    print('xrl-elf-unhide: %d symbol(s) made default-visibility in %d file(s)' % (total, len(sys.argv) - 1))

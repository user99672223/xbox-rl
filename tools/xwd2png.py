"""xwd2png.py IN.xwd OUT.png - convert an xwd screenshot (ZPixmap, 24/32 bpp TrueColor) to PNG, standard library only."""
import struct
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
(hs, ver, fmt, depth, w, h, xoff, bo, bu, bbo, bp, bpp, bpl, vc, rm, gm, bm, bits, cme, ncol) = struct.unpack('>20I', d[:80])
pix = d[hs + ncol * 12:]
print('xwd: %dx%d depth %d bpp %d bpl %d byte_order %d masks %x %x %x' % (w, h, depth, bpp, bpl, bo, rm, gm, bm))
rows = []
for y in range(h):
    row = pix[y * bpl:y * bpl + w * (bpp // 8)]
    out = bytearray()
    for x in range(w):
        px = int.from_bytes(row[x * (bpp // 8):(x + 1) * (bpp // 8)], 'little' if bo == 0 else 'big')
        out += bytes(((px & rm) >> (rm & -rm).bit_length() - 1, (px & gm) >> (gm & -gm).bit_length() - 1, (px & bm) >> (bm & -bm).bit_length() - 1))
    rows.append(b'\x00' + bytes(out))
raw = b''.join(rows)


def chunk(t, c):
    return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
print('wrote', sys.argv[2], len(png))

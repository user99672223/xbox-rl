#!/usr/bin/env python3
"""raw2png.py <raw BGRX file> <width> <height> <out.png> - a framebuffer dump (xbridge --dump, /dev/fb0) as a PNG."""
import struct
import sys
import zlib


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    data = open(src, 'rb').read()
    rows = []
    for y in range(h):
        line = data[y * w * 4:(y + 1) * w * 4]
        px = bytearray(w * 3)
        px[0::3], px[1::3], px[2::3] = line[2::4], line[1::4], line[0::4]   # BGRX -> RGB
        rows.append(b'\0' + bytes(px))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
    print('wrote %s %dx%d' % (dst, w, h))


if __name__ == '__main__':
    main()

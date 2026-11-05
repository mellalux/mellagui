#!/usr/bin/env python3
"""Convert a binary P6 PPM (as written by the headless backend) to PNG: ppm2png.py in.ppm out.png"""
import struct
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
raw = parts[3]
rows = b''.join(b'\0' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, c):
    return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)

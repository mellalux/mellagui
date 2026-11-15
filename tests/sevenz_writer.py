"""A writer of .7z archives for the tests of the file manager's listing (no 7z tool on the dev machine). Written from the format's description (7zFormat.txt):
one LZMA folder holding every non-empty file, sizes as substreams, names in UTF-16LE, modification times, empty files and folders as 'empty stream' entries; the
header may itself be LZMA packed (an 'encoded header', what 7-Zip does by default). Only the structure is checked against the reader, plus the two real
archives that were found on the internet (see CLAUDE.md)."""
import lzma
import struct
import zlib

EPOCH = 11644473600


def num(v):
    if v < 0x80:
        return bytes([v])
    for n in range(1, 9):
        if v < (1 << (7 * (n + 1))) or n == 8:
            if n == 8:
                return b'\xff' + struct.pack('<Q', v)
            first = (0xff << (8 - n)) & 0xff | (v >> (8 * n))
            return bytes([first]) + (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')
    raise ValueError(v)


def bits(flags):
    out = bytearray()
    for i in range(0, len(flags), 8):
        b = 0
        for j, f in enumerate(flags[i:i + 8]):
            if f:
                b |= 0x80 >> j
        out.append(b)
    return bytes(out)


def lzma1(data):
    f = [{'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 20, 'lc': 3, 'lp': 0, 'pb': 2}]
    c = lzma.compress(data, format=lzma.FORMAT_RAW, filters=f)
    return c, bytes([0x5d]) + struct.pack('<I', 1 << 20)


def folder_header(pack_pos, pack_size, unpack_size, props, crc=None):
    h = b'\x06' + num(pack_pos) + num(1) + b'\x09' + num(pack_size) + b'\x00'
    h += b'\x07\x0b' + num(1) + b'\x00' + num(1) + b'\x23\x03\x01\x01' + num(len(props)) + props + b'\x0c' + num(unpack_size)
    if crc is not None:
        h += b'\x0a\x01' + struct.pack('<I', crc)
    return h + b'\x00'


def write(path, entries, encode_header=True):
    """entries: [(name, bytes | None for a folder, unix mtime)]"""
    files = [(n, d, t) for n, d, t in entries]
    streams = [d for n, d, t in files if d]
    blob = b''.join(streams)
    packed, props = lzma1(blob) if blob else (b'', b'')
    h = b'\x01'
    if streams:
        h += b'\x04' + folder_header(0, len(packed), len(blob), props)
        h += b'\x08\x0d' + num(len(streams)) + b'\x09' + b''.join(num(len(s)) for s in streams[:-1]) + b'\x00' + b'\x00'
    h += b'\x05' + num(len(files))
    empty = [0 if d else 1 for n, d, t in files]
    if any(empty):
        v = bits(empty)
        h += b'\x0e' + num(len(v)) + v
        ef = bits([1 if d == b'' else 0 for n, d, t in files if not d])
        if any(d == b'' for n, d, t in files):
            h += b'\x0f' + num(len(ef)) + ef
    names = b''.join(n.encode('utf-16-le') + b'\0\0' for n, d, t in files)
    h += b'\x11' + num(len(names) + 1) + b'\x00' + names
    times = b''.join(struct.pack('<Q', (t + EPOCH) * 10000000) for n, d, t in files)
    h += b'\x14' + num(len(times) + 2) + b'\x01\x00' + times
    h += b'\x00\x00'
    body = packed
    if encode_header:
        hp, hprops = lzma1(h)
        pack_pos = len(body)
        body += hp
        h = b'\x17' + folder_header(pack_pos, len(hp), len(h), hprops, zlib.crc32(h) & 0xffffffff) + b'\x00'
    start = struct.pack('<QQI', len(body), len(h), zlib.crc32(h) & 0xffffffff)
    sig = b'7z\xbc\xaf\x27\x1c\x00\x04' + struct.pack('<I', zlib.crc32(start) & 0xffffffff) + start
    open(path, 'wb').write(sig + body + h)

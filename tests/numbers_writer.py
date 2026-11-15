"""A writer of Apple Numbers packages (.numbers, 2013 and later) for the tests of the file manager's preview. THERE IS NO REAL NUMBERS FILE ON THIS MACHINE and
Apple publishes no description of the format, so this is the author's understanding of it and nothing else: a test that passes proves the reader and this
writer agree, not that either agrees with Numbers. It writes: a ZIP with Index/Document.iwa, Index/Tables/DataList.iwa, Index/Tables/Tile-N.iwa and a preview
JPEG; each .iwa is chunks (a 0 byte, a 3 byte length, raw Snappy) of a stream of objects (a varint length, an ArchiveInfo { identifier, MessageInfo { type,
length, references } }, then the messages); the objects are a document (type 1), sheets (2), table infos (6000), table models (6001), tiles (6002) and string
lists (6005); a tile has rows { index, cell count, cell buffer, cell offsets, storage version } and a cell is a storage version byte, a type byte, a flags word
at offset 8 and then the values in the order of the flags (decimal128, double, seconds, string id).

    write(path, sheets, offset_unit=4)  sheets = [{'name': 'Sheet 1', 'tables': [{'name': 'Table 1', 'rows': [[cell, ...], ...]}]}]

A cell is None (empty), a str (text), an int / float (a number as decimal128), ('d', float) a double, ('t', seconds) a date, ('b', bool).
"""
import struct
import zipfile
from decimal import Decimal


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def f_varint(num, v):
    return varint(num << 3) + varint(v)


def f_bytes(num, data):
    if isinstance(data, str):
        data = data.encode()
    return varint(num << 3 | 2) + varint(len(data)) + data


def snappy_literal(data):
    """Raw Snappy made of literals only (a valid stream; the reader's copy elements are tested with the Parquet vectors)."""
    out = bytearray(varint(len(data)))
    i = 0
    while i < len(data):
        n = min(len(data) - i, 60000)
        if n <= 60:
            out.append((n - 1) << 2)
        elif n <= 256:
            out += bytes([60 << 2, n - 1])
        else:
            out += bytes([61 << 2]) + struct.pack('<H', n - 1)
        out += data[i:i + n]
        i += n
    return bytes(out)


def iwa(stream):
    out = bytearray()
    for i in range(0, max(len(stream), 1), 65536):
        c = snappy_literal(stream[i:i + 65536])
        out += b'\x00' + struct.pack('<I', len(c))[:3] + c
    return bytes(out)


def obj(oid, typ, msg, refs=()):
    mi = f_varint(1, typ) + f_bytes(2, varint(1) + varint(0) + varint(5)) + f_varint(3, len(msg))
    if refs:
        mi += f_bytes(5, b''.join(varint(r) for r in refs))
    ai = f_varint(1, oid) + f_bytes(2, mi)
    return varint(len(ai)) + ai + msg


def ref(oid):
    return f_bytes(1, f_varint(1, oid))                 # a TSP.Reference: { identifier = 1 }


def decimal128(value):
    d = Decimal(str(value)).normalize()
    sign, digits, exp = d.as_tuple()
    m = int(''.join(map(str, digits))) if digits else 0
    biased = exp + 0x1820
    b = bytearray(m.to_bytes(14, 'little'))
    b.append(((biased & 0x7f) << 1) | ((m >> 112) & 1))
    b.append(((sign & 1) << 7) | ((biased >> 7) & 0x7f))
    return bytes(b)


def cell_bytes(c, string_id):
    if c is None:
        return None
    if isinstance(c, bool):
        c = ('b', c)
    if isinstance(c, str):
        body, flags, typ = struct.pack('<I', string_id[c]), 0x8, 3
    elif isinstance(c, (int, float)):
        body, flags, typ = decimal128(c), 0x1, 2
    elif c[0] == 'd':
        body, flags, typ = struct.pack('<d', c[1]), 0x2, 2
    elif c[0] == 't':
        body, flags, typ = struct.pack('<d', c[1]), 0x4, 5
    elif c[0] == 'b':
        body, flags, typ = struct.pack('<d', 1.0 if c[1] else 0.0), 0x2, 6
    else:
        raise ValueError(c)
    data = bytes([5, typ, 0, 0, 0, 0, 0, 0]) + struct.pack('<I', flags) + body
    return data + bytes(-len(data) % 4)                 # cells start on a multiple of four bytes


def tile_message(rows, string_id, first_row, unit):
    infos = b''
    ncells = 0
    maxcol = 0
    for r, row in enumerate(rows):
        buf = bytearray()
        offs = []
        for c in row:
            cb = cell_bytes(c, string_id)
            if cb is None:
                offs.append(0xffff)
            else:
                offs.append(len(buf) // unit)
                buf += cb
                ncells += 1
        while offs and offs[-1] == 0xffff:
            offs.pop()
        maxcol = max(maxcol, len(offs))
        offs += [0xffff] * 0
        infos += f_bytes(5, f_varint(1, first_row + r) + f_varint(2, sum(1 for c in row if c is not None)) + f_bytes(3, bytes(buf))
                         + f_bytes(4, b''.join(struct.pack('<H', o) for o in offs)) + f_varint(6, 5))
    return f_varint(1, maxcol) + f_varint(2, len(rows)) + f_varint(3, ncells) + f_varint(4, len(rows)) + infos


def write(path, sheets, offset_unit=4, preview=None, rows_per_tile=256):
    docobjs, listobjs, tileobjs = [], [], []
    next_id = [10]

    def new_id():
        next_id[0] += 1
        return next_id[0]
    sheet_ids = []
    for sh in sheets:
        sid = new_id()
        sheet_ids.append(sid)
        info_ids = []
        for tb in sh['tables']:
            tid, mid, lid = new_id(), new_id(), new_id()
            info_ids.append(tid)
            strings = []
            for row in tb['rows']:
                for c in row:
                    if isinstance(c, str) and c not in strings:
                        strings.append(c)
            string_id = {s: i + 1 for i, s in enumerate(strings)}
            entries = b''.join(f_bytes(3, f_varint(1, i + 1) + f_varint(2, 1) + f_bytes(3, s)) for i, s in enumerate(strings))
            listobjs.append(obj(lid, 6005, f_varint(1, 1) + f_varint(2, len(strings) + 1) + entries))
            tile_ids = []
            storage = b''
            for k in range(0, max(len(tb['rows']), 1), rows_per_tile):
                tl = new_id()
                tile_ids.append(tl)
                tileobjs.append(obj(tl, 6002, tile_message(tb['rows'][k:k + rows_per_tile], string_id, 0, offset_unit)))
                storage += f_bytes(1, f_varint(1, k // rows_per_tile) + f_bytes(2, f_varint(1, tl)))
            ncols = max((len(r) for r in tb['rows']), default=0)
            data_store = f_bytes(3, storage) + f_bytes(4, f_varint(1, lid))
            model = (f_bytes(1, '8F2B9A52-6C1D-4E57-9C1E-0A1B2C3D4E5F') + f_bytes(4, tb['name']) + f_varint(6, len(tb['rows'])) + f_varint(7, ncols)
                     + f_bytes(8, data_store))
            docobjs.append(obj(mid, 6001, model, refs=tile_ids + [lid]))
            docobjs.append(obj(tid, 6000, f_bytes(2, f_varint(1, mid)), refs=[mid]))
        sheet_msg = f_bytes(1, sh['name']) + b''.join(f_bytes(2, f_varint(1, i)) for i in info_ids)
        docobjs.append(obj(sid, 2, sheet_msg, refs=info_ids))
    doc = obj(1, 1, b''.join(f_bytes(1, f_varint(1, s)) for s in sheet_ids), refs=sheet_ids)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Index/Document.iwa', iwa(doc + b''.join(docobjs)))
        z.writestr('Index/Tables/DataList.iwa', iwa(b''.join(listobjs)))
        for i, t in enumerate(tileobjs):
            z.writestr('Index/Tables/Tile-%d.iwa' % i, iwa(t))
        z.writestr('Metadata/Properties.plist', '<?xml version="1.0"?><plist version="1.0"><dict/></plist>')
        if preview:
            z.writestr('preview.jpg', preview, compress_type=zipfile.ZIP_STORED)

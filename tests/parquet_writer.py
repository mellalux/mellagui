"""A small Parquet writer for the tests (there is no pyarrow here): flat schemas, plain and dictionary pages, data pages v1 / v2, uncompressed /
snappy (literal blocks only) / gzip. It is written from the format's description, so it shows that the reader understands THAT; real files from other
writers were not available."""
import struct
import zlib


def uv(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zz(n):
    return uv((n << 1) ^ (n >> 63))


class S:
    """A Thrift compact struct."""
    def __init__(self):
        self.b = bytearray()
        self.last = 0

    def field(self, fid, ty, payload):
        d = fid - self.last
        if 0 < d <= 15:
            self.b.append(d << 4 | ty)
        else:
            self.b.append(ty)
            self.b += zz(fid)
        self.last = fid
        self.b += payload

    def i32(self, fid, v): self.field(fid, 5, zz(v))
    def i64(self, fid, v): self.field(fid, 6, zz(v))
    def bin(self, fid, bs): self.field(fid, 8, uv(len(bs)) + bs)
    def bool(self, fid, v): self.field(fid, 1 if v else 2, b'')

    def lst(self, fid, ety, items):
        n = len(items)
        hdr = bytes([n << 4 | ety]) if n < 15 else bytes([0xf0 | ety]) + uv(n)
        self.field(fid, 9, hdr + b''.join(items))

    def struct(self, fid, sub): self.field(fid, 12, sub.done())
    def done(self): return bytes(self.b) + b'\x00'


def snappy(data):
    out = bytearray(uv(len(data)))
    for i in range(0, len(data), 60):
        chunk = data[i:i + 60]
        out.append((len(chunk) - 1) << 2)
        out += chunk
    return bytes(out)


def gzip_raw(data):
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    return b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff' + body + struct.pack('<II', zlib.crc32(data) & 0xffffffff, len(data) & 0xffffffff)


def compress(codec, data):
    return data if codec == 0 else snappy(data) if codec == 1 else gzip_raw(data) if codec == 2 else b'junk' + data


def rle_runs(vals, bw):
    out = bytearray()
    i = 0
    vb = (bw + 7) // 8
    while i < len(vals):
        j = i
        while j < len(vals) and vals[j] == vals[i]:
            j += 1
        out += uv((j - i) << 1) + vals[i].to_bytes(vb, 'little')
        i = j
    return bytes(out)


def bit_packed(vals, bw):
    padded = list(vals) + [0] * (-len(vals) % 8)
    bits = 0
    for k, v in enumerate(padded):
        bits |= v << (k * bw)
    return uv((len(padded) // 8) << 1 | 1) + bits.to_bytes(len(padded) * bw // 8, 'little')


# physical types
BOOL, I32, I64, I96, FLOAT, DOUBLE, BYTES, FIXED = range(8)


def plain(ty, vals):
    out = bytearray()
    if ty == BOOL:
        bits = 0
        for k, v in enumerate(vals):
            bits |= int(v) << k
        return bits.to_bytes((len(vals) + 7) // 8, 'little')
    for v in vals:
        if ty == I32: out += struct.pack('<i', v)
        elif ty == I64: out += struct.pack('<q', v)
        elif ty == FLOAT: out += struct.pack('<f', v)
        elif ty == DOUBLE: out += struct.pack('<d', v)
        elif ty == BYTES:
            bs = v.encode() if isinstance(v, str) else v
            out += struct.pack('<I', len(bs)) + bs
    return bytes(out)


def write(path, columns, row_groups, codec=0, dictionary=(), v2=False):
    """columns: [(name, type, optional, converted_type or None)]; row_groups: a list of {name: [values, None for null]} dicts."""
    f = bytearray(b'PAR1')
    rg_structs = []
    for rg in row_groups:
        nrows = len(next(iter(rg.values())))
        chunks = []
        for name, ty, opt, conv in columns:
            vals = rg[name]
            present = [v for v in vals if v is not None]
            dict_off = 0
            use_dict = name in dictionary
            if use_dict:
                uniq = list(dict.fromkeys(present))
                dbody = plain(ty, uniq)
                dc = compress(codec, dbody)
                ph = S()
                ph.i32(1, 2); ph.i32(2, len(dbody)); ph.i32(3, len(dc))
                dh = S(); dh.i32(1, len(uniq)); dh.i32(2, 0)
                ph.struct(7, dh)
                dict_off = len(f)
                f += ph.done() + dc
                bw = max(1, (len(uniq) - 1).bit_length())
                data = bytes([bw]) + bit_packed([uniq.index(v) for v in present], bw)
                enc = 8
            else:
                data = plain(ty, present)
                enc = 0
            levels = b''
            if opt:
                lv = rle_runs([0 if v is None else 1 for v in vals], 1)
                levels = lv if v2 else struct.pack('<I', len(lv)) + lv
            data_off = len(f)
            if v2:
                comp = compress(codec, data)
                ph = S()
                ph.i32(1, 3); ph.i32(2, len(levels) + len(data)); ph.i32(3, len(levels) + len(comp))
                h2 = S(); h2.i32(1, nrows); h2.i32(2, sum(v is None for v in vals)); h2.i32(3, nrows); h2.i32(4, enc)
                h2.i32(5, len(levels)); h2.i32(6, 0); h2.bool(7, codec != 0)
                ph.struct(8, h2)
                f += ph.done() + levels + comp
            else:
                body = levels + data
                comp = compress(codec, body)
                ph = S()
                ph.i32(1, 0); ph.i32(2, len(body)); ph.i32(3, len(comp))
                h1 = S(); h1.i32(1, nrows); h1.i32(2, enc); h1.i32(3, 3); h1.i32(4, 3)
                ph.struct(5, h1)
                f += ph.done() + comp
            md = S()
            md.i32(1, ty)
            md.lst(2, 5, [zz(enc), zz(3)])
            md.lst(3, 8, [uv(len(name)) + name.encode()])
            md.i32(4, codec)
            md.i64(5, nrows)
            md.i64(6, 0)
            md.i64(7, 0)
            md.i64(9, data_off)
            if use_dict:
                md.i64(11, dict_off)
            cc = S()
            cc.i64(2, data_off)
            cc.struct(3, md)
            chunks.append(cc.done())
        g = S()
        g.lst(1, 12, chunks)
        g.i64(2, 0)
        g.i64(3, nrows)
        rg_structs.append(g.done())
    schema = []
    root = S(); root.bin(4, b'schema'); root.i32(5, len(columns)); schema.append(root.done())
    for name, ty, opt, conv in columns:
        e = S()
        e.i32(1, ty)
        e.i32(3, 1 if opt else 0)
        e.bin(4, name.encode())
        if conv is not None:
            e.i32(6, conv)
        schema.append(e.done())
    fm = S()
    fm.i32(1, 1)
    fm.lst(2, 12, schema)
    fm.i64(3, sum(len(next(iter(rg.values()))) for rg in row_groups))
    fm.lst(4, 12, rg_structs)
    fm.bin(6, b'mellagui test writer')
    foot = fm.done()
    f += foot + struct.pack('<I', len(foot)) + b'PAR1'
    with open(path, 'wb') as fh:
        fh.write(f)

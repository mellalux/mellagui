"""A small Parquet writer for the tests (there is no pyarrow here): flat schemas, plain and dictionary pages, data pages v1 / v2, uncompressed /
snappy (literal blocks only) / gzip. It is written from the format's description, so it shows that the reader understands THAT; real files from other
writers were not available."""
import ctypes
import ctypes.util
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


def _lib(name):
    try:
        return ctypes.CDLL(name)
    except OSError:
        return None


def have_codec(codec):
    return {0: True, 1: True, 2: True, 4: _lib('libbrotlienc.so.1') is not None, 5: _lib('liblz4.so.1') is not None, 6: _lib('libzstd.so.1') is not None,
            7: _lib('liblz4.so.1') is not None}.get(codec, False)


def _lz4(data):
    lib = _lib('liblz4.so.1')
    cap = len(data) + len(data) // 255 + 32
    out = ctypes.create_string_buffer(cap)
    n = lib.LZ4_compress_default(data, out, len(data), cap)
    return out.raw[:n]


def compress(codec, data):
    if codec == 0: return data
    if codec == 1: return snappy(data)
    if codec == 2: return gzip_raw(data)
    if codec == 7: return _lz4(data)
    if codec == 5:                                                      # Hadoop's framing: raw size, then (compressed size, block)
        c = _lz4(data)
        return struct.pack('>II', len(data), len(c)) + c
    if codec == 6:
        lib = _lib('libzstd.so.1')
        lib.ZSTD_compressBound.restype = ctypes.c_size_t
        cap = lib.ZSTD_compressBound(ctypes.c_size_t(len(data)))
        out = ctypes.create_string_buffer(cap)
        lib.ZSTD_compress.restype = ctypes.c_size_t
        n = lib.ZSTD_compress(out, ctypes.c_size_t(cap), data, ctypes.c_size_t(len(data)), 3)
        return out.raw[:n]
    if codec == 4:
        lib = _lib('libbrotlienc.so.1')
        cap = len(data) + 1024
        out = ctypes.create_string_buffer(cap)
        n = ctypes.c_size_t(cap)
        lib.BrotliEncoderCompress(5, 22, 0, ctypes.c_size_t(len(data)), data, ctypes.byref(n), out)
        return out.raw[:n.value]
    return b'junk' + data


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


def delta_bp(vals, block=128, mini=4):
    per = block // mini
    out = bytearray(uv(block) + uv(mini) + uv(len(vals)) + zz(vals[0] if vals else 0))
    deltas = [vals[i] - vals[i - 1] for i in range(1, len(vals))]
    for b in range(0, len(deltas), block):
        grp = deltas[b:b + block]
        md = min(grp)
        adj = [d - md for d in grp]
        widths, packs = [], []
        for m in range(mini):
            part = adj[m * per:(m + 1) * per]
            if not part:
                widths.append(0)
                continue
            w = max(part).bit_length()
            widths.append(w)
            bits = 0
            for k, v in enumerate(part + [0] * (per - len(part))):
                bits |= v << (k * w)
            packs.append(bits.to_bytes(per * w // 8, 'little'))
        out += zz(md) + bytes(widths) + b''.join(packs)
    return bytes(out)


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
        elif ty == FIXED:
            out += v
    return bytes(out)


def write(path, columns, row_groups, codec=0, dictionary=(), v2=False, encodings=None):
    """columns: [(name, type, optional, converted_type or None)]; row_groups: a list of {name: [values, None for null]} dicts."""
    f = bytearray(b'PAR1')
    rg_structs = []
    for rg in row_groups:
        nrows = len(next(iter(rg.values())))
        chunks = []
        for col in columns:
            name, ty, opt, conv = col[:4]
            extra = col[4] if len(col) > 4 else {}
            is_list = extra.get('list', False)
            vals = rg[name]
            dict_off = 0
            use_dict = name in dictionary and not is_list
            if is_list:                                                 # optional group (LIST) / repeated group list / optional element: max def 3, max rep 1
                reps, defs, present = [], [], []
                for row in vals:
                    if row is None:
                        reps.append(0); defs.append(0)
                    elif not row:
                        reps.append(0); defs.append(1)
                    else:
                        for k, v in enumerate(row):
                            reps.append(0 if k == 0 else 1)
                            if v is None:
                                defs.append(2)
                            else:
                                defs.append(3)
                                present.append(v)
                nrows_page = len(reps)
            else:
                present = [v for v in vals if v is not None]
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
                how = (encodings or {}).get(name)
                if how == 'delta':
                    data, enc = delta_bp(present), 5
                elif how == 'dlba':
                    enc = 6
                    bs = [v.encode() if isinstance(v, str) else v for v in present]
                    data = delta_bp([len(b) for b in bs]) + b''.join(bs)
                elif how == 'dba':
                    enc = 7
                    bs = [v.encode() if isinstance(v, str) else v for v in present]
                    pre, suf, prev = [], [], b''
                    for b in bs:
                        k = 0
                        while k < len(prev) and k < len(b) and prev[k] == b[k]:
                            k += 1
                        pre.append(k); suf.append(b[k:]); prev = b
                    data = delta_bp(pre) + delta_bp([len(x) for x in suf]) + b''.join(suf)
                elif how == 'bss':
                    enc = 9
                    raw = plain(ty, present)
                    wd = 4 if ty in (I32, FLOAT) else 8
                    data = bytes(raw[i * wd + k] for k in range(wd) for i in range(len(present)))
                else:
                    data, enc = plain(ty, present), 0
            levels = b''
            if is_list:
                rl, dl = rle_runs(reps, 1), rle_runs(defs, 2)
                levels = (rl + dl) if v2 else struct.pack('<I', len(rl)) + rl + struct.pack('<I', len(dl)) + dl
                rep_len, def_len = len(rl), len(dl)
                npage = len(reps)                                       # the number of VALUES in the page
                nnull = 0
            elif opt:
                lv = rle_runs([0 if v is None else 1 for v in vals], 1)
                levels = lv if v2 else struct.pack('<I', len(lv)) + lv
                rep_len, def_len = 0, len(lv)
                nnull = sum(v is None for v in vals)
            else:
                rep_len = def_len = 0
                nnull = 0
            if not is_list:
                npage = nrows
            data_off = len(f)
            if v2:
                comp = compress(codec, data)
                ph = S()
                ph.i32(1, 3); ph.i32(2, len(levels) + len(data)); ph.i32(3, len(levels) + len(comp))
                h2 = S(); h2.i32(1, npage); h2.i32(2, nnull); h2.i32(3, nrows); h2.i32(4, enc)
                h2.i32(5, def_len); h2.i32(6, rep_len); h2.bool(7, codec != 0)
                ph.struct(8, h2)
                f += ph.done() + levels + comp
            else:
                body = levels + data
                comp = compress(codec, body)
                ph = S()
                ph.i32(1, 0); ph.i32(2, len(body)); ph.i32(3, len(comp))
                h1 = S(); h1.i32(1, npage); h1.i32(2, enc); h1.i32(3, 3); h1.i32(4, 3)
                ph.struct(5, h1)
                f += ph.done() + comp
            md = S()
            md.i32(1, ty)
            md.lst(2, 5, [zz(enc), zz(3)])
            cpath = [name, 'list', 'element'] if is_list else [name]
            md.lst(3, 8, [uv(len(x)) + x.encode() for x in cpath])
            md.i32(4, codec)
            md.i64(5, npage)
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
    nel = 0
    for col in columns:
        nel += 1
    root = S(); root.bin(4, b'schema'); root.i32(5, len(columns)); schema.append(root.done())
    for col in columns:
        name, ty, opt, conv = col[:4]
        extra = col[4] if len(col) > 4 else {}
        if extra.get('list'):
            g = S(); g.i32(3, 1); g.bin(4, name.encode()); g.i32(5, 1); g.i32(6, 3); schema.append(g.done())       # optional group, converted type LIST
            g2 = S(); g2.i32(3, 2); g2.bin(4, b'list'); g2.i32(5, 1); schema.append(g2.done())                    # repeated group
            e = S(); e.i32(1, ty); e.i32(3, 1); e.bin(4, b'element'); schema.append(e.done())                     # optional element
            continue
        e = S()
        e.i32(1, ty)
        if 'tlen' in extra:
            e.i32(2, extra['tlen'])
        e.i32(3, 1 if opt else 0)
        e.bin(4, name.encode())
        if conv is not None:
            e.i32(6, conv)
        if 'scale' in extra:
            e.i32(7, extra['scale'])
            e.i32(8, extra['precision'])
        lt = None                                                       # the LogicalType union, as newer writers add it next to the converted type
        if extra.get('uuid'):
            lt = S(); lt.struct(13, S())
        elif conv == 0:
            lt = S(); lt.struct(1, S())
        elif conv == 6:
            lt = S(); lt.struct(6, S())
        elif conv in (9, 10):
            lt = S(); ts = S(); ts.bool(1, True); unit = S(); unit.struct(1 if conv == 9 else 2, S()); ts.struct(2, unit); lt.struct(8, ts)
        elif conv == 8:
            lt = S(); tm = S(); tm.bool(1, True); unit = S(); unit.struct(2, S()); tm.struct(2, unit); lt.struct(7, tm)
        if lt is not None:
            e.struct(10, lt)
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

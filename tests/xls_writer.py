"""A small writer of Excel 97-2003 workbooks (.xls: BIFF8 records inside an OLE2 compound file), written from the format's description for the tests of
the file manager's preview (no Excel, xlwt or LibreOffice here). It writes what the reader has to understand: shared strings split over CONTINUE
records the way Excel does it (a piece that continues a string's letters starts with its own flags byte), rich and phonetic strings, LABEL, NUMBER, RK,
MULRK, BOOLERR, BLANK, FORMULA with the four kinds of results (and a STRING record behind a text one), custom number formats, the 1904 date system, hidden
sheets, a stream in the mini stream (a tiny workbook), 4096-byte sectors, a DIFAT chain (a big file) and records the reader does not know.

    write(path, sheets, ...)  with  sheets = [{'name': 'A', 'hidden': False, 'cells': {(row, col): spec}}]   (row, col count from 0)

A spec is a tuple: ('s', text) shared string; ('s', text, 'rich') / ('s', text, 'ext') with formatting runs / phonetic data; ('l', text) a LABEL;
('n', float[, xf]) NUMBER; ('rk', value[, xf]) RK (an int, or a float that RK can hold); ('b', bool); ('e', code); ('blank',);
('f', float[, xf]) / ('fs', text) / ('fb', bool) / ('fe', code) a formula with that stored result.
"""
import struct

XF_GENERAL, XF_DATE, XF_PERCENT, XF_DATETIME, XF_KR = 16, 17, 18, 19, 20          # the cell formats of this writer


def rec(rid, data=b''):
    assert len(data) <= 8224
    return struct.pack('<HH', rid, len(data)) + data


def xl_string(text, short=False, force_wide=False):
    """An XLUnicodeString (u16 length) or ShortXLUnicodeString (u8 length): compressed (one byte a letter) when every letter is below 256."""
    wide = force_wide or any(ord(c) > 255 for c in text)
    body = text.encode('utf-16-le') if wide else text.encode('latin-1')
    n = len(body) // (2 if wide else 1)                      # the length counts UTF-16 units
    return (struct.pack('<B', n) if short else struct.pack('<H', n)) + bytes([1 if wide else 0]) + body


def rk_encode(v):
    if isinstance(v, int) and -(1 << 29) <= v < (1 << 29):
        return ((v << 2) | 2) & 0xffffffff
    if isinstance(v, float) and abs(v * 100 - round(v * 100)) < 1e-9 and abs(round(v * 100)) < (1 << 29):
        return ((int(round(v * 100)) << 2) | 3) & 0xffffffff
    bits = struct.unpack('<Q', struct.pack('<d', float(v)))[0]
    assert bits & 0xffffffff == 0 and bits & (3 << 32) == 0, 'not an RK float'
    return (bits >> 32) & 0xfffffffc


def sst_records(strings, limit):
    """The SST record and its CONTINUE records. `strings` = [(text, kind)] with kind '', 'rich' or 'ext'. A record holds at most `limit` bytes."""
    records = [[]]                                   # each record: a list of byte chunks
    room = [limit - 8]                               # the first record starts with cstTotal / cstUnique

    def new():
        records.append([])
        room[0] = limit

    def put_atomic(b):                               # a header or similar: never split
        if len(b) > room[0]:
            new()
        records[-1].append(b)
        room[0] -= len(b)

    def put_free(b):                                 # runs and phonetic data: split anywhere, no flags byte
        while b:
            if room[0] == 0:
                new()
            take = b[:room[0]]
            records[-1].append(take)
            room[0] -= len(take)
            b = b[len(take):]

    for text, kind in strings:
        wide = any(ord(c) > 255 for c in text)
        flags = (1 if wide else 0) | (8 if kind == 'rich' else 0) | (4 if kind == 'ext' else 0)
        runs = 2 if kind == 'rich' else 0
        ext = 14 if kind == 'ext' else 0
        nunits = len(text.encode('utf-16-le')) // 2 if wide else len(text)
        head = struct.pack('<HB', nunits, flags) + (struct.pack('<H', runs) if kind == 'rich' else b'') + (struct.pack('<I', ext) if kind == 'ext' else b'')
        put_atomic(head)
        unit = 2 if wide else 1
        data = text.encode('utf-16-le') if wide else text.encode('latin-1')
        while data:
            n = (room[0] // unit) * unit
            if n == 0:                                   # no room for a letter: the rest goes on in a CONTINUE record that starts with its flags byte
                new()
                records[-1].append(bytes([1 if wide else 0]))
                room[0] -= 1
                n = (room[0] // unit) * unit
            take = data[:n]
            records[-1].append(take)
            room[0] -= len(take)
            data = data[len(take):]
        if kind == 'rich':
            put_free(b'\x01\x00\x00\x00' * runs)
        if kind == 'ext':
            put_free(bytes(range(ext)))
    out = []
    for i, chunks in enumerate(records):
        body = b''.join(chunks)
        if i == 0:
            body = struct.pack('<II', len(strings), len(strings)) + body
        out.append(rec(0x00FC if i == 0 else 0x003C, body))
    return b''.join(out)


def cell_records(row, col, spec, sst_index, state):
    kind = spec[0]
    xf = spec[2] if len(spec) > 2 and isinstance(spec[2], int) else XF_GENERAL
    if kind == 's':
        return rec(0x00FD, struct.pack('<HHHI', row, col, xf, sst_index[(spec[1], spec[2] if len(spec) > 2 else '')]))
    if kind == 'l':
        return rec(0x0204, struct.pack('<HHH', row, col, xf) + xl_string(spec[1]))
    if kind == 'n':
        return rec(0x0203, struct.pack('<HHHd', row, col, xf, spec[1]))
    if kind == 'rk':
        return rec(0x027E, struct.pack('<HHHI', row, col, xf, rk_encode(spec[1])))
    if kind == 'b':
        return rec(0x0205, struct.pack('<HHHBB', row, col, xf, 1 if spec[1] else 0, 0))
    if kind == 'e':
        return rec(0x0205, struct.pack('<HHHBB', row, col, xf, spec[1], 1))
    if kind == 'blank':
        return rec(0x0201, struct.pack('<HHH', row, col, XF_GENERAL))
    if kind in ('f', 'fs', 'fb', 'fe'):
        if kind == 'f':
            res = struct.pack('<d', spec[1])
        elif kind == 'fs':
            res = b'\x00\x00\x00\x00\x00\x00\xff\xff'
        elif kind == 'fb':
            res = bytes([1, 0, 1 if spec[1] else 0, 0, 0, 0, 0xff, 0xff])
        else:
            res = bytes([2, 0, spec[1], 0, 0, 0, 0xff, 0xff])
        formula = rec(0x0006, struct.pack('<HHH', row, col, xf) + res + struct.pack('<HIH', 0, 0, 3) + b'\x1e\x01\x00')
        if kind == 'fs':
            formula += rec(0x04BC, b'\x00' * 12) + rec(0x0207, xl_string(spec[1]))          # a shared formula record may sit before the STRING
        return formula
    raise ValueError(kind)


def sheet_stream(cells, sst_index, extra_noise=True):
    out = [rec(0x0809, struct.pack('<HHHHII', 0x0600, 0x0010, 0x0DBB, 0x07CC, 0, 6))]
    byrow = {}
    for (r, c) in cells:
        byrow.setdefault(r, []).append(c)
    rows = sorted(byrow)
    maxrow = (max(rows) + 1) if rows else 0
    maxcol = (max(c for _, c in cells) + 1) if cells else 0
    out.append(rec(0x0200, struct.pack('<IIHHH', rows[0] if rows else 0, maxrow, 0, maxcol, 0)))
    if extra_noise:
        out.append(rec(0x0233, b'\x01\x02\x03'))                                     # a record this reader does not know
    for r in rows:
        out.append(rec(0x0208, struct.pack('<HHHHHHHI', r, 0, maxcol, 0xFF, 0, 0, 0, 0x0F)))
        cols = sorted(byrow[r])
        i = 0
        while i < len(cols):
            c = cols[i]
            spec = cells[(r, c)]
            run = [c]                                                                # consecutive RK numbers go into one MULRK record
            while spec[0] == 'rk' and i + len(run) < len(cols) and cols[i + len(run)] == run[-1] + 1 and cells[(r, cols[i + len(run)])][0] == 'rk':
                run.append(cols[i + len(run)])
            if len(run) > 1:
                body = struct.pack('<HH', r, run[0])
                for cc in run:
                    s2 = cells[(r, cc)]
                    body += struct.pack('<HI', s2[2] if len(s2) > 2 else XF_GENERAL, rk_encode(s2[1]))
                out.append(rec(0x00BD, body + struct.pack('<H', run[-1])))
                i += len(run)
                continue
            out.append(cell_records(r, c, spec, sst_index, None))
            i += 1
    out.append(rec(0x000A))
    return b''.join(out)


def workbook_stream(sheets, date1904=False, sst_limit=8224, encrypted=False, vers=0x0600):
    sst_keys = []
    for sh in sheets:
        for key in sorted(sh['cells']):
            spec = sh['cells'][key]
            if spec[0] == 's':
                k = (spec[1], spec[2] if len(spec) > 2 else '')
                if k not in sst_keys:
                    sst_keys.append(k)
    sst_index = {k: i for i, k in enumerate(sst_keys)}
    custom = [(164, 'yyyy\\-mm\\-dd\\ hh:mm'), (165, '0.00" kr"')]
    xfs = [0] * 16 + [0, 14, 10, 164, 165]                                          # 16 style XFs, then the cell XFs of this writer
    head = [rec(0x0809, struct.pack('<HHHHII', vers, 0x0005, 0x0DBB, 0x07CC, 0, 6))]
    if encrypted:
        head.append(rec(0x002F, b'\x00\x00\x01\x00\x01\x00' + bytes(48)))
    head.append(rec(0x0022, struct.pack('<H', 1 if date1904 else 0)))
    for fid, code in custom:
        head.append(rec(0x041E, struct.pack('<H', fid) + xl_string(code)))
    for ifmt in xfs:
        head.append(rec(0x00E0, struct.pack('<HH', 0, ifmt) + bytes(16)))
    bs_len = sum(len(rec(0x0085, bytes(6) + xl_string(sh['name'], short=True))) for sh in sheets)
    tail = [sst_records([k for k in sst_keys], sst_limit), rec(0x000A)] if sst_keys else [rec(0x000A)]
    glen = sum(len(x) for x in head) + bs_len + sum(len(x) for x in tail)
    sheet_data = [sheet_stream(sh['cells'], sst_index) for sh in sheets]
    pos = glen
    bounds = []
    for sh, data in zip(sheets, sheet_data):
        bounds.append(rec(0x0085, struct.pack('<IBB', pos, 1 if sh.get('hidden') else 0, 0) + xl_string(sh['name'], short=True)))
        pos += len(data)
    return b''.join(head) + b''.join(bounds) + b''.join(tail) + b''.join(sheet_data)


def ole2(streams, shift=9, cutoff=4096):
    """An OLE2 compound file holding the named streams (a dict name -> bytes), the smaller ones in the mini stream."""
    ssz = 1 << shift
    per = ssz // 4
    NOSTREAM, END, FATSEC, DIFSEC, FREE = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFC, 0xFFFFFFFF
    names = list(streams)
    big = [n for n in names if len(streams[n]) >= cutoff]
    small = [n for n in names if len(streams[n]) < cutoff]
    # the mini stream: every small stream in 64-byte sectors
    mini = bytearray()
    minifat = []
    start_of = {}
    for n in small:
        data = streams[n]
        k = (len(data) + 63) // 64
        start_of[n] = len(mini) // 64 if k else END
        for i in range(k):
            minifat.append(len(mini) // 64 + 1 if i < k - 1 else END)
            mini += data[i * 64:(i + 1) * 64].ljust(64, b'\0')
    sectors = []                                                                    # the data sectors in order: (kind, payload)
    chains = {}

    def add_stream(data):
        first = len(sectors)
        k = (len(data) + ssz - 1) // ssz
        for i in range(k):
            sectors.append(data[i * ssz:(i + 1) * ssz].ljust(ssz, b'\0'))
        return first, k
    fat = []

    def chain(first, k):
        for i in range(k):
            fat.append(first + i + 1 if i < k - 1 else END)
    for n in big:
        first, k = add_stream(streams[n])
        start_of[n] = first
        chain(first, k)
    mfirst, mk = (add_stream(bytes(mini)) if mini else (END, 0))
    chain(mfirst, mk) if mk else None
    mf_first, mf_k = (END, 0)
    if minifat:
        mf_first, mf_k = add_stream(b''.join(struct.pack('<I', v) for v in minifat))
        chain(mf_first, mf_k)
    # the directory
    def entry(name, typ, left, right, child, start, size):
        nm = (name + '\0').encode('utf-16-le')
        return (nm.ljust(64, b'\0') + struct.pack('<HBB', len(nm), typ, 1) + struct.pack('<III', left, right, child) + bytes(16) + bytes(4) + bytes(16)
                + struct.pack('<IQ', start, size))
    ents = [entry('Root Entry', 5, NOSTREAM, NOSTREAM, 1 if names else NOSTREAM, mfirst if mk else END, len(mini))]
    for i, n in enumerate(names):
        ents.append(entry(n, 2, NOSTREAM, i + 2 if i + 1 < len(names) else NOSTREAM, NOSTREAM, start_of[n], len(streams[n])))
    d = b''.join(ents)
    dfirst, dk = add_stream(d.ljust(((len(d) + ssz - 1) // ssz) * ssz, b'\0'))
    chain(dfirst, dk)
    # the FAT (and the DIFAT sectors that hold the FAT's own numbers when there are over 109 of them)
    nfat = 1
    while True:
        total = len(sectors) + nfat
        ndif = max(0, -(-(nfat - 109) // (per - 1))) if nfat > 109 else 0
        if (total + ndif) <= nfat * per:
            break
        nfat += 1
    ndif = max(0, -(-(nfat - 109) // (per - 1))) if nfat > 109 else 0
    fat_first = len(sectors)
    full = fat + [FATSEC] * nfat + [DIFSEC] * ndif
    full += [FREE] * (nfat * per - len(full))
    for i in range(nfat):
        sectors.append(b''.join(struct.pack('<I', v) for v in full[i * per:(i + 1) * per]))
    fat_ids = [fat_first + i for i in range(nfat)]
    dif_first = len(sectors) if ndif else END
    rest = fat_ids[109:]
    for i in range(ndif):
        part = rest[i * (per - 1):(i + 1) * (per - 1)]
        part += [FREE] * (per - 1 - len(part))
        nxt = len(sectors) + 1 if i < ndif - 1 else END
        sectors.append(b''.join(struct.pack('<I', v) for v in part) + struct.pack('<I', nxt))
    header = b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1' + bytes(16) + struct.pack('<HHHHH', 0x3E, 3 if shift == 9 else 4, 0xFFFE, shift, 6) + bytes(6)
    header += struct.pack('<IIIIIIIII', dk if shift == 12 else 0, nfat, dfirst, 0, cutoff, mf_first, mf_k, dif_first, ndif)
    header += b''.join(struct.pack('<I', v) for v in (fat_ids[:109] + [FREE] * (109 - len(fat_ids[:109]))))
    return header.ljust(ssz, b'\0') + b''.join(sectors)


def write(path, sheets, stream_name='Workbook', shift=9, **kw):
    with open(path, 'wb') as f:
        f.write(ole2({stream_name: workbook_stream(sheets, **kw)}, shift=shift))

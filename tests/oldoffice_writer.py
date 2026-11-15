"""Writers of tiny Word 97 (.doc) and PowerPoint 97 (.ppt) files for the tests of the file manager's preview, built from the format descriptions (MS-DOC, MS-PPT):
the streams go into an OLE2 container made by xls_writer.ole2. A .doc has a WordDocument stream (FIB, then the text) and a table stream with the CLX (the piece
table); a .ppt has a PowerPoint Document stream with a DocumentContainer holding a SlideListWithText. Only what the reader looks at is written."""
import struct
import xls_writer


def fib(ccp_text, fc_clx, lcb_clx, table1=True, nfib=0x00c1, lid=0x0409):
    base = struct.pack('<HHHHHH', 0xa5ec, nfib, 0x0001, lid, 0, 0x0200 if table1 else 0) + bytes(20)         # wIdent nFib nProduct lid pnNext flags ... (32 bytes)
    base = base[:10] + struct.pack('<H', 0x0200 if table1 else 0) + base[12:32]
    csw = struct.pack('<H', 14) + bytes(28)
    lw = [0] * 22
    lw[3] = ccp_text
    cslw = struct.pack('<H', 22) + struct.pack('<22I', *lw)
    pairs = [(0, 0)] * 93
    pairs[33] = (fc_clx, lcb_clx)
    fclcb = struct.pack('<H', 93) + b''.join(struct.pack('<II', a, b) for a, b in pairs)
    return base + csw + cslw + fclcb


def doc(path, pieces, table1=True):
    """pieces: [(text, wide)] -- the text of each piece (wide = UTF-16, else cp1252 bytes)."""
    head_len = len(fib(0, 0, 0)) + 16
    text = b''
    pcds = b''
    cps = [0]
    for t, wide in pieces:
        data = t.encode('utf-16-le') if wide else t.encode('cp1252')
        off = head_len + len(text)
        fc = off if wide else (off * 2) | 0x40000000
        pcds += struct.pack('<HIH', 0, fc, 0)
        text += data
        cps.append(cps[-1] + len(t))
    plc = b''.join(struct.pack('<I', c) for c in cps) + pcds
    clx = b'\x01' + struct.pack('<H', 2) + b'\x00\x00' + b'\x02' + struct.pack('<I', len(plc)) + plc
    word = fib(cps[-1], 0, len(clx), table1) .ljust(head_len, b'\0') + text
    streams = {'WordDocument': word, '1Table' if table1 else '0Table': clx.ljust(4096, b'\0')}
    open(path, 'wb').write(xls_writer.ole2(streams))


def rec(ver, inst, typ, body):
    return struct.pack('<HHI', ver | (inst << 4), typ, len(body)) + body


def ppt(path, slides):
    """slides: [[(text_type, text, wide)]]"""
    slwt = b''
    for i, texts in enumerate(slides):
        slwt += rec(0, 0, 0x03f3, struct.pack('<IIII', 256 + i, 0, i + 1, 0))
        for tt, text, wide in texts:
            slwt += rec(0, tt, 0x0f9f, struct.pack('<I', tt))
            slwt += rec(0, 0, 0x0fa0, text.encode('utf-16-le')) if wide else rec(0, 0, 0x0fa8, text.encode('cp1252'))
    old_doc = rec(0xf, 0, 0x03e8, rec(0xf, 0, 0x0ff0, rec(0, 0, 0x03f3, bytes(16)) + rec(0, 0, 0x0fa8, b'OLD VERSION')))       # an older edit of the document stays in the stream
    doc = rec(0xf, 0, 0x03e8, rec(0xf, 0, 0x0ff0, slwt) + rec(0xf, 2, 0x0ff0, rec(0, 0, 0x03f3, bytes(16)) + rec(0, 0, 0x0fa8, b'notes are left out')))
    open(path, 'wb').write(xls_writer.ole2({'PowerPoint Document': old_doc + doc, 'Current User': b'\0' * 32}))

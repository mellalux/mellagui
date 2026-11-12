#!/usr/bin/env python3
"""A stand-in for one Smart Life / Tuya device in the local network (tests of mellagui-smart's local commands).

    fake_tuya_device.py VERSION KEY LOG [MODE]     VERSION 3.3 or 3.4, KEY the 16 character local key; prints "port N"

Speaks the device side of the TCP protocol with Python's own crypto: 3.3 = AES-ECB commands with a CRC32, 3.4 = the session key
negotiation and HMAC-SHA256 on every frame. Every command's values are written to LOG as a JSON line ({"dps": {...}}).
MODE: ok (default), silent (takes the command, never confirms), refuse (answers with an error), deaf (accepts the connection and says nothing),
old22 (a 3.3 device that does not know the question for its values: it reports those it is told to set to nothing).
It also says its values when asked (3.3: command 10, 3.4: command 16): $FAKE_DPS (JSON by number, default {"1": false}) changed by the commands.
"""
import binascii, hashlib, hmac, json, os, socket, struct, sys, threading
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

VERSION, KEY, LOG = sys.argv[1], sys.argv[2].encode(), sys.argv[3]
MODE = sys.argv[4] if len(sys.argv) > 4 else 'ok'
DPS = json.loads(os.environ.get('FAKE_DPS', '{"1": false}'))


def ecb(key, data, enc):
    c = Cipher(algorithms.AES(key), modes.ECB())
    o = c.encryptor() if enc else c.decryptor()
    return o.update(data) + o.finalize()


def pad(b):
    n = 16 - len(b) % 16
    return b + bytes([n]) * n


def unpad(b):
    return b[:-b[-1]]


def log(obj):
    with open(LOG, 'a') as f:
        f.write(json.dumps(obj, sort_keys=True) + '\n')


def frame(seq, cmd, payload, hkey):
    head = struct.pack('>4I', 0x55aa, seq, cmd, len(payload) + (36 if hkey else 8)) + payload
    tail = hmac.new(hkey, head, hashlib.sha256).digest() if hkey else struct.pack('>I', binascii.crc32(head) & 0xffffffff)
    return head + tail + struct.pack('>I', 0xaa55)


def read_frame(conn):
    buf = b''
    while len(buf) < 16:
        d = conn.recv(16 - len(buf))
        if not d:
            return None
        buf += d
    prefix, seq, cmd, length = struct.unpack('>4I', buf)
    while len(buf) < 16 + length:
        d = conn.recv(16 + length - len(buf))
        if not d:
            return None
        buf += d
    return seq, cmd, buf


def serve(conn):
    key = KEY                                       # the session key after the negotiation (3.4)
    mine = os.urandom(16)
    theirs = None
    try:
        while True:
            f = read_frame(conn)
            if not f:
                return
            seq, cmd, raw = f
            if MODE == 'deaf':
                continue
            if VERSION == '3.3':
                payload = raw[16:-8]
                if struct.unpack('>I', raw[-8:-4])[0] != binascii.crc32(raw[:-8]) & 0xffffffff:
                    log({'error': 'crc'})
                    return
                if cmd == 10 and MODE != 'old22':               # the question for the values: no version header, the answer has a return code
                    try:
                        q = json.loads(unpad(ecb(KEY, payload, False)))
                        assert q['gwId'] and q['devId']
                    except Exception as e:
                        log({'error': 'question %r' % e})
                        return
                    log({'asked': True})
                    conn.sendall(frame(seq, 10, struct.pack('>I', 0) + ecb(KEY, pad(json.dumps({'devId': q['devId'], 'dps': DPS}).encode()), True), None))
                    continue
                if cmd == 13 and MODE == 'old22':               # "set these to nothing": confirmed, then reported like a change
                    q = json.loads(unpad(ecb(KEY, payload[15:], False)))
                    log({'asked': sorted(q['dps'])})
                    conn.sendall(frame(seq, 13, struct.pack('>I', 0), None))
                    said = {k: v for k, v in DPS.items() if k in q['dps']}
                    conn.sendall(frame(0, 8, struct.pack('>I', 0) + b'3.3' + bytes(12) + ecb(KEY, pad(json.dumps({'devId': q['devId'], 'dps': said}).encode()), True), None))
                    continue
                try:
                    if cmd != 7 or payload[:3] != b'3.3':
                        raise ValueError('no header')
                    data = json.loads(unpad(ecb(KEY, payload[15:], False)))
                    assert data['devId'] and isinstance(data['t'], str)
                except Exception as e:
                    log({'error': 'payload %r' % e})
                    conn.sendall(frame(seq, cmd, struct.pack('>I', 1) + b'json obj data unvalid', None))
                    continue
                log({'dps': data['dps']})
                if MODE not in ('refuse', 'silent'):
                    DPS.update(data['dps'])
                if MODE == 'refuse':
                    conn.sendall(frame(seq, cmd, struct.pack('>I', 1) + b'dp not allowed', None))
                elif MODE != 'silent':
                    conn.sendall(frame(seq, cmd, struct.pack('>I', 0), None))
                continue
            # ---- 3.4
            payload, mac = raw[16:-36], raw[-36:-4]
            if not hmac.compare_digest(mac, hmac.new(key, raw[:-36], hashlib.sha256).digest()):
                log({'error': 'hmac'})
                return                              # (a real device just drops the connection)
            if cmd == 3:
                theirs = unpad(ecb(KEY, payload, False))
                answer = mine + hmac.new(KEY, theirs, hashlib.sha256).digest()
                conn.sendall(frame(seq, 4, struct.pack('>I', 0) + ecb(KEY, pad(answer), True), KEY))
            elif cmd == 5:
                proof = unpad(ecb(KEY, payload, False))
                if proof != hmac.new(KEY, mine, hashlib.sha256).digest():
                    log({'error': 'proof'})
                    return
                key = ecb(KEY, bytes(a ^ b for a, b in zip(theirs, mine)), True)
                log({'session': True})
            elif cmd == 13:
                plain = unpad(ecb(key, payload, False))
                if plain[:3] != b'3.4':
                    log({'error': 'no header'})
                    return
                data = json.loads(plain[15:])
                assert data['protocol'] == 5 and isinstance(data['t'], int)
                log({'dps': data['data']['dps']})
                if MODE not in ('refuse', 'silent'):
                    DPS.update(data['data']['dps'])
                if MODE == 'refuse':
                    conn.sendall(frame(seq, cmd, struct.pack('>I', 1) + ecb(key, pad(b''), True), key))
                elif MODE != 'silent':
                    conn.sendall(frame(seq, cmd, struct.pack('>I', 0) + ecb(key, pad(b''), True), key))
            elif cmd == 16:                                     # the question for the values
                q = json.loads(unpad(ecb(key, payload, False)))
                assert q['devId']
                log({'asked': True})
                conn.sendall(frame(seq, 16, struct.pack('>I', 0) + ecb(key, pad(json.dumps({'dps': DPS}).encode()), True), key))
    except (OSError, ValueError, AssertionError) as e:
        log({'error': repr(e)})
    finally:
        conn.close()


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 0))
srv.listen(8)
print('port %d' % srv.getsockname()[1], flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()

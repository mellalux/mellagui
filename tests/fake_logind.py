"""A fake systemd-logind on a private D-Bus socket: just enough of the protocol (EXTERNAL auth, fd passing, Hello,
Manager.GetSession, Session.TakeControl / TakeDevice / ReleaseDevice / PauseDeviceComplete / ReleaseControl, Seat.SwitchTo,
and the PauseDevice / ResumeDevice signals) for src/logind.c. Every call is recorded in .events."""
import array
import os
import socket
import struct
import tempfile
import threading
import time


def _pad(b, a):
    return b + b'\0' * (-len(b) % a)


def _field(code, t, v):
    f = bytes([code, 1]) + t.encode() + b'\0'
    if t in 'os':
        f = _pad(f, 4) + struct.pack('<I', len(v.encode())) + v.encode() + b'\0'
    elif t == 'g':
        f = f[:0] + bytes([code, 1]) + b'g\0' + bytes([len(v)]) + v.encode() + b'\0'
    elif t == 'u':
        f = _pad(f, 4) + struct.pack('<I', v)
    return f


def build(mtype, flags, serial, fields, body=b''):
    f = b''
    for code, t, v in fields:
        f = _pad(f, 8) + _field(code, t, v)
    head = b'l' + bytes([mtype, flags, 1]) + struct.pack('<III', len(body), serial, len(f)) + f
    return _pad(head, 8) + body


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def align(self, a):
        self.p += -self.p % a

    def u32(self):
        self.align(4)
        v = struct.unpack_from('<I', self.b, self.p)[0]
        self.p += 4
        return v

    def string(self):
        n = self.u32()
        s = self.b[self.p:self.p + n].decode()
        self.p += n + 1
        return s

    def sig(self):
        n = self.b[self.p]
        s = self.b[self.p + 1:self.p + 1 + n].decode()
        self.p += n + 2
        return s


class FakeLogind:
    SESSION = '/org/freedesktop/login1/session/_37'

    def __init__(self, path=None, *, get_session_error=False, take_control_error=False, inactive=False):
        self.dir = tempfile.mkdtemp(prefix='fake-logind-')
        self.path = path or os.path.join(self.dir, 'bus')
        self.get_session_error, self.take_control_error, self.inactive = get_session_error, take_control_error, inactive
        self.events = []
        self.cond = threading.Condition()
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_UNIX)
        self.srv.bind(self.path)
        self.srv.listen(1)
        self.conn = None
        self.serial = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.files = []

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        for s in (self.conn, self.srv):
            try:
                s.close()
            except Exception:
                pass
        for f in self.files:
            f.close()

    def record(self, *ev):
        with self.cond:
            self.events.append(ev)
            self.cond.notify_all()

    def wait(self, ev, timeout=5.0):
        end = time.time() + timeout
        with self.cond:
            while ev not in self.events:
                left = end - time.time()
                if left <= 0:
                    return False
                self.cond.wait(left)
        return True

    # ---- wire ---------------------------------------------------------------------------------------------------
    def send(self, data, fds=()):
        with self.lock:
            if fds:
                self.conn.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))])
            else:
                self.conn.sendall(data)

    def next_serial(self):
        self.serial += 1
        return self.serial

    def reply(self, call, sig='', body=b'', fds=()):
        fields = [(5, 'u', call['serial']), (6, 's', ':1.7')]
        if sig:
            fields.append((8, 'g', sig))
        if fds:
            fields.append((9, 'u', len(fds)))
        self.send(build(2, 1, self.next_serial(), fields, body), fds)

    def error(self, call, name, text):
        body = _pad(b'', 4) + struct.pack('<I', len(text)) + text.encode() + b'\0'
        self.send(build(3, 1, self.next_serial(), [(4, 's', name), (5, 'u', call['serial']), (6, 's', ':1.7'), (8, 'g', 's')], body))

    def signal(self, member, sig, body, fds=()):
        fields = [(1, 'o', self.SESSION), (2, 's', 'org.freedesktop.login1.Session'), (3, 's', member), (8, 'g', sig)]
        if fds:
            fields.append((9, 'u', len(fds)))
        self.send(build(4, 1, self.next_serial(), fields, body), fds)

    def emit_pause(self, major, minor, typ):
        t = typ.encode()
        self.signal('PauseDevice', 'uus', struct.pack('<III', major, minor, len(t)) + t + b'\0')

    def emit_resume(self, major, minor, content=b'resumed'):
        f = tempfile.TemporaryFile()
        f.write(content)
        f.flush()
        f.seek(0)
        self.files.append(f)
        self.signal('ResumeDevice', 'uuh', struct.pack('<III', major, minor, 0), (f.fileno(),))

    # ---- server -------------------------------------------------------------------------------------------------
    def parse(self, data):
        mtype, flags = data[1], data[2]
        blen, serial, flen = struct.unpack_from('<III', data, 4)
        r = Reader(data[:16 + flen])
        r.p = 16
        call = {'type': mtype, 'flags': flags, 'serial': serial}
        names = {1: 'path', 2: 'iface', 3: 'member', 6: 'dest'}
        while r.p < 16 + flen:
            r.align(8)
            if r.p >= 16 + flen:
                break
            code = data[r.p]
            r.p += 1
            sg = r.sig()
            if sg in ('o', 's'):
                v = r.string()
            elif sg == 'g':
                v = r.sig()
            else:
                v = r.u32()
            if code in names:
                call[names[code]] = v
            elif code == 8:
                call['sig'] = v
        body_off = 16 + flen + (-(16 + flen) % 8)
        call['body'] = data[body_off:body_off + blen]
        return call

    def run(self):
        try:
            self.conn, _ = self.srv.accept()
            buf = b''
            while b'BEGIN\r\n' not in buf:
                d = self.conn.recv(4096)
                if not d:
                    return
                buf += d
                if b'\r\n' in buf and b'AUTH EXTERNAL' in buf and not getattr(self, 'authed', False):
                    self.authed = True
                    self.conn.sendall(b'OK 0123456789abcdef0123456789abcdef\r\n')
                if b'NEGOTIATE_UNIX_FD' in buf and not getattr(self, 'negotiated', False):
                    self.negotiated = True
                    self.conn.sendall(b'AGREE_UNIX_FD\r\n')
            buf = buf.split(b'BEGIN\r\n', 1)[1]
            while True:
                while len(buf) >= 16:
                    blen, _, flen = struct.unpack_from('<III', buf, 4)
                    total = 16 + flen + (-(16 + flen) % 8) + blen
                    if len(buf) < total:
                        break
                    self.handle(self.parse(buf[:total]))
                    buf = buf[total:]
                d = self.conn.recv(65536)
                if not d:
                    return
                buf += d
        except OSError:
            pass

    def handle(self, c):
        m = c.get('member')
        if m == 'Hello':
            s = b':1.7'
            return self.reply(c, 's', struct.pack('<I', len(s)) + s + b'\0')
        r = Reader(c['body'])
        if m in ('GetSession', 'GetSessionByPID'):
            arg = r.string() if m == 'GetSession' else r.u32()
            self.record(m, arg)
            if self.get_session_error:
                return self.error(c, 'org.freedesktop.login1.NoSessionForPID', 'no session')
            p = self.SESSION.encode()
            return self.reply(c, 'o', struct.pack('<I', len(p)) + p + b'\0')
        if m == 'SwitchTo':
            if c.get('path') != '/org/freedesktop/login1/seat/seat0':
                return self.error(c, 'org.freedesktop.DBus.Error.UnknownObject', 'Unknown object ' + c.get('path', ''))
        elif c.get('path') != self.SESSION:
            return self.error(c, 'org.freedesktop.DBus.Error.UnknownObject', 'no such object')
        if m == 'Get':                                           # Properties.Get(session iface, "Seat") -> v (so)
            iface, prop = r.string(), r.string()
            self.record('Get', iface, prop)
            if prop != 'Seat':
                return self.error(c, 'org.freedesktop.DBus.Error.UnknownProperty', prop)
            v = b'\x04(so)\0'
            v = _pad(v, 8) + struct.pack('<I', 5) + b'seat0\0'
            sp = b'/org/freedesktop/login1/seat/seat0'
            v = _pad(v, 4) + struct.pack('<I', len(sp)) + sp + b'\0'
            return self.reply(c, 'v', v)
        if m == 'TakeControl':
            self.record('TakeControl', r.u32())
            if self.take_control_error:
                return self.error(c, 'org.freedesktop.DBus.Error.AccessDenied', 'busy')
            return self.reply(c)
        if m == 'TakeDevice':
            major, minor = r.u32(), r.u32()
            self.record('TakeDevice', major, minor)
            path = os.path.join(self.dir, 'dev-%d-%d' % (major, minor))
            f = open(path, 'w+b')
            self.files.append(f)
            self.devfiles = getattr(self, 'devfiles', {})
            self.devfiles[(major, minor)] = path
            return self.reply(c, 'hb', struct.pack('<II', 0, 1 if self.inactive else 0), (f.fileno(),))
        if m in ('ReleaseDevice', 'PauseDeviceComplete'):
            self.record(m, r.u32(), r.u32())
        elif m == 'SwitchTo':
            self.record('SwitchTo', r.u32())
        elif m == 'ReleaseControl':
            self.record('ReleaseControl')
        else:
            return self.error(c, 'org.freedesktop.DBus.Error.UnknownMethod', 'no ' + str(m))
        if not c['flags'] & 1:
            self.reply(c)

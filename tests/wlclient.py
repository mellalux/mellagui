"""Minimal raw-socket Wayland client library used by the MellaGUI test-suite.

No libwayland: the wire format is implemented here, independently of the
compositor's own wire.c, so protocol bugs on either side show up as test failures.
"""
import mmap
import os
import select
import socket
import struct
import time

# Event signatures: interface -> {opcode: (name, signature)}
# signature letters: i int, u uint, f fixed, s string, o object, n new_id, a array, h fd
EVENTS = {
    'wl_display': {0: ('error', 'ous'), 1: ('delete_id', 'u')},
    'wl_registry': {0: ('global', 'usu'), 1: ('global_remove', 'u')},
    'wl_callback': {0: ('done', 'u')},
    'wl_shm': {0: ('format', 'u')},
    'wl_buffer': {0: ('release', '')},
    'wl_surface': {0: ('enter', 'o'), 1: ('leave', 'o')},
    'wl_seat': {0: ('capabilities', 'u'), 1: ('name', 's')},
    'wl_pointer': {0: ('enter', 'uoff'), 1: ('leave', 'uo'), 2: ('motion', 'uff'),
                   3: ('button', 'uuuu'), 4: ('axis', 'uuf'), 5: ('frame', ''),
                   6: ('axis_source', 'u'), 7: ('axis_stop', 'uu'), 8: ('axis_discrete', 'ui')},
    'wl_keyboard': {0: ('keymap', 'uhu'), 1: ('enter', 'uoa'), 2: ('leave', 'uo'),
                    3: ('key', 'uuuu'), 4: ('modifiers', 'uuuuu'), 5: ('repeat_info', 'ii')},
    'wl_output': {0: ('geometry', 'iiiiissi'), 1: ('mode', 'uiii'), 2: ('done', ''), 3: ('scale', 'i')},
    'xdg_wm_base': {0: ('ping', 'u')},
    'xdg_surface': {0: ('configure', 'u')},
    'xdg_toplevel': {0: ('configure', 'iia'), 1: ('close', ''), 2: ('configure_bounds', 'ii'),
                     3: ('wm_capabilities', 'a')},
    'xdg_popup': {0: ('configure', 'iiii'), 1: ('popup_done', ''), 2: ('repositioned', 'u')},
    'zxdg_toplevel_decoration_v1': {0: ('configure', 'u')},
    'wl_data_device': {0: ('data_offer', 'n'), 1: ('enter', 'uoffo'), 2: ('leave', ''),
                       3: ('motion', 'uff'), 4: ('drop', ''), 5: ('selection', 'o')},
    'wl_data_offer': {0: ('offer', 's'), 1: ('source_actions', 'u'), 2: ('action', 'u')},
    'mellagui_settings_v1': {0: ('value', 'ss'), 1: ('done', '')},
    'zwp_idle_inhibit_manager_v1': {},
    'zwp_idle_inhibitor_v1': {},
    'mellagui_tray_v1': {},
    'mellagui_tray_item_v1': {0: ('size', 'i'), 1: ('clicked', 'u')},
    'mellagui_capture_v1': {0: ('output', 'isiiii'), 1: ('done', ''), 2: ('ready', 'iii'), 3: ('failed', 'is')},
    'zwp_linux_dmabuf_v1': {0: ('format', 'u'), 1: ('modifier', 'uuu')},
    'zwp_linux_buffer_params_v1': {0: ('created', 'n'), 1: ('failed', '')},
    'zwp_linux_dmabuf_feedback_v1': {0: ('done', ''), 1: ('format_table', 'hu'), 2: ('main_device', 'a'), 3: ('tranche_done', ''),
                                     4: ('tranche_target_device', 'a'), 5: ('tranche_formats', 'a'), 6: ('tranche_flags', 'u')},
    'wl_data_source': {0: ('target', 's'), 1: ('send', 'sh'), 2: ('cancelled', ''),
                       3: ('dnd_drop_performed', ''), 4: ('dnd_finished', ''), 5: ('action', 'u')},
}

# Request opcodes by interface for the requests the tests use.
REQ = {
    'wl_display': {'sync': 0, 'get_registry': 1},
    'wl_registry': {'bind': 0},
    'wl_compositor': {'create_surface': 0, 'create_region': 1},
    'wl_shm': {'create_pool': 0},
    'wl_shm_pool': {'create_buffer': 0, 'destroy': 1, 'resize': 2},
    'wl_buffer': {'destroy': 0},
    'wl_surface': {'destroy': 0, 'attach': 1, 'damage': 2, 'frame': 3, 'set_opaque_region': 4,
                   'set_input_region': 5, 'commit': 6, 'set_buffer_transform': 7,
                   'set_buffer_scale': 8, 'damage_buffer': 9},
    'wl_region': {'destroy': 0, 'add': 1, 'subtract': 2},
    'wl_subcompositor': {'destroy': 0, 'get_subsurface': 1},
    'wl_subsurface': {'destroy': 0, 'set_position': 1, 'place_above': 2, 'place_below': 3,
                      'set_sync': 4, 'set_desync': 5},
    'xdg_wm_base': {'destroy': 0, 'create_positioner': 1, 'get_xdg_surface': 2, 'pong': 3},
    'xdg_positioner': {'destroy': 0, 'set_size': 1, 'set_anchor_rect': 2, 'set_anchor': 3,
                       'set_gravity': 4, 'set_constraint_adjustment': 5, 'set_offset': 6},
    'xdg_surface': {'destroy': 0, 'get_toplevel': 1, 'get_popup': 2, 'set_window_geometry': 3,
                    'ack_configure': 4},
    'xdg_toplevel': {'destroy': 0, 'set_parent': 1, 'set_title': 2, 'set_app_id': 3,
                     'show_window_menu': 4, 'move': 5, 'resize': 6, 'set_max_size': 7,
                     'set_min_size': 8, 'set_maximized': 9, 'unset_maximized': 10,
                     'set_fullscreen': 11, 'unset_fullscreen': 12, 'set_minimized': 13},
    'xdg_popup': {'destroy': 0, 'grab': 1},
    'zxdg_decoration_manager_v1': {'destroy': 0, 'get_toplevel_decoration': 1},
    'zxdg_toplevel_decoration_v1': {'destroy': 0, 'set_mode': 1, 'unset_mode': 2},
    'wl_seat': {'get_pointer': 0, 'get_keyboard': 1, 'get_touch': 2, 'release': 3},
    'wl_pointer': {'set_cursor': 0, 'release': 1},
    'wl_keyboard': {'release': 0},
    'wl_data_device_manager': {'create_data_source': 0, 'get_data_device': 1},
    'wl_data_device': {'start_drag': 0, 'set_selection': 1, 'release': 2},
    'wl_data_source': {'offer': 0, 'destroy': 1},
    'wl_data_offer': {'accept': 0, 'receive': 1, 'destroy': 2},
    'mellagui_settings_v1': {'destroy': 0, 'set': 1},
    'mellagui_capture_v1': {'destroy': 0, 'list': 1, 'grab': 2},
    'zwp_idle_inhibit_manager_v1': {'destroy': 0, 'create_inhibitor': 1},
    'zwp_idle_inhibitor_v1': {'destroy': 0},
    'mellagui_tray_v1': {'destroy': 0, 'create_item': 1},
    'mellagui_tray_item_v1': {'destroy': 0, 'set_icon': 1, 'set_text': 2},
    'zwp_linux_dmabuf_v1': {'destroy': 0, 'create_params': 1, 'get_default_feedback': 2, 'get_surface_feedback': 3},
    'zwp_linux_buffer_params_v1': {'destroy': 0, 'add': 1, 'create': 2, 'create_immed': 3},
    'zwp_linux_dmabuf_feedback_v1': {'destroy': 0},
}

XDG_STATE_MAXIMIZED, XDG_STATE_FULLSCREEN, XDG_STATE_RESIZING, XDG_STATE_ACTIVATED = 1, 2, 3, 4


class Event:
    def __init__(self, obj, iface, name, args, fds):
        self.obj, self.iface, self.name, self.args, self.fds = obj, iface, name, args, fds

    def __repr__(self):
        return 'Event(%s#%d.%s%r)' % (self.iface, self.obj, self.name, tuple(self.args))


class ProtocolError(Exception):
    def __init__(self, obj, code, msg):
        super().__init__('protocol error on object %d code %d: %s' % (obj, code, msg))
        self.obj, self.code, self.msg = obj, code, msg


def socket_path(name=None):
    name = name or os.environ.get('WAYLAND_DISPLAY', 'wayland-0')
    if name.startswith('/'):
        return name
    return os.path.join(os.environ['XDG_RUNTIME_DIR'], name)


class Wl:
    def __init__(self, path=None, name=None):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path or socket_path(name))
        self.buf = b''
        self.fdq = []
        self.objs = {1: 'wl_display'}
        self.next_id = 2
        self.queue = []          # events not yet consumed
        self.globals = {}        # interface -> (name, version)
        self.error = None
        self.closed = False
        self.deleted = set()

    # ---- low level ----------------------------------------------------------
    def new_id(self, iface):
        oid = self.next_id
        self.next_id += 1
        self.objs[oid] = iface
        return oid

    def send_raw(self, data, fds=()):
        anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack('%di' % len(fds), *fds))] if fds else []
        self.sock.sendmsg([data], anc)

    def request_raw(self, obj, opcode, payload=b'', fds=()):
        size = 8 + len(payload)
        self.send_raw(struct.pack('<II', obj, (size << 16) | opcode) + payload, fds)

    @staticmethod
    def pack_string(s):
        b = s.encode() + b'\0'
        return struct.pack('<I', len(b)) + b + b'\0' * (-len(b) % 4)

    @staticmethod
    def pack_array(b):
        return struct.pack('<I', len(b)) + b + b'\0' * (-len(b) % 4)

    def request(self, obj, name, *args, fds=()):
        """args are pre-typed: ints become uint32 unless wrapped; use fmt-specific helpers below."""
        iface = self.objs[obj]
        opcode = REQ[iface][name]
        payload = b''
        for a in args:
            if isinstance(a, str):
                payload += self.pack_string(a)
            elif isinstance(a, bytes):
                payload += self.pack_array(a)
            elif isinstance(a, Int):
                payload += struct.pack('<i', a.v)
            else:
                payload += struct.pack('<I', a)
        self.request_raw(obj, opcode, payload, fds)

    def _parse(self):
        out = []
        while len(self.buf) >= 8:
            oid, w = struct.unpack_from('<II', self.buf)
            size, opcode = w >> 16, w & 0xffff
            if size < 8 or len(self.buf) < size:
                break
            body = self.buf[8:size]
            self.buf = self.buf[size:]
            out.append(self._decode(oid, opcode, body))
        return out

    def _decode(self, oid, opcode, body):
        iface = self.objs.get(oid, '?')
        name, sig = EVENTS.get(iface, {}).get(opcode, ('event%d' % opcode, ''))
        args, off, fds = [], 0, []
        for c in sig:
            if c in 'iu':
                args.append(struct.unpack_from('<i' if c == 'i' else '<I', body, off)[0]); off += 4
            elif c == 'f':
                args.append(struct.unpack_from('<i', body, off)[0] / 256.0); off += 4
            elif c == 's':
                n = struct.unpack_from('<I', body, off)[0]; off += 4
                args.append(body[off:off + n - 1].decode() if n else None); off += (n + 3) & ~3
            elif c == 'a':
                n = struct.unpack_from('<I', body, off)[0]; off += 4
                args.append(body[off:off + n]); off += (n + 3) & ~3
            elif c == 'o':
                args.append(struct.unpack_from('<I', body, off)[0]); off += 4
            elif c == 'n':
                nid = struct.unpack_from('<I', body, off)[0]; off += 4
                args.append(nid)
            elif c == 'h':
                fds.append(self.fdq.pop(0))
        ev = Event(oid, iface, name, args, fds)
        if iface == 'wl_data_device' and name == 'data_offer':
            self.objs[args[0]] = 'wl_data_offer'          # server-created offer
        if iface == 'wl_display':
            if name == 'error':
                self.error = ProtocolError(args[0], args[1], args[2])
            elif name == 'delete_id':
                self.deleted.add(args[0])
                self.objs.pop(args[0], None)
        if iface == 'wl_display' and name == 'delete_id':
            pass
        return ev

    def pump(self, timeout=0.0):
        """Read whatever is available (waiting up to timeout) and queue the events."""
        if self.closed:
            return False
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return False
        try:
            data, anc, flags, _ = self.sock.recvmsg(65536, socket.CMSG_LEN(64 * 4))
        except ConnectionResetError:
            self.closed = True
            return True
        for level, typ, cdata in anc:
            if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                n = len(cdata) // 4
                self.fdq.extend(struct.unpack('%di' % n, cdata[:n * 4]))
        if not data:
            self.closed = True
            return True
        self.buf += data
        self.queue.extend(self._parse())
        return True

    def events(self, timeout=0.0):
        """Drain and return all queued events."""
        self.pump(timeout)
        while self.pump(0):
            pass
        ev, self.queue = self.queue, []
        return ev

    def wait_for(self, pred, timeout=3.0, keep=True):
        """Block until an event matching pred arrives; return it. Others are kept in the queue."""
        end = time.time() + timeout
        while True:
            for i, ev in enumerate(self.queue):
                if pred(ev):
                    self.queue = self.queue[:i] + self.queue[i + 1:]
                    return ev
            left = end - time.time()
            if left <= 0:
                return None
            if self.closed:
                return None
            self.pump(min(left, 0.2))

    def roundtrip(self, timeout=3.0):
        cb = self.new_id('wl_callback')
        self.request(1, 'sync', cb)
        got = []
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            rest = []
            for ev in self.queue:
                if ev.obj == cb and ev.name == 'done':
                    got.append(ev)
                else:
                    rest.append(ev)
            self.queue = rest
            if got:
                return True
            if self.closed:
                return False
        return False

    # ---- registry -------------------------------------------------------------
    def get_registry(self):
        reg = self.new_id('wl_registry')
        self.request(1, 'get_registry', reg)
        self.roundtrip()
        for ev in list(self.queue):
            if ev.obj == reg and ev.name == 'global':
                self.globals[ev.args[1]] = (ev.args[0], ev.args[2])
        self.queue = [e for e in self.queue if not (e.obj == reg and e.name == 'global')]
        self.registry = reg
        return reg

    def bind(self, iface, version=None):
        name, maxv = self.globals[iface]
        v = min(version or maxv, maxv)
        oid = self.new_id(iface)
        self.request_raw(self.registry, 0, struct.pack('<I', name) + self.pack_string(iface) +
                         struct.pack('<II', v, oid))
        return oid

    # ---- helpers ----------------------------------------------------------------------
    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.closed = True


class Int:
    def __init__(self, v): self.v = v


def I(v):
    return Int(v)


class Shm:
    """A wl_shm pool with an anonymous memfd."""

    def __init__(self, wl, size):
        self.wl = wl
        self.fd = os.memfd_create('mellagui-test')
        os.ftruncate(self.fd, size)
        self.map = mmap.mmap(self.fd, size)
        self.size = size
        shm = wl.bind('wl_shm') if not hasattr(wl, 'shm') else wl.shm
        wl.shm = shm
        self.pool = wl.new_id('wl_shm_pool')
        wl.request(shm, 'create_pool', self.pool, size, fds=[self.fd])

    def buffer(self, w, h, argb=0xff336699, fmt=0, offset=0, fill=True):
        stride = w * 4
        if fill:
            row = struct.pack('<I', argb) * w
            for y in range(h):
                self.map[offset + y * stride: offset + (y + 1) * stride] = row
        b = self.wl.new_id('wl_buffer')
        self.wl.request(self.pool, 'create_buffer', b, offset, I(w), I(h), I(stride), fmt)
        return b


class Client:
    """A connected client with the common globals bound."""

    def __init__(self, name=None):
        self.wl = Wl(name=name)
        self.wl.get_registry()
        self.compositor = self.wl.bind('wl_compositor')
        self.wm_base = self.wl.bind('xdg_wm_base')


class Window:
    """xdg_toplevel with an shm buffer, following the standard configure dance."""

    def __init__(self, client, w=300, h=200, title='test', app_id='mellagui.test', color=0xff336699,
                 decoration=False, show=True):
        self.c = client
        wl = client.wl
        self.size = (w, h)
        self.color = color
        self.surface = wl.new_id('wl_surface')
        wl.request(client.compositor, 'create_surface', self.surface)
        self.xdg = wl.new_id('xdg_surface')
        wl.request(client.wm_base, 'get_xdg_surface', self.xdg, self.surface)
        self.toplevel = wl.new_id('xdg_toplevel')
        wl.request(self.xdg, 'get_toplevel', self.toplevel)
        wl.request(self.toplevel, 'set_title', title)
        wl.request(self.toplevel, 'set_app_id', app_id)
        self.shm = Shm(wl, max(4 * w * h * 4, 8 * 1024 * 1024))
        self.buffer = None
        self.configures = []
        if show:
            self.show()

    def wait_configure(self, timeout=3.0, latest=True):
        """Wait for an xdg_surface.configure (paired with the xdg_toplevel.configure before it).

        With latest=True every configure that is already queued is consumed and only
        the newest one is acked and returned.
        """
        wl = self.c.wl
        end = time.time() + timeout
        pair = None
        while True:
            while wl.pump(0):     # take in everything that already arrived
                pass
            q, rest, tl = wl.queue, [], None
            for ev in q:
                if ev.obj == self.toplevel and ev.name == 'configure':
                    tl = ev
                elif ev.obj == self.xdg and ev.name == 'configure' and tl is not None and \
                        (pair is None or latest):
                    pair = (tl, ev)
                    tl = None
                else:
                    rest.append(ev)
            wl.queue = rest
            if tl is not None:
                wl.queue.insert(0, tl)  # keep an orphan toplevel configure for the next round
            if pair is not None:
                break
            left = end - time.time()
            if left <= 0 or wl.closed:
                return None
            wl.pump(min(left, 0.1))
        tl, xs = pair
        wl.request(self.xdg, 'ack_configure', xs.args[0])
        states = struct.unpack('<%dI' % (len(tl.args[2]) // 4), tl.args[2]) if tl.args[2] else ()
        self.configures.append((tl.args[0], tl.args[1], states, xs.args[0]))
        return tl.args[0], tl.args[1], states, xs.args[0]

    def show(self):
        wl = self.c.wl
        wl.request(self.surface, 'commit')           # initial commit
        cfg = self.wait_configure()
        assert cfg is not None, 'no initial configure'
        self.commit_buffer(*self.size)
        return cfg

    def commit_buffer(self, w, h, color=None, geometry=True):
        wl = self.c.wl
        if color is not None:
            self.color = color
        self.size = (w, h)
        need = 4 * w * h
        if need > self.shm.size:
            raise RuntimeError('shm pool too small')
        self.buffer = self.shm.buffer(w, h, self.color)
        wl.request(self.surface, 'attach', self.buffer, I(0), I(0))
        wl.request(self.surface, 'damage', I(0), I(0), I(w), I(h))
        if geometry:
            wl.request(self.xdg, 'set_window_geometry', I(0), I(0), I(w), I(h))
        wl.request(self.surface, 'commit')

    def handle_configure(self, color=None):
        """Process a pending configure (resize/state change): ack and re-commit at the new size."""
        cfg = self.wait_configure(1.0)
        if cfg is None:
            return None
        w, h, states, _ = cfg
        if w > 0 and h > 0:
            self.commit_buffer(w, h, color)
        else:
            self.commit_buffer(*self.size, color=color)
        return cfg


class Ctl:
    """Headless control socket (see backend.h)."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rw')

    def cmd(self, line):
        self.f.write(line + '\n')
        self.f.flush()
        lines = []
        while True:
            l = self.f.readline()
            if not l:
                raise EOFError('control socket closed')
            l = l.rstrip('\n')
            if l == 'ok' or l.startswith('ok ') or l.startswith('err '):
                self.last = l
                if l.startswith('err '):
                    raise RuntimeError(l)
                return lines
            lines.append(l)

    def kbd(self):
        """{'layout': 'ee', 'slot': 1, 'sticky': 0, 'latched': 0, 'slocked': 0, 'sound': 'none', 'sounds': 0} (LM_MOD_* masks)."""
        self.cmd('kbd')
        return {k: (v if k in ('layout', 'sound') else int(v)) for k, v in (kv.split('=') for kv in self.last.split()[1:])}

    def windows(self):
        res = {'windows': []}
        for l in self.cmd('windows'):
            p = l.split()
            if p[0] == 'workspace':
                res['workspace'] = int(p[1])
            elif p[0] == 'pointer':
                res['pointer'] = (int(p[1]), int(p[2]))
            elif p[0] == 'window':
                import re
                m = re.match(r'window (\d+) "(.*?)" "(.*?)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) ws=(\d+) flags=(\S+)', l)
                res['windows'].append(dict(id=int(m[1]), title=m[2], app_id=m[3], x=int(m[4]), y=int(m[5]),
                                           w=int(m[6]), h=int(m[7]), ws=int(m[8]), flags=set(m[9].split(','))))
        return res

    def dump(self, path):
        self.cmd('dump ' + path)
        return Ppm(path)

    def sync(self):
        self.cmd('sync')

    def key(self, code, down):
        self.cmd('key %d %d' % (code, 1 if down else 0))

    def tap(self, code):
        self.key(code, True)
        self.key(code, False)

    def abs(self, x, y):
        self.cmd('abs %d %d' % (x, y))

    def button(self, code, down):
        self.cmd('button %d %d' % (code, 1 if down else 0))

    def click(self, x, y, code=272):
        self.abs(x, y)
        self.button(code, True)
        self.button(code, False)


class Ppm:
    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        parts = d.split(b'\n', 3)
        self.w, self.h = map(int, parts[1].split())
        self.data = parts[3]

    def px(self, x, y):
        o = (y * self.w + x) * 3
        return tuple(self.data[o:o + 3])

    def count(self, rgb, x0=0, y0=0, x1=None, y1=None):
        x1 = x1 if x1 is not None else self.w
        y1 = y1 if y1 is not None else self.h
        n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                if self.px(x, y) == tuple(rgb):
                    n += 1
        return n

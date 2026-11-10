"""End-to-end tests: start the compositor with the headless backend and talk to it
through the raw wire client in wlclient.py.

Environment:
    MELLAGUI_BIN   compositor binary (default build/mellagui; use build/mellagui-asan for sanitizers)
"""
import base64
import io
import random
import re
import json
import os
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wlclient import *  # noqa: E402

ROOT = os.path.dirname(HERE)
BIN = os.environ.get('MELLAGUI_BIN', os.path.join(ROOT, 'build', 'mellagui'))

KEY_ESC, KEY_1, KEY_2, KEY_Q, KEY_A, KEY_M, KEY_ENTER, KEY_F, KEY_TAB = 1, 2, 3, 16, 30, 50, 28, 33, 15
KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT = 125, 42, 29, 56
KEY_UP, KEY_LEFT, KEY_RIGHT, KEY_DOWN = 103, 105, 106, 108
KEY_SPACE, KEY_RIGHTALT = 57, 100
KEY_G = 34
KEY_V = 47
KEY_S = 31
KEY_C = 46
BTN_LEFT, BTN_RIGHT = 272, 273

proc = None
tmp = None
ctl = None
LOG = None
OUT = None


def setUpModule():
    global proc, tmp, ctl, LOG, OUT
    tmp = tempfile.mkdtemp(prefix='mellagui-test-')
    os.chmod(tmp, 0o700)
    os.environ['XDG_RUNTIME_DIR'] = tmp
    os.environ['WAYLAND_DISPLAY'] = 'mellagui-test'
    os.environ['XDG_CACHE_HOME'] = tmp        # mellagui-run keeps its history here
    os.environ['XDG_CONFIG_HOME'] = os.path.join(tmp, 'config')      # the terminal saves its zoom there: never the real ~/.config
    os.environ['XDG_DATA_HOME'] = os.path.join(tmp, 'data')          # the file manager's trash: never the real ~/.local/share/Trash
    os.environ['MELLAGUI_ICON_PATH'] = os.path.join(tmp, 'no-icons')  # the dock finds no icons of programs installed on this machine
    LOG = os.path.join(tmp, 'mellagui.log')
    OUT = os.path.join(tmp, 'mellagui.out')        # a file, not a pipe: nobody reads a pipe while the tests run and 64 KB of log would block the compositor
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0:exitcode=77',
               UBSAN_OPTIONS='print_stacktrace=1')
    proc = subprocess.Popen([BIN, '--backend', 'headless', '--size', '1024x600', '--socket', 'mellagui-test',
                             '--control', os.path.join(tmp, 'ctl'), '--log-file', LOG, '--log-level', 'debug',
                             '--terminal', 'true'],
                            env=env, stdout=open(OUT, 'wb'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if os.path.exists(os.path.join(tmp, 'ctl')) and os.path.exists(os.path.join(tmp, 'mellagui-test')):
            break
        if proc.poll() is not None:
            raise RuntimeError('compositor exited early: %s' % open(OUT, errors='replace').read())
        time.sleep(0.05)
    ctl = Ctl(os.path.join(tmp, 'ctl'))


def tearDownModule():
    code = None
    try:
        ctl.cmd('quit')
    except Exception:
        pass
    try:
        code = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = 'timeout'
    with open(OUT, errors='replace') as f:
        out = f.read()
    shutil.rmtree(tmp, ignore_errors=True)
    if code != 0:
        raise AssertionError('compositor exit status %r\n%s' % (code, out))
    if 'Sanitizer' in out or 'runtime error' in out:
        raise AssertionError('sanitizer output:\n' + out)


def alive():
    return proc.poll() is None


class Base(unittest.TestCase):
    def setUp(self):
        self.clients = []
        # all windows from earlier tests must be gone
        for _ in range(50):
            if not ctl.windows()['windows']:
                break
            time.sleep(0.02)
        if ctl.windows()['workspace'] != 0:                  # a test that ended on another workspace must not leak into the next ones
            ctl.key(KEY_LEFTMETA, True)
            ctl.tap(2)                                      # Super+1
            ctl.key(KEY_LEFTMETA, False)
            ctl.sync()
        ctl.cmd('cascade')                                  # windows are placed the same way whatever ran before
        ctl.cmd('abs 900 500')

    def tearDown(self):
        for c in self.clients:
            c.wl.close()
        self.clients = []
        ctl.sync()
        self.assertTrue(alive(), 'compositor died')

    def client(self):
        c = Client()
        self.clients.append(c)
        return c

    def own_clipboard(self, text):
        """Another client owns the clipboard (it serves `text` to whoever asks, in a thread)."""
        import threading
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        mgr = c.wl.bind('wl_data_device_manager', 3)
        dev = c.wl.new_id('wl_data_device')
        c.wl.request(mgr, 'get_data_device', dev, seat)
        w = Window(c, 200, 150, title='clipboard owner')
        c.wl.roundtrip()
        src = c.wl.new_id('wl_data_source')
        c.wl.request(mgr, 'create_data_source', src)
        c.wl.request(src, 'offer', 'text/plain;charset=utf-8')
        c.wl.request(dev, 'set_selection', src, w.configures[-1][3])
        c.wl.roundtrip()
        stop = threading.Event()
        def serve():
            while not stop.is_set():
                try:
                    ev = c.wl.wait_for(lambda e: e.obj == src and e.name == 'send', 0.2)
                except Exception:
                    return
                if ev:
                    os.write(ev.fds[0], text.encode())
                    os.close(ev.fds[0])
        t = threading.Thread(target=serve, daemon=True)
        t.start()
        self.addCleanup(lambda: (stop.set(), t.join(1)))
        return c

    @staticmethod
    def menu_state():
        d = dict(entries=[])
        for l in ctl.cmd('dockmenu'):
            m = re.match(r'dockmenu open=(\d) item=(-?\d+) sel=(-?\d+) owner=(-?\d+)', l)
            if m:
                d.update(open=int(m[1]), owner=int(m[4]))
            m = re.match(r'entry (\d) "(.*)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) enabled=(\d)', l)
            if m:
                d['entries'].append(dict(label=m[2], x=int(m[3]) + int(m[5]) // 2, y=int(m[4]) + int(m[6]) // 2, enabled=int(m[7])))
        return d

    def right_click(self, x, y):
        ctl.abs(x, y)
        ctl.button(273, True)
        ctl.button(273, False)
        time.sleep(0.2)


    def window(self, w=300, h=200, **kw):
        c = self.client()
        win = Window(c, w, h, **kw)
        c.wl.roundtrip()
        return c, win

    def wins(self):
        for _ in range(20):
            r = ctl.windows()
            if r['windows']:
                break
            time.sleep(0.02)
        return ctl.windows()



    def private_compositor(self, name, *args, env=None):
        sock = os.path.join(tmp, name + '.ctl')
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0:exitcode=77', **(env or {}))
        p = subprocess.Popen([BIN, '--backend', 'headless', '--size', '800x500', '--socket', name, '--control', sock,
                              '--terminal', 'true'] + list(args), env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(sock) and os.path.exists(os.path.join(tmp, name)):
                break
            if p.poll() is not None:
                self.fail('compositor exited early: %s' % p.stdout.read().decode())
            time.sleep(0.05)
        c = Ctl(sock)

        def stop():
            try:
                c.cmd('quit')
            except Exception:
                pass
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.addCleanup(stop)
        return p, c, stop


class TestRegistry(Base):
    def test_globals(self):
        c = self.client()
        g = c.wl.globals
        for name, minv in [('wl_compositor', 4), ('wl_subcompositor', 1), ('wl_shm', 1), ('wl_seat', 5),
                           ('wl_output', 3), ('xdg_wm_base', 2), ('wl_data_device_manager', 3),
                           ('zxdg_decoration_manager_v1', 1)]:
            self.assertIn(name, g)
            self.assertGreaterEqual(g[name][1], minv, name)

    def test_shm_formats(self):
        c = self.client()
        shm = c.wl.bind('wl_shm')
        c.wl.roundtrip()
        fmts = [e.args[0] for e in c.wl.queue if e.iface == 'wl_shm' and e.name == 'format']
        self.assertIn(0, fmts)
        self.assertIn(1, fmts)

    def test_output_events(self):
        c = self.client()
        out = c.wl.bind('wl_output')
        c.wl.roundtrip()
        names = [e.name for e in c.wl.queue if e.obj == out]
        self.assertIn('geometry', names)
        self.assertIn('mode', names)
        self.assertIn('done', names)
        mode = [e for e in c.wl.queue if e.obj == out and e.name == 'mode'][0]
        self.assertEqual((mode.args[1], mode.args[2]), (1024, 600))


class TestWindows(Base):
    def test_map_and_destroy(self):
        c, w = self.window(320, 240, title='Alpha', app_id='app.alpha')
        r = self.wins()
        self.assertEqual(len(r['windows']), 1)
        win = r['windows'][0]
        self.assertEqual((win['title'], win['app_id'], win['w'], win['h']), ('Alpha', 'app.alpha', 320, 240))
        self.assertIn('mapped', win['flags'])
        self.assertIn('focused', win['flags'])
        c.wl.request(w.toplevel, 'destroy')
        c.wl.request(w.xdg, 'destroy')
        c.wl.request(w.surface, 'destroy')
        c.wl.roundtrip()
        for _ in range(50):
            if not ctl.windows()['windows']:
                break
            time.sleep(0.02)
        self.assertEqual(ctl.windows()['windows'], [])

    def test_disconnect_removes_window(self):
        c, w = self.window()
        self.assertEqual(len(self.wins()['windows']), 1)
        c.wl.close()
        for _ in range(50):
            if not ctl.windows()['windows']:
                break
            time.sleep(0.02)
        self.assertEqual(ctl.windows()['windows'], [])

    def test_title_change(self):
        c, w = self.window(title='One')
        c.wl.request(w.toplevel, 'set_title', 'Two')
        c.wl.roundtrip()
        self.assertEqual(ctl.windows()['windows'][0]['title'], 'Two')

    def test_rendering_and_decorations(self):
        c, w = self.window(300, 200, color=0xff00ff00)
        r = self.wins()['windows'][0]
        ppm = ctl.dump(os.path.join(tmp, 'frame.ppm'))
        self.assertEqual(ppm.px(r['x'] + 10, r['y'] + 10), (0, 255, 0))
        self.assertEqual(ppm.px(r['x'] + 299, r['y'] + 199), (0, 255, 0))
        # title bar is above the content and not green
        self.assertNotEqual(ppm.px(r['x'] + 10, r['y'] - 10), (0, 255, 0))
        # just outside the frame is wallpaper (blue-ish gradient)
        b = ppm.px(r['x'] + 330, r['y'] + 50)
        self.assertGreater(b[2], b[0])

    def test_alpha_blending(self):
        c = self.client()
        w = Window(c, 100, 100, color=0xff000000)
        w.commit_buffer(100, 100, color=0x80800000)  # premultiplied half-transparent red over...
        c.wl.roundtrip()
        r = self.wins()['windows'][0]
        ppm = ctl.dump(os.path.join(tmp, 'a.ppm'))
        px = ppm.px(r['x'] + 50, r['y'] + 50)
        self.assertGreater(px[0], 100)  # red from the buffer
        self.assertLess(px[0], 200)

    def test_resize_buffer_updates_size(self):
        c, w = self.window(200, 150)
        w.commit_buffer(260, 180)
        c.wl.roundtrip()
        r = self.wins()['windows'][0]
        self.assertEqual((r['w'], r['h']), (260, 180))

    def test_two_windows_stack_and_focus(self):
        c1, w1 = self.window(200, 150, title='A', color=0xffff0000)
        c2, w2 = self.window(200, 150, title='B', color=0xff0000ff)
        r = self.wins()['windows']
        self.assertEqual(len(r), 2)
        focused = [x for x in r if 'focused' in x['flags']]
        self.assertEqual(len(focused), 1)
        self.assertEqual(focused[0]['title'], 'B')
        # click on A's content to focus it
        a = [x for x in r if x['title'] == 'A'][0]
        b = [x for x in r if x['title'] == 'B'][0]
        px = a['x'] + 5
        py = a['y'] + 5
        if not (b['x'] <= px < b['x'] + b['w'] and b['y'] <= py < b['y'] + b['h']):
            ctl.click(px, py)
            ctl.sync()
            r = ctl.windows()['windows']
            self.assertEqual([x['title'] for x in r if 'focused' in x['flags']], ['A'])
        else:
            ctl.click(a['x'] + 5, a['y'] - 10)  # its title bar
            ctl.sync()
            r = ctl.windows()['windows']
            self.assertEqual([x['title'] for x in r if 'focused' in x['flags']], ['A'])

    def test_xdg_activated_state(self):
        c, w = self.window()
        # after mapping the window is focused, so a configure with ACTIVATED must have arrived
        c.wl.roundtrip()
        cfgs = []
        for ev in c.wl.queue:
            if ev.obj == w.toplevel and ev.name == 'configure':
                cfgs.append(struct.unpack('<%dI' % (len(ev.args[2]) // 4), ev.args[2]) if ev.args[2] else ())
        self.assertTrue(any(XDG_STATE_ACTIVATED in s for s in cfgs), cfgs)

    def test_close_request_via_shortcut(self):
        c, w = self.window()
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_Q)
        ctl.key(KEY_LEFTMETA, False)
        ev = c.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', 2.0)
        self.assertIsNotNone(ev, 'no xdg_toplevel.close')

    def test_maximize_shortcut(self):
        c, w = self.window(300, 200)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_M)
        ctl.key(KEY_LEFTMETA, False)
        cfg = w.handle_configure()
        self.assertIsNotNone(cfg)
        self.assertIn(XDG_STATE_MAXIMIZED, cfg[2])
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertIn('maximized', r['flags'])
        self.assertEqual((r['w'], r['h']), (1024 - 8, 600 - 32 - 28 - 4))
        # and back
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_M)
        ctl.key(KEY_LEFTMETA, False)
        cfg = w.handle_configure()
        self.assertNotIn(XDG_STATE_MAXIMIZED, cfg[2])
        self.assertEqual((cfg[0], cfg[1]), (300, 200))

    def super_key(self, code):
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(code)
        ctl.key(KEY_LEFTMETA, False)

    def test_tile_left_right_and_restore(self):
        c, w = self.window(300, 200)
        r0 = ctl.windows()['windows'][0]
        self.super_key(KEY_LEFT)
        cfg = w.handle_configure()
        self.assertEqual((cfg[0], cfg[1]), (512 - 8, 600 - 32 - 28 - 4))
        self.assertNotIn(XDG_STATE_MAXIMIZED, cfg[2])
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertIn('tiled-left', r['flags'])
        self.assertEqual((r['x'], r['y'], r['w'], r['h']), (4, 60, 504, 536))
        self.super_key(KEY_RIGHT)
        cfg = w.handle_configure()
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertIn('tiled-right', r['flags'])
        self.assertEqual((r['x'], r['w']), (516, 504))
        # Super+Down returns to the pre-tile geometry
        self.super_key(KEY_DOWN)
        cfg = w.handle_configure()
        self.assertEqual((cfg[0], cfg[1]), (300, 200))
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertNotIn('tiled-right', r['flags'])
        self.assertEqual((r['x'], r['y'], r['w'], r['h']), (r0['x'], r0['y'], 300, 200))

    def test_tile_then_maximize_and_up_down(self):
        c, w = self.window(300, 200)
        self.super_key(KEY_LEFT)
        w.handle_configure()
        self.super_key(KEY_UP)
        cfg = w.handle_configure()
        self.assertIn(XDG_STATE_MAXIMIZED, cfg[2])
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertIn('maximized', r['flags'])
        self.assertNotIn('tiled-left', r['flags'])
        self.super_key(KEY_DOWN)  # un-maximize back to the original geometry
        cfg = w.handle_configure()
        self.assertNotIn(XDG_STATE_MAXIMIZED, cfg[2])
        self.assertEqual((cfg[0], cfg[1]), (300, 200))

    def test_drag_tiled_window_restores_size(self):
        c, w = self.window(300, 200)
        self.super_key(KEY_RIGHT)
        w.handle_configure()
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        sx, sy = r['x'] + 100, r['y'] - 14
        ctl.abs(sx, sy)
        ctl.button(BTN_LEFT, True)
        ctl.abs(sx - 50, sy + 40)
        cfg = w.handle_configure()
        ctl.button(BTN_LEFT, False)
        self.assertEqual((cfg[0], cfg[1]), (300, 200))
        c.wl.roundtrip()
        self.assertNotIn('tiled-right', ctl.windows()['windows'][0]['flags'])

    def test_fullscreen_hides_panel(self):
        c, w = self.window(300, 200, color=0xffff00ff)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_F)
        ctl.key(KEY_LEFTMETA, False)
        cfg = w.handle_configure()
        self.assertIn(XDG_STATE_FULLSCREEN, cfg[2])
        c.wl.roundtrip()
        ppm = ctl.dump(os.path.join(tmp, 'fs.ppm'))
        self.assertEqual(ppm.px(512, 5), (255, 0, 255))  # panel area is covered
        self.assertEqual(ppm.px(0, 0), (255, 0, 255))

    def test_move_with_titlebar(self):
        c, w = self.window(300, 200)
        r = ctl.windows()['windows'][0]
        sx, sy = r['x'] + 50, r['y'] - 14
        ctl.abs(sx, sy)
        ctl.button(BTN_LEFT, True)
        ctl.abs(sx + 120, sy + 60)
        ctl.button(BTN_LEFT, False)
        ctl.sync()
        r2 = ctl.windows()['windows'][0]
        self.assertEqual((r2['x'] - r['x'], r2['y'] - r['y']), (120, 60))

    def test_resize_with_border(self):
        c, w = self.window(300, 200)
        r = ctl.windows()['windows'][0]
        # bottom-right corner of the frame
        ox, oy = r['x'] + r['w'] + 2, r['y'] + r['h'] + 2
        ctl.abs(ox, oy)
        ctl.button(BTN_LEFT, True)
        ctl.abs(ox + 50, oy + 40)
        cfg = w.handle_configure()
        ctl.button(BTN_LEFT, False)
        self.assertIsNotNone(cfg)
        self.assertEqual((cfg[0], cfg[1]), (350, 240))
        self.assertIn(XDG_STATE_RESIZING, cfg[2])

    def test_close_button(self):
        c, w = self.window(300, 200)
        r = ctl.windows()['windows'][0]
        ctl.click(r['x'] + r['w'] + 4 - 4 - 10, r['y'] - 14)
        ev = c.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', 2.0)
        self.assertIsNotNone(ev)

    def test_workspaces(self):
        c, w = self.window(300, 200, color=0xffff0000)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_2)
        ctl.key(KEY_LEFTMETA, False)
        ctl.sync()
        self.assertEqual(ctl.windows()['workspace'], 1)
        r = ctl.windows()['windows'][0]
        ppm = ctl.dump(os.path.join(tmp, 'ws.ppm'))
        self.assertNotEqual(ppm.px(r['x'] + 20, r['y'] + 20), (255, 0, 0))
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_1)
        ctl.key(KEY_LEFTMETA, False)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'ws.ppm'))
        self.assertEqual(ppm.px(r['x'] + 20, r['y'] + 20), (255, 0, 0))

    def test_the_panel_throws_a_shadow_on_what_is_below_it(self):
        ctl.abs(900, 500)
        c, w = self.window(900, 400, color=0xffc0c0c0)             # a light window right under the panel
        r = ctl.windows()['windows'][0]
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_M)                                             # maximized: its title bar starts under the panel
        ctl.key(KEY_LEFTMETA, False)
        c.wl.roundtrip()
        time.sleep(0.2)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'pshadow.ppm'))
        top = sum(ppm.px(500, 32 + 1))                              # just under the 32 px panel
        low = sum(ppm.px(500, 32 + 20))                             # beyond the shadow (the title bar is 28 px: stay in its colour)
        self.assertLess(top, low - 15, 'no shadow under the panel (%d vs %d)' % (top, low))
        self.assertLess(sum(ppm.px(500, 32 + 7)), low)             # it fades out gradually, not in one step
        self.assertLess(top, sum(ppm.px(500, 32 + 7)))

    def test_no_shadow_on_a_fullscreen_window(self):
        c, w = self.window(300, 200, color=0xffc0c0c0)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_F)
        ctl.key(KEY_LEFTMETA, False)
        c.wl.roundtrip()
        w.handle_configure()
        c.wl.roundtrip()
        time.sleep(0.2)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'pshadow2.ppm'))
        self.assertEqual(ppm.px(500, 2), (0xc0, 0xc0, 0xc0))        # no panel ...
        self.assertEqual(ppm.px(500, 35), (0xc0, 0xc0, 0xc0))       # ... and no shadow

    def test_panel_workspace_click(self):
        ctl.click(76, 14)    # second workspace cell
        ctl.sync()
        self.assertEqual(ctl.windows()['workspace'], 1)
        ctl.click(50, 14)    # the first
        ctl.sync()
        self.assertEqual(ctl.windows()['workspace'], 0)

    # The round power button (top right) opens the session dialog: a card with round Lock and Exit buttons.
    # Geometry on the shared 1024x600 output: power button centre (1008, 14); card 264x172 at (380, 214); the Lock disc has its
    # centre at (460, 292), the Exit disc at (564, 292); (460, 316) / (564, 316) are plain disc pixels below the icons.
    POWER_BTN = (1008, 14)
    LOCK_DISC, EXIT_DISC = (460, 316), (564, 316)
    DISC_IDLE, DISC_LOCK_SEL, DISC_EXIT_SEL = (0x2d, 0x3a, 0x5a), (0x6f, 0xa8, 0xff), (0xc0, 0x39, 0x2b)

    def disc(self, name, pos):
        ctl.sync()
        return ctl.dump(os.path.join(tmp, name + '.ppm')).px(*pos)

    def test_power_button_opens_the_session_dialog_and_esc_closes_it(self):
        self.assertNotEqual(self.disc('pw0', self.LOCK_DISC), self.DISC_IDLE)
        ctl.click(*self.POWER_BTN)
        self.assertEqual(self.disc('pw1', self.LOCK_DISC), self.DISC_IDLE)           # the dialog is there
        self.assertEqual(self.disc('pw2', self.EXIT_DISC), self.DISC_IDLE)
        ctl.tap(KEY_ESC)
        self.assertNotEqual(self.disc('pw3', self.LOCK_DISC), self.DISC_IDLE)
        self.assertTrue(alive())

    def test_session_dialog_highlights_with_pointer_and_keys(self):
        ctl.click(*self.POWER_BTN)
        ctl.abs(480, 270)                                                             # the pointer highlights the button under it (its arrow
                                                                                      # is drawn away from the pixels looked at)
        self.assertEqual(self.disc('pw4', self.LOCK_DISC), self.DISC_LOCK_SEL)
        ctl.abs(584, 270)
        self.assertEqual(self.disc('pw5', self.EXIT_DISC), self.DISC_EXIT_SEL)
        self.assertEqual(self.disc('pw6', self.LOCK_DISC), self.DISC_IDLE)
        ctl.tap(105)                                                                  # Left: the other button
        self.assertEqual(self.disc('pw7', self.LOCK_DISC), self.DISC_LOCK_SEL)
        ctl.tap(106)                                                                  # Right
        self.assertEqual(self.disc('pw8', self.EXIT_DISC), self.DISC_EXIT_SEL)
        ctl.tap(KEY_ESC)

    def test_session_dialog_is_modal(self):
        ctl.click(*self.POWER_BTN)
        ctl.click(100, 500)                                                           # outside the card: closes, goes no further
        self.assertNotEqual(self.disc('pw9', self.LOCK_DISC), self.DISC_IDLE)
        ctl.click(*self.POWER_BTN)
        ctl.click(512, 230)                                                           # on the card (the title): stays open
        self.assertEqual(self.disc('pw10', self.LOCK_DISC), self.DISC_IDLE)
        ctl.tap(KEY_ENTER)                                                            # nothing highlighted yet: Enter does nothing
        ctl.tap(57)
        self.assertEqual(self.disc('pw11', self.LOCK_DISC), self.DISC_IDLE)
        ctl.tap(KEY_ESC)
        self.assertTrue(alive())

    def start_exit_instance(self, *extra):
        d = tempfile.mkdtemp(prefix='mellagui-exit-')
        os.chmod(d, 0o700)
        p2 = subprocess.Popen([BIN, '--backend', 'headless', '--size', '1024x600', '--socket', 'mellagui-exit',
                               '--control', os.path.join(d, 'ctl'), '--terminal', 'true'] + list(extra),
                              env=dict(os.environ, XDG_RUNTIME_DIR=d),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def cleanup():
            if p2.poll() is None:
                p2.kill()
            p2.wait()
            p2.stdout.close()
            shutil.rmtree(d, ignore_errors=True)
        self.addCleanup(cleanup)
        for _ in range(100):
            if os.path.exists(os.path.join(d, 'ctl')):
                break
            time.sleep(0.05)
        return p2, Ctl(os.path.join(d, 'ctl'))

    def test_session_dialog_suspend_restart_and_shutdown_run_the_configured_commands(self):
        marks = {k: os.path.join(tmp, 'pwr-%s.mark' % k) for k in ('suspend', 'reboot', 'poweroff')}
        for m in marks.values():
            if os.path.exists(m):
                os.unlink(m)
        conf = os.path.join(tmp, 'pwr.conf')
        with open(conf, 'w') as f:
            f.write('[commands]\nsuspend = touch %s\nreboot = touch %s\npoweroff = touch %s\n' % (marks['suspend'], marks['reboot'], marks['poweroff']))
        p2, c = self.start_exit_instance('--config', conf)
        def appeared(name):
            for _ in range(60):
                if os.path.exists(marks[name]):
                    return True
                time.sleep(0.05)
            return False
        for key, name in ((31, 'suspend'), (19, 'reboot'), (25, 'poweroff')):          # the letters S, R, P in the open dialog
            c.click(*self.POWER_BTN)
            c.tap(key)
            self.assertTrue(appeared(name), '%s: the key did not run the command' % name)
            os.unlink(marks[name])
        SUSPEND, RESTART, SHUTDOWN = (428, 394), (512, 394), (596, 394)                # the lower row of the card on a 1024x600 output
        for pos, name in ((SUSPEND, 'suspend'), (RESTART, 'reboot'), (SHUTDOWN, 'poweroff')):
            c.click(*self.POWER_BTN)
            c.click(*pos)
            self.assertTrue(appeared(name), '%s: the button did not run the command' % name)
            os.unlink(marks[name])
        c.click(*self.POWER_BTN)                                                        # Right x3 from nothing highlighted: Suspend, then Enter
        for _ in range(3):
            c.tap(106)
        c.tap(KEY_ENTER)
        self.assertTrue(appeared('suspend'), 'Enter on the highlighted Suspend did not run it')
        self.assertIsNone(p2.poll())                                                     # (nothing quit the compositor)

    def test_session_dialog_exit_button_quits(self):
        # separate instance: the shared compositor must keep running
        p2, c2 = self.start_exit_instance()
        c2.click(*self.POWER_BTN)
        c2.sync()
        self.assertIsNone(p2.poll())                                                  # opening the dialog quits nothing
        c2.click(*self.EXIT_DISC)
        self.assertEqual(p2.wait(timeout=5), 0)

    def test_session_dialog_keys_quit_and_cancel(self):
        p2, c2 = self.start_exit_instance()
        c2.click(*self.POWER_BTN)
        c2.tap(KEY_ESC)
        c2.sync()
        self.assertIsNone(p2.poll())
        c2.click(*self.POWER_BTN)
        c2.tap(106)                                                                   # Right -> Lock, Right -> Exit
        c2.tap(106)
        try:
            c2.tap(KEY_ENTER)                                                         # quits: the control connection goes away
        except OSError:
            pass
        self.assertEqual(p2.wait(timeout=5), 0)

    def test_session_dialog_release_off_the_button_does_nothing(self):
        p2, c2 = self.start_exit_instance()
        c2.click(*self.POWER_BTN)
        c2.abs(*self.EXIT_DISC)
        c2.button(272, True)
        c2.abs(100, 500)
        c2.button(272, False)
        c2.sync()
        self.assertIsNone(p2.poll())

    @staticmethod
    def about(c):
        """(open, {'card': (x, y, w, h), 'close': (x, y, w, h)}, [(label, value)]) of the control socket's `about`."""
        info, rows = {}, []
        lines = c.cmd('about')
        is_open = lines[0].endswith('=1')
        for l in lines[1:]:
            m = re.match(r'(card|close) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', l)
            if m:
                info[m[1]] = tuple(int(m[i]) for i in range(2, 6))
            elif l.startswith('row '):
                rows.append(tuple(l[4:].split('|', 1)))
        return is_open, info, rows

    def test_the_logo_button_opens_the_about_dialog(self):
        ctl.abs(900, 500)
        ctl.sync()
        before = ctl.dump(os.path.join(tmp, 'about0.ppm'))
        self.assertFalse(self.about(ctl)[0])
        ctl.click(16, 14)                       # the round logo button top left
        ctl.abs(10, 300)
        ctl.sync()
        is_open, info, rows = self.about(ctl)
        self.assertTrue(is_open)
        opened = ctl.dump(os.path.join(tmp, 'about1.ppm'))
        self.assertNotEqual(opened.px(40, 300), before.px(40, 300))        # the output is dimmed behind the card
        cx, cy, cw, ch = info['card']
        self.assertEqual(opened.px(cx + 8, cy + ch // 2), (0x14, 0x17, 0x1f))       # the card's own background
        labels = [l for l, _ in rows]
        for want in ('Computer', 'System', 'Memory', 'Display', 'Graphics'):
            self.assertIn(want, labels)
        self.assertEqual(dict(rows)['Display'].split()[0], 'headless-0')
        ctl.tap(KEY_ESC)                        # Escape closes it
        ctl.sync()
        self.assertFalse(self.about(ctl)[0])
        closed = ctl.dump(os.path.join(tmp, 'about2.ppm'))
        self.assertEqual(closed.px(40, 300), before.px(40, 300))
        ctl.click(16, 14)                       # open again: a click outside the card closes it and nothing below gets it
        self.assertTrue(self.about(ctl)[0])
        ctl.click(20, 300)
        self.assertFalse(self.about(ctl)[0])
        ctl.click(16, 14)                       # a click inside the card keeps it open, the Close button ends it
        x, y, w, h = self.about(ctl)[1]['card']
        ctl.click(x + 30, y + 30)
        self.assertTrue(self.about(ctl)[0])
        x, y, w, h = self.about(ctl)[1]['close']
        ctl.click(x + w // 2, y + h // 2)
        self.assertFalse(self.about(ctl)[0])
        ctl.click(16, 14)
        ctl.tap(KEY_ENTER)                      # Enter closes it, too
        self.assertFalse(self.about(ctl)[0])

    def test_the_logo_is_painted_in_the_round_button(self):
        ctl.abs(900, 500)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'logo0.ppm'))
        self.assertEqual(ppm.px(5, 3), ppm.px(2, 14))                       # the corner of the button's square is the panel
        self.assertNotEqual(ppm.px(16, 14), ppm.px(2, 14))                  # the middle is the picture
        self.assertTrue(len({ppm.px(x, y) for x in range(8, 25) for y in range(6, 23)}) > 8)   # a picture, not a flat disc

    def test_taskbar_click_minimize_and_restore(self):
        c1, w1 = self.window(300, 200)
        c2, w2 = self.window(300, 200)
        ids = [r['id'] for r in ctl.windows()['windows']]
        self.assertEqual(len(ids), 2)
        def flags(i):
            return [r for r in ctl.windows()['windows'] if r['id'] == ids[i]][0]['flags']
        self.assertIn('focused', flags(1))
        ctl.click(250, 14)       # first task button -> focus window 1
        ctl.sync()
        self.assertIn('focused', flags(0))
        ctl.click(250, 14)       # focused task button again -> minimize
        ctl.sync()
        self.assertIn('minimized', flags(0))
        self.assertIn('focused', flags(1))
        ctl.click(250, 14)       # minimized task button -> restore and focus
        ctl.sync()
        self.assertNotIn('minimized', flags(0))
        self.assertIn('focused', flags(0))

    def test_a_right_click_on_the_title_bar_or_the_taskbar_button_opens_the_window_menu(self):
        c1, w1 = self.window(300, 200, title='menu window')
        r = ctl.windows()['windows'][0]
        self.right_click(r['x'] + 100, r['y'] - 12)                      # the title bar
        m = self.menu_state()
        self.assertEqual((m['open'], [e['label'] for e in m['entries']]),
                         (1, ['Maximize', 'Minimize', 'Move to workspace 1', 'Move to workspace 2', 'Move to workspace 3', 'Move to workspace 4', 'Close']))
        self.assertEqual([e['enabled'] for e in m['entries']], [1, 1, 0, 1, 1, 1, 1])        # (it is on workspace 1)
        ctl.click(m['entries'][0]['x'], m['entries'][0]['y'])            # Maximize
        ctl.sync()
        self.assertIn('maximized', ctl.windows()['windows'][0]['flags'])
        r = ctl.windows()['windows'][0]
        self.right_click(r['x'] + 100, r['y'] - 12)
        self.assertEqual(self.menu_state()['entries'][0]['label'], 'Restore')
        ctl.tap(KEY_ESC)                                                 # Esc closes the menu only
        self.assertEqual(self.menu_state()['open'], 0)
        self.assertEqual(len(ctl.windows()['windows']), 1)
        self.right_click(250, 14)                                        # the taskbar button
        m = self.menu_state()
        self.assertEqual(m['open'], 1)
        ctl.click(m['entries'][3]['x'], m['entries'][3]['y'])            # Move to workspace 2: the window leaves this workspace
        ctl.sync()
        self.assertEqual(ctl.windows()['windows'][0]['ws'], 1)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(3)                                                       # Super+2: there it is
        ctl.key(KEY_LEFTMETA, False)
        ctl.sync()
        r = ctl.windows()['windows'][0]
        self.right_click(r['x'] + 100, r['y'] - 12)
        m = self.menu_state()
        ctl.click(m['entries'][-1]['x'], m['entries'][-1]['y'])          # Close: the program is asked to
        ev = c1.wl.wait_for(lambda e: e.obj == w1.toplevel and e.name == 'close', 2.0)
        self.assertIsNotNone(ev, 'Close did not ask the window to close')
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(2)                                                       # back to workspace 1 for the next tests
        ctl.key(KEY_LEFTMETA, False)

    def test_taskbar_paints_buttons(self):
        c, w = self.window(300, 200)
        ctl.abs(900, 500)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'tb.ppm'))
        self.assertEqual(ppm.px(204, 14), (0x6f, 0xa8, 0xff))   # active task button background
        self.assertEqual(ppm.px(500, 14), (0x14, 0x17, 0x1f))    # plain panel beyond it

    def test_taskbar_middle_click_closes(self):
        c, w = self.window(300, 200)
        ctl.abs(250, 14)
        ctl.button(274, True)
        ctl.button(274, False)
        ev = c.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', 2.0)
        self.assertIsNotNone(ev, 'no xdg_toplevel.close')

    def test_oversized_window_is_asked_to_fit(self):
        c, w = self.window(2000, 1000)
        cfg = w.handle_configure()
        self.assertIsNotNone(cfg, 'no configure for an oversized window')
        self.assertEqual((cfg[0], cfg[1]), (1024 - 8, 600 - 32 - 28 - 4))
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertEqual((r['x'], r['y'], r['w'], r['h']), (4, 60, 1016, 536))

    def test_frame_callback(self):
        c, w = self.window()
        cb = c.wl.new_id('wl_callback')
        c.wl.request(w.surface, 'frame', cb)
        w.commit_buffer(300, 200, color=0xff112233)
        ev = c.wl.wait_for(lambda e: e.obj == cb and e.name == 'done', 2.0)
        self.assertIsNotNone(ev, 'frame callback not delivered')

    def test_decoration_manager_server_side(self):
        c = self.client()
        mgr = c.wl.bind('zxdg_decoration_manager_v1')
        w = Window(c, 200, 100, show=False)
        dec = c.wl.new_id('zxdg_toplevel_decoration_v1')
        c.wl.request(mgr, 'get_toplevel_decoration', dec, w.toplevel)
        c.wl.request(w.surface, 'commit')
        ev = c.wl.wait_for(lambda e: e.obj == dec and e.name == 'configure', 2.0)
        self.assertIsNotNone(ev)
        self.assertEqual(ev.args[0], 2)  # server_side


class TestProtocolErrors(Base):
    def test_buffer_before_configure(self):
        c = self.client()
        s = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', s)
        x = c.wl.new_id('xdg_surface')
        c.wl.request(c.wm_base, 'get_xdg_surface', x, s)
        t = c.wl.new_id('xdg_toplevel')
        c.wl.request(x, 'get_toplevel', t)
        shm = Shm(c.wl, 4096)
        b = shm.buffer(10, 10)
        c.wl.request(s, 'attach', b, I(0), I(0))
        c.wl.request(s, 'commit')
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_bad_shm_pool(self):
        c = self.client()
        shm = c.wl.bind('wl_shm')
        fd = os.memfd_create('x')
        os.ftruncate(fd, 64)
        pool = c.wl.new_id('wl_shm_pool')
        c.wl.request(shm, 'create_pool', pool, I(-5), fds=[fd])
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_buffer_out_of_pool(self):
        c = self.client()
        shm = Shm(c.wl, 4096)
        b = c.wl.new_id('wl_buffer')
        c.wl.request(shm.pool, 'create_buffer', b, 0, I(100), I(100), I(400), 0)
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_bad_format(self):
        c = self.client()
        shm = Shm(c.wl, 4096)
        b = c.wl.new_id('wl_buffer')
        c.wl.request(shm.pool, 'create_buffer', b, 0, I(10), I(10), I(40), 0x12345678)
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_duplicate_role(self):
        c = self.client()
        s = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', s)
        x = c.wl.new_id('xdg_surface')
        c.wl.request(c.wm_base, 'get_xdg_surface', x, s)
        x2 = c.wl.new_id('xdg_surface')
        c.wl.request(c.wm_base, 'get_xdg_surface', x2, s)
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_bad_scale(self):
        c = self.client()
        s = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', s)
        c.wl.request(s, 'set_buffer_scale', I(-1))
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)

    def test_invalid_object(self):
        c = self.client()
        c.wl.request_raw(9999, 0, b'')
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)
        self.assertEqual(c.wl.error.code, 0)  # wl_display.error.invalid_object

    def test_bind_bad_version(self):
        c = self.client()
        name, v = c.wl.globals['wl_compositor']
        oid = c.wl.new_id('wl_compositor')
        c.wl.request_raw(c.wl.registry, 0, struct.pack('<I', name) + c.wl.pack_string('wl_compositor') +
                         struct.pack('<II', v + 5, oid))
        c.wl.roundtrip()
        self.assertIsNotNone(c.wl.error)


class TestRobustness(Base):
    def other_client_works(self):
        c, w = self.window(100, 100)
        self.assertEqual(len(ctl.windows()['windows']), 1)

    def raw(self, data, fds=()):
        s = socket.socket(socket.AF_UNIX)
        s.connect(socket_path())
        try:
            if fds:
                s.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack('%di' % len(fds), *fds))])
            else:
                s.sendall(data)
            s.settimeout(1.0)
            try:
                s.recv(4096)
            except (socket.timeout, ConnectionResetError):
                pass
        finally:
            s.close()

    def test_garbage(self):
        self.raw(os.urandom(4096))
        self.assertTrue(alive())
        self.other_client_works()

    def test_truncated_header(self):
        self.raw(b'\x01\x00\x00')
        self.other_client_works()

    def test_oversized_message(self):
        self.raw(struct.pack('<II', 1, (0xffff << 16) | 1) + b'\0' * 100)
        self.other_client_works()

    def test_undersized_message(self):
        self.raw(struct.pack('<II', 1, (4 << 16) | 1))
        self.other_client_works()

    def test_unaligned_size(self):
        self.raw(struct.pack('<II', 1, (13 << 16) | 1) + b'\0' * 5)
        self.other_client_works()

    def test_bad_opcode(self):
        self.raw(struct.pack('<II', 1, (8 << 16) | 77))
        self.other_client_works()

    def test_string_without_nul(self):
        c = self.client()
        name, v = c.wl.globals['wl_compositor']
        payload = struct.pack('<I', name) + struct.pack('<I', 4) + b'abcd' + struct.pack('<II', 1, 50)
        c.wl.request_raw(c.wl.registry, 0, payload)
        c.wl.roundtrip()
        self.assertTrue(alive())
        self.other_client_works()

    def test_string_length_overflow(self):
        c = self.client()
        payload = struct.pack('<I', 1) + struct.pack('<I', 0xffffffff) + b'x' * 8
        c.wl.request_raw(c.wl.registry, 0, payload)
        c.wl.roundtrip()
        self.other_client_works()

    def test_fd_flood(self):
        fds = [os.open('/dev/null', os.O_RDONLY) for _ in range(30)]
        try:
            self.raw(struct.pack('<II', 1, (8 << 16) | 1), fds)
        finally:
            for f in fds:
                os.close(f)
        self.other_client_works()

    def test_many_objects(self):
        c = self.client()
        for _ in range(2000):
            s = c.wl.new_id('wl_surface')
            c.wl.request(c.compositor, 'create_surface', s)
        c.wl.roundtrip(5.0)
        self.assertIsNone(c.wl.error)
        c.wl.close()
        self.other_client_works()

    def test_huge_surface_commit(self):
        c = self.client()
        shm = Shm(c.wl, 4096)
        s = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', s)
        b = c.wl.new_id('wl_buffer')
        c.wl.request(shm.pool, 'create_buffer', b, 0, I(100000), I(100000), I(400000), 0)
        try:
            c.wl.roundtrip()
        except (BrokenPipeError, ConnectionResetError, ProtocolError):
            pass                                            # the compositor may have disconnected us already: that is the right answer
        self.assertTrue(alive())
        self.other_client_works()

    def test_random_valid_framing(self):
        import random
        rnd = random.Random(1234)
        for _ in range(30):
            c = self.client()
            for _ in range(40):
                oid = rnd.choice([1, 2, 3, 4, 5, rnd.randrange(1, 40)])
                op = rnd.randrange(0, 12)
                body = os.urandom(rnd.randrange(0, 5) * 4)
                try:
                    c.wl.request_raw(oid, op, body)
                except OSError:
                    break
            c.wl.pump(0.05)
            c.wl.close()
        self.assertTrue(alive())
        self.other_client_works()

    def test_connect_disconnect_storm(self):
        for _ in range(100):
            s = socket.socket(socket.AF_UNIX)
            s.connect(socket_path())
            s.close()
        self.other_client_works()


class TestInput(Base):
    def keyboard(self, c, seat):
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        return kb

    def test_keyboard_focus_and_keys(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = self.keyboard(c, seat)
        w = Window(c, 300, 200)
        c.wl.roundtrip()
        ev = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'keymap', 2.0)
        self.assertIsNotNone(ev, 'no keymap')
        self.assertEqual(ev.args[0], 1)
        size = ev.args[1]
        data = os.pread(ev.fds[0], size, 0)
        self.assertTrue(data.startswith(b'xkb_keymap'))
        self.assertIn(b'xkb_symbols', data)
        self.assertIsNotNone(c.wl.wait_for(lambda e: e.obj == kb and e.name == 'enter', 2.0), 'no keyboard enter')
        ctl.tap(KEY_A)
        down = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'key' and e.args[3] == 1, 2.0)
        up = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'key' and e.args[3] == 0, 2.0)
        self.assertIsNotNone(down)
        self.assertIsNotNone(up)
        self.assertEqual(down.args[2], KEY_A)

    def test_modifiers_reported(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = self.keyboard(c, seat)
        w = Window(c, 300, 200)
        c.wl.roundtrip()
        ctl.key(KEY_LEFTSHIFT, True)
        ev = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'modifiers' and e.args[1] & 1, 2.0)
        ctl.key(KEY_LEFTSHIFT, False)
        self.assertIsNotNone(ev, 'shift modifier not reported')

    def test_shortcut_keys_not_forwarded(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = self.keyboard(c, seat)
        w = Window(c, 300, 200)
        c.wl.roundtrip()
        c.wl.events(0.1)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_2)
        ctl.key(KEY_LEFTMETA, False)
        ctl.sync()
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_1)
        ctl.key(KEY_LEFTMETA, False)
        ctl.sync()
        c.wl.roundtrip()
        keys = [e for e in c.wl.queue if e.obj == kb and e.name == 'key' and e.args[2] in (KEY_1, KEY_2)]
        self.assertEqual(keys, [])

    def test_pointer_enter_motion_button(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        ptr = c.wl.new_id('wl_pointer')
        c.wl.request(seat, 'get_pointer', ptr)
        w = Window(c, 300, 200)
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        ctl.abs(r['x'] + 40, r['y'] + 30)
        ctl.sync()
        enter = c.wl.wait_for(lambda e: e.obj == ptr and e.name == 'enter', 2.0)
        self.assertIsNotNone(enter)
        self.assertAlmostEqual(enter.args[2], 40, delta=1)
        self.assertAlmostEqual(enter.args[3], 30, delta=1)
        ctl.button(BTN_LEFT, True)
        ctl.button(BTN_LEFT, False)
        b1 = c.wl.wait_for(lambda e: e.obj == ptr and e.name == 'button' and e.args[3] == 1, 2.0)
        b0 = c.wl.wait_for(lambda e: e.obj == ptr and e.name == 'button' and e.args[3] == 0, 2.0)
        self.assertIsNotNone(b1)
        self.assertIsNotNone(b0)
        self.assertEqual(b1.args[2], BTN_LEFT)
        ctl.abs(900, 500)
        ctl.sync()
        self.assertIsNotNone(c.wl.wait_for(lambda e: e.obj == ptr and e.name == 'leave', 2.0))

    def test_axis(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        ptr = c.wl.new_id('wl_pointer')
        c.wl.request(seat, 'get_pointer', ptr)
        w = Window(c, 300, 200)
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        ctl.abs(r['x'] + 40, r['y'] + 30)
        ctl.cmd('axis v 15 1')
        ev = c.wl.wait_for(lambda e: e.obj == ptr and e.name == 'axis', 2.0)
        self.assertIsNotNone(ev)
        self.assertAlmostEqual(ev.args[2], 15, delta=1)

    def test_capabilities(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        c.wl.roundtrip()
        ev = [e for e in c.wl.queue if e.obj == seat and e.name == 'capabilities'][0]
        self.assertTrue(ev.args[0] & 1 and ev.args[0] & 2)


class TestSurfaceTree(Base):
    def test_subsurface_rendering(self):
        c, w = self.window(200, 150, color=0xff0000ff)
        sub = c.wl.bind('wl_subcompositor')
        s2 = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', s2)
        ss = c.wl.new_id('wl_subsurface')
        c.wl.request(sub, 'get_subsurface', ss, s2, w.surface)
        c.wl.request(ss, 'set_position', I(20), I(20))
        c.wl.request(ss, 'set_desync')
        buf = w.shm.buffer(50, 50, 0xffffff00, offset=4 * 200 * 150)
        c.wl.request(s2, 'attach', buf, I(0), I(0))
        c.wl.request(s2, 'damage', I(0), I(0), I(50), I(50))
        c.wl.request(s2, 'commit')
        c.wl.request(w.surface, 'commit')   # sub-surface position is applied with the parent commit
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        ppm = ctl.dump(os.path.join(tmp, 'sub.ppm'))
        self.assertEqual(ppm.px(r['x'] + 30, r['y'] + 30), (255, 255, 0))
        self.assertEqual(ppm.px(r['x'] + 5, r['y'] + 5), (0, 0, 255))

    def test_popup(self):
        c, w = self.window(300, 200, color=0xff202020)
        pos = c.wl.new_id('xdg_positioner')
        c.wl.request(c.wm_base, 'create_positioner', pos)
        c.wl.request(pos, 'set_size', I(80), I(60))
        c.wl.request(pos, 'set_anchor_rect', I(10), I(10), I(20), I(20))
        c.wl.request(pos, 'set_anchor', 4)    # bottom
        c.wl.request(pos, 'set_gravity', 4)   # bottom
        ps = c.wl.new_id('wl_surface')
        c.wl.request(c.compositor, 'create_surface', ps)
        px = c.wl.new_id('xdg_surface')
        c.wl.request(c.wm_base, 'get_xdg_surface', px, ps)
        pp = c.wl.new_id('xdg_popup')
        c.wl.request(px, 'get_popup', pp, w.xdg, pos)
        c.wl.request(ps, 'commit')
        cfg = c.wl.wait_for(lambda e: e.obj == pp and e.name == 'configure', 2.0)
        self.assertIsNotNone(cfg)
        self.assertEqual((cfg.args[2], cfg.args[3]), (80, 60))
        xs = c.wl.wait_for(lambda e: e.obj == px and e.name == 'configure', 2.0)
        c.wl.request(px, 'ack_configure', xs.args[0])
        buf = w.shm.buffer(80, 60, 0xffff8000, offset=4 * 300 * 200)
        c.wl.request(ps, 'attach', buf, I(0), I(0))
        c.wl.request(ps, 'damage', I(0), I(0), I(80), I(60))
        c.wl.request(ps, 'commit')
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        ppm = ctl.dump(os.path.join(tmp, 'popup.ppm'))
        self.assertEqual(ppm.px(r['x'] + cfg.args[0] + 5, r['y'] + cfg.args[1] + 5), (255, 128, 0))


class TestClipboard(Base):
    def test_selection_between_clients(self):
        a = self.client()
        seat_a = a.wl.bind('wl_seat', 5)
        mgr_a = a.wl.bind('wl_data_device_manager', 3)
        dev_a = a.wl.new_id('wl_data_device')
        a.wl.request(mgr_a, 'get_data_device', dev_a, seat_a)
        wa = Window(a, 200, 150, title='A')
        a.wl.roundtrip()
        src = a.wl.new_id('wl_data_source')
        a.wl.request(mgr_a, 'create_data_source', src)
        a.wl.request(src, 'offer', 'text/plain')
        # find the serial of the keyboard enter / any valid serial: use latest configure serial of A
        serial = wa.configures[-1][3]
        a.wl.request(dev_a, 'set_selection', src, serial)
        a.wl.roundtrip()

        b = self.client()
        seat_b = b.wl.bind('wl_seat', 5)
        mgr_b = b.wl.bind('wl_data_device_manager', 3)
        dev_b = b.wl.new_id('wl_data_device')
        b.wl.request(mgr_b, 'get_data_device', dev_b, seat_b)
        kb = b.wl.new_id('wl_keyboard')
        b.wl.request(seat_b, 'get_keyboard', kb)
        wb = Window(b, 200, 150, title='B')
        b.wl.roundtrip()
        sel = b.wl.wait_for(lambda e: e.obj == dev_b and e.name == 'selection', 2.0)
        self.assertIsNotNone(sel, 'no selection event for the focused client')


class TestGadget(Base):
    """The desktop gadget: clipboard history read by the compositor itself."""
    def rect(self):
        """The gadget card (x, y, w, h) or None when hidden."""
        ctl.cmd('gadget-rect')
        p = ctl.last.split()
        return None if p[1] == 'hidden' else tuple(int(v) for v in p[1:5])

    def is_card(self, rgb):
        """The card is slightly translucent: its dark blue shows the wallpaper a little."""
        return all(abs(a - b) <= 10 for a, b in zip(rgb, (0x16, 0x1a, 0x24)))

    def drag(self, x, y, dx, dy):
        ctl.abs(x, y)
        ctl.button(272, True)
        ctl.cmd('motion %d %d' % (dx, dy))
        ctl.button(272, False)

    def reset_gadget(self):
        x, y, w, h = self.rect()
        ctl.click(x + w // 2, y + 10)
        ctl.click(x + w // 2, y + 10)       # double click on the title bar: default size and place
        ctl.abs(5, 300)

    def setUp(self):
        super().setUp()
        ctl.cmd('gadget 1')
        self.reset_gadget()
        ctl.cmd('clip clear')
        self.addCleanup(lambda: ctl.cmd('gadget 1'))
        self.c = self.client()
        self.seat = self.c.wl.bind('wl_seat', 5)
        self.mgr = self.c.wl.bind('wl_data_device_manager', 3)
        self.dev = self.c.wl.new_id('wl_data_device')
        self.c.wl.request(self.mgr, 'get_data_device', self.dev, self.seat)
        self.win = Window(self.c, 200, 150, title='copier')
        self.c.wl.roundtrip()

    def copy(self, text, mime='text/plain;charset=utf-8', answer=True):
        c = self.c
        src = c.wl.new_id('wl_data_source')
        c.wl.request(self.mgr, 'create_data_source', src)
        c.wl.request(src, 'offer', mime)
        c.wl.request(self.dev, 'set_selection', src, self.win.configures[-1][3])
        c.wl.roundtrip()
        if not answer:
            return src
        ev = c.wl.wait_for(lambda e: e.obj == src and e.name == 'send', 2.0)
        self.assertIsNotNone(ev, 'the compositor did not ask for the clipboard text')
        os.write(ev.fds[0], text.encode())
        os.close(ev.fds[0])
        return src

    def clips(self):
        return [l.split(' ', 2)[2] for l in ctl.cmd('clip')]

    def wait_clips(self, n):
        for _ in range(100):
            got = self.clips()
            if len(got) == n:
                return got
            time.sleep(0.02)
        return self.clips()

    def test_history_newest_first_without_duplicates(self):
        self.copy('first')
        self.assertEqual(self.wait_clips(1), ['first'])
        self.copy('second')
        self.assertEqual(self.wait_clips(2), ['second', 'first'])
        self.copy('first')                               # copied again: moves to the top
        for _ in range(100):
            if self.clips()[:1] == ['first']:
                break
            time.sleep(0.02)
        self.assertEqual(self.clips(), ['first', 'second'])

    def test_clicking_an_entry_makes_it_the_current_clipboard(self):
        self.copy('first')
        self.wait_clips(1)
        self.copy('second')
        self.assertEqual(self.wait_clips(2), ['second', 'first'])
        self.c.wl.roundtrip()
        self.c.wl.queue.clear()
        x, y, w, h = self.rect()
        ctl.click(x + w // 2, y + 26 + 36 + 18)                   # the second slot: "first"
        sel = self.c.wl.wait_for(lambda e: e.obj == self.dev and e.name == 'selection' and e.args[0], 2.0)
        self.assertIsNotNone(sel, 'the focused client was not told about the new selection')
        r, wfd = os.pipe()
        self.c.wl.request(sel.args[0], 'receive', 'text/plain;charset=utf-8', fds=(wfd,))
        os.close(wfd)
        self.c.wl.roundtrip()
        import select
        self.assertTrue(select.select([r], [], [], 2.0)[0], 'the compositor did not serve the text')
        self.assertEqual(os.read(r, 100), b'first')
        os.close(r)
        for _ in range(100):                                      # the picked entry moved to the top
            if self.clips()[:1] == ['first']:
                break
            time.sleep(0.02)
        self.assertEqual(self.clips(), ['first', 'second'])

    def pick_with_keys(self, *keys):
        """Super+C, then `keys`: the focused client's clipboard text afterwards."""
        self.c.wl.roundtrip()
        self.c.wl.queue.clear()
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_C)
        ctl.key(KEY_LEFTMETA, False)
        for k in keys:
            ctl.tap(k)
        sel = self.c.wl.wait_for(lambda e: e.obj == self.dev and e.name == 'selection' and e.args[0], 2.0)
        if sel is None:
            return None
        r, wfd = os.pipe()
        self.c.wl.request(sel.args[0], 'receive', 'text/plain;charset=utf-8', fds=(wfd,))
        os.close(wfd)
        self.c.wl.roundtrip()
        import select
        got = os.read(r, 100) if select.select([r], [], [], 2.0)[0] else None
        os.close(r)
        return got

    def test_arrow_keys_pick_a_history_entry(self):
        for t in ('one', 'two', 'three'):
            self.copy(t)
            self.wait_clips(min(len(self.clips()) + 1, 3))
        self.assertEqual(self.wait_clips(3), ['three', 'two', 'one'])
        ppm = ctl.dump(os.path.join(tmp, 'clipnav0.ppm'))
        x, y, w, h = self.rect()
        # Super+C, Down, Down, Enter: the third entry ("one") becomes the clipboard and moves to the top
        self.assertEqual(self.pick_with_keys(KEY_DOWN, KEY_DOWN, KEY_ENTER), b'one')
        for _ in range(100):
            if self.clips()[:1] == ['one']:
                break
            time.sleep(0.02)
        self.assertEqual(self.clips(), ['one', 'three', 'two'])
        # the mode is over: arrows go to the application again (no key swallowed -> the client sees them)
        self.c.wl.queue.clear()
        kb = self.c.wl.new_id('wl_keyboard')
        self.c.wl.request(self.seat, 'get_keyboard', kb)
        self.c.wl.roundtrip()
        self.c.wl.queue.clear()
        ctl.tap(KEY_DOWN)
        ev = self.c.wl.wait_for(lambda e: e.obj == kb and e.name == 'key', 2.0)
        self.assertIsNotNone(ev, 'Down no longer reaches the application after Enter')

    def test_escape_and_other_keys_leave_the_keyboard_mode(self):
        for t in ('one', 'two'):
            self.copy(t)
        self.wait_clips(2)
        kb = self.c.wl.new_id('wl_keyboard')
        self.c.wl.request(self.seat, 'get_keyboard', kb)
        self.c.wl.roundtrip()
        self.c.wl.queue.clear()
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_C)
        ctl.key(KEY_LEFTMETA, False)
        ctl.tap(KEY_DOWN)
        ctl.tap(KEY_ESC)                                       # Esc ends the mode and is eaten
        ctl.tap(KEY_DOWN)                                      # now it is the application's
        ev = self.c.wl.wait_for(lambda e: e.obj == kb and e.name == 'key' and e.args[2] == KEY_DOWN, 1.5)
        self.assertIsNotNone(ev, 'Down did not reach the application after Esc')
        self.assertEqual(self.clips(), ['two', 'one'])         # nothing was picked

    def test_history_is_saved_and_restored(self):
        path = os.path.join(tmp, 'clip-history')
        for suffix in ('', '.1'):
            if os.path.exists(path + suffix):
                os.unlink(path + suffix)
        p, c, stop = self.private_compositor('mellagui-clip1', '--clipboard-file', path)
        cl = Client('mellagui-clip1')
        seat = cl.wl.bind('wl_seat', 5)
        mgr = cl.wl.bind('wl_data_device_manager', 3)
        dev = cl.wl.new_id('wl_data_device')
        cl.wl.request(mgr, 'get_data_device', dev, seat)
        win = Window(cl, 200, 150, title='copier')
        cl.wl.roundtrip()
        for text in ('first', 'two\\nlines \\\\ back'.replace('\\n', '\n'), 'third'):
            src = cl.wl.new_id('wl_data_source')
            cl.wl.request(mgr, 'create_data_source', src)
            cl.wl.request(src, 'offer', 'text/plain;charset=utf-8')
            cl.wl.request(dev, 'set_selection', src, win.configures[-1][3])
            cl.wl.roundtrip()
            ev = cl.wl.wait_for(lambda e: e.obj == src and e.name == 'send', 2.0)
            self.assertIsNotNone(ev)
            os.write(ev.fds[0], text.encode())
            os.close(ev.fds[0])
            for _ in range(100):
                if [l.split(' ', 2)[2] for l in c.cmd('clip')][:1] == [text.replace('\n', '\\n')]:
                    break
                time.sleep(0.02)
        self.assertEqual(oct(os.stat(path).st_mode & 0o777), '0o600')            # clipboards hold secrets
        before = [l.split(' ', 2)[2] for l in c.cmd('clip')]
        self.assertEqual(len(before), 3)
        stop()
        # a new run starts with the same history
        p2, c2, stop2 = self.private_compositor('mellagui-clip2', '--clipboard-file', path)
        self.assertEqual([l.split(' ', 2)[2] for l in c2.cmd('clip')], before)
        c2.cmd('clip clear')
        self.assertEqual(os.path.getsize(path), 0)                                 # clearing is saved, too

    def test_history_is_limited_and_multiline_text_is_kept(self):
        for i in range(55):
            ctl.cmd('clip add item %d' % i)
        got = self.clips()
        self.assertEqual(len(got), 50)                                # fifty entries are kept (it used to be five)
        self.assertEqual(got[0], 'item 54')
        self.assertEqual(got[-1], 'item 5')
        self.copy('two\nlines')
        for _ in range(100):
            if self.clips()[:1] == ['two\\nlines']:
                break
            time.sleep(0.02)
        self.assertEqual(self.clips()[0], 'two\\nlines')
        self.assertEqual(len(self.clips()), 50)

    # ---- the card scrolls, with small arrow buttons; entries and the whole history can be deleted -----------------------------------

    def view(self):
        ctl.cmd('clip')
        f = dict(kv.split('=') for kv in ctl.last.split()[1:])
        return int(f['top']), int(f['slots']), int(f['n'])

    def buttons(self):
        """Centres of the title bar buttons (up, down, clear all) and a function giving the x of entry k and the centre of the card."""
        x, y, w, h = self.rect()
        clear = x + w - 14 - 20 + 10
        return (clear - 48, y + 13), (clear - 24, y + 13), (clear, y + 13)

    def entry_pos(self, k, delete=False):
        x, y, w, h = self.rect()
        return (x + w - 12 - 13 if delete else x + w // 2 - 20, y + 26 + 4 + k * 36 + 16)

    def test_the_card_shows_five_entries_and_the_arrows_scroll_by_pages(self):
        for i in range(20):
            ctl.cmd('clip add e%02d' % i)
        top, slots, n = self.view()
        self.assertEqual((top, slots, n), (0, 5, 20))
        up, down, clear = self.buttons()
        ctl.click(*down)
        self.assertEqual(self.view()[0], 5)                           # a page down
        ctl.click(*down)
        ctl.click(*down)
        self.assertEqual(self.view()[0], 15)                          # the end: 20 - 5
        ctl.click(*down)
        self.assertEqual(self.view()[0], 15)                          # no further
        ctl.click(*up)
        self.assertEqual(self.view()[0], 10)
        ctl.click(*up)
        ctl.click(*up)
        ctl.click(*up)
        self.assertEqual(self.view()[0], 0)
        # a click on a visible entry after scrolling picks THAT entry
        ctl.click(*down)
        ctl.click(*self.entry_pos(2))
        for _ in range(100):
            if self.clips()[0] == 'e12':                              # (the 20 entries were added oldest first: e19 is the newest, so top 5 -> e14.. , entry 2 = e12)
                break
            time.sleep(0.02)
        self.assertEqual(self.clips()[0], 'e12')

    def test_a_taller_card_shows_more_entries(self):
        for i in range(30):
            ctl.cmd('clip add e%02d' % i)
        x, y, w, h = self.rect()
        self.drag(x + w // 2, y + h - 2, 0, 0)                        # (just a click on the bottom edge: no resize)
        ctl.abs(x + w // 2, y + h - 3)
        ctl.button(272, True)
        ctl.cmd('motion 0 220')                                       # drag the bottom edge down
        ctl.button(272, False)
        top, slots, n = self.view()
        self.assertGreaterEqual(slots, 10)
        self.assertLessEqual(slots, 30)

    def test_the_x_forgets_one_entry(self):
        for i in range(8):
            ctl.cmd('clip add e%d' % i)
        ctl.click(*self.entry_pos(1, delete=True))                    # the x of the second entry (e6)
        got = self.clips()
        self.assertEqual(len(got), 7)
        self.assertNotIn('e6', got)
        self.assertEqual(got[:2], ['e7', 'e5'])
        up, down, clear = self.buttons()                              # scrolled down, the x belongs to the entry it is on
        ctl.click(*down)
        ctl.click(*self.entry_pos(0, delete=True))
        self.assertEqual(len(self.clips()), 6)
        self.assertNotIn('e4', self.clips())                          # (e7 e5 e4 e3 e2 e1 e0: scrolled to the end the card shows e4 first)

    def test_the_trash_can_forgets_everything(self):
        for i in range(8):
            ctl.cmd('clip add e%d' % i)
        up, down, clear = self.buttons()
        ctl.click(*clear)
        self.assertEqual(self.clips(), [])
        self.assertEqual(self.view()[0], 0)

    def test_a_new_copy_brings_the_newest_back_into_view(self):
        for i in range(12):
            ctl.cmd('clip add e%02d' % i)
        up, down, clear = self.buttons()
        ctl.click(*down)
        self.assertEqual(self.view()[0], 5)
        ctl.cmd('clip add brand new')
        self.assertEqual(self.view()[0], 0)

    def test_keyboard_mode_follows_the_cursor_over_the_whole_history_and_deletes(self):
        for i in range(12):
            ctl.cmd('clip add e%02d' % i)
        self.c.wl.roundtrip()
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_C)
        ctl.key(KEY_LEFTMETA, False)
        for _ in range(8):
            ctl.tap(KEY_DOWN)                                         # past the five that were in view
        self.assertEqual(self.view()[0], 4)                           # the card scrolled (cursor on entry 8 = the last visible one)
        ctl.tap(111)                                                  # Delete: forget entry 8 (e03)
        got = self.clips()
        self.assertEqual(len(got), 11)
        self.assertNotIn('e03', got)
        ctl.tap(107)                                                  # End
        self.assertEqual(self.view()[0], 6)                           # 11 - 5
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(111)                                                  # Shift+Delete: the whole history
        ctl.key(KEY_LEFTSHIFT, False)
        self.assertEqual(self.clips(), [])
        ctl.tap(KEY_A)                                                # the mode ended by itself: keys go to the application again
        self.c.wl.roundtrip()

    def test_non_text_selection_is_ignored(self):
        self.copy('', mime='image/png', answer=False)
        time.sleep(0.2)
        self.assertEqual(self.clips(), [])

    def test_silent_source_times_out(self):
        self.copy('', answer=False)                      # never answers: no entry, no hang
        time.sleep(1.3)
        self.assertEqual(self.clips(), [])
        self.copy('later')
        self.assertEqual(self.wait_clips(1), ['later'])

    def test_gadget_is_painted_and_toggles(self):
        ctl.sync()
        x, y, w, h = self.rect()
        probe = (x + w // 2, y + h - 20)                 # an empty spot of the card body
        shown = ctl.dump(os.path.join(tmp, 'gadget1.ppm')).px(*probe)
        self.assertTrue(self.is_card(shown), shown)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_G)
        ctl.key(KEY_LEFTMETA, False)
        self.assertTrue(ctl.last.startswith('ok'))
        ctl.cmd('clip')
        self.assertRegex(ctl.last, r' gadget=0( |$)')
        ctl.sync()
        hidden = ctl.dump(os.path.join(tmp, 'gadget0.ppm')).px(*probe)
        self.assertNotEqual(hidden, shown)
        self.assertIsNone(self.rect())

    def test_gadget_shows_text(self):
        self.copy('hello gadget')
        self.wait_clips(1)
        ctl.sync()
        full = ctl.dump(os.path.join(tmp, 'gfull.ppm'))
        # the first entry slot (below the 26 px title bar) now contains bright text pixels
        x, y, w, h = self.rect()
        box = (x, y + 26, x + w, y + 26 + 36)
        self.assertGreater(full.count((0xe6, 0xe9, 0xef), *box), 20)

    def test_default_place_is_top_right_below_the_panel(self):
        x, y, w, h = self.rect()
        self.assertEqual((x + w, y), (1024 - 12, 32 + 12))
        self.assertEqual((w, h), (250, 214))

    def test_title_bar_drags_the_gadget(self):
        x, y, w, h = self.rect()
        self.drag(x + 60, y + 10, -200, 150)
        self.assertEqual(self.rect(), (x - 200, y + 150, w, h))
        ctl.sync()
        full = ctl.dump(os.path.join(tmp, 'gmoved.ppm'))
        nx, ny = x - 200, y + 150
        self.assertTrue(self.is_card(full.px(nx + w // 2, ny + h - 20)))            # card is at the new place
        self.assertFalse(self.is_card(full.px(x + w // 2, y + h - 20)))             # and gone from the old one

    def test_drag_keeps_the_gadget_on_the_output_below_the_panel(self):
        x, y, w, h = self.rect()
        self.drag(x + 60, y + 10, -2000, -2000)
        self.assertEqual(self.rect(), (0, 32, w, h))
        self.drag(60, 38, 3000, 3000)
        self.assertEqual(self.rect(), (1024 - w, 600 - h, w, h))

    def test_edges_resize_the_gadget(self):
        x, y, w, h = self.rect()
        self.drag(x + w - 2, y + h - 2, 100, 150)                # bottom right corner
        nx, ny, nw, nh = self.rect()
        self.assertEqual((nx, ny), (x - 0, y))
        self.assertEqual((nw, nh), (min(w + 100, 640, 1024 - x), min(h + 150, 560, 600 - y)))
        x, y, w, h = self.rect()
        self.drag(x + 2, y + h // 2, -50, 0)                     # left edge keeps the right edge in place
        self.assertEqual(self.rect(), (x - 50, y, w + 50, h))

    def test_resize_has_limits(self):
        x, y, w, h = self.rect()
        self.drag(x + w - 2, y + h - 2, -3000, -3000)
        self.assertEqual(self.rect(), (x, y, 180, 70))           # minimum: title + one entry
        x, y, w, h = self.rect()
        self.drag(x + 2, y + h - 2, -3000, 3000)                 # grows left/down, bounded by the output and 640 wide
        nx, ny, nw, nh = self.rect()
        self.assertEqual((nw, nx + nw), (640, x + w))
        self.assertLessEqual(ny + nh, 600)

    def test_small_gadget_shows_fewer_entries(self):
        for i in range(3):
            self.copy('entry %d' % i)
            self.wait_clips(i + 1)
        x, y, w, h = self.rect()
        self.drag(x + w - 2, y + h - 2, 0, -(h - 86))            # one slot left
        ctl.sync()
        _, _, _, nh = self.rect()
        self.assertEqual(nh, 86)
        full = ctl.dump(os.path.join(tmp, 'gsmall.ppm'))
        bright = (0xe6, 0xe9, 0xef)
        self.assertGreater(full.count(bright, x, y + 26, x + w, y + nh), 20)      # the newest entry is shown
        self.assertEqual(full.count(bright, x, y + nh + 20, x + w, y + nh + 80), 0)

    def test_double_click_on_the_title_resets(self):
        x, y, w, h = self.rect()
        self.drag(x + 60, y + 10, -300, 100)
        self.drag(self.rect()[0] + self.rect()[2] - 2, self.rect()[1] + self.rect()[3] - 2, -50, 20)
        self.assertNotEqual(self.rect(), (x, y, w, h))
        self.reset_gadget()
        self.assertEqual(self.rect(), (x, y, w, h))


KEYCODES = {c: k for k, c in enumerate('1234567890', 2)}
KEYCODES.update({c: k for k, c in enumerate('qwertyuiop', 16)})
KEYCODES.update({c: k for k, c in enumerate('asdfghjkl', 30)})
KEYCODES.update({c: k for k, c in enumerate('zxcvbnm', 44)})
KEYCODES[' '] = 57


def type_text(text):
    for ch in text:
        ctl.tap(KEYCODES[ch])
        time.sleep(0.01)
    ctl.tap(KEY_ENTER)


class TestKeyboard(Base):
    """Keyboard layouts (XKB groups) and sticky keys."""
    LAYOUT_BTN = (690, 14)          # panel indicator on the 1024 px wide test output
    STICKY_BTN = (754, 14)

    def setUp(self):
        super().setUp()
        self.addCleanup(lambda: (ctl.cmd('sticky 0'), ctl.cmd('layout 0')))
        self.c = self.client()
        self.seat = self.c.wl.bind('wl_seat', 5)
        self.kb = self.c.wl.new_id('wl_keyboard')
        self.c.wl.request(self.seat, 'get_keyboard', self.kb)
        self.w = Window(self.c, 300, 200)
        self.c.wl.roundtrip()
        self.c.wl.events(0.1)
        self.c.wl.queue.clear()

    def mods(self, **want):
        """Wait for a modifiers event whose (dep, lat, lock, group) match the given fields."""
        idx = dict(dep=1, lat=2, lock=3, group=4)

        def pred(e):
            return e.obj == self.kb and e.name == 'modifiers' and all(e.args[idx[k]] == v for k, v in want.items())
        return self.c.wl.wait_for(pred, 2.0)

    def test_keymap_tables_selftest(self):
        path = os.path.join(ROOT, 'build', 'keymap-test')
        if not os.path.exists(path):
            self.skipTest('keymap-test not built')
        r = subprocess.run([path], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_bad_mode_option_is_refused(self):
        for bad in ('foo', '10x10', '1920x810@5', '1920', 'prefered'):
            r = subprocess.run([BIN, '--backend', 'headless', '--mode', bad], capture_output=True, text=True, timeout=10)
            self.assertEqual(r.returncode, 2, bad)
            self.assertIn('bad --mode', r.stderr)

    def test_keymap_has_all_layouts(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        Window(c, 200, 100)
        c.wl.roundtrip()
        ev = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'keymap', 2.0)
        self.assertIsNotNone(ev)
        data = os.pread(ev.fds[0], ev.args[1], 0).decode()
        for n, name in enumerate(['English (US)', 'Estonian', 'German', 'Finnish']):
            self.assertIn('name[Group%d] = "%s";' % (n + 1, name), data)
        self.assertIn('ISO_Level3_Shift', data)          # AltGr exists with more than one layout
        self.assertIn('U00F5', data)                     # o-tilde (Estonian)
        self.assertIn('U0161', data)                     # s-caron
        self.assertEqual(data.count('{'), data.count('}'))

    def test_layout_switch_reaches_client(self):
        self.assertEqual(ctl.kbd()['layout'], 'us')
        ctl.cmd('layout next')
        self.assertEqual(ctl.kbd()['layout'], 'ee')
        self.assertIsNotNone(self.mods(group=1), 'client was not told about the new group')
        ctl.cmd('layout prev')
        self.assertIsNotNone(self.mods(group=0))
        ctl.cmd('layout prev')                           # wraps around
        self.assertEqual(ctl.kbd()['layout'], 'fi')
        self.assertIsNotNone(self.mods(group=3))

    def test_super_space_switches_layout_and_is_not_forwarded(self):
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_SPACE)
        ctl.key(KEY_LEFTMETA, False)
        self.assertEqual(ctl.kbd()['layout'], 'ee')
        ctl.key(KEY_LEFTMETA, True)
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(KEY_SPACE)                               # Super+Shift+Space = previous
        ctl.key(KEY_LEFTSHIFT, False)
        ctl.key(KEY_LEFTMETA, False)
        self.assertEqual(ctl.kbd()['layout'], 'us')
        ctl.sync()
        self.c.wl.roundtrip()
        self.assertEqual([e for e in self.c.wl.queue if e.obj == self.kb and e.name == 'key'
                          and e.args[2] == KEY_SPACE], [])

    def test_panel_indicator_cycles_layouts(self):
        ctl.click(*self.LAYOUT_BTN)
        self.assertEqual(ctl.kbd()['layout'], 'ee')
        ctl.click(*self.LAYOUT_BTN)
        self.assertEqual(ctl.kbd()['layout'], 'de')
        ctl.click(*self.LAYOUT_BTN, code=273)            # right click: previous
        self.assertEqual(ctl.kbd()['layout'], 'ee')
        ctl.abs(900, 500)

    def test_altgr_is_right_alt_with_layouts(self):
        ctl.key(KEY_RIGHTALT, True)
        self.assertIsNotNone(self.mods(dep=1 << 7), 'right Alt should be AltGr (Mod5)')
        ctl.key(KEY_RIGHTALT, False)
        self.assertIsNotNone(self.mods(dep=0))

    def test_sticky_latch_is_used_once(self):
        ctl.cmd('sticky 1')
        ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['latched'], 1)
        ctl.tap(KEY_A)
        self.assertEqual(ctl.kbd()['latched'], 0)
        ctl.sync()
        self.c.wl.roundtrip()
        # (kind, latched mask or key state): the latch must still be visible while the key is pressed, and be
        # cleared by a modifiers event right after it
        seq = []
        for e in self.c.wl.queue:
            if e.obj == self.kb and e.name == 'modifiers':
                seq.append(('mods', e.args[2]))
            elif e.obj == self.kb and e.name == 'key' and e.args[2] == KEY_A:
                seq.append(('key', e.args[3]))
        down = seq.index(('key', 1))
        self.assertIn(('mods', 1), seq[:down], 'latched Shift not sent before the key press')
        self.assertEqual(seq[down - 1], ('mods', 1))
        self.assertEqual(seq[down + 1], ('mods', 0), 'latch was not released right after the key press')

    def test_caps_lock_state_is_tracked(self):
        self.assertEqual(ctl.kbd()['locks'], 0)
        ctl.tap(58)                                  # KEY_CAPSLOCK
        self.assertEqual(ctl.kbd()['locks'], 1 << 1)
        ctl.tap(69)                                  # KEY_NUMLOCK
        self.assertEqual(ctl.kbd()['locks'], (1 << 1) | (1 << 4))
        ctl.tap(58)
        ctl.tap(69)
        self.assertEqual(ctl.kbd()['locks'], 0)

    def test_sticky_beeps(self):
        s0 = ctl.kbd()['sounds']
        ctl.cmd('sticky 1')
        self.assertEqual((ctl.kbd()['sound'], ctl.kbd()['sounds']), ('sticky-on', s0 + 1))
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual(ctl.kbd()['sound'], 'latch')
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual(ctl.kbd()['sound'], 'lock')
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual(ctl.kbd()['sound'], 'unlock')
        n = ctl.kbd()['sounds']
        ctl.tap(KEY_A)                                   # a plain key makes no sound
        self.assertEqual(ctl.kbd()['sounds'], n)
        ctl.cmd('sticky 0')
        self.assertEqual(ctl.kbd()['sound'], 'sticky-off')

    def test_sticky_double_tap_locks_third_unlocks(self):
        ctl.cmd('sticky 1')
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual((ctl.kbd()['latched'], ctl.kbd()['slocked']), (4, 0))
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual((ctl.kbd()['latched'], ctl.kbd()['slocked']), (0, 4))
        self.assertIsNotNone(self.mods(lock=4))
        ctl.tap(KEY_A)                                   # a locked modifier survives key presses
        self.assertEqual(ctl.kbd()['slocked'], 4)
        ctl.tap(KEY_LEFTCTRL)
        self.assertEqual((ctl.kbd()['latched'], ctl.kbd()['slocked']), (0, 0))
        self.assertIsNotNone(self.mods(lock=0))

    def test_sticky_chord_does_not_latch(self):
        ctl.cmd('sticky 1')
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(KEY_A)
        ctl.key(KEY_LEFTSHIFT, False)
        self.assertEqual(ctl.kbd()['latched'], 0)

    def test_sticky_off_does_not_latch(self):
        ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['latched'], 0)

    def test_sticky_modifiers_combine(self):
        ctl.cmd('sticky 1')
        ctl.tap(KEY_LEFTCTRL)
        ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['latched'], 5)
        ctl.tap(KEY_A)
        self.assertEqual(ctl.kbd()['latched'], 0)

    def test_five_shifts_toggle_sticky(self):
        self.assertEqual(ctl.kbd()['sticky'], 0)
        for _ in range(5):
            ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['sticky'], 1)
        self.assertEqual(ctl.kbd()['latched'], 0)
        for _ in range(5):
            ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['sticky'], 0)
        for _ in range(3):                               # another key in between restarts the count
            ctl.tap(KEY_LEFTSHIFT)
        ctl.tap(KEY_A)
        for _ in range(3):
            ctl.tap(KEY_LEFTSHIFT)
        self.assertEqual(ctl.kbd()['sticky'], 0)

    def test_latched_super_works_for_shortcuts(self):
        ctl.cmd('sticky 1')
        ctl.tap(KEY_LEFTMETA)
        ctl.tap(KEY_SPACE)                               # Super (latched) + Space
        self.assertEqual(ctl.kbd()['layout'], 'ee')
        self.assertEqual(ctl.kbd()['latched'], 0)

    def test_panel_sticky_button_toggles(self):
        ctl.click(*self.STICKY_BTN)
        self.assertEqual(ctl.kbd()['sticky'], 1)
        ctl.click(*self.STICKY_BTN)
        self.assertEqual(ctl.kbd()['sticky'], 0)
        ctl.abs(900, 500)

    def test_panel_shows_layout_and_sticky(self):
        ctl.abs(900, 500)
        ctl.sync()
        before = ctl.dump(os.path.join(tmp, 'kbd0.ppm'))
        ctl.cmd('layout next')
        ctl.cmd('sticky 1')
        ctl.sync()
        after = ctl.dump(os.path.join(tmp, 'kbd1.ppm'))
        sticky_before = [before.px(x, 16) for x in range(789, 864, 3)]
        sticky_after = [after.px(x, 16) for x in range(789, 864, 3)]
        self.assertNotEqual(sticky_before, sticky_after, 'sticky button looks the same when enabled')
        layout_before = [before.px(x, 16) for x in range(746, 778, 2)]
        layout_after = [after.px(x, 16) for x in range(746, 778, 2)]
        self.assertNotEqual(layout_before, layout_after, 'layout indicator did not change')


def wait_for_window(app_id, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        for w in ctl.windows()['windows']:
            if w['app_id'] == app_id:
                return w
        time.sleep(0.05)
    return None


class TestClients(Base):
    def spawn(self, name, **env):
        path = os.path.join(ROOT, 'build', name)
        if not os.path.exists(path):
            self.skipTest('%s not built' % name)
        if 'XDG_CONFIG_HOME' not in env:                         # the terminal remembers its zoom: every test starts from a clean slate
            d = tempfile.mkdtemp(prefix='cfg-', dir=tmp)
            self.addCleanup(shutil.rmtree, d, ignore_errors=True)
            env = dict(env, XDG_CONFIG_HOME=d)
        e = dict(os.environ, **env)
        p = subprocess.Popen([path], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        return p

    def test_calc_engine_selftest(self):
        path = os.path.join(ROOT, 'build', 'mellagui-calc')
        if not os.path.exists(path):
            self.skipTest('mellagui-calc not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_calc_functions_memory_copy_and_paste(self):
        p = self.spawn('mellagui-calc')
        win = wait_for_window('mellagui.calc')
        self.assertIsNotNone(win)
        time.sleep(0.3)
        n = [0]

        def result():
            n[0] += 1
            ctl.abs(win['x'] + 100, win['y'] + 120)           # the pointer inside the window: the keyboard goes here; away from the keys
            time.sleep(0.1)
            ppm = ctl.dump(os.path.join(tmp, 'calcf%d.ppm' % n[0]))
            return [ppm.px(win['x'] + x, win['y'] + y) for y in range(36, 76, 2) for x in range(8, 272, 2)]

        def keys(*codes):
            for c in codes:
                ctl.tap(c)
            time.sleep(0.1)

        ctl.abs(win['x'] + 100, win['y'] + 120)
        zero = result()
        keys(4)                                                 # 3
        three = result()
        self.assertNotEqual(three, zero)
        keys(1)                                                 # Esc
        keys(10, 19)                                            # 9 r: the square root is 3, the same picture as a typed 3
        self.assertEqual(result(), three)
        ctl.key(29, True); ctl.tap(46); ctl.key(29, False)      # Ctrl+C copies the 3
        keys(1)
        self.assertEqual(result(), zero)
        ctl.key(29, True); ctl.tap(47); ctl.key(29, False)      # Ctrl+V pastes it back
        time.sleep(0.1)
        self.assertEqual(result(), three)
        keys(1)
        keys(4, 25)                                             # 3 p: M+ keeps it in the memory (p = KEY_P)
        keys(1, 50)                                             # Esc, m: MR types it again
        self.assertEqual(result(), three)
        self.assertTrue(alive_p(p))

    def test_calc_clicks_and_keys(self):
        p = self.spawn('mellagui-calc')
        win = wait_for_window('mellagui.calc')
        self.assertIsNotNone(win, 'calculator window did not appear')
        self.assertEqual((win['w'], win['h']), (280, 440))
        time.sleep(0.3)
        n = [0]

        def centre(row, col):       # layout: 8px padding and gaps, 60x42 keys, 73px display on top
            return win['x'] + 8 + col * 68 + 30, win['y'] + 89 + row * 50 + 21

        def click(row, col):
            ctl.click(*centre(row, col))
            time.sleep(0.05)

        def display():
            n[0] += 1
            ctl.abs(900, 500)       # keep the hover highlight out of the picture
            time.sleep(0.1)
            ppm = ctl.dump(os.path.join(tmp, 'calc%d.ppm' % n[0]))
            return [ppm.px(win['x'] + x, win['y'] + y) for y in range(36, 76, 2) for x in range(8, 272, 2)]     # the result, not the sum above it

        zero = display()
        # grid: row0 MC MR M+ M- | row1 sqrt x2 1/x / | row2 C <- % x | row3 7 8 9 - | row4 4 5 6 + | row5 1 2 3 = | row6 +/- 0 .
        click(5, 0); click(5, 1); click(4, 3); click(5, 2); click(5, 3)    # 1 2 + 3 =
        mouse_15 = display()
        self.assertNotEqual(mouse_15, zero)
        click(2, 0)                                                       # C
        self.assertEqual(display(), zero)
        click(5, 0); click(4, 1)                                          # 1 5
        self.assertEqual(display(), mouse_15)                             # same text as 12+3=
        # keyboard: Esc clears, digits and Enter are accepted
        ctl.tap(KEY_ESC)
        time.sleep(0.05)
        self.assertEqual(display(), zero)
        ctl.abs(win['x'] + 100, win['y'] + 100)     # keyboard focus follows the focused window
        ctl.tap(2); ctl.tap(6)                      # KEY_1 KEY_5
        time.sleep(0.05)
        self.assertEqual(display(), mouse_15)
        ctl.tap(KEY_ESC)
        # Super+Q closes it politely
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_Q)
        ctl.key(KEY_LEFTMETA, False)
        self.assertEqual(p.wait(timeout=5), 0)

    LETTERS = dict(zip('qwertyuiop', range(16, 26)), **dict(zip('asdfghjkl', range(30, 39))),
                   **dict(zip('zxcvbnm', range(44, 51))))

    def type_text(self, text):
        for ch in text:
            ctl.tap(57 if ch == ' ' else self.LETTERS[ch])
            time.sleep(0.01)

    def run_dialog_runs(self, open_it):
        flag = 'lrflag'
        path = os.path.join(os.getcwd(), flag)
        if os.path.exists(path):
            os.unlink(path)
        self.addCleanup(lambda: os.path.exists(path) and os.unlink(path))
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-run')):
            self.skipTest('mellagui-run not built')
        open_it()
        win = wait_for_window('mellagui.run')
        self.assertIsNotNone(win, 'run dialog did not appear')
        time.sleep(0.3)
        self.type_text('touch ' + flag)
        ctl.tap(KEY_ENTER)
        for _ in range(100):
            if os.path.exists(path) and not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.run']:
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(path), 'command was not run')
        self.assertFalse([w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.run'],
                         'dialog stays open after running')

    # ---- the clipboard in text fields and their right-click menu -------------------------------------------------------------

    def open_run(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-run')):
            self.skipTest('mellagui-run not built')
        ctl.click(826, 14)
        win = wait_for_window('mellagui.run')
        self.assertIsNotNone(win, 'run dialog did not appear')
        time.sleep(0.3)
        return win

    def flag_path(self, name):
        path = os.path.join(os.getcwd(), name)
        if os.path.exists(path):
            os.unlink(path)
        self.addCleanup(lambda: os.path.exists(path) and os.unlink(path))
        return path

    def wait_flag(self, path):
        for _ in range(100):
            if os.path.exists(path):
                return True
            time.sleep(0.05)
        return False

    def test_run_pastes_with_ctrl_v_and_shift_insert(self):
        flag = self.flag_path('run-paste-flag')
        self.own_clipboard('touch run-paste-flag\n')                 # (the line end is not wanted in a field)
        self.open_run()
        self.chord(self.CTRL, 47)                                    # Ctrl+V
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        self.assertTrue(self.wait_flag(flag), 'Ctrl+V did not paste into the Run line')
        flag2 = self.flag_path('run-paste-flag2')
        self.own_clipboard('touch run-paste-flag2')
        self.open_run()
        ctl.key(self.SHIFT, True)
        ctl.tap(110)                                                 # Shift+Insert
        ctl.key(self.SHIFT, False)
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        self.assertTrue(self.wait_flag(flag2), 'Shift+Insert did not paste into the Run line')

    def test_run_line_has_a_caret_and_a_selection(self):
        flag = self.flag_path('runcaretflag')
        win = self.open_run()
        self.type_text('qq touch runcaretflag')
        ctl.tap(102)                                                 # Home
        for _ in range(3):
            ctl.tap(111)                                             # Delete x3: "qq " goes
        ctl.tap(107)                                                 # End
        self.type_text('x')
        ctl.tap(14)                                                  # Backspace: the x is gone again
        self.chord(self.CTRL, 30)                                    # Ctrl+A selects the whole line ...
        self.type_text('touch runcaretflag')                         # ... and typing replaces it
        ctl.key(self.SHIFT, True)
        for _ in range(4):
            ctl.tap(105)                                             # Shift+Left x4 selects "flag"
        ctl.key(self.SHIFT, False)
        self.chord(self.CTRL, 45)                                    # Ctrl+X cuts the selection (not the whole line)
        self.chord(self.CTRL, 47)                                    # Ctrl+V puts it back
        time.sleep(0.3)
        ctl.tap(KEY_ENTER)
        self.assertTrue(self.wait_flag(flag), 'the line was not "touch runcaretflag"')

    def test_run_menu_select_all_and_a_click_places_the_caret(self):
        flag = self.flag_path('runclickflag')
        win = self.open_run()
        self.type_text('touch runclickflag')
        ctl.click(win['x'] + 70, win['y'] + 36)                      # a click in the line: the caret goes there (after "to")
        time.sleep(0.2)
        ctl.tap(14)                                                  # Backspace removes the o before it ...
        time.sleep(0.2)
        self.type_text('o')                                          # ... which is typed again at the caret
        time.sleep(0.2)
        self.right_click(win['x'] + 100, win['y'] + 36)
        m = self.menu_state()
        self.assertEqual([e['label'] for e in m['entries']][3], 'Select all')
        ctl.click(m['entries'][3]['x'], m['entries'][3]['y'])
        self.chord(self.CTRL, 45)                                    # Ctrl+X: everything is selected, so everything is cut
        self.chord(self.CTRL, 47)                                    # and pasted again
        time.sleep(0.3)
        ctl.tap(KEY_ENTER)
        self.assertTrue(self.wait_flag(flag), 'the line was changed by the click / selection')

    def test_run_right_click_menu_cut_paste_and_clear(self):
        flag = self.flag_path('runmenuflag')
        win = self.open_run()
        self.right_click(win['x'] + 100, win['y'] + 30)
        m = self.menu_state()
        self.assertEqual((m['open'], m['owner']), (1, 1))
        self.assertEqual([(e['label'], e['enabled']) for e in m['entries']], [('Cut', 0), ('Copy', 0), ('Paste', 1), ('Select all', 0), ('Clear', 0)])   # an empty line
        ctl.tap(KEY_ESC)                                             # Esc closes the menu, not the dialog
        self.assertEqual(self.menu_state()['open'], 0)
        self.assertIsNotNone(wait_for_window('mellagui.run', 1.0))
        self.type_text('zz')
        self.right_click(win['x'] + 100, win['y'] + 30)
        m = self.menu_state()
        self.assertEqual([e['enabled'] for e in m['entries']], [1, 1, 1, 1, 1])
        clear = m['entries'][4]
        ctl.click(clear['x'], clear['y'])                            # Clear: the line is empty again
        self.type_text('touch runmenuflag')
        self.right_click(win['x'] + 100, win['y'] + 30)
        cut = self.menu_state()['entries'][0]
        ctl.click(cut['x'], cut['y'])                                # Cut: the clipboard has the line, the line is empty
        self.right_click(win['x'] + 100, win['y'] + 30)
        paste = self.menu_state()['entries'][2]
        ctl.click(paste['x'], paste['y'])                            # Paste puts it back
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        self.assertTrue(self.wait_flag(flag), 'the menu did not do what it says (the line should be "touch runmenuflag")')

    # ---- mellagui-edit -----------------------------------------------------------------------------------------------

    def test_edit_selftest(self):
        path = os.path.join(ROOT, 'build', 'mellagui-edit')
        if not os.path.exists(path):
            self.skipTest('mellagui-edit not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def edit_start(self, content=None, name='doc.txt', env=None):
        exe = os.path.join(ROOT, 'build', 'mellagui-edit')
        if not os.path.exists(exe):
            self.skipTest('mellagui-edit not built')
        d = tempfile.mkdtemp(prefix='edit-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        path = os.path.join(d, name)
        if content is not None:
            with open(path, 'wb') as f:
                f.write(content)
        for w in ctl.windows()['windows']:
            self.assertNotEqual(w['app_id'], 'mellagui.edit', 'an editor from an earlier test is still open')
        p = subprocess.Popen([exe, path], env=dict(os.environ, **(env or {})), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.edit')
        self.assertIsNotNone(win, 'the editor did not appear')
        time.sleep(0.4)
        return p, path

    def type_chars(self, text):
        """Types letters, digits and / . - _ on the US layout (enough for temporary file names)."""
        for ch in text:
            code = {'/': 53, '.': 52, '-': 12, '_': 12}.get(ch)
            if code is None:
                code = (11 if ch == '0' else 1 + int(ch)) if ch.isdigit() else self.LETTERS[ch.lower()]
            shifted = ch.isupper() or ch == '_'
            if shifted:
                ctl.key(self.SHIFT, True)
            ctl.tap(code)
            if shifted:
                ctl.key(self.SHIFT, False)
            time.sleep(0.01)

    def chord(self, mod, code):
        ctl.key(mod, True)
        ctl.tap(code)
        ctl.key(mod, False)
        time.sleep(0.05)

    def read_file(self, path):
        with open(path, 'rb') as f:
            return f.read()

    CTRL, SHIFT = 29, 42
    K_S, K_Q, K_Z, K_A, K_C, K_V, K_X, K_Y, K_F = 31, 16, 44, 30, 46, 47, 45, 21, 33
    K_HOME, K_END, K_BS, K_LEFT, K_UP, K_DOWN = 102, 107, 14, 105, 103, 108

    def test_edit_types_and_saves(self):
        p, path = self.edit_start(b'hello\nworld\n')
        ctl.tap(self.K_END)                                    # the cursor starts at the top of the file
        self.type_text('ab')
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'helloab\nworld\n')
        ctl.tap(self.K_DOWN); ctl.tap(self.K_HOME)
        ctl.tap(KEY_ENTER)                                     # a new line above "world"
        time.sleep(0.2)
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'helloab\n\nworld\n')
        self.chord(self.CTRL, self.K_Q)                        # saved: leaves at once
        self.assertEqual(p.wait(timeout=5), 0)

    def test_edit_undo_redo_and_unsaved_quit(self):
        p, path = self.edit_start(b'one\n')
        ctl.tap(self.K_END)
        self.type_text('xyz')
        self.chord(self.CTRL, self.K_Z)                        # typing run is one undo step
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'one\n')
        self.chord(self.CTRL, self.K_Y)                        # redo brings it back (and marks it modified)
        ctl.key(KEY_LEFTMETA, True); ctl.tap(KEY_Q); ctl.key(KEY_LEFTMETA, False)    # a polite close is refused
        time.sleep(0.3)
        self.assertIsNone(p.poll(), 'the compositor close request must not drop unsaved changes')
        self.assertEqual(self.read_file(path), b'one\n')
        ctl.tap(KEY_ESC)                                       # Esc withdraws the question
        self.chord(self.CTRL, self.K_Q)                        # modified: Ctrl+Q asks first too...
        time.sleep(0.3)
        self.assertIsNone(p.poll(), 'quit with unsaved changes must ask first')
        self.chord(self.CTRL, self.K_Q)                        # ...and a second one discards
        self.assertEqual(p.wait(timeout=5), 0)
        self.assertEqual(self.read_file(path), b'one\n')       # nothing was written

    def test_edit_clipboard_and_selection(self):
        p, path = self.edit_start(b'abc def\n')
        self.chord(self.CTRL, self.K_A)
        self.chord(self.CTRL, self.K_C)
        ctl.tap(self.K_END)                                    # cursor to the end, selection gone
        ctl.tap(57)
        self.chord(self.CTRL, self.K_V)
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'abc def abc def\n')
        ctl.tap(self.K_HOME)                                   # Shift+Right x3 selects "abc", Ctrl+X cuts it
        ctl.key(self.SHIFT, True)
        for _ in range(3):
            ctl.tap(106)
        ctl.key(self.SHIFT, False)
        self.chord(self.CTRL, self.K_X)
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b' def abc def\n')

    def test_edit_find_goto_and_open(self):
        p, path = self.edit_start(b'alpha\nbeta\ngamma wor\nworld\n')
        self.chord(self.CTRL, self.K_F)
        self.type_text('wor')
        ctl.tap(KEY_ENTER)                                     # selects the first match, "wor" on line 3
        self.type_text('x')                                    # typing replaces the selection
        self.chord(self.CTRL, 34)                              # Ctrl+G
        ctl.tap(3)                                             # "2"
        ctl.tap(KEY_ENTER)
        self.type_text('q')                                    # at the start of line 2
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'alpha\nqbeta\ngamma x\nworld\n')
        other = os.path.join(os.path.dirname(path), 'other.txt')
        with open(other, 'wb') as f:
            f.write(b'second\n')
        self.chord(self.CTRL, 24)                              # Ctrl+O: the open prompt
        self.type_chars(other)
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.type_text('z')
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(other), b'zsecond\n')
        self.assertEqual(self.read_file(path), b'alpha\nqbeta\ngamma x\nworld\n')

    def test_edit_keeps_crlf_and_missing_final_newline(self):
        p, path = self.edit_start(b'a\r\nb')
        ctl.tap(self.K_END)
        self.type_text('x')
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'ax\r\nb')

    def test_edit_new_file_and_refused_binary(self):
        p, path = self.edit_start(None, 'fresh.txt')            # a name that does not exist yet
        self.type_text('hi')
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'hi\n')
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)
        d = os.path.dirname(path)
        bin_path = os.path.join(d, 'blob.bin')
        with open(bin_path, 'wb') as f:
            f.write(b'\x00\x01\x02binary')
        exe = os.path.join(ROOT, 'build', 'mellagui-edit')
        p2 = subprocess.Popen([exe, bin_path], env=dict(os.environ), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p2.poll() is None) and (p2.kill(), p2.wait()))
        self.assertIsNotNone(wait_for_window('mellagui.edit'))
        time.sleep(0.3)
        self.type_text('z')                                    # empty document, not the binary file...
        self.chord(self.CTRL, self.K_S)                        # ...and saving it does overwrite? no: the path was never taken
        time.sleep(0.2)
        self.assertEqual(self.read_file(bin_path), b'\x00\x01\x02binary', 'a refused file must not be overwritten')

    def test_edit_prompt_completes_names_and_resolves_relative_ones(self):
        p, path = self.edit_start(b'one\n')
        d = os.path.dirname(path)
        for name in ('alpha.txt', 'alpine.txt'):
            with open(os.path.join(d, name), 'wb') as f:
                f.write(b'old ' + name.encode() + b'\n')
        self.type_text('hi')
        ctl.key(self.CTRL, True); ctl.key(self.SHIFT, True); ctl.tap(self.K_S); ctl.key(self.SHIFT, False); ctl.key(self.CTRL, False)
        self.type_chars('new.txt')                             # typing over the suggested name; relative to the file's folder
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.assertEqual(self.read_file(os.path.join(d, 'new.txt')), b'hione\n')
        self.assertEqual(self.read_file(path), b'one\n')
        self.chord(self.CTRL, 24)                              # Ctrl+O starts in the folder: only a name is needed
        self.type_chars('alph')
        ctl.tap(KEY_TAB)                                       # alph -> alpha.txt
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.type_text('z')
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(os.path.join(d, 'alpha.txt')), b'zold alpha.txt\n')
        # Save as onto another existing file asks first
        ctl.key(self.CTRL, True); ctl.key(self.SHIFT, True); ctl.tap(self.K_S); ctl.key(self.SHIFT, False); ctl.key(self.CTRL, False)
        self.type_chars('alpine.txt')
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.assertEqual(self.read_file(os.path.join(d, 'alpine.txt')), b'old alpine.txt\n', 'must not be replaced at once')
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.assertEqual(self.read_file(os.path.join(d, 'alpine.txt')), b'zold alpha.txt\n')

    def edit_window(self):
        return [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.edit'][0]

    def test_edit_zoom_grows_the_window_and_is_remembered(self):
        confdir = os.path.join(tmp, 'econf')
        os.makedirs(confdir, exist_ok=True)
        self.addCleanup(shutil.rmtree, confdir, ignore_errors=True)
        conf = os.path.join(confdir, 'mellagui', 'editor.conf')
        p, path = self.edit_start(b'one\n', env={'XDG_CONFIG_HOME': confdir})
        w0 = self.edit_window()
        self.assertEqual(w0['w'], 760)
        def bar(x):                                              # A- 438, zoom 474, A+ 526 (each 32..48 wide; Cut, Copy and Paste sit before them), the bar is 24 px high
            win = self.edit_window()
            ctl.click(win['x'] + x + 10, win['y'] + 12)
            time.sleep(0.3)
        bar(526)                                                 # A+: 110 %, 9 px cells; the same 90 columns stay visible
        w1 = self.edit_window()
        self.assertEqual(w1['w'], 45 + 88 * 9 + 14)       # 88 columns stay visible (the scroll bar takes 14 px of the 90)
        self.assertGreater(w1['h'], w0['h'])
        bar(526)                                                 # 125 %: not a whole multiple, anti-aliased text
        time.sleep(0.3)
        win = self.edit_window()
        ppm = ctl.dump(os.path.join(tmp, 'edit-zoom.ppm'))
        colours = set()
        for y in range(win['y'] + 24, win['y'] + 24 + 30):
            for x in range(win['x'] + 50, win['x'] + 50 + 60):
                colours.add(ppm.px(x, y))
        self.assertGreater(len(colours), 6, 'a fractional size should show intermediate shades')
        end = time.time() + 3
        while time.time() < end and not (os.path.exists(conf) and 'zoom=125' in open(conf).read()):
            time.sleep(0.05)
        self.assertIn('zoom=125', open(conf).read())
        ctl.abs(win['x'] + 200, win['y'] + 200)
        self.chord(self.CTRL, 11)                                # Ctrl+0: back to 100 %
        time.sleep(0.3)
        w2 = self.edit_window()
        self.assertEqual(w2['w'], 40 + 88 * 8 + 14)               # (the scroll bar takes 14 px of the 90 columns)
        ctl.tap(self.K_END)
        self.type_text('x')                                      # typing and saving still work
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'onex\n')
        self.chord(self.CTRL, 13)                                # Ctrl+= : 110 %
        time.sleep(0.5)
        self.assertEqual(self.edit_window()['w'], 45 + 88 * 9 + 14)
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)
        self.assertIn('zoom=110', open(conf).read())
        p2, path2 = self.edit_start(b'two\n', 'second.txt', env={'XDG_CONFIG_HOME': confdir})   # the next editor starts with it
        self.assertGreater(self.edit_window()['w'], 760)

    def test_edit_buttons_and_help(self):
        p, path = self.edit_start(b'one\n')
        d = os.path.dirname(path)
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.edit'][0]
        def button(x):                                          # New 6, Open 50, Save 102, Save as 154; the bar is 24 px high
            ctl.click(win['x'] + x + 10, win['y'] + 12)
            time.sleep(0.2)
        ctl.tap(59)                                            # F1: the key list; the next key only closes it
        ctl.tap(self.K_X)
        self.type_text('y')                                    # so this is the first typed letter
        button(102)                                            # the Save button
        time.sleep(0.2)
        self.assertEqual(self.read_file(path), b'yone\n')
        other = os.path.join(d, 'other.txt')
        with open(other, 'wb') as f:
            f.write(b'second\n')
        button(50)                                             # the Open button
        self.type_chars(other)
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        self.type_text('q')
        button(102)
        self.assertEqual(self.read_file(other), b'qsecond\n')

    def test_edit_cut_copy_paste_buttons_and_the_classic_keys(self):
        p, path = self.edit_start(b'abc def\n')
        win = self.edit_window()
        def button(x):                                          # Cut 230, Copy 274, Paste 326 (Save as ends at 226); the bar is 24 px high
            ctl.click(win['x'] + x + 10, win['y'] + 12)
            time.sleep(0.25)
        def select_abc():
            ctl.tap(self.K_HOME)
            ctl.key(self.SHIFT, True)
            for _ in range(3):
                ctl.tap(106)
            ctl.key(self.SHIFT, False)
        def save():
            self.chord(self.CTRL, self.K_S)
            time.sleep(0.25)
        ctl.abs(win['x'] + 300, win['y'] + 200)
        select_abc()
        button(274)                                             # Copy: the text stays, the selection too
        save()
        self.assertEqual(self.read_file(path), b'abc def\n')
        ctl.tap(self.K_END)
        button(326)                                             # Paste at the end
        save()
        self.assertEqual(self.read_file(path), b'abc defabc\n')
        select_abc()
        button(230)                                             # Cut "abc" from the start
        save()
        self.assertEqual(self.read_file(path), b' defabc\n')
        button(326)                                             # ... and paste it back where the cursor is
        save()
        self.assertEqual(self.read_file(path), b'abc defabc\n')
        ctl.tap(self.K_HOME)                                    # Cut and Copy with nothing selected do nothing (the clipboard keeps "abc")
        button(230)
        button(274)
        save()
        self.assertEqual(self.read_file(path), b'abc defabc\n')
        select_abc()
        self.chord(self.CTRL, 110)                              # Ctrl+Insert: copy
        ctl.tap(self.K_END)
        ctl.key(self.SHIFT, True)
        ctl.tap(110)                                            # Shift+Insert: paste
        ctl.key(self.SHIFT, False)
        save()
        self.assertEqual(self.read_file(path), b'abc defabcabc\n')
        select_abc()
        ctl.key(self.SHIFT, True)
        ctl.tap(111)                                            # Shift+Delete: cut
        ctl.key(self.SHIFT, False)
        save()
        self.assertEqual(self.read_file(path), b' defabcabc\n')

    def test_edit_right_click_menu_on_the_text(self):
        p, path = self.edit_start(b'abc def\n')
        win = self.edit_window()
        ctl.abs(win['x'] + 300, win['y'] + 200)
        ctl.tap(self.K_HOME)
        ctl.key(self.SHIFT, True)
        for _ in range(3):
            ctl.tap(106)
        ctl.key(self.SHIFT, False)
        self.right_click(win['x'] + 300, win['y'] + 200)
        m = self.menu_state()
        self.assertEqual((m['open'], [(e['label'], e['enabled']) for e in m['entries']]),
                         (1, [('Cut', 1), ('Copy', 1), ('Paste', 1), ('Select all', 1)]))
        ctl.click(m['entries'][0]['x'], m['entries'][0]['y'])           # Cut "abc"
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path), b' def\n')
        self.right_click(win['x'] + 300, win['y'] + 200)                  # nothing selected: Cut and Copy are dim
        m = self.menu_state()
        self.assertEqual([e['enabled'] for e in m['entries']], [0, 0, 1, 1])
        ctl.click(m['entries'][2]['x'], m['entries'][2]['y'])           # Paste: "abc" is back (at the end of the line: the click placed the caret there)
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path), b' defabc\n')
        self.right_click(win['x'] + 300, win['y'] + 200)
        ctl.click(*[self.menu_state()['entries'][3][k] for k in ('x', 'y')])      # Select all ...
        self.right_click(win['x'] + 300, win['y'] + 200)
        ctl.click(*[self.menu_state()['entries'][0][k] for k in ('x', 'y')])      # ... and Cut
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path), b'\n')                       # (the final line end stays)

    def test_edit_prompts_paste_and_have_the_field_menu(self):
        self.own_clipboard('two')                                        # (first: its window must not take the focus from the editor)
        p, path = self.edit_start(b'one two three\n')
        win = self.edit_window()
        ctl.abs(win['x'] + 300, win['y'] + 200)
        time.sleep(0.2)
        self.chord(self.CTRL, 33)                                        # Ctrl+F: the find prompt
        self.chord(self.CTRL, 47)                                        # Ctrl+V: "two" into it
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        time.sleep(0.2)
        self.chord(self.CTRL, self.K_X)                                  # the match is selected: cut it
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path), b'one  three\n')
        self.chord(self.CTRL, 33)                                        # the prompt again: its right-click menu
        self.right_click(win['x'] + 300, win['y'] + 200)
        m = self.menu_state()
        self.assertEqual((m['open'], [e['label'] for e in m['entries']]), (1, ['Cut', 'Copy', 'Paste', 'Select all', 'Clear']))
        ctl.tap(KEY_ESC)
        ctl.tap(KEY_ESC)

    # ---- mellagui-files ----------------------------------------------------------------------------------------------

    def test_files_selftest(self):
        path = os.path.join(ROOT, 'build', 'mellagui-files')
        if not os.path.exists(path):
            self.skipTest('mellagui-files not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def files_start(self, *args, app_id='mellagui.files', extra_files=0, env=None):
        """A folder with sub/, a.txt, p.ppm (a 1x1 image), .hidden and the browser on it. Returns (process, folder)."""
        exe = os.path.join(ROOT, 'build', 'mellagui-files')
        if not os.path.exists(exe):
            self.skipTest('mellagui-files not built')
        d = tempfile.mkdtemp(prefix='files-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        os.mkdir(os.path.join(d, 'sub'))
        with open(os.path.join(d, 'sub', 'inner.txt'), 'wb') as f:
            f.write(b'inner\n')
        with open(os.path.join(d, 'a.txt'), 'wb') as f:
            f.write(b'text a\n')
        with open(os.path.join(d, 'p.ppm'), 'wb') as f:
            f.write(b'P6\n1 1\n255\n\x10\x20\x30')
        with open(os.path.join(d, '.hidden'), 'wb') as f:
            f.write(b'secret\n')
        with open(os.path.join(d, 'blob.bin'), 'wb') as f:
            f.write(b'\x00\x01\x02')
        for i in range(extra_files):
            open(os.path.join(d, 'f%03d.txt' % i), 'wb').close()
        for w in ctl.windows()['windows']:
            self.assertNotIn(w['app_id'], ('mellagui.files', 'mellagui.files.choose', 'mellagui.edit', 'mellagui.view', 'mellagui.term'),
                             'a window of an earlier test is still open')
        p = subprocess.Popen([exe] + list(args) + [d], env=dict(os.environ, **(env or {})), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        def cleanup():
            for pid in os.listdir('/proc'):                       # the programs it started are its children
                if not pid.isdigit():
                    continue
                try:
                    with open('/proc/%s/stat' % pid) as f:
                        ppid = int(f.read().rsplit(')', 1)[1].split()[1])
                    if ppid == p.pid or ppid == 1 and False:
                        os.kill(int(pid), 9)
                except (OSError, ValueError, IndexError):
                    pass
            if p.poll() is None:
                p.kill()
                p.wait()
            time.sleep(0.2)
        self.addCleanup(cleanup)
        self.assertIsNotNone(wait_for_window(app_id), 'the file browser did not appear')
        time.sleep(0.4)
        return p, d

    def window_title(self, app_id, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            for w in ctl.windows()['windows']:
                if w['app_id'] == app_id:
                    return w['title']
            time.sleep(0.05)
        return None

    def wait_title(self, app_id, title, timeout=5.0):
        end = time.time() + timeout
        t = None
        while time.time() < end:
            t = self.window_title(app_id, 0.1)
            if t == title:
                return
            time.sleep(0.05)
        self.fail('title of %s is %r, wanted %r' % (app_id, t, title))

    def test_files_navigates_with_keys(self):
        p, d = self.files_start()
        base = os.path.basename(d)
        self.wait_title('mellagui.files', base + ' - Files')
        ctl.tap(KEY_ENTER)                                     # the first entry is selected: the folder "sub" (folders come first)
        self.wait_title('mellagui.files', 'sub - Files')
        ctl.tap(self.K_BS)                                     # Backspace: back up, with "sub" selected again
        self.wait_title('mellagui.files', base + ' - Files')
        ctl.tap(KEY_ENTER)                                     # so Enter goes into it once more
        self.wait_title('mellagui.files', 'sub - Files')
        ctl.tap(105)                                           # Left goes up too
        self.wait_title('mellagui.files', base + ' - Files')

    def thumb(self, win, name):
        """(top, height, width) of the scroll bar's thumb in the file browser's right edge, from a screenshot."""
        time.sleep(0.2)                                                  # (the program repaints after the pointer moved away)
        ppm = ctl.dump(os.path.join(tmp, name))
        thumb = (0x35, 0x3b, 0x49)
        x = win['x'] + win['w'] - 5
        ys = [y for y in range(win['y'], win['y'] + win['h']) if ppm.px(x, y) == thumb]
        if not ys:
            return None
        mid = (ys[0] + ys[-1]) // 2
        width = sum(1 for xx in range(win['x'] + win['w'] - 60, win['x'] + win['w']) if ppm.px(xx, mid) == thumb)
        return ys[0], ys[-1] - ys[0] + 1, width

    def test_files_scrollbar_is_wide_and_can_be_clicked_and_dragged(self):
        p, d = self.files_start(extra_files=150)
        win = wait_for_window('mellagui.files')
        t0 = self.thumb(win, 'sb0.ppm')
        self.assertIsNotNone(t0)
        self.assertEqual(t0[2], 14 - 4)                                  # 14 px bar, the thumb 2 px in from each side
        self.assertGreater(t0[1], 20)
        right = win['x'] + win['w'] - 6
        ctl.click(right, win['y'] + win['h'] - 70)                       # the track below the thumb (the status line and the key bar take 50 px): one page down
        time.sleep(0.3)
        t1 = self.thumb(win, 'sb1.ppm')
        self.assertGreater(t1[0], t0[0])
        ctl.abs(right, t1[0] + 5)                                        # drag the thumb to the bottom
        ctl.button(272, True)
        ctl.abs(right, win['y'] + win['h'] - 20)
        ctl.button(272, False)
        time.sleep(0.3)
        t2 = self.thumb(win, 'sb2.ppm')
        self.assertGreater(t2[0], t1[0] + 50)
        self.assertGreater(t2[0] + t2[1], win['y'] + win['h'] - 60)      # it ends at the bottom of the track
        ctl.click(right, win['y'] + 60)                                  # the track above it: one page up
        time.sleep(0.3)
        self.assertLess(self.thumb(win, 'sb3.ppm')[0], t2[0])

    def test_files_scrollbar_width_comes_from_the_config(self):
        conf = os.path.join(tmp, 'sbw.conf')
        with open(conf, 'w') as f:
            f.write('[windows]\nscrollbar-width = 30\n')
        p, d = self.files_start(extra_files=150, env={'MELLAGUI_CONFIG': conf})
        win = wait_for_window('mellagui.files')
        ppm = ctl.dump(os.path.join(tmp, 'sbw.ppm'))
        t = self.thumb(win, 'sbw.ppm')
        self.assertEqual(t[2], 30 - 4)

    def test_files_opens_text_image_and_refuses_binary(self):
        p, d = self.files_start()
        # rows: .. / sub (selected) / a.txt / blob.bin / p.ppm (the hidden file is not shown)
        ctl.tap(self.K_DOWN)
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.edit', 'a.txt - Editor')
        ctl.key(KEY_LEFTMETA, True); ctl.tap(KEY_Q); ctl.key(KEY_LEFTMETA, False)
        for _ in range(50):
            if self.window_title('mellagui.edit', 0.05) is None:
                break
        ctl.tap(self.K_DOWN)                                   # blob.bin
        ctl.tap(KEY_ENTER)
        time.sleep(0.5)
        self.assertIsNone(self.window_title('mellagui.edit', 0.2), 'a binary file must not be opened')
        ctl.tap(self.K_DOWN)                                   # p.ppm
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.view', 'p.ppm - Images')

    def test_files_opens_videos_in_the_viewer(self):
        rec = os.path.join(ROOT, 'build', 'mellagui-record')
        if not os.path.exists(rec):
            self.skipTest('mellagui-record not built')
        p, d = self.files_start()
        r = subprocess.run([rec, '--start', '--duration', '0.5', '--dir', d, '--name', 'clip.mp4', '--codec', 'mjpeg'], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        ctl.key(KEY_LEFTCTRL, True); ctl.tap(19); ctl.key(KEY_LEFTCTRL, False)            # Ctrl+R: read the folder again
        time.sleep(0.3)
        ctl.tap(KEY_C)                                                                    # c: the first name that starts with it
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.view', 'clip.mp4 - Images')

    def test_files_starts_programs(self):
        calc = os.path.join(ROOT, 'build', 'mellagui-calc')
        if not os.path.exists(calc):
            self.skipTest('mellagui-calc not built')
        p, d = self.files_start()
        prog = os.path.join(d, 'zcalc')
        shutil.copy(calc, prog)                                                            # an ELF program that may be executed
        plain = os.path.join(d, 'ycalc')
        shutil.copy(calc, plain)
        os.chmod(plain, 0o644)                                                             # ... and one that may not
        ctl.key(KEY_LEFTCTRL, True); ctl.tap(19); ctl.key(KEY_LEFTCTRL, False)
        time.sleep(0.3)
        ctl.tap(21)                                                                        # y: the program that cannot run
        ctl.tap(KEY_ENTER)
        time.sleep(0.6)
        self.assertIsNone(self.window_title('mellagui.calc', 0.2), 'a file without the execute permission was started')
        ctl.tap(44)                                                                        # z
        ctl.tap(KEY_ENTER)
        self.assertIsNotNone(wait_for_window('mellagui.calc'), 'Enter on a program did not start it')

    FKEYS = {3: 61, 5: 63, 6: 64, 7: 65, 8: 66, 10: 68}
    K_TAB, K_INS = 15, 110

    def test_files_two_panes_have_their_own_folders(self):
        p, d = self.files_start()
        base = os.path.basename(d)
        self.wait_title('mellagui.files', base + ' - Files')
        ctl.tap(KEY_ENTER)                                     # the left pane goes into sub
        self.wait_title('mellagui.files', 'sub - Files')
        ctl.tap(self.K_TAB)                                    # the right pane was left alone
        self.wait_title('mellagui.files', base + ' - Files')
        ctl.tap(self.K_TAB)
        self.wait_title('mellagui.files', 'sub - Files')

    def test_files_preview_shows_a_picture_in_the_other_pane(self):
        p, d = self.files_start()
        win = wait_for_window('mellagui.files')
        for _ in range(3):                                     # a.txt, blob.bin, p.ppm (a 1x1 picture of 10 20 30)
            ctl.tap(self.K_DOWN)
        ctl.tap(self.FKEYS[3])
        time.sleep(0.6)
        ppm = ctl.dump(os.path.join(tmp, 'files-pv.ppm'))
        def has_colour(x0, x1):
            return any(ppm.px(x, y) == (0x10, 0x20, 0x30) for y in range(win['y'] + 60, win['y'] + win['h'] - 60, 1)
                       for x in range(win['x'] + x0, win['x'] + x1))
        self.assertTrue(has_colour(win['w'] // 2, win['w'] - 4), 'the picture should be in the right (inactive) pane')
        self.assertFalse(has_colour(0, win['w'] // 2 - 4))
        ctl.tap(self.FKEYS[3])                                   # off again
        time.sleep(0.3)
        ppm = ctl.dump(os.path.join(tmp, 'files-pv2.ppm'))
        self.assertFalse(has_colour(win['w'] // 2, win['w'] - 4))

    def test_files_preview_shows_text_and_hex(self):
        p, d = self.files_start()
        win = wait_for_window('mellagui.files')
        ctl.tap(self.K_DOWN)                                   # a.txt
        self.chord(self.CTRL, self.K_Q)                        # Ctrl+Q: preview
        time.sleep(0.4)
        ppm = ctl.dump(os.path.join(tmp, 'files-pt.ppm'))
        text_pixels = sum(1 for y in range(win['y'] + 56, win['y'] + 120) for x in range(win['x'] + win['w'] // 2 + 6, win['x'] + win['w'] // 2 + 120)
                          if ppm.px(x, y) == (0xd8, 0xde, 0xe9))
        self.assertGreater(text_pixels, 20, 'the text of a.txt should be drawn in the right pane')
        ctl.tap(self.K_DOWN)                                   # blob.bin: a hex dump (dim colour 7b 82 94)
        time.sleep(0.4)
        ppm = ctl.dump(os.path.join(tmp, 'files-ph.ppm'))
        dim = sum(1 for y in range(win['y'] + 56, win['y'] + 120) for x in range(win['x'] + win['w'] // 2 + 6, win['x'] + win['w'] // 2 + 200)
                  if ppm.px(x, y) == (0x7b, 0x82, 0x94))
        self.assertGreater(dim, 20)

    def test_files_prompts_paste_and_have_the_field_menu(self):
        self.own_clipboard('pasted folder\n')                          # (first: its window must not take the focus from the file manager)
        p, d = self.files_start()
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.files'][0]
        ctl.abs(win['x'] + 200, win['y'] + 150)
        ctl.tap(self.FKEYS[7])                                           # F7: the new folder prompt
        self.chord(self.CTRL, 47)                                        # Ctrl+V
        time.sleep(0.4)
        self.right_click(win['x'] + 200, win['y'] + 150)                 # the right-click menu of the prompt
        m = self.menu_state()
        self.assertEqual((m['open'], [(e['label'], e['enabled']) for e in m['entries']]),
                         (1, [('Cut', 1), ('Copy', 1), ('Paste', 1), ('Select all', 1), ('Clear', 1)]))
        ctl.click(m['entries'][4]['x'], m['entries'][4]['y'])           # Clear
        self.assertEqual(self.menu_state()['open'], 0)
        self.right_click(win['x'] + 200, win['y'] + 150)
        m = self.menu_state()
        self.assertEqual([e['enabled'] for e in m['entries']], [0, 0, 1, 0, 0])
        ctl.click(m['entries'][2]['x'], m['entries'][2]['y'])           # Paste
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertTrue(os.path.isdir(os.path.join(d, 'pasted folder')), os.listdir(d))

    def test_files_right_click_menu_on_a_file_and_on_empty_space(self):
        p, d = self.files_start()
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.files'][0]
        names = []
        for y in range(win['y'] + 40, win['y'] + 150, 6):                # find the row of a.txt: its menu has an enabled Edit
            self.right_click(win['x'] + 100, y)
            m = self.menu_state()
            if m['open'] and len(m['entries']) == 8 and m['entries'][1]['enabled'] == 1:
                break
            if m['open']:
                ctl.tap(KEY_ESC)
        m = self.menu_state()
        self.assertEqual((m['open'], [e['label'] for e in m['entries']]),
                         (1, ['Open', 'Edit', 'Rename...', 'Copy...', 'Move...', 'Delete...', 'New folder...', 'Terminal here']))
        ctl.click(m['entries'][2]['x'], m['entries'][2]['y'])            # Rename...: the prompt starts with the name of the file under the pointer
        time.sleep(0.2)
        for _ in range(12):
            ctl.tap(14)                                                  # Backspace x12
        self.type_chars('renamed.txt')
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertEqual(sorted(n for n in os.listdir(d) if not n.startswith('.')), ['blob.bin', 'p.ppm', 'renamed.txt', 'sub'])
        self.right_click(win['x'] + 100, win['y'] + win['h'] - 90)       # below the last row of the list: the menu of the empty place
        m = self.menu_state()
        self.assertEqual((m['open'], [e['label'] for e in m['entries']]), (1, ['New folder...', 'Terminal here', 'Read the folder again']))
        ctl.click(m['entries'][0]['x'], m['entries'][0]['y'])
        time.sleep(0.2)
        self.type_chars('fromrc')
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertTrue(os.path.isdir(os.path.join(d, 'fromrc')))

    def test_files_copy_move_mkdir_and_delete(self):
        p, d = self.files_start()
        sub = os.path.join(d, 'sub')
        ctl.tap(self.K_TAB)                                    # the right pane goes into sub, that is where things will land
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.files', 'sub - Files')
        ctl.tap(self.K_TAB)
        self.wait_title('mellagui.files', os.path.basename(d) + ' - Files')
        ctl.tap(self.K_DOWN)                                   # a.txt
        ctl.tap(self.FKEYS[5])                                   # copy: the question starts with the other pane's folder
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertEqual(self.read_file(os.path.join(sub, 'a.txt')), b'text a\n')
        self.assertTrue(os.path.exists(os.path.join(d, 'a.txt')), 'a copy leaves the original')
        ctl.tap(self.K_DOWN)                                   # blob.bin
        ctl.tap(self.FKEYS[6])                                   # move
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertTrue(os.path.exists(os.path.join(sub, 'blob.bin')))
        self.assertFalse(os.path.exists(os.path.join(d, 'blob.bin')))
        ctl.tap(self.FKEYS[7])                                   # new folder
        self.type_chars('made')
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertTrue(os.path.isdir(os.path.join(d, 'made')))
        ctl.tap(30)                                            # the letter "a" jumps to a.txt
        ctl.tap(self.FKEYS[8])                                   # delete asks first ...
        ctl.tap(KEY_ESC)
        time.sleep(0.3)
        self.assertTrue(os.path.exists(os.path.join(d, 'a.txt')), 'Esc must keep the file')
        ctl.tap(self.FKEYS[8])
        ctl.tap(KEY_ENTER)                                     # ... and Enter moves it to the trash
        time.sleep(0.4)
        self.assertFalse(os.path.exists(os.path.join(d, 'a.txt')))
        self.assertTrue(os.path.exists(os.path.join(sub, 'a.txt')), 'only the one in this folder is deleted')
        trash = os.path.join(os.environ['XDG_DATA_HOME'], 'Trash')
        self.assertEqual(self.read_file(os.path.join(trash, 'files', 'a.txt')), b'text a\n')
        info = self.read_file(os.path.join(trash, 'info', 'a.txt.trashinfo')).decode()
        self.assertIn('[Trash Info]', info)
        self.assertIn('Path=' + os.path.join(d, 'a.txt'), info)
        ctl.tap(self.K_END)                                    # p.ppm: Shift+F8 deletes for good
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(self.FKEYS[8])
        ctl.key(KEY_LEFTSHIFT, False)
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertFalse(os.path.exists(os.path.join(d, 'p.ppm')))
        self.assertFalse(os.path.exists(os.path.join(trash, 'files', 'p.ppm')), 'Shift+F8 does not use the trash')

    def test_files_tab_completes_the_target_of_a_copy(self):
        p, d = self.files_start()
        ctl.tap(self.K_DOWN)                                   # a.txt
        ctl.tap(self.FKEYS[5])                                 # the question starts with this folder (the other pane shows it, too)
        self.type_chars('su')
        ctl.tap(self.K_TAB)                                    # su -> sub/
        ctl.tap(KEY_ENTER)
        time.sleep(0.5)
        self.assertEqual(self.read_file(os.path.join(d, 'sub', 'a.txt')), b'text a\n')

    def trash_one_file(self, d, name):
        """Makes d/name, shows it (Ctrl+R), puts the cursor on it (its first letter) and sends it to the trash with F8, Enter."""
        with open(os.path.join(d, name), 'wb') as f:
            f.write(b'restore me\n')
        self.chord(self.CTRL, 19)                              # Ctrl+R: read the folder again
        time.sleep(0.3)
        ctl.tap(self.LETTERS[name[0]])
        ctl.tap(self.FKEYS[8])
        ctl.tap(KEY_ENTER)
        time.sleep(0.5)
        self.assertFalse(os.path.exists(os.path.join(d, name)))
        trash = os.path.join(os.environ['XDG_DATA_HOME'], 'Trash')
        self.assertTrue(os.path.exists(os.path.join(trash, 'files', name)))
        return trash

    def test_files_trash_and_restore_with_keys(self):
        p, d = self.files_start()
        trash = self.trash_one_file(d, 'zz_back_keys.txt')
        self.chord(self.CTRL, 48)                              # Ctrl+B: the trash
        self.wait_title('mellagui.files', 'files - Files')
        ctl.tap(self.LETTERS['z'])
        self.chord(self.CTRL, 44)                              # Ctrl+Z: put it back
        time.sleep(0.6)
        self.assertEqual(self.read_file(os.path.join(d, 'zz_back_keys.txt')), b'restore me\n')
        self.assertFalse(os.path.exists(os.path.join(trash, 'files', 'zz_back_keys.txt')))
        self.assertFalse(os.path.exists(os.path.join(trash, 'info', 'zz_back_keys.txt.trashinfo')))

    def test_files_trash_and_restore_buttons(self):
        p, d = self.files_start()
        win = wait_for_window('mellagui.files')
        trash = self.trash_one_file(d, 'zz_back_buttons.txt')
        ctl.click(win['x'] + 380, win['y'] + 15)               # the Trash button (x 354..414)
        self.wait_title('mellagui.files', 'files - Files')
        ctl.tap(self.LETTERS['z'])
        ctl.click(win['x'] + 450, win['y'] + 15)               # the Restore button (x 420..496)
        time.sleep(0.6)
        self.assertEqual(self.read_file(os.path.join(d, 'zz_back_buttons.txt')), b'restore me\n')

    def test_files_empty_trash_asks_and_empties(self):
        p, d = self.files_start()
        win = wait_for_window('mellagui.files')
        trash = self.trash_one_file(d, 'zz_empty_me.txt')
        self.assertTrue(os.path.exists(os.path.join(trash, 'info', 'zz_empty_me.txt.trashinfo')))
        ctl.click(win['x'] + 520, win['y'] + 15)               # the Empty button (x 502..562)
        time.sleep(0.3)
        ctl.tap(KEY_ESC)                                       # Esc: the question is withdrawn
        time.sleep(0.3)
        self.assertTrue(os.path.exists(os.path.join(trash, 'files', 'zz_empty_me.txt')))
        self.chord(self.CTRL, 18)                              # Ctrl+E asks again
        ctl.tap(KEY_ENTER)
        time.sleep(0.8)
        self.assertEqual(os.listdir(os.path.join(trash, 'files')), [])
        self.assertEqual(os.listdir(os.path.join(trash, 'info')), [])

    def test_files_marked_files_move_together_and_nothing_is_replaced(self):
        p, d = self.files_start()
        sub = os.path.join(d, 'sub')
        with open(os.path.join(sub, 'a.txt'), 'wb') as f:
            f.write(b'already here\n')
        ctl.tap(self.K_TAB)
        ctl.tap(KEY_ENTER)                                     # right pane: sub
        self.wait_title('mellagui.files', 'sub - Files')
        ctl.tap(self.K_TAB)
        ctl.tap(self.K_DOWN)                                   # a.txt
        ctl.tap(self.K_INS)                                    # mark a.txt and move down
        ctl.tap(self.K_DOWN)                                   # skip blob.bin
        ctl.tap(57)                                            # Space marks p.ppm, too
        ctl.tap(self.FKEYS[6])
        ctl.tap(KEY_ENTER)
        time.sleep(0.5)
        self.assertEqual(self.read_file(os.path.join(sub, 'a.txt')), b'already here\n', 'an existing file is never replaced')
        self.assertTrue(os.path.exists(os.path.join(d, 'a.txt')), 'the one that could not be moved stays')
        self.assertTrue(os.path.exists(os.path.join(sub, 'p.ppm')))
        self.assertFalse(os.path.exists(os.path.join(d, 'p.ppm')))

    def test_files_rename(self):
        p, d = self.files_start()
        ctl.tap(self.K_DOWN)                                   # a.txt
        ctl.tap(60)                                            # F2: the name comes up to be edited
        for _ in range(5):
            ctl.tap(self.K_BS)                                 # "a.txt" -> ""
        self.type_chars('b.txt')
        ctl.tap(KEY_ENTER)
        time.sleep(0.4)
        self.assertTrue(os.path.exists(os.path.join(d, 'b.txt')))
        self.assertFalse(os.path.exists(os.path.join(d, 'a.txt')))

    def test_files_choose_sets_the_wallpaper(self):
        self.addCleanup(lambda: ctl.cmd('settings wallpaper ""'))
        ctl.cmd('settings wallpaper ""')
        p, d = self.files_start('--choose', 'wallpaper', app_id='mellagui.files.choose')
        self.assertEqual(self.window_title('mellagui.files.choose'), os.path.basename(d) + ' - Choose a picture')
        ctl.tap(self.K_DOWN)                                   # a.txt (sub is selected first): not a picture, nothing happens
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ')
        self.assertIsNone(p.poll())
        ctl.tap(self.K_DOWN)                                   # blob.bin
        ctl.tap(self.K_DOWN)                                   # p.ppm
        ctl.tap(KEY_ENTER)
        for _ in range(100):
            if p.poll() is not None:
                break
            time.sleep(0.05)
        self.assertEqual(p.poll(), 0, 'the chooser should end after the choice')
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ' + os.path.join(d, 'p.ppm'))

    def test_files_choose_accepts_a_jpeg(self):
        self.addCleanup(lambda: ctl.cmd('settings wallpaper ""'))
        ctl.cmd('settings wallpaper ""')
        p, d = self.files_start('--choose', 'wallpaper', app_id='mellagui.files.choose')
        with open(os.path.join(d, 'z.JPG'), 'wb') as f:                   # sorts last; the extension is matched without case
            f.write(JPEG_RED_BLUE)
        ctl.tap(63)                                                       # F5: read the folder again
        time.sleep(0.3)
        ctl.tap(self.K_END)
        ctl.tap(KEY_ENTER)
        for _ in range(100):
            if p.poll() is not None:
                break
            time.sleep(0.05)
        self.assertEqual(p.poll(), 0)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ' + os.path.join(d, 'z.JPG'))
        ctl.cmd('setting wallpaper-status')
        self.assertEqual(ctl.last, 'ok ok')

    def test_files_choose_escape_cancels_and_only_wallpaper_is_allowed(self):
        self.addCleanup(lambda: ctl.cmd('settings wallpaper ""'))
        ctl.cmd('settings wallpaper ""')
        p, d = self.files_start('--choose', 'wallpaper', app_id='mellagui.files.choose')
        ctl.tap(KEY_ESC)
        for _ in range(100):
            if p.poll() is not None:
                break
            time.sleep(0.05)
        self.assertEqual(p.poll(), 0)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ')
        r = subprocess.run([os.path.join(ROOT, 'build', 'mellagui-files'), '--choose', 'lock-now'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 2)

    def test_files_hidden_toggle_and_letter_jump(self):
        p, d = self.files_start()
        ctl.tap(self.K_END)                                    # last entry: p.ppm; the letter "a" wraps around to a.txt
        ctl.tap(30)
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.edit', 'a.txt - Editor')
        ctl.key(KEY_LEFTMETA, True); ctl.tap(KEY_Q); ctl.key(KEY_LEFTMETA, False)
        for _ in range(50):
            if self.window_title('mellagui.edit', 0.05) is None:
                break
        self.chord(self.CTRL, 35)                              # Ctrl+H: now ".hidden" sorts right after the folder
        ctl.tap(self.K_HOME); ctl.tap(self.K_DOWN); ctl.tap(self.K_DOWN)    # .. / sub / .hidden
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.edit', '.hidden - Editor')

    def test_files_terminal_starts_in_the_folder(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-term')):
            self.skipTest('mellagui-term not built')
        p, d = self.files_start()
        self.chord(self.CTRL, 20)                              # Ctrl+T
        self.assertIsNotNone(wait_for_window('mellagui.term'), 'the terminal did not appear')
        found = None
        for pid in os.listdir('/proc'):
            if pid.isdigit():
                try:
                    if os.readlink('/proc/%s/exe' % pid).endswith('/mellagui-term') and os.path.realpath('/proc/%s/cwd' % pid) == os.path.realpath(d):
                        found = pid
                except OSError:
                    pass
        self.assertIsNotNone(found, 'no mellagui-term running in ' + d)

    def test_files_path_prompt(self):
        p, d = self.files_start()
        self.chord(self.CTRL, 38)                              # Ctrl+L
        for _ in range(len(d) + 1):                            # empty the prompt (it starts with the current path)
            ctl.tap(self.K_BS)
        self.type_chars(os.path.join(d, 'sub'))
        ctl.tap(KEY_ENTER)
        self.wait_title('mellagui.files', 'sub - Files')

    def test_the_shortcut_list_opens_with_super_slash_and_closes_with_esc(self):
        path = os.path.join(ROOT, 'build', 'mellagui-keys')
        if not os.path.exists(path):
            self.skipTest('mellagui-keys not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(53)                                              # Super+/
        ctl.key(KEY_LEFTMETA, False)
        win = wait_for_window('mellagui.keys')
        self.assertIsNotNone(win, 'the shortcut list did not open')
        time.sleep(0.4)
        ppm = ctl.dump(os.path.join(tmp, 'keys-list.ppm'))
        lit = sum(1 for y in range(win['y'] + 40, win['y'] + 200, 2) for x in range(win['x'] + 20, win['x'] + 300, 2) if ppm.px(x, y)[0] > 150)
        self.assertGreater(lit, 100, 'the list shows no text')
        ctl.tap(KEY_ESC)
        for _ in range(60):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.keys']:
                break
            time.sleep(0.05)
        self.assertFalse([w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.keys'])

    def test_run_dialog_selftest(self):
        path = os.path.join(ROOT, 'build', 'mellagui-run')
        if not os.path.exists(path):
            self.skipTest('mellagui-run not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_run_dialog_from_panel_button(self):
        self.run_dialog_runs(lambda: ctl.click(826, 14))

    def test_run_dialog_from_super_r(self):
        def open_it():
            ctl.key(KEY_LEFTMETA, True)
            ctl.tap(19)             # KEY_R
            ctl.key(KEY_LEFTMETA, False)
        self.run_dialog_runs(open_it)

    def test_run_dialog_escape_closes_without_running(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-run')):
            self.skipTest('mellagui-run not built')
        ctl.click(826, 14)
        self.assertIsNotNone(wait_for_window('mellagui.run'))
        time.sleep(0.3)
        ctl.tap(KEY_ESC)
        for _ in range(100):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.run']:
                break
            time.sleep(0.05)
        self.assertFalse([w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.run'])

    def test_terminal_runs_shell_and_gets_keys(self):
        out = os.path.join(tmp, 'term-out.txt')
        script = os.path.join(tmp, 'shell.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nread line\necho "got:$line" > "%s"\nread line2\n' % out)
        os.chmod(script, 0o755)
        p = self.spawn('mellagui-term', SHELL=script)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        type_text('mellagui')
        for _ in range(50):
            if os.path.exists(out) and open(out).read().strip():
                break
            time.sleep(0.05)
        self.assertEqual(open(out).read().strip(), 'got:mellagui')
        # the terminal drew text: more than 2 colours in its client area
        ppm = ctl.dump(os.path.join(tmp, 'term.ppm'))
        cols = {ppm.px(win['x'] + x, win['y'] + 5) for x in range(0, 300)}
        self.assertGreaterEqual(len(cols), 2)
        # Super+Q closes it politely
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_Q)
        ctl.key(KEY_LEFTMETA, False)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_terminal_exits_with_shell(self):
        script = os.path.join(tmp, 'quick.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nexit 0\n')
        os.chmod(script, 0o755)
        p = self.spawn('mellagui-term', SHELL=script)
        self.assertEqual(p.wait(timeout=5), 0)

    FAKE_MONITOR = ('monitor HDMI-A-1 Test Monitor\n10 100 20\n12 100 50\n62 100 30\n8d 2 2\n60 18 17\n14 12 5\nd6 5 1\n'
                    'monitor DP-1 Second One\n10 100 70\n62 100 40\n')

    def fake_monitor(self, tag):
        path = os.path.join(tmp, 'fake-monitor-' + tag)
        with open(path, 'w') as f:
            f.write(self.FAKE_MONITOR)
        return path

    def fake_values(self, path, name):
        """{code: current} of one monitor in the fake file."""
        out, cur = {}, None
        for line in open(path).read().splitlines():
            if line.startswith('monitor '):
                cur = line.split()[1]
            elif cur == name:
                code, mx, val = line.split()
                out[code] = int(val)
        return out

    def test_monitor_cli_lists_and_sets(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-monitor')
        if not os.path.exists(exe):
            self.skipTest('mellagui-monitor not built')
        path = self.fake_monitor('cli')
        env = dict(os.environ, MELLAGUI_MONITOR_FAKE=path)
        out = subprocess.run([exe, '--list'], env=env, capture_output=True, text=True, timeout=10).stdout.splitlines()
        self.assertEqual(out[0], 'HDMI-A-1|Test Monitor|fake|10=20/100|12=50/100|62=30/100|8d=2/2|60=17/18|14=5/12|d6=1/5')
        self.assertEqual(out[1], 'DP-1|Second One|fake|10=70/100|62=40/100')
        self.assertEqual(subprocess.run([exe, '--set', 'HDMI-A-1', '10', '65'], env=env, timeout=10).returncode, 0)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['10'], 65)
        self.assertEqual(self.fake_values(path, 'DP-1')['10'], 70)                 # the other monitor is untouched
        self.assertEqual(subprocess.run([exe, '--set', 'HDMI-A-1', '10', '500'], env=env, timeout=10).returncode, 0)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['10'], 100)            # clamped to the monitor's maximum
        self.assertEqual(subprocess.run([exe, '--set', 'NOPE-1', '10', '5'], env=env, capture_output=True, timeout=10).returncode, 2)
        self.assertEqual(subprocess.run([exe, '--set', 'HDMI-A-1', 'ff', '5'], env=env, capture_output=True, timeout=10).returncode, 2)

    def test_monitor_panel_sliders_chips_and_tabs(self):
        path = self.fake_monitor('ui')
        self.spawn('mellagui-monitor', MELLAGUI_MONITOR_FAKE=path)
        win = wait_for_window('mellagui.monitor')
        self.assertIsNotNone(win, 'monitor panel did not appear')
        time.sleep(0.5)
        x0, y0 = win['x'], win['y']
        # two monitors: tabs on top; brightness slider of the first one (track from x+130, 350 px wide)
        ctl.click(x0 + 20 + 30, y0 + 16 + 14)                                          # tab "HDMI-A-1"
        time.sleep(0.2)
        top = 16 + 40 + 24 + 30                                                        # tabs, heading, connector line
        ctl.click(x0 + 20 + 110 + 175, y0 + top + 15)                                  # middle of the brightness track
        for _ in range(40):
            if self.fake_values(path, 'HDMI-A-1')['10'] == 50:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['10'], 50)
        # the "HDMI 2" input chip: below three sliders and the mute row
        chips_y = top + 3 * 34 + 32 + 6 + 24
        ctl.click(x0 + 498, y0 + chips_y + 13)
        for _ in range(40):
            if self.fake_values(path, 'HDMI-A-1')['60'] == 18:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['60'], 18)
        # mute: the check box under the sliders
        ctl.click(x0 + 20 + 8, y0 + top + 3 * 34 + 13)
        for _ in range(40):
            if self.fake_values(path, 'HDMI-A-1')['8d'] == 1:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['8d'], 1)
        # the second tab shows the other monitor, which only has brightness: its slider works
        ctl.click(x0 + 20 + 88 + 6 + 28, y0 + 16 + 14)                                  # tab "DP-1"
        time.sleep(0.3)
        ctl.click(x0 + 20 + 110 + 350, y0 + top + 15)                                  # the far right end = maximum
        for _ in range(40):
            if self.fake_values(path, 'DP-1')['10'] == 100:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'DP-1')['10'], 100)
        self.assertEqual(self.fake_values(path, 'HDMI-A-1')['10'], 50)
        # DP-1 has no mute feature of its own: "Mute" silences it with volume 0 and gives the volume back
        mute_y = y0 + top + 2 * 34 + 13                                                # brightness, volume, then the check box
        ctl.click(x0 + 20 + 8, mute_y)
        for _ in range(40):
            if self.fake_values(path, 'DP-1')['62'] == 0:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'DP-1')['62'], 0)
        ctl.click(x0 + 20 + 8, mute_y)
        for _ in range(40):
            if self.fake_values(path, 'DP-1')['62'] == 40:
                break
            time.sleep(0.05)
        self.assertEqual(self.fake_values(path, 'DP-1')['62'], 40)

    def test_terminal_paste_button(self):
        out = os.path.join(tmp, 'term-paste.txt')
        script = os.path.join(tmp, 'paste.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nread line\necho "got:$line" > "%s"\nread line2\n' % out)
        os.chmod(script, 0o755)
        if os.path.exists(out):
            os.unlink(out)
        # another client owns the clipboard while it has the focus; the terminal takes the focus afterwards
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        mgr = c.wl.bind('wl_data_device_manager', 3)
        dev = c.wl.new_id('wl_data_device')
        c.wl.request(mgr, 'get_data_device', dev, seat)
        w = Window(c, 200, 150, title='copier')
        c.wl.roundtrip()
        src = c.wl.new_id('wl_data_source')
        c.wl.request(mgr, 'create_data_source', src)
        for mime in ('text/html', 'image/png', 'UTF8_STRING', 'STRING', 'text/plain;charset=utf-8'):
            c.wl.request(src, 'offer', mime)       # more than 4 types: the text one comes last
        c.wl.request(dev, 'set_selection', src, w.configures[-1][3])
        c.wl.roundtrip()
        self.spawn('mellagui-term', SHELL=script)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        ctl.click(win['x'] + 20, win['y'] + 8)     # the "Paste" button of the bar
        # two "send" requests arrive: the compositor's own clipboard-history read, then the terminal's
        for _ in range(2):
            ev = c.wl.wait_for(lambda e: e.obj == src and e.name == 'send', 3.0)
            self.assertIsNotNone(ev, 'the terminal did not ask for the clipboard text')
            os.write(ev.fds[0], b'hello\n')
            os.close(ev.fds[0])
        for _ in range(50):
            if os.path.exists(out) and open(out).read().strip():
                break
            time.sleep(0.05)
        self.assertEqual(open(out).read().strip(), 'got:hello')

    def test_terminal_right_click_menu_pastes(self):
        out = os.path.join(tmp, 'term-rcpaste.txt')
        script = os.path.join(tmp, 'rcpaste.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nread line\necho "got:$line" > "%s"\nread line2\n' % out)
        os.chmod(script, 0o755)
        if os.path.exists(out):
            os.unlink(out)
        self.own_clipboard('from the menu\n')
        self.spawn('mellagui-term', SHELL=script)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        self.right_click(win['x'] + 200, win['y'] + 150)
        m = self.menu_state()
        self.assertEqual((m['open'], [(e['label'], e['enabled']) for e in m['entries']]), (1, [('Copy', 0), ('Paste', 1)]))   # nothing is selected
        ctl.click(m['entries'][1]['x'], m['entries'][1]['y'])
        for _ in range(60):
            if os.path.exists(out) and open(out).read().strip():
                break
            time.sleep(0.05)
        self.assertEqual(open(out).read().strip(), 'got:from the menu')

    # the bar: Paste 4..60, A- 64..96, zoom 100..160 (a drop-down), A+ 164..196, Reset 200..256; the list opens below it
    BAR_H = 24

    def term(self, name='term-size.txt'):
        out = os.path.join(tmp, name)
        script = os.path.join(tmp, name + '.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nwhile read l; do stty size >> "%s"; done\n' % out)
        os.chmod(script, 0o755)
        if os.path.exists(out):
            os.unlink(out)
        self.spawn('mellagui-term', SHELL=script)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        return win, out

    def size_now(self, out, n):
        """The terminal's (rows, cols) after typing a line; n = how many answers the file should hold."""
        type_text('go')
        for _ in range(60):
            if os.path.exists(out) and len(open(out).read().split('\n')) > n:
                break
            time.sleep(0.05)
        rows, cols = map(int, open(out).read().strip().split('\n')[-1].split())
        return rows, cols

    def bar_click(self, x):
        """Clicks the terminal's button bar at x (a zoom moves the window, so its position is read every time)."""
        win = self.term_window()
        ctl.click(win['x'] + x, win['y'] + 8)

    def term_window(self):
        """The terminal's window as it is now (a zoom moves and resizes it)."""
        return [x for x in ctl.windows()['windows'] if x['app_id'] == 'mellagui.term'][0]

    def test_terminal_zoom_dropdown_picks_a_size(self):
        win, out = self.term()
        self.assertEqual(self.size_now(out, 1)[1], 80)
        w100 = win['w']
        ctl.click(win['x'] + 130, win['y'] + 8)                                  # the zoom button opens the list
        time.sleep(0.3)
        ppm = ctl.dump(os.path.join(tmp, 'term-menu.ppm'))
        self.assertGreater(ppm.count((0x88, 0xc0, 0xd0), win['x'], win['y'], win['x'] + 200, win['y'] + 320), 4)   # the dot of the active entry
        # entries (20 px each): 50 60 75 90 100 110 125 150 175 200 250 300 400 500; "200%" is the 10th
        ctl.click(win['x'] + 130, win['y'] + self.BAR_H + 2 + 9 * 20 + 10)
        time.sleep(0.3)
        rows, cols = self.size_now(out, 2)
        w200 = self.term_window()
        self.assertGreater(w200['w'], w100, 'the window grows with the font')
        self.assertLessEqual(w200['x'] + w200['w'], 1024, 'and stays on the screen')
        self.assertGreater(cols, 40)                                              # 80 columns of 16 px do not fit: the window is as wide as the screen allows
        self.assertEqual((w200['w'] - 6 - 14) % 16, 0)
        ctl.click(win['x'] + 130, win['y'] + 8)
        time.sleep(0.2)
        ctl.tap(KEY_ESC)                                                          # Esc closes the list without changing anything
        time.sleep(0.2)
        self.assertEqual(self.size_now(out, 3)[1], cols)

    def test_terminal_zoom_steps_are_fine_and_reset_goes_back(self):
        win, out = self.term('term-steps.txt')
        self.bar_click(180)                                  # A+: 100 -> 110 %, 9 px cells
        time.sleep(0.3)
        self.assertEqual(self.size_now(out, 1)[1], 80)                           # the grid stays, the window grows
        self.assertEqual(self.term_window()['w'], 80 * 9 + 6 + 14)
        self.bar_click(180)                                  # 125 %, 10 px cells
        time.sleep(0.3)
        self.assertEqual(self.size_now(out, 2)[1], 80)
        self.assertEqual(self.term_window()['w'], 80 * 10 + 6 + 14)
        self.bar_click(80)                                   # A-: back to 110 %
        time.sleep(0.3)
        self.assertEqual(self.term_window()['w'], 80 * 9 + 6 + 14)
        self.bar_click(220)                                  # Reset
        time.sleep(0.3)
        self.assertEqual(self.size_now(out, 3)[1], 80)
        self.assertEqual(self.term_window()['w'], 80 * 8 + 6 + 14)

    def test_terminal_fractional_zoom_is_smooth_and_keeps_the_text_readable(self):
        win, out = self.term('term-frac.txt')
        for _ in range(2):
            self.bar_click(180)                              # 125 %: 10x20 cells, a real fraction of 8x16
            time.sleep(0.15)
        type_text('go')
        time.sleep(0.5)
        win = self.term_window()                                                 # it grew, so it moved
        ppm = ctl.dump(os.path.join(tmp, 'term-frac.ppm'))
        colours = set()
        for y in range(win['y'] + self.BAR_H + 3, win['y'] + self.BAR_H + 3 + 40):
            for x in range(win['x'] + 3, win['x'] + 3 + 120):
                colours.add(ppm.px(x, y))
        self.assertGreater(len(colours), 6, 'a fractional size should show intermediate (anti-aliased) shades')
        self.bar_click(220)                                  # Reset: whole pixels again, only fg and bg
        time.sleep(0.4)
        win = self.term_window()
        ppm = ctl.dump(os.path.join(tmp, 'term-frac1.ppm'))
        colours = set()
        for y in range(win['y'] + self.BAR_H + 3, win['y'] + self.BAR_H + 3 + 40):
            for x in range(win['x'] + 3, win['x'] + 3 + 120):
                colours.add(ppm.px(x, y))
        self.assertLessEqual(len(colours), 4, colours)

    def cell_px(self, out, n):
        """The terminal's cell width in pixels, from its own answer (columns) and the window it made (w = cols * cell + 6)."""
        cols = self.size_now(out, n)[1]
        w = [x for x in ctl.windows()['windows'] if x['app_id'] == 'mellagui.term'][0]['w']
        self.assertEqual((w - 6 - 14) % cols, 0, (w, cols))      # (14 = the scroll bar strip)
        return (w - 6 - 14) // cols

    def conf(self, name):
        d = os.path.join(tmp, name)
        os.makedirs(d, exist_ok=True)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        return d, os.path.join(d, 'mellagui', 'terminal.conf')

    def term_in(self, confdir, name):
        out = os.path.join(tmp, name)
        script = os.path.join(tmp, name + '.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nwhile read l; do stty size >> "%s"; done\n' % out)
        os.chmod(script, 0o755)
        if os.path.exists(out):
            os.unlink(out)
        p = self.spawn('mellagui-term', SHELL=script, XDG_CONFIG_HOME=confdir)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        return p, win, out

    def wait_conf(self, path, text, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if os.path.exists(path) and text in open(path).read():
                return True
            time.sleep(0.05)
        return False

    def test_terminal_saves_its_zoom_and_the_next_one_starts_with_it(self):
        confdir, path = self.conf('tconf1')
        p, win, out = self.term_in(confdir, 'term-save1.txt')
        self.assertFalse(os.path.exists(path))                                    # nothing is written for nothing
        self.assertEqual(self.cell_px(out, 1), 8)
        ctl.click(win['x'] + 180, win['y'] + 8)                                  # A+: 110 %
        ctl.click(win['x'] + 180, win['y'] + 8)                                  # 125 %
        self.assertTrue(self.wait_conf(path, 'zoom=125'), 'the zoom was not saved')
        text = open(path).read()
        self.assertEqual([l for l in text.split('\n') if l and not l.startswith('#')], ['zoom=125'])
        p.terminate()
        p.wait(timeout=5)
        ctl.sync()
        for _ in range(60):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.term']:
                break
            time.sleep(0.05)
        p2, win2, out2 = self.term_in(confdir, 'term-save2.txt')                  # the next terminal
        self.assertEqual(self.cell_px(out2, 1), 10)                               # starts at 125 %: 10 px cells
        ctl.click(win2['x'] + 220, win2['y'] + 8)                                # Reset is saved, too
        self.assertTrue(self.wait_conf(path, 'zoom=100'))

    def test_a_normal_exit_saves_the_size_at_once(self):
        confdir, path = self.conf('tconf2')
        script = os.path.join(tmp, 'exit-at-once.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nread l\nexit 0\n')
        os.chmod(script, 0o755)
        p = self.spawn('mellagui-term', SHELL=script, XDG_CONFIG_HOME=confdir)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win)
        time.sleep(0.4)
        ctl.click(win['x'] + 130, win['y'] + 8)                                  # the zoom list
        time.sleep(0.2)
        ctl.click(win['x'] + 130, win['y'] + self.BAR_H + 2 + 9 * 20 + 10)       # 200 %
        type_text('x')                                                            # the shell ends: the terminal exits inside the save delay
        p.wait(timeout=5)
        self.assertIn('zoom=200', open(path).read())

    def test_a_damaged_or_foreign_file_is_ignored(self):
        for n, content in enumerate(('zoom=abc\n', 'zoom=9999\n', 'zoom=10\n', 'garbage\x00\xff\n', '')):
            confdir, path = self.conf('tconf-bad%d' % n)
            os.makedirs(os.path.dirname(path))
            with open(path, 'w', errors='ignore') as f:
                f.write(content)
            p, win, out = self.term_in(confdir, 'term-bad%d.txt' % n)
            self.assertEqual(self.cell_px(out, 1), 8, repr(content))              # 100 %
            p.terminate()
            p.wait(timeout=5)
            for _ in range(60):
                if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.term']:
                    break
                time.sleep(0.05)

    def test_a_saved_value_between_the_steps_is_kept(self):
        confdir, path = self.conf('tconf3')
        os.makedirs(os.path.dirname(path))
        with open(path, 'w') as f:
            f.write('# hand edited\nzoom=135\nfuture-key=1\n')
        p, win, out = self.term_in(confdir, 'term-save5.txt')
        self.assertEqual(self.cell_px(out, 1), 11)                                # 135 % of 8 px, rounded
        ctl.click(win['x'] + 180, win['y'] + 8)                                  # A+ goes to the next level above: 150 %
        self.assertTrue(self.wait_conf(path, 'zoom=150'))

    def test_terminal_ctrl_keys_and_wheel_zoom_in_steps(self):
        win, out = self.term('term-keys.txt')
        ctl.abs(win['x'] + 100, win['y'] + 100)
        ctl.key(KEY_LEFTCTRL, True)
        ctl.tap(13)                                                               # Ctrl+= : 110 %
        ctl.key(KEY_LEFTCTRL, False)
        time.sleep(0.3)
        self.assertEqual(self.term_window()['w'], 80 * 9 + 6 + 14)                     # the window grew, still 80 columns
        self.assertEqual(self.size_now(out, 1)[1], 80)
        ctl.key(KEY_LEFTCTRL, True)
        ctl.tap(11)                                                               # Ctrl+0 : reset
        ctl.key(KEY_LEFTCTRL, False)
        time.sleep(0.3)
        self.assertEqual(self.size_now(out, 2)[1], 80)
        self.assertEqual(self.term_window()['w'], 80 * 8 + 6 + 14)

    def test_terminal_has_a_scrollbar_for_the_scrollback(self):
        script = os.path.join(tmp, 'many.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\ni=0\nwhile [ $i -lt 300 ]; do echo line$i; i=$((i+1)); done\nread l\n')
        os.chmod(script, 0o755)
        self.spawn('mellagui-term', SHELL=script)
        win = wait_for_window('mellagui.term')
        self.assertIsNotNone(win)
        time.sleep(0.6)
        ctl.abs(win['x'] + 100, win['y'] + 100)                          # (a hovered thumb is lighter)
        t0 = self.thumb(win, 'tsb0.ppm')
        self.assertIsNotNone(t0, 'the scrollback should show a thumb')
        self.assertEqual(t0[2], 14 - 4)
        self.assertGreater(t0[0] + t0[1], win['y'] + win['h'] - 6)       # live view: the thumb is at the bottom
        right = win['x'] + win['w'] - 6
        ctl.click(right, win['y'] + 60)                                  # the track above it: one page back
        time.sleep(0.3)
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t1 = self.thumb(win, 'tsb1.ppm')
        self.assertLess(t1[0], t0[0])
        ctl.abs(right, t1[0] + 5)                                        # drag the thumb to the top: the oldest line
        ctl.button(272, True)
        ctl.abs(right, win['y'] + 10)
        ctl.button(272, False)
        time.sleep(0.3)
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t2 = self.thumb(win, 'tsb2.ppm')
        self.assertLess(t2[0], win['y'] + 60)
        ctl.abs(right, t2[0] + 5)                                        # and back down to the live view
        ctl.button(272, True)
        ctl.abs(right, win['y'] + win['h'] - 2)
        ctl.button(272, False)
        time.sleep(0.3)
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t3 = self.thumb(win, 'tsb3.ppm')
        self.assertGreater(t3[0] + t3[1], win['y'] + win['h'] - 6)

    def test_terminal_scrollbar_is_empty_without_scrollback(self):
        win, out = self.term('term-nosb.txt')
        ctl.abs(win['x'] + 100, win['y'] + 100)
        self.assertIsNone(self.thumb(win, 'tsb-none.ppm'))

    def test_edit_has_a_scrollbar_and_the_wheel_stays_where_it_is(self):
        p, path = self.edit_start(''.join('line %d\n' % i for i in range(150)).encode())
        win = self.edit_window()
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t0 = self.thumb(win, 'esb0.ppm')
        self.assertIsNotNone(t0)
        self.assertEqual(t0[2], 14 - 4)
        self.assertLess(t0[0], win['y'] + 40)                            # the cursor is on line 1: the thumb is at the top
        right = win['x'] + win['w'] - 6
        ctl.click(right, min(win['y'] + win['h'] - 40, 500))             # the track below it: one page down (not where the dock floats)
        time.sleep(0.3)
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t1 = self.thumb(win, 'esb1.ppm')
        self.assertGreater(t1[0], t0[0])
        time.sleep(0.3)
        t1b = self.thumb(win, 'esb1b.ppm')                               # (the view does not jump back to the cursor)
        self.assertEqual(t1b[0], t1[0])
        ctl.abs(right, t1[0] + 5)                                        # drag to the bottom
        ctl.button(272, True)
        ctl.abs(right, win['y'] + win['h'] - 20)
        ctl.button(272, False)
        time.sleep(0.3)
        ctl.abs(win['x'] + 100, win['y'] + 100)
        t2 = self.thumb(win, 'esb2.ppm')
        self.assertGreater(t2[0], t1[0] + 50)
        ctl.cmd('axis v 15 1')                                           # the wheel scrolls from there, the cursor stays on line 1
        time.sleep(0.3)
        t3 = self.thumb(win, 'esb3.ppm')
        self.assertIsNotNone(t3)
        self.assertGreaterEqual(t3[0], t2[0])
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)

    # ---- the on-screen keyboard ----------------------------------------------------------------------------------------------

    def osk(self, arg=''):
        """The control socket's `osk` answer: (head fields, {evdev code: (x, y, w, h, label, on)} of the keys)."""
        if arg == 'open':                                                        # (the next test must find the keyboard closed and in its default place)
            self.addCleanup(ctl.cmd, 'osk reset')
            self.addCleanup(ctl.cmd, 'osk close')
        return self.osk_parse('\n'.join(ctl.cmd('osk %s' % arg if arg else 'osk')))

    @staticmethod
    def osk_parse(text):
        head = dict(kv.split('=') for kv in text.split('\n')[0].split()[1:])
        keys = {}
        for m in re.finditer(r'key (\d+) code=(\d+) kind=(\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) on=(\d) held=(\d) label=(.*)', text):
            keys[int(m.group(2))] = (int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)), m.group(10), int(m.group(8)), int(m.group(9)))
        return head, keys

    def osk_click(self, code):
        x, y, w, h = self.osk()[1][code][:4]
        ctl.click(x + w // 2, y + h // 2)
        time.sleep(0.1)

    def test_osk_opens_with_a_key_closes_with_hide_and_lists_its_keys(self):
        head, keys = self.osk()
        self.assertEqual(head['open'], '0')
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(37)                                                              # Super+K
        ctl.key(KEY_LEFTMETA, False)
        time.sleep(0.2)
        head, keys = self.osk()
        self.assertEqual(head['open'], '1')
        self.assertEqual(int(head['n']), len(keys))                              # (Hide has no code: it is listed under code 0)
        self.assertEqual(keys[30][4], 'a')
        self.assertEqual(keys[30][5], 0)
        card = re.search(r'card x=(\d+) y=(\d+) w=(\d+) h=(\d+)', '\n'.join(ctl.cmd('osk')))
        self.assertLessEqual(int(card.group(2)) + int(card.group(4)), 600)       # it sits inside the output, along the bottom
        self.assertGreater(int(card.group(2)) + int(card.group(4)) // 2, 300)           # its centre is in the lower half
        self.osk_click(0)                                                        # the Hide key
        time.sleep(0.1)
        self.assertEqual(self.osk()[0]['open'], '0')
        self.osk('open')
        self.assertEqual(self.osk()[0]['open'], '1')
        self.osk('close')
        self.assertEqual(self.osk()[0]['open'], '0')

    def test_osk_types_into_the_editor_with_shift_and_does_not_take_the_focus(self):
        p, path = self.edit_start(b'')
        win = self.edit_window()
        self.assertIn('focused', win['flags'])
        self.osk('open')
        self.osk_click(35)                                                       # h
        self.osk_click(23)                                                       # i
        self.osk_click(42)                                                       # Shift latches ...
        head, keys = self.osk()
        self.assertEqual(keys[35][4], 'H')                                       # ... and the labels change
        self.assertEqual(keys[42][5], 1)
        self.osk_click(35)                                                       # H, and the latch is gone again
        head, keys = self.osk()
        self.assertEqual(keys[35][4], 'h')
        self.assertEqual(keys[42][5], 0)
        self.assertEqual(int(head['latched']), 0)
        self.assertIn('focused', self.edit_window()['flags'])                    # the editor kept the focus
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path).strip(), b'hiH')
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_osk_ctrl_latch_makes_a_shortcut_and_a_second_tap_lets_go(self):
        p, path = self.edit_start(b'abc\n')
        self.osk('open')
        self.osk_click(29)                                                       # Ctrl on
        self.assertEqual(self.osk()[1][29][5], 1)
        self.osk_click(29)                                                       # off again
        self.assertEqual(self.osk()[1][29][5], 0)
        self.osk_click(29)                                                       # Ctrl + A: select everything
        self.osk_click(30)
        self.assertEqual(int(self.osk()[0]['latched']), 0)
        self.osk_click(48)                                                       # b replaces the selection
        time.sleep(0.2)
        self.chord(self.CTRL, self.K_S)
        time.sleep(0.3)
        self.assertEqual(self.read_file(path).strip(), b'b')
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)

    def osk_card(self):
        m = re.search(r'card x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', '\n'.join(ctl.cmd('osk')))
        return tuple(int(v) for v in m.groups())

    def test_osk_is_moved_by_its_top_strip_and_resized_by_its_right_and_bottom_margin(self):
        self.osk('open')
        x, y, w, h = self.osk_card()
        ctl.abs(x + 40, y + 5)                                                   # the top strip moves it
        ctl.button(272, True)
        ctl.abs(x + 40 - 4, y + 5 - 120)
        ctl.button(272, False)
        time.sleep(0.1)
        x2, y2, w2, h2 = self.osk_card()
        self.assertEqual((x2, y2, w2, h2), (x - 4, y - 120, w, h))
        ctl.abs(x2 + w2 - 4, y2 + h2 - 4)                                        # the corner resizes: wider and taller
        ctl.button(272, True)
        ctl.abs(x2 + w2 - 4 - 200, y2 + h2 - 4 + 40)
        ctl.button(272, False)
        time.sleep(0.1)
        x3, y3, w3, h3 = self.osk_card()
        self.assertEqual((x3, y3), (x2, y2))                                     # the top left corner stays
        self.assertEqual(w3, w2 - 200)
        self.assertGreater(h3, h2)
        head, keys = self.osk()
        self.assertGreater(keys[30][0], x3)                                      # the keys follow the card
        self.assertLess(keys[28][0] + keys[28][2], x3 + w3)
        ctl.abs(x3 + 40, y3 + 5)                                                 # it cannot be dragged off the screen
        ctl.button(272, True)
        ctl.abs(-500, -500)
        ctl.button(272, False)
        time.sleep(0.1)
        x4, y4, w4, h4 = self.osk_card()
        self.assertGreaterEqual(x4, 0)
        self.assertGreaterEqual(y4, 0)
        ctl.abs(x4 + w4 - 4, y4 + h4 - 4)                                        # and cannot be made smaller than its keys allow
        ctl.button(272, True)
        ctl.abs(x4 + 10, y4 + 10)
        ctl.button(272, False)
        time.sleep(0.1)
        self.assertGreaterEqual(self.osk_card()[2], 240)
        self.osk('close')
        self.osk('open')
        self.assertEqual(self.osk_card()[:2], (x4, y4))                          # it comes back where it was left
        x5, y5 = self.osk_card()[:2]
        ctl.click(x5 + 40, y5 + 5)                                               # a double click on the margin puts it back
        ctl.click(x5 + 40, y5 + 5)
        time.sleep(0.1)
        self.assertEqual(self.osk_card(), (4, 600 - 306 - 6, 1016, 306))

    def test_osk_shows_what_is_held_down_on_the_real_keyboard(self):
        self.osk('open')
        keys = self.osk()[1]
        self.assertEqual((keys[30][6], keys[42][6], keys[54][6]), (0, 0, 0))
        ctl.key(30, True)                                                        # a, on the real keyboard
        time.sleep(0.1)
        keys = self.osk()[1]
        self.assertEqual(keys[30][6], 1)
        self.assertEqual(keys[31][6], 0)
        ctl.key(30, False)
        ctl.key(54, True)                                                        # the right Shift: both Shift keys light and the letters change
        time.sleep(0.1)
        keys = self.osk()[1]
        self.assertEqual((keys[42][6], keys[54][6], keys[30][4]), (1, 1, 'A'))
        ctl.key(54, False)
        time.sleep(0.1)
        keys = self.osk()[1]
        self.assertEqual((keys[30][6], keys[42][6], keys[54][6], keys[30][4]), (0, 0, 0, 'a'))

    def test_windows_make_room_for_the_keyboard_and_come_back_with_the_setting_on(self):
        self.addCleanup(ctl.cmd, 'settings osk-avoid 1')
        p, path = self.edit_start(b'one\n')
        w0 = self.edit_window()
        self.osk('open')
        time.sleep(0.5)
        card = self.osk_card()
        w1 = self.edit_window()
        self.assertLessEqual(w1['y'] + w1['h'] + 4, card[1])                     # (4 = the frame) the window ends above the keyboard
        self.assertGreaterEqual(w1['y'], 28)                                     # and still starts below the panel
        self.osk('close')
        time.sleep(0.5)
        self.assertEqual({k: self.edit_window()[k] for k in 'xywh'}, {k: w0[k] for k in 'xywh'})   # back where it was
        ctl.cmd('settings osk-avoid 0')
        self.osk('open')
        time.sleep(0.4)
        self.assertEqual({k: self.edit_window()[k] for k in 'xywh'}, {k: w0[k] for k in 'xywh'})   # the setting is off: it stays under the keyboard
        ctl.cmd('settings osk-avoid 1')                                          # switching it on moves the windows at once
        time.sleep(0.4)
        w2 = self.edit_window()
        self.assertLessEqual(w2['y'] + w2['h'] + 4, card[1])
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_a_maximized_window_gets_smaller_for_the_keyboard(self):
        self.addCleanup(ctl.cmd, 'settings osk-avoid 1')
        p, path = self.edit_start(b'one\n')
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_UP)                                                          # Super+Up: maximize
        ctl.key(KEY_LEFTMETA, False)
        time.sleep(0.5)
        full = self.edit_window()
        self.osk('open')
        time.sleep(0.5)
        card = self.osk_card()
        part = self.edit_window()
        self.assertLess(part['h'], full['h'])
        self.assertLessEqual(part['y'] + part['h'] + 4, card[1])
        self.osk('close')
        time.sleep(0.5)
        self.assertEqual(self.edit_window()['h'], full['h'])
        self.chord(self.CTRL, self.K_Q)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_osk_labels_follow_the_layout(self):
        self.addCleanup(settings_restore)
        ctl.cmd('settings layouts us,ee')
        self.osk('open')
        self.assertEqual(self.osk()[1][26][4], '[')                             # the key right of P
        ctl.cmd('layout next')
        time.sleep(0.2)
        self.assertEqual(self.osk()[1][26][4], '\u00fc')                         # u-umlaut on the Estonian layout
        ctl.cmd('layout prev')
        self.osk('close')


DEFAULT_SETTINGS = {'layouts': 'us,ee,de,fi', 'layout': 'us', 'sticky': '0', 'repeat-delay': '400',
                    'repeat-rate': '25', 'pointer-speed': '100', 'natural-scroll': '0', 'sound': '1', 'gadget': '1', 'mixer': '0', 'sysinfo': '0'}


def settings():
    """Current settings from the control socket as a dict (the `file` entry is "-" when nothing is saved)."""
    ctl.cmd('settings')
    return dict(kv.split('=', 1) for kv in ctl.last.split()[1:])


def settings_restore():
    for k in ('layouts', 'layout', 'sticky', 'repeat-delay', 'repeat-rate', 'pointer-speed', 'natural-scroll',
              'sound', 'gadget', 'mixer', 'sysinfo'):
        ctl.cmd('settings %s %s' % (k, DEFAULT_SETTINGS[k]))


class TestSettings(Base):
    """mellagui_settings_v1, the settings file and the mellagui-settings panel."""

    def setUp(self):
        super().setUp()
        self.addCleanup(settings_restore)

    def bound(self):
        """A raw client bound to mellagui_settings_v1 with the first value burst already read."""
        c = self.client()
        obj = c.wl.bind('mellagui_settings_v1')
        c.wl.roundtrip()
        return c, obj

    @staticmethod
    def burst(c, obj, timeout=2.0):
        """Wait for one value burst (up to `done`); returns {key: value}."""
        vals = {}
        end = time.time() + timeout
        while time.time() < end:
            c.wl.pump(0.05)
            for e in list(c.wl.queue):
                if e.obj != obj:
                    continue
                c.wl.queue.remove(e)
                if e.name == 'value':
                    vals[e.args[0]] = e.args[1]
                elif e.name == 'done':
                    return vals
        return None

    def test_control_socket_reads_and_changes(self):
        s = settings()
        for k, v in DEFAULT_SETTINGS.items():
            self.assertEqual(s[k], v, k)
        ctl.cmd('settings repeat-rate 40')
        self.assertEqual(settings()['repeat-rate'], '40')
        ctl.cmd('settings pointer-speed 150')
        self.assertEqual(settings()['pointer-speed'], '150')

    def test_bad_values_are_refused(self):
        for k, v in [('repeat-rate', 'fast'), ('repeat-rate', '1000'), ('repeat-delay', '5'), ('sticky', '2'),
                     ('layouts', 'us,xx'), ('layout', 'xx'), ('pointer-speed', '0'), ('nonsense', '1'),
                     ('file', '/etc/passwd')]:
            with self.assertRaises(RuntimeError, msg='%s=%s' % (k, v)):
                ctl.cmd('settings %s %s' % (k, v))
        self.assertEqual(settings()['repeat-rate'], '25')
        # the active layout has to be one of the enabled ones
        ctl.cmd('settings layouts us,ee')
        with self.assertRaises(RuntimeError):
            ctl.cmd('settings layout de')
        self.assertEqual(settings()['layout'], 'us')

    def test_protocol_burst_on_bind_and_after_set(self):
        c = self.client()
        obj = c.wl.bind('mellagui_settings_v1')
        first = self.burst(c, obj)
        self.assertIsNotNone(first, 'no value burst after binding')
        self.assertEqual(first['layouts'], 'us,ee,de,fi')
        self.assertIn('file', first)
        c.wl.request(obj, 'set', 'repeat-delay', '700')
        got = self.burst(c, obj)
        self.assertEqual(got['repeat-delay'], '700')
        self.assertEqual(settings()['repeat-delay'], '700')
        # a refused change answers with the unchanged value
        c.wl.request(obj, 'set', 'repeat-delay', 'soon')
        self.assertEqual(self.burst(c, obj)['repeat-delay'], '700')

    def test_changes_from_elsewhere_are_announced(self):
        c, obj = self.bound()
        self.burst(c, obj, 0.2)
        c.wl.queue.clear()
        ctl.cmd('sticky 1')
        self.assertEqual(self.burst(c, obj)['sticky'], '1')
        ctl.cmd('layout next')
        self.assertEqual(self.burst(c, obj)['layout'], 'ee')
        ctl.cmd('gadget 0')
        self.assertEqual(self.burst(c, obj)['gadget'], '0')
        ctl.cmd('gadget 1')

    def test_settings_reach_every_client(self):
        a, oa = self.bound()
        b, ob = self.bound()
        self.burst(a, oa, 0.2)
        self.burst(b, ob, 0.2)
        a.wl.request(oa, 'set', 'sound', '0')
        self.assertEqual(self.burst(b, ob)['sound'], '0')

    def test_new_layout_set_sends_a_new_keymap_and_group(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        Window(c, 200, 100)
        c.wl.roundtrip()
        c.wl.events(0.1)
        c.wl.queue.clear()
        ctl.cmd('settings layouts de,ee')
        ev = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'keymap', 2.0)
        self.assertIsNotNone(ev, 'clients are not sent the new keymap')
        data = os.pread(ev.fds[0], ev.args[1], 0).decode()
        self.assertIn('name[Group1] = "German";', data)
        self.assertIn('name[Group2] = "Estonian";', data)
        self.assertNotIn('Group3', data)
        self.assertNotIn('English (US)', data)
        self.assertIsNotNone(c.wl.wait_for(lambda e: e.obj == kb and e.name == 'modifiers' and e.args[4] == 0, 2.0))
        self.assertEqual(ctl.kbd()['layout'], 'de')
        ctl.cmd('settings layout ee')
        self.assertIsNotNone(c.wl.wait_for(lambda e: e.obj == kb and e.name == 'modifiers' and e.args[4] == 1, 2.0))
        self.assertEqual(ctl.kbd()['layout'], 'ee')

    def test_removing_the_active_layout_falls_back_to_the_first(self):
        ctl.cmd('settings layout ee')
        ctl.cmd('settings layouts us,de')
        self.assertEqual(ctl.kbd()['layout'], 'us')
        ctl.cmd('layout next')
        self.assertEqual(ctl.kbd()['layout'], 'de')
        ctl.cmd('settings layouts de,us,fi')               # reorder: the active layout stays the active one
        self.assertEqual(ctl.kbd()['layout'], 'de')
        self.assertEqual(ctl.kbd()['slot'], 0)

    def test_repeat_info_reaches_keyboards(self):
        c = self.client()
        seat = c.wl.bind('wl_seat', 5)
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        c.wl.roundtrip()
        c.wl.events(0.1)
        c.wl.queue.clear()
        ctl.cmd('settings repeat-rate 11')
        ctl.cmd('settings repeat-delay 650')
        ev = c.wl.wait_for(lambda e: e.obj == kb and e.name == 'repeat_info' and e.args == [11, 650], 2.0)
        self.assertIsNotNone(ev, 'repeat_info with the new values was not sent')

    def test_gadget_setting_toggles_the_gadget(self):
        ctl.cmd('settings gadget 0')
        ctl.cmd('clip')
        self.assertIn('gadget=0', ctl.last)
        ctl.cmd('settings gadget 1')
        ctl.cmd('clip')
        self.assertIn('gadget=1', ctl.last)

    # ---- the file ---------------------------------------------------------------------------------------------------

    # ---- resolution and sound outputs -----------------------------------------------------------------------------

    @staticmethod
    def get_setting(c, key):
        c.cmd('setting ' + key)
        return c.last[3:]

    def test_resolution_changes_reverts_and_persists(self):
        path = os.path.join(tmp, 'res', 'settings.conf')
        p, c, stop = self.private_compositor('mellagui-res1', '--settings', path, env={'MELLAGUI_REVERT_MS': '800'})
        g = lambda k: self.get_setting(c, k)
        self.assertEqual((g('resolution'), g('resolution-now'), g('resolution-confirm')), ('auto', '800x500@60', ''))
        self.assertIn('1280x720@60|', g('resolutions'))
        self.assertIn('800x500@60|preferred', g('resolutions'))
        # a change goes through at once ...
        c.cmd('settings resolution 1280x720@60')
        self.assertEqual(g('resolution-now'), '1280x720@60')
        self.assertNotEqual(g('resolution-confirm'), '')
        ppm = c.dump(os.path.join(tmp, 'res1.ppm'))
        self.assertEqual((ppm.w, ppm.h), (1280, 720))
        # ... and is undone when it is not kept (the screen could be black)
        time.sleep(1.6)
        self.assertEqual((g('resolution'), g('resolution-now'), g('resolution-confirm')), ('auto', '800x500@60', ''))
        self.assertFalse(os.path.exists(path) and 'resolution=1280' in open(path).read())
        # kept: stays after the revert time and is saved
        c.cmd('settings resolution 1024x768@60')
        c.cmd('settings resolution-confirm 1')
        time.sleep(1.3)
        self.assertEqual((g('resolution-now'), g('resolution-confirm')), ('1024x768@60', ''))
        for _ in range(40):
            if os.path.exists(path) and 'resolution=1024x768@60' in open(path).read():
                break
            time.sleep(0.05)
        self.assertIn('resolution=1024x768@60', open(path).read())
        # nonsense is refused, the screen stays as it is
        for bad in ('10x10', 'foo', '1920x1080@5', '0x0'):
            with self.assertRaises(RuntimeError):
                c.cmd('settings resolution ' + bad)
        self.assertEqual(g('resolution-now'), '1024x768@60')
        # "Go back" by hand
        c.cmd('settings resolution 800x600@60')
        self.assertEqual(g('resolution-now'), '800x600@60')
        c.cmd('settings resolution-confirm 0')
        self.assertEqual((g('resolution'), g('resolution-now')), ('1024x768@60', '1024x768@60'))
        stop()
        # the saved resolution is used at the next start
        p2, c2, stop2 = self.private_compositor('mellagui-res2', '--settings', path)
        self.assertEqual(self.get_setting(c2, 'resolution-now'), '1024x768@60')
        self.assertEqual(self.get_setting(c2, 'resolution'), '1024x768@60')

    def wait_window(self, c, app_id, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            for w in c.windows()['windows']:
                if w['app_id'] == app_id:
                    return w
            time.sleep(0.05)
        self.fail('no window ' + app_id)

    def test_maximized_window_follows_a_resolution_change(self):
        exe = self.exe()
        p, c, stop = self.private_compositor('mellagui-res3', env={'MELLAGUI_REVERT_MS': '60000'})
        panel = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-res3'), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (panel.poll() is None) and (panel.kill(), panel.wait()))
        self.wait_window(c, 'mellagui.settings')
        c.abs(400, 300)
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_M)
        c.key(KEY_LEFTMETA, False)
        for _ in range(60):
            w = c.windows()['windows'][0]
            if 'maximized' in w['flags'] and (w['w'], w['h']) == (800 - 8, 500 - 64):
                break
            time.sleep(0.05)
        self.assertEqual((w['w'], w['h']), (800 - 8, 500 - 64))
        c.cmd('settings resolution 1280x720@60')
        for _ in range(60):
            w = c.windows()['windows'][0]
            if (w['w'], w['h']) == (1280 - 8, 720 - 64):
                break
            time.sleep(0.05)
        self.assertEqual((w['w'], w['h']), (1280 - 8, 720 - 64))
        c.cmd('settings resolution-confirm 0')
        for _ in range(60):
            w = c.windows()['windows'][0]
            if (w['w'], w['h']) == (800 - 8, 500 - 64):
                break
            time.sleep(0.05)
        self.assertEqual((w['w'], w['h']), (800 - 8, 500 - 64))

    def audio_env(self):
        """A fake /proc/asound (a card with analog, digital and three HDMI outputs) and a fake `pactl` with two sinks."""
        d = os.path.join(tmp, 'audio')
        os.makedirs(os.path.join(d, 'asound'), exist_ok=True)
        os.makedirs(os.path.join(d, 'bin'), exist_ok=True)
        with open(os.path.join(d, 'asound', 'cards'), 'w') as f:
            f.write(' 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n                      HDA Intel PCH at 0xa0710000 irq 53\n'
                    ' 1 [U0x46d0x825    ]: USB-Audio - USB Device 0x46d:0x825\n                      USB Device at usb-0000:00:14.0-1.4.1.1\n')
        with open(os.path.join(d, 'asound', 'pcm'), 'w') as f:
            f.write('00-00: CS4206 Analog : CS4206 Analog : playback 1 : capture 1\n00-01: CS4206 Digital : CS4206 Digital : playback 1 : capture 1\n'
                    '00-03: HDMI 0 : HDMI 0 : playback 1\n00-07: HDMI 1 : HDMI 1 : playback 1\n00-08: HDMI 2 : HDMI 2 : playback 1\n'
                    '01-00: USB Audio : USB Audio : capture 1\n')
        state = os.path.join(d, 'sink-state')
        if os.path.exists(state):
            os.unlink(state)
        pactl = os.path.join(d, 'bin', 'pactl')
        with open(pactl, 'w') as f:
            f.write("""#!/bin/sh
state="%s"
case "$1" in
  list) printf 'Sink #1\\n\\tState: RUNNING\\n\\tName: alsa_output.analog-stereo\\n\\tDescription: Built-in Audio Analog Stereo\\nSink #2\\n\\tName: alsa_output.hdmi-stereo\\n\\tDescription: Built-in Audio Digital Stereo (HDMI)\\n' ;;
  get-default-sink) if [ -f "$state" ]; then cat "$state"; else echo alsa_output.analog-stereo; fi ;;
  set-default-sink) case "$2" in alsa_output.analog-stereo|alsa_output.hdmi-stereo) echo "$2" > "$state" ;; esac ;;
esac
""" % state)
        os.chmod(pactl, 0o755)
        pwcli = os.path.join(d, 'bin', 'pw-cli')                                # no PipeWire here: never ask the real one
        with open(pwcli, 'w') as f:
            f.write('#!/bin/sh\nexit 1\n')
        os.chmod(pwcli, 0o755)
        return state, {'MELLAGUI_ASOUND_DIR': os.path.join(d, 'asound'), 'PATH': os.path.join(d, 'bin') + ':' + os.environ['PATH']}

    def test_sound_outputs_list_hdmi_and_can_be_chosen(self):
        state, env = self.audio_env()
        path = os.path.join(tmp, 'audio', 'settings.conf')
        p, c, stop = self.private_compositor('mellagui-aud1', '--settings', path, env=env)
        g = lambda k: self.get_setting(c, k)
        outs = g('audio-outs')
        self.assertTrue(outs.startswith('default|System default;'), outs)
        self.assertIn('plughw:CARD=PCH,DEV=3|HDA Intel PCH - HDMI 0 (HDMI)', outs)
        self.assertIn('plughw:CARD=PCH,DEV=8|HDA Intel PCH - HDMI 2 (HDMI)', outs)
        self.assertNotIn('USB Audio', outs)                                   # capture-only
        self.assertEqual(g('audio-out'), 'default')
        c.cmd('settings audio-out plughw:CARD=PCH,DEV=3')
        self.assertEqual(g('audio-out'), 'plughw:CARD=PCH,DEV=3')
        for bad in ('a;b', 'x|y', 'a' * 64, 'quo"te', '../etc', 'a$b'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('settings audio-out ' + bad)
        self.assertEqual(g('audio-out'), 'plughw:CARD=PCH,DEV=3')
        for _ in range(40):
            if os.path.exists(path) and 'audio-out=plughw:CARD=PCH,DEV=3' in open(path).read():
                break
            time.sleep(0.05)
        self.assertIn('audio-out=plughw:CARD=PCH,DEV=3', open(path).read())
        # system output (PulseAudio / PipeWire) through pactl
        self.assertEqual((g('sinks'), g('sink')), ('', ''))                   # nothing asked yet
        c.cmd('settings audio-rescan 1')
        self.assertIn('alsa_output.hdmi-stereo|Built-in Audio Digital Stereo (HDMI)', g('sinks'))
        self.assertEqual(g('sink'), 'alsa_output.analog-stereo')
        c.cmd('settings sink alsa_output.hdmi-stereo')
        self.assertEqual(g('sink'), 'alsa_output.hdmi-stereo')
        self.assertEqual(open(state).read().strip(), 'alsa_output.hdmi-stereo')
        with self.assertRaises(RuntimeError):
            c.cmd('settings sink alsa_output.nosuch')                           # pactl refuses it: not accepted
        with self.assertRaises(RuntimeError):
            c.cmd('settings sink bad;sink')                                     # not even a valid name
        stop()
        p2, c2, stop2 = self.private_compositor('mellagui-aud2', '--settings', path, env=env)
        self.assertEqual(self.get_setting(c2, 'audio-out'), 'plughw:CARD=PCH,DEV=3')     # remembered

    def test_sound_outputs_through_wpctl_when_pactl_is_missing(self):
        d = os.path.join(tmp, 'audio-wp')
        stubs = os.path.join(d, 'bin')
        os.makedirs(stubs, exist_ok=True)
        with open(os.path.join(stubs, 'pactl'), 'w') as f:
            f.write('#!/bin/sh\nexit 1\n')                                      # pulseaudio-utils without a server
        os.chmod(os.path.join(stubs, 'pactl'), 0o755)
        wj = os.path.join(d, 'wpctl.json')
        if os.path.exists(wj):
            os.unlink(wj)
        p, c, stop = self.private_compositor('mellagui-aud5', env={'MELLAGUI_FAKE_WPCTL': wj, 'PATH': stubs + ':' + FAKES + ':' + os.environ['PATH']})
        g = lambda k: self.get_setting(c, k)
        self.assertEqual(g('sinks-status'), '')
        c.cmd('settings audio-rescan 1')
        self.assertEqual(g('sinks-status'), 'ok')
        self.assertEqual(g('sinks'), '53|Built-in Audio Analog Stereo;54|Built-in Audio Digital Stereo (HDMI)')
        self.assertEqual(g('sink'), '53')
        c.cmd('settings sink 54')
        self.assertEqual(g('sink'), '54')
        with open(wj) as f:
            self.assertEqual(json.load(f)['default'], 54)
        with self.assertRaises(RuntimeError):
            c.cmd('settings sink 99')                                            # no such object: wpctl leaves the default alone
        self.assertEqual(g('sink'), '54')

    def test_sound_status_says_why_there_are_no_outputs(self):
        # nothing installed
        p, c, stop = self.private_compositor('mellagui-aud6', env={'PATH': os.path.join(tmp, 'nonexistent-bin')})
        c.cmd('settings audio-rescan 1')
        self.assertEqual(self.get_setting(c, 'sinks-status'), 'missing-tools')
        stop()
        # the tools are there but no server answers (PipeWire is not running)
        d = os.path.join(tmp, 'audio-noserver')
        os.makedirs(d, exist_ok=True)
        for prog in ('pactl', 'wpctl'):
            with open(os.path.join(d, prog), 'w') as f:
                f.write('#!/bin/sh\nexit 1\n')
            os.chmod(os.path.join(d, prog), 0o755)
        p, c, stop = self.private_compositor('mellagui-aud7', env={'PATH': d + ':' + os.environ['PATH']})
        c.cmd('settings audio-rescan 1')
        self.assertEqual(self.get_setting(c, 'sinks-status'), 'no-server')
        self.assertEqual(self.get_setting(c, 'sinks'), '')

    def pw_env(self, tag):
        d = os.path.join(tmp, 'pw-' + tag)
        os.makedirs(d, exist_ok=True)
        pj = os.path.join(d, 'pw.json')
        if os.path.exists(pj):
            os.unlink(pj)
        return pj, {'MELLAGUI_FAKE_PW': pj, 'MELLAGUI_FAKE_WPCTL': os.path.join(d, 'wpctl.json'), 'PATH': FAKES + ':' + os.environ['PATH']}

    def test_hdmi_is_chosen_by_switching_the_card_profile(self):
        pj, env = self.pw_env('switch')
        p, c, stop = self.private_compositor('mellagui-pw1', env=env)
        g = lambda k: self.get_setting(c, k)
        self.assertEqual((g('audio-profiles'), g('audio-profile')), ('', ''))              # nothing asked yet
        c.cmd('settings audio-rescan 1')
        self.assertEqual(g('audio-profiles'), '43:3|Built-in Audio - Analog Stereo Output;43:6|Built-in Audio - Digital Stereo (HDMI) Output;'
                                              '43:15|Built-in Audio - Digital Stereo (IEC958) Output')
        self.assertEqual(g('audio-profile'), '43:3')                      # the active profile is analog + an IEC958 input
        c.cmd('settings audio-profile 43:6')                              # HDMI
        with open(pj) as f:
            self.assertEqual(json.load(f)['cur']['43'], 6)
        self.assertEqual(g('audio-profile'), '43:6')
        time.sleep(1.2)                                                   # the follow-up rescan refreshes the sink list by itself
        c.cmd('settings audio-profile 43:3')
        self.assertEqual(g('audio-profile'), '43:3')
        for bad in ('43:99', '99:1', 'foo', '43:', '43:6x', '-1:2'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('settings audio-profile ' + bad)
        with open(pj) as f:
            self.assertEqual(json.load(f)['cur']['43'], 3)                # refused: nothing changed

    def test_card_profiles_are_empty_without_pw_cli(self):
        p, c, stop = self.private_compositor('mellagui-pw2', env={'PATH': os.path.join(tmp, 'nonexistent-bin')})
        c.cmd('settings audio-rescan 1')
        self.assertEqual((self.get_setting(c, 'audio-profiles'), self.get_setting(c, 'audio-profile')), ('', ''))
        with self.assertRaises(RuntimeError):
            c.cmd('settings audio-profile 43:6')

    def test_panel_sound_page_switches_to_hdmi(self):
        exe = self.exe()
        pj, env = self.pw_env('panel')
        path = os.path.join(tmp, 'pw-panel', 'settings.conf')
        p, c, stop = self.private_compositor('mellagui-pw3', '--settings', path, env=env)
        panel = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-pw3'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (panel.poll() is None) and (panel.kill(), panel.wait()))
        win = self.wait_window(c, 'mellagui.settings')
        x0, y0 = win['x'], win['y']
        time.sleep(0.3)
        c.click(x0 + 70, y0 + 12 + 4 * 32 + 16)                           # Sound tab: asks the sound server again
        time.sleep(0.8)
        # "Output device" heading at y=14, then Analog (42), HDMI (70), IEC958 (98)
        c.click(x0 + 200, y0 + 70 + 14)
        time.sleep(0.4)
        self.assertEqual(self.get_setting(c, 'audio-profile'), '43:6')
        with open(pj) as f:
            self.assertEqual(json.load(f)['cur']['43'], 6)

    def test_sound_list_is_empty_without_a_sound_server(self):
        p, c, stop = self.private_compositor('mellagui-aud3', env={'MELLAGUI_ASOUND_DIR': os.path.join(tmp, 'nonexistent'),
                                                                'PATH': os.path.join(tmp, 'nonexistent-bin')})
        self.assertEqual(self.get_setting(c, 'audio-outs'), 'default|System default')
        c.cmd('settings audio-rescan 1')
        self.assertEqual((self.get_setting(c, 'sinks'), self.get_setting(c, 'sink')), ('', ''))
        with self.assertRaises(RuntimeError):
            c.cmd('settings sink alsa_output.hdmi-stereo')

    def test_display_page_button_opens_the_monitor_panel(self):
        exe = self.exe()
        fake = os.path.join(tmp, 'fake-monitor-button')
        with open(fake, 'w') as f:
            f.write('monitor HDMI-A-1 Test Monitor\n10 100 20\n')
        p, c, stop = self.private_compositor('mellagui-mon1')
        panel = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-mon1', MELLAGUI_MONITOR_FAKE=fake),
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (panel.poll() is None) and (panel.kill(), panel.wait()))
        win = self.wait_window(c, 'mellagui.settings')
        x0, y0 = win['x'], win['y']
        time.sleep(0.3)
        c.click(x0 + 70, y0 + 12 + 3 * 32 + 16)                               # Display page
        time.sleep(0.4)
        modes = min(7, len(self.get_setting(c, 'resolutions').split(';')))
        # rows: monitor heading + line (14, 42), resolution heading (76), Automatic (104), Preferred, then the modes; the button follows by 10 px
        c.click(x0 + 300, y0 + 104 + (2 + modes) * 28 + 10 + 12)
        mon = self.wait_window(c, 'mellagui.monitor')
        self.assertIsNotNone(mon, 'the "Monitor settings..." button did not open the monitor panel')

    def test_panel_display_and_sound_pages(self):
        exe = self.exe()
        state, env = self.audio_env()
        env['MELLAGUI_REVERT_MS'] = '60000'
        path = os.path.join(tmp, 'audio', 'panel.conf')
        if os.path.exists(path):
            os.unlink(path)
        p, c, stop = self.private_compositor('mellagui-aud4', '--settings', path, env=env)
        panel = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-aud4'), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (panel.poll() is None) and (panel.kill(), panel.wait()))
        win = self.wait_window(c, 'mellagui.settings')
        x0, y0 = win['x'], win['y']
        time.sleep(0.3)
        g = lambda k: self.get_setting(c, k)
        # Sound page (no card profiles here): "Applications" heading at y=14, its two rows at 42 and 70, then the beeps heading at 104
        # and its rows from 132: default, HDMI 0, HDMI 1, HDMI 2 (the HDMI outputs are shown before the analog ones)
        c.click(x0 + 70, y0 + 12 + 4 * 32 + 16)
        time.sleep(0.5)
        c.click(x0 + 200, y0 + 70 + 14)                                       # second system output: "Digital Stereo (HDMI)"
        time.sleep(0.2)
        self.assertEqual(g('sink'), 'alsa_output.hdmi-stereo')
        c.click(x0 + 200, y0 + 132 + 28 + 14)                                 # beeps: HDMI 0
        time.sleep(0.2)
        self.assertEqual(g('audio-out'), 'plughw:CARD=PCH,DEV=3')
        # Display page: rows are Automatic, Preferred, then the modes (800x500 preferred, 1920x1080, 1280x720, ...)
        c.click(x0 + 70, y0 + 12 + 3 * 32 + 16)
        time.sleep(0.3)
        c.click(x0 + 200, y0 + 42 + 2 * 28 + 6 + 4 * 28 + 14)                 # third mode = 1280x720 (below the two monitor lines)
        time.sleep(0.4)
        self.assertEqual(g('resolution'), '1280x720@60')
        self.assertEqual(g('resolution-now'), '1280x720@60')
        self.assertNotEqual(g('resolution-confirm'), '')
        self.assertRegex(g('monitor'), r'^MellaGUI\|headless\|headless-0\|\d+\|\d+$')    # make|model|connector|mm
        # the panel is now larger relative to the screen; find it again and press "Keep" (first button below the message)
        win = self.wait_window(c, 'mellagui.settings')
        x0, y0 = win['x'], win['y']
        c.click(x0 + 160 + 45, y0 + 42 + 12)
        time.sleep(0.3)
        self.assertEqual(g('resolution-confirm'), '')
        self.assertEqual(g('resolution-now'), '1280x720@60')
        for _ in range(40):
            if os.path.exists(path) and 'resolution=1280x720@60' in open(path).read():
                break
            time.sleep(0.05)
        self.assertIn('resolution=1280x720@60', open(path).read())

    def test_settings_survive_a_restart(self):
        path = os.path.join(tmp, 'persist', 'deeper', 'settings.conf')       # directories are created
        p, c, stop = self.private_compositor('mellagui-set1', '--settings', path)
        c.cmd('settings layouts ee,fi')
        c.cmd('settings layout fi')
        c.cmd('settings repeat-rate 33')
        c.cmd('settings pointer-speed 175')
        c.cmd('settings natural-scroll 1')
        c.cmd('settings gadget 0')
        stop()                                                              # quitting flushes the pending save
        text = open(path).read()
        for line in ('layouts=ee,fi', 'layout=fi', 'repeat-rate=33', 'pointer-speed=175', 'natural-scroll=1',
                     'gadget=0'):
            self.assertIn(line, text.split('\n'))
        self.assertEqual(p.returncode, 0)
        p2, c2, stop2 = self.private_compositor('mellagui-set2', '--settings', path)
        c2.cmd('settings')
        s = dict(kv.split('=', 1) for kv in c2.last.split()[1:])
        self.assertEqual((s['layouts'], s['layout'], s['repeat-rate'], s['pointer-speed'], s['natural-scroll'],
                          s['gadget']), ('ee,fi', 'fi', '33', '175', '1', '0'))
        self.assertEqual(s['file'], path)
        c2.cmd('kbd')
        self.assertIn('layout=fi slot=1', c2.last)

    def test_command_line_beats_the_file_and_junk_is_ignored(self):
        path = os.path.join(tmp, 'cli.conf')
        with open(path, 'w') as f:
            f.write('# comment\nlayouts = de, ee\nrepeat-rate=oops\nunknown=1\nno equals sign\nsound=0\n')
        p, c, stop = self.private_compositor('mellagui-set3', '--settings', path, '--layouts', 'fi,us')
        c.cmd('settings')
        s = dict(kv.split('=', 1) for kv in c.last.split()[1:])
        self.assertEqual(s['layouts'], 'fi,us')              # --layouts wins
        self.assertEqual(s['sound'], '0')                    # from the file
        self.assertEqual(s['repeat-rate'], '25')             # bad value ignored

    def test_no_file_without_settings_option_on_headless(self):
        self.assertEqual(settings()['file'], '-')

    # ---- the client ---------------------------------------------------------------------------------------------------

    def exe(self, name='mellagui-settings'):
        path = os.path.join(ROOT, 'build', name)
        if not os.path.exists(path):
            self.skipTest('%s not built' % name)
        return path

    def test_client_selftest(self):
        r = subprocess.run([self.exe(), '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_client_cli(self):
        exe = self.exe()
        r = subprocess.run([exe, '--get', 'repeat-rate'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.stdout.strip(), 'repeat-rate=25')
        r = subprocess.run([exe, '--set', 'repeat-rate=30', 'layouts=ee,us'], capture_output=True, text=True,
                           timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ['repeat-rate=30', 'layouts=ee,us'])
        self.assertEqual(settings()['layouts'], 'ee,us')
        r = subprocess.run([exe, '--set', 'sticky=9'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 1)
        r = subprocess.run([exe, '--get'], capture_output=True, text=True, timeout=10)
        self.assertIn('layouts=ee,us', r.stdout.split())

    # Window geometry of the panel: 620x440, content column starts at x=160, rows are 28 px high.
    def open_panel(self):
        p = subprocess.Popen([self.exe()], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.settings')
        self.assertIsNotNone(win, 'settings window did not appear')
        self.assertEqual((win['w'], win['h']), (620, 440))
        time.sleep(0.3)
        return p, win

    def test_panel_opens_from_super_i(self):
        self.exe()
        n = len(ctl.windows()['windows'])
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(23)                                             # KEY_I
        ctl.key(KEY_LEFTMETA, False)
        # headless tests start mellagui with --terminal true, but the settings command is the sibling binary
        win = wait_for_window('mellagui.settings')
        self.assertIsNotNone(win, 'Super+I did not open the settings panel')
        ctl.abs(win['x'] + 100, win['y'] + 100)
        ctl.tap(KEY_ESC)                                        # Esc closes it
        for _ in range(100):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.settings']:
                break
            time.sleep(0.05)
        self.assertEqual(len(ctl.windows()['windows']), n)

    def test_panel_clicks_change_settings(self):
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        # layout rows start at y=42: us, ee, de, fi. Radio "active" at x = 160 + 440 - 140.
        ctl.click(x0 + 480, y0 + 42 + 28 + 14)                  # Estonian -> active
        time.sleep(0.15)
        self.assertEqual(settings()['layout'], 'ee')
        ctl.click(x0 + 168, y0 + 42 + 3 * 28 + 14)              # untick Finnish
        time.sleep(0.15)
        self.assertEqual(settings()['layouts'], 'us,ee,de')
        ctl.click(x0 + 168, y0 + 42 + 3 * 28 + 14)              # Finnish is now the 4th row again: tick it
        time.sleep(0.15)
        self.assertEqual(settings()['layouts'], 'us,ee,de,fi')
        # move German one up: the ^ button of the third row (x = 160 + 440 - 40)
        ctl.click(x0 + 580, y0 + 42 + 2 * 28 + 14)
        time.sleep(0.15)
        self.assertEqual(settings()['layouts'], 'us,de,ee,fi')
        ctl.click(x0 + 168, y0 + 232)                           # sticky keys box
        time.sleep(0.15)
        self.assertEqual(settings()['sticky'], '1')
        self.assertEqual(ctl.kbd()['sticky'], 1)
        # the delay slider: track x = 310..530; clicking the right end gives the maximum
        ctl.click(x0 + 531, y0 + 284)
        time.sleep(0.15)
        self.assertEqual(settings()['repeat-delay'], '1000')
        ctl.click(x0 + 310, y0 + 284)
        time.sleep(0.15)
        self.assertEqual(settings()['repeat-delay'], '150')

    def test_panel_slider_drag(self):
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.abs(x0 + 310 + 110, y0 + 312)                       # repeat rate slider (1..60), mid track
        ctl.button(272, True)
        ctl.abs(x0 + 310 + 220, y0 + 312)                       # drag to the right end
        ctl.button(272, False)
        time.sleep(0.15)
        self.assertEqual(settings()['repeat-rate'], '60')

    def test_panel_pages_and_keyboard_navigation(self):
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 32 + 16)                   # "Pointer" tab
        time.sleep(0.15)
        ctl.click(x0 + 168, y0 + 42 + 28 + 14)                  # natural scrolling box (second row, below the speed slider)
        time.sleep(0.15)
        self.assertEqual(settings()['natural-scroll'], '1')
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.15)
        ctl.click(x0 + 168, y0 + 42 + 28 + 14)                  # sound box (second row, below the gadget box)
        time.sleep(0.15)
        self.assertEqual(settings()['sound'], '0')
        # keyboard only: the focus sits on the sound box (last clicked); Tab runs on through the mixer and system monitor boxes,
        # the wallpaper field, its four radio buttons and its three buttons, wraps over the thirteen page tabs (the headings are skipped)
        # and reaches the gadget checkbox with the 24th press; Space toggles it
        ctl.abs(x0 + 400, y0 + 300)
        for _ in range(24):
            ctl.tap(KEY_TAB)
        ctl.tap(KEY_SPACE)
        time.sleep(0.15)
        self.assertEqual(settings()['gadget'], '0')

    def test_panel_security_page_sets_and_removes_the_lock(self):
        def lock_kind():
            ctl.cmd('lock')
            return ctl.last.split()[1]

        def type_text(text):
            for ch in text:
                ctl.tap(TestLock.CODES[ch])
            ctl.sync()
            time.sleep(0.1)
        self.assertEqual(lock_kind(), 'kind=none')
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 5 * 32 + 16)               # "Security" tab
        time.sleep(0.2)
        self.assertEqual(settings()['lock-kind'], 'none')
        ctl.click(x0 + 300, y0 + 111)                           # New
        type_text('hunter22x')
        ctl.click(x0 + 300, y0 + 141)                           # Repeat: a different text is caught in the panel
        type_text('hunter22y')
        ctl.click(x0 + 204, y0 + 287)                            # Apply
        time.sleep(0.2)
        self.assertEqual(lock_kind(), 'kind=none')
        ctl.click(x0 + 300, y0 + 141)
        for _ in range(9):
            ctl.tap(14)                                         # Backspace
        type_text('hunter22x')
        ctl.click(x0 + 204, y0 + 287)
        time.sleep(0.3)
        self.assertEqual(lock_kind(), 'kind=password')
        self.assertEqual(settings()['lock-kind'], 'password')
        # remove it again through the panel: the page now has a "Current password" field on top and a Remove button
        ctl.click(x0 + 300, y0 + 83)                            # Current password
        type_text('hunter22x')
        ctl.click(x0 + 445, y0 + 317)                           # Remove lock
        time.sleep(0.3)
        self.assertEqual(lock_kind(), 'kind=none')

    def test_panel_desktop_page_has_the_mixer_checkbox(self):
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.2)
        ctl.click(x0 + 168, y0 + 42 + 2 * 28 + 14)              # third row: Volume mixer gadget
        time.sleep(0.3)
        self.assertEqual(settings()['mixer'], '1')
        ctl.cmd('gadget-rect mixer')
        self.assertNotIn('hidden', ctl.last)
        ctl.click(x0 + 168, y0 + 42 + 2 * 28 + 14)
        time.sleep(0.3)
        self.assertEqual(settings()['mixer'], '0')
        ctl.click(x0 + 168, y0 + 42 + 3 * 28 + 14)              # fourth row: System monitor gadget
        time.sleep(0.3)
        self.assertEqual(settings()['sysinfo'], '1')
        ctl.cmd('gadget-rect system')
        self.assertNotIn('hidden', ctl.last)
        ctl.click(x0 + 168, y0 + 42 + 3 * 28 + 14)
        time.sleep(0.3)
        self.assertEqual(settings()['sysinfo'], '0')

    def test_panel_wallpaper_browse_button_opens_the_picture_chooser(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-files')):
            self.skipTest('mellagui-files not built')
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.2)
        ctl.click(x0 + 460, y0 + 291)                           # "Browse..."
        chooser = wait_for_window('mellagui.files.choose')
        self.assertIsNotNone(chooser, 'the picture chooser did not open')
        self.assertIn('Choose a picture', chooser['title'])
        time.sleep(0.3)
        ctl.tap(KEY_ESC)                                        # Esc cancels it
        for _ in range(100):
            if wait_for_window('mellagui.files.choose', 0.05) is None:
                break
        self.assertIsNone(wait_for_window('mellagui.files.choose', 0.3))

    def test_panel_wallpaper_page_field_radios_and_buttons(self):
        self.addCleanup(lambda: (ctl.cmd('settings wallpaper ""'), ctl.cmd('settings wallpaper-mode fill')))
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.2)
        ctl.click(x0 + 410, y0 + 265)                           # Stretch
        time.sleep(0.2)
        self.assertEqual(settings()['wallpaper-mode'], 'stretch')
        ctl.click(x0 + 190, y0 + 265)                           # Fill
        time.sleep(0.2)
        self.assertEqual(settings()['wallpaper-mode'], 'fill')
        ctl.click(x0 + 300, y0 + 231)                           # the file field: type a name and press Enter
        type_text('abc')
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok abc')
        ctl.cmd('setting wallpaper-status')
        self.assertTrue(ctl.last.startswith('ok error:'), ctl.last)                    # no such file
        ctl.click(x0 + 320, y0 + 291)                           # "No picture"
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ')
        ctl.click(x0 + 300, y0 + 231)                           # type without Enter, then the Apply button
        for ch in 'xyz':
            ctl.tap(KEYCODES[ch])
        ctl.click(x0 + 200, y0 + 291)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok xyz')

    def test_panel_text_fields_paste_and_have_a_right_click_menu(self):
        self.addCleanup(lambda: ctl.cmd('settings wallpaper ""'))
        self.own_clipboard('pasted.png\n')
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.2)
        ctl.click(x0 + 300, y0 + 231)                           # the file field
        ctl.key(KEY_LEFTCTRL, True)
        ctl.tap(47)                                             # Ctrl+V
        ctl.key(KEY_LEFTCTRL, False)
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok pasted.png')
        self.right_click(x0 + 300, y0 + 231)                    # the right-click menu: Clear empties the field
        m = self.menu_state()
        self.assertEqual((m['open'], [e['label'] for e in m['entries']]), (1, ['Cut', 'Copy', 'Paste', 'Select all', 'Clear']))
        self.assertEqual([e['enabled'] for e in m['entries']], [1, 1, 1, 1, 1])
        clear = m['entries'][4]
        ctl.click(clear['x'], clear['y'])
        time.sleep(0.2)
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok ')
        self.right_click(x0 + 300, y0 + 231)                    # Paste from the menu
        paste = self.menu_state()['entries'][2]
        ctl.click(paste['x'], paste['y'])
        time.sleep(0.4)
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok pasted.png')
        self.right_click(x0 + 300, y0 + 100)                    # a place that is no field: no menu
        self.assertEqual(self.menu_state()['open'], 0)

    def test_panel_text_fields_have_a_caret_and_a_selection(self):
        self.addCleanup(lambda: ctl.cmd('settings wallpaper ""'))
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 70, y0 + 12 + 64 + 16)                   # "Desktop" tab
        time.sleep(0.2)
        ctl.click(x0 + 300, y0 + 231)                           # the file field
        for ch in 'abxcd':                                      # (type_text() would press Enter at the end)
            ctl.tap(KEYCODES[ch])
        ctl.tap(105)                                            # Left x2: the caret is behind the x
        ctl.tap(105)
        ctl.tap(14)                                             # Backspace removes the x
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok abcd')
        ctl.click(x0 + 300, y0 + 231)
        ctl.key(KEY_LEFTCTRL, True)
        ctl.tap(30)                                             # Ctrl+A: everything is selected ...
        ctl.key(KEY_LEFTCTRL, False)
        ctl.tap(KEYCODES['z'])                                  # ... and typing replaces it
        ctl.tap(KEYCODES['z'])
        ctl.tap(102)                                            # Home, then Shift+End selects all of it again, Delete removes it
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(107)
        ctl.key(KEY_LEFTSHIFT, False)
        ctl.tap(111)
        ctl.tap(KEYCODES['q'])
        ctl.tap(KEY_ENTER)
        time.sleep(0.3)
        ctl.cmd('setting wallpaper')
        self.assertEqual(ctl.last, 'ok q')

    def test_panel_follows_changes_made_elsewhere(self):
        p, win = self.open_panel()
        ctl.abs(900, 500)
        before = ctl.dump(os.path.join(tmp, 'set0.ppm'))
        ctl.cmd('settings repeat-delay 1000')
        time.sleep(0.2)
        after = ctl.dump(os.path.join(tmp, 'set1.ppm'))
        x0, y0 = win['x'], win['y']
        row = [(before.px(x0 + x, y0 + 284), after.px(x0 + x, y0 + 284)) for x in range(300, 560, 2)]
        self.assertTrue(any(a != b for a, b in row), 'the slider did not move')

    def test_try_field_follows_the_layout(self):
        p, win = self.open_panel()
        x0, y0 = win['x'], win['y']
        ctl.click(x0 + 200, y0 + 394)                           # the "Try it" field
        time.sleep(0.15)

        def field():
            ctl.abs(900, 500)
            time.sleep(0.15)
            ppm = ctl.dump(os.path.join(tmp, 'try.ppm'))
            ctl.abs(x0 + 200, y0 + 394)
            return [ppm.px(x0 + x, y0 + y) for y in range(384, 404) for x in range(166, 190)]
        empty = field()
        ctl.tap(39)                                             # KEY_SEMICOLON: ';' on us, o-umlaut on ee
        us_text = field()
        self.assertNotEqual(us_text, empty)
        ctl.tap(14)                                             # Backspace
        self.assertEqual(field(), empty)
        ctl.cmd('settings layout ee')
        time.sleep(0.15)
        ctl.tap(39)
        ee_text = field()
        self.assertNotEqual(ee_text, empty)
        self.assertNotEqual(ee_text, us_text)


FAKES = os.path.join(ROOT, 'tests', 'fakes')


class TestOskSaved(Base):
    """The on-screen keyboard's place and size are kept in the gadgets file and used by the next run."""

    def test_place_and_size_are_saved_and_a_reset_forgets_them(self):
        path = os.path.join(tmp, 'gadgets-osk.conf')
        if os.path.exists(path):
            os.unlink(path)
        p, c, stop = self.private_compositor('mellagui-osk1', '--gadgets-file', path)
        c.cmd('osk open')
        m = re.search(r'card x=(\d+) y=(\d+) w=(\d+) h=(\d+)', '\n'.join(c.cmd('osk')))
        x, y, w, h = map(int, m.groups())
        c.abs(x + 40, y + 5)
        c.button(272, True)
        c.abs(x + 40, y - 95)                                                    # 100 px up
        c.button(272, False)
        c.abs(x + w - 4, y - 100 + h - 4)
        c.button(272, True)
        c.abs(x + w - 4 - 300, y - 100 + h - 4)                                  # 300 px narrower
        c.button(272, False)
        m = re.search(r'card x=(\d+) y=(\d+) w=(\d+) h=(\d+)', '\n'.join(c.cmd('osk')))
        placed = tuple(map(int, m.groups()))
        self.assertEqual(placed[:3], (x, y - 100, w - 300))
        lines = open(path).read().split('\n')[:-1]
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith('keyboard %d %d %d ' % placed[:3]), lines)
        p2, c2, stop2 = self.private_compositor('mellagui-osk2', '--gadgets-file', path)
        c2.cmd('osk open')
        m = re.search(r'card x=(\d+) y=(\d+) w=(\d+) h=(\d+)', '\n'.join(c2.cmd('osk')))
        self.assertEqual(tuple(map(int, m.groups())), placed)                    # the next run starts where it was left
        c2.cmd('osk reset')
        self.assertEqual(open(path).read(), '')                                  # the file forgets it
        m = re.search(r'card x=(\d+) y=(\d+) w=(\d+) h=(\d+)', '\n'.join(c2.cmd('osk')))
        self.assertEqual(int(m.group(3)), w)                                     # the default width again


class TestMixer(Base):
    """The volume mixer gadget: state from pactl / amixer, sliders and buttons, docking, the setting and Super+V."""

    # Card layout (src/mixer.c, src/wm.c): title 32 px, pad 14, master row 40, output row 30, stream rows 40.
    TITLE, PAD = 26, 12

    def mixer_env(self, tag, extra=None):
        d = os.path.join(tmp, 'mixer-' + tag)
        os.makedirs(d, exist_ok=True)
        env = {'MELLAGUI_FAKE_STATE': os.path.join(d, 'pactl.json'), 'MELLAGUI_FAKE_AMIXER': os.path.join(d, 'amixer.json'),
               'PATH': FAKES + ':' + os.environ['PATH']}
        env.update(extra or {})
        for f in ('pactl.json', 'amixer.json'):
            if os.path.exists(os.path.join(d, f)):
                os.unlink(os.path.join(d, f))
        return env, d

    @staticmethod
    def state(path):
        with open(path) as f:
            return json.load(f)

    def start(self, name, env, *args):
        nodock = os.path.join(tmp, 'nodock.conf')                         # the dock floats over the lower part of the small screen: not wanted here
        with open(nodock, 'w') as f:
            f.write('[dock]\nenabled = no\n')
        p, c, stop = self.private_compositor(name, '--mixer', '--config', nodock, *args, env=env)
        for _ in range(100):
            c.cmd('mixer')
            if ' ready=1' in c.last:
                break
            time.sleep(0.05)
        return c

    @staticmethod
    def mixer_info(c):
        c.cmd('mixer')
        head, _, output = c.last[3:].partition(' output=')                     # the output name has spaces: it comes last
        info = dict(kv.split('=', 1) for kv in head.split())
        info['output'] = output
        return info

    @staticmethod
    def card(c):
        c.cmd('gadget-rect mixer')
        parts = c.last.split()
        return None if parts[1] == 'hidden' else tuple(map(int, parts[1:5]))

    def geometry(self, c):
        x, y, w, h = self.card(c)
        row = y + self.TITLE + 4
        g = {'card': (x, y, w, h)}
        sw = w - 2 * self.PAD - 24 - 12 - 46
        g['mute'] = (x + self.PAD + 12, row + 17)                                  # centre of the master mute button
        g['slider'] = (x + self.PAD + 24 + 12, row + 17, sw)                       # left, y, width
        g['output'] = (x + w // 2, row + 34 + 12)
        srow = row + 34 + 28 + 4
        g['streams'] = [((x + self.PAD + 22 + 10, srow + 36 * i + 26, w - 2 * self.PAD - 32 - 46),
                         (x + self.PAD + 11, srow + 36 * i + 26)) for i in range(2)]   # (slider left/y/width, mute centre)
        return g

    def wait_for(self, pred, what, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return
            time.sleep(0.05)
        self.fail('timeout: ' + what)

    def test_mixer_shows_pulse_state_and_follows_the_controls(self):
        env, d = self.mixer_env('pulse')
        c = self.start('mellagui-mix1', env)
        info = self.mixer_info(c)
        self.assertEqual((info['backend'], info['volume'], info['mute'], info['streams']), ('pulse', '64', '0', '2'))
        self.assertIn('Analog', info['output'])
        g = self.geometry(c)
        pj = os.path.join(d, 'pactl.json')
        sx, sy, sw = g['slider']
        # click on the slider at 25 %
        c.click(sx + sw // 4, sy)
        self.wait_for(lambda: self.state(pj)['sinks'][0]['vol'] == 25, 'sink volume 25')
        self.assertEqual(self.mixer_info(c)['volume'], '25')
        # drag from 10 % to 90 %: few commands (coalesced), the last value wins
        c.abs(sx + sw // 10, sy)
        c.button(272, True)
        for k in range(1, 41):
            c.abs(sx + sw // 10 + (sw * 8 // 10) * k // 40, sy)
        c.button(272, False)
        self.wait_for(lambda: 88 <= self.state(pj)['sinks'][0]['vol'] <= 92, 'sink volume about 90 after the drag')
        sets = [l for l in self.state(pj)['log'] if l.startswith('set-sink-volume')]
        self.assertLess(len(sets), 41, 'slider drag commands were not coalesced: %d' % len(sets))
        # mute button
        c.click(*g['mute'])
        self.wait_for(lambda: self.state(pj)['sinks'][0]['mute'] is True, 'muted')
        self.assertEqual(self.mixer_info(c)['mute'], '1')
        c.click(*g['mute'])
        self.wait_for(lambda: self.state(pj)['sinks'][0]['mute'] is False, 'unmuted')
        # an application's slider and mute
        (ax, ay, aw), (mx, my) = g['streams'][1]
        c.click(ax + aw * 3 // 4, ay)
        self.wait_for(lambda: self.state(pj)['inputs'][1]['vol'] == 75, 'stream volume 75')
        c.click(mx, my)
        self.wait_for(lambda: self.state(pj)['inputs'][1]['mute'] is True, 'stream muted')
        self.assertFalse(self.state(pj)['inputs'][0]['mute'])
        # the output button makes the next sink (HDMI) the default one
        c.click(*g['output'])
        self.wait_for(lambda: self.state(pj)['default'] == 'alsa_output.hdmi-stereo', 'default sink changed')
        self.wait_for(lambda: 'HDMI' in self.mixer_info(c)['output'], 'output text shows HDMI')
        self.assertEqual(self.mixer_info(c)['volume'], '100')                  # the HDMI sink's own level
        # changes made elsewhere show up (polling): the other sink is quieter now
        st = self.state(pj)
        st['sinks'][1]['vol'] = 33
        json.dump(st, open(pj, 'w'))
        self.wait_for(lambda: self.mixer_info(c)['volume'] == '33', 'polled volume', timeout=5.0)

    def test_mixer_screen_has_the_card_docked_below_the_clipboard(self):
        env, d = self.mixer_env('dock')
        c = self.start('mellagui-mix2', env, '--size', '1024x800')
        c.cmd('gadget-rect')
        cb = tuple(map(int, c.last.split()[1:5]))
        x, y, w, h = self.card(c)
        self.assertEqual(x + w, cb[0] + cb[2])                                  # same right edge
        self.assertGreater(y, cb[1] + cb[3])                                    # below the clipboard card
        # dragging the mixer's title bar moves only the mixer
        c.abs(x + 60, y + 10)
        c.button(272, True)
        c.abs(x - 140, y + 90)
        c.button(272, False)
        x2, y2, w2, h2 = self.card(c)
        self.assertEqual((x2, y2), (x - 200, y + 80))
        c.cmd('gadget-rect')
        self.assertEqual(tuple(map(int, c.last.split()[1:5])), cb)
        # the volume controls still work after moving
        g = self.geometry(c)
        sx, sy, sw = g['slider']
        c.click(sx + sw // 2, sy)
        self.wait_for(lambda: self.state(os.path.join(d, 'pactl.json'))['sinks'][0]['vol'] == 50, 'volume 50 at the new place')

    def test_gadgets_stick_to_each_other_and_to_the_screen_edge(self):
        env, d = self.mixer_env('snap')
        c = self.start('mellagui-snap1', env, '--size', '1280x900')
        c.cmd('gadget-rect')
        cb = tuple(map(int, c.last.split()[1:5]))
        x, y, w, h = self.card(c)                                               # docked below the clipboard with a 12 px gap
        self.assertEqual(y, cb[1] + cb[3] + 12)

        def drag(dx, dy):
            gx, gy, gw, gh = self.card(c)
            c.abs(gx + 60, gy + 10)
            c.button(272, True)
            c.abs(gx + 60 + dx, gy + 10 + dy)
            c.button(272, False)
            return self.card(c)
        # a little closer: it jumps flush under the clipboard and lines up with its right edge
        self.assertEqual(drag(-5, -8), (cb[0] + cb[2] - w, cb[1] + cb[3], w, h))
        # far away from everything: moves exactly as dragged
        nx, ny, nw, nh = drag(-300, 200)
        self.assertEqual((nx, ny), (cb[0] + cb[2] - w - 300, cb[1] + cb[3] + 200))
        # next to the clipboard (left of it, 9 px away): flush against its left side
        gx, gy, gw, gh = self.card(c)
        self.assertEqual(drag(cb[0] - gw - 9 - gx, cb[1] + 100 - gy), (cb[0] - gw, cb[1] + 100, gw, gh))
        # the screen edge: 6 px short of the left edge snaps to it
        gx, gy, gw, gh = self.card(c)
        self.assertEqual(drag(6 - gx, 0)[0], 0)

    def test_resizing_a_gadget_sticks_the_edge_to_neighbours_and_the_screen(self):
        env, d = self.mixer_env('snap-resize')
        c = self.start('mellagui-snap2', env, '--size', '1280x900')
        c.cmd('gadget-rect')
        cb = tuple(map(int, c.last.split()[1:5]))
        x, y, w, h = self.card(c)                                               # docked 12 px below the clipboard

        def pull(px, py, dx, dy):
            c.abs(px, py)
            c.button(272, True)
            c.abs(px + dx, py + dy)
            c.button(272, False)
            return self.card(c)
        # the top edge pulled up 9 px (3 px short of the clipboard): flush against the clipboard's bottom edge
        self.assertEqual(pull(x + w // 2, y + 1, 0, -9), (x, cb[1] + cb[3], w, h + 12))
        # 40 px: far from it, the edge goes exactly where the pointer goes
        x, y, w, h = self.card(c)
        self.assertEqual(pull(x + w // 2, y + 1, 0, 40), (x, y + 40, w, h - 40))
        # the right edge 7 px short of the screen edge: sticks to the screen
        x, y, w, h = self.card(c)
        got = pull(x + w - 2, y + h // 2, 1280 - 5 - (x + w), 0)                # 5 px short of the screen's right edge
        self.assertEqual(got[0] + got[2], 1280)

    def test_gadget_places_and_sizes_are_saved(self):
        env, d = self.mixer_env('saved')
        path = os.path.join(tmp, 'gadgets-saved.conf')
        if os.path.exists(path):
            os.unlink(path)
        c = self.start('mellagui-gsave1', env, '--size', '1280x900', '--gadgets-file', path)
        x, y, w, h = self.card(c)
        c.cmd('gadget-rect')
        cb = tuple(map(int, c.last.split()[1:5]))

        def grab(px, py, dx, dy):
            c.abs(px, py)
            c.button(272, True)
            c.abs(px + dx, py + dy)
            c.button(272, False)
        # a plain click on a title bar saves nothing; moving the mixer and stretching it do
        c.click(x + 60, y + 10)
        self.assertFalse(os.path.exists(path))
        time.sleep(0.6)                                                         # (not a double click)
        grab(x + 60, y + 10, -300, 100)
        grab(x - 300 + 100, y + 100 + h - 2, 0, 50)                                   # (its bottom edge, at the new place)
        mx, my, mw, mh = self.card(c)
        self.assertEqual((mx, my, mw), (x - 300, y + 100, w))
        self.assertGreater(mh, h)
        self.assertEqual(open(path).read().split('\n')[:-1], ['mixer %d %d %d %d' % (mx, my, mw, mh)])      # only the moved one
        # a new run puts it back where it was, the clipboard stays docked
        c2 = self.start('mellagui-gsave2', env, '--size', '1280x900', '--gadgets-file', path)
        self.assertEqual(self.card(c2), (mx, my, mw, mh))
        c2.cmd('gadget-rect')
        self.assertEqual(tuple(map(int, c2.last.split()[1:5])), cb)
        # a double click on the title bar puts it back to its default; the file forgets it
        c2.click(mx + 60, my + 10)
        c2.click(mx + 60, my + 10)
        for _ in range(40):
            if self.card(c2) == (x, y, w, h) or self.card(c2)[:2] == (cb[0], cb[1] + cb[3] + 12):
                break
            time.sleep(0.05)
        self.assertEqual(self.card(c2)[2:], (w, h))
        self.assertEqual(open(path).read(), '')

    def test_mixer_docks_beside_the_clipboard_on_a_small_screen(self):
        env, d = self.mixer_env('small')
        c = self.start('mellagui-mix6', env, '--size', '800x400')                  # 800x400: no room below the clipboard card
        c.cmd('gadget-rect')
        cb = tuple(map(int, c.last.split()[1:5]))
        x, y, w, h = self.card(c)
        self.assertEqual((x + w + 12, y), (cb[0], cb[1]))                       # left of it, same top
        self.assertGreaterEqual(x, 0)

    def test_mixer_toggles_with_super_v_the_setting_and_the_panel(self):
        env, d = self.mixer_env('toggle')
        p, c, stop = self.private_compositor('mellagui-mix3', env=env)
        self.assertIsNone(self.card(c))                                         # hidden by default
        c.cmd('settings')
        self.assertIn('mixer=0', c.last)
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_V)
        c.key(KEY_LEFTMETA, False)
        self.wait_for(lambda: self.card(c) is not None, 'Super+V shows the mixer')
        c.cmd('settings')
        self.assertIn('mixer=1', c.last)
        c.cmd('settings mixer 0')
        self.assertIsNone(self.card(c))
        c.cmd('settings mixer 1')
        self.assertIsNotNone(self.card(c))
        self.wait_for(lambda: self.mixer_info(c)['ready'] == '1', 'the mixer read its state')
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_V)
        c.key(KEY_LEFTMETA, False)
        self.wait_for(lambda: self.card(c) is None, 'Super+V hides it again')

    def test_mixer_uses_wpctl_when_there_is_no_pactl(self):
        d = os.path.join(tmp, 'mixer-wpctl')
        stubs = os.path.join(d, 'bin')
        os.makedirs(stubs, exist_ok=True)
        with open(os.path.join(stubs, 'pactl'), 'w') as f:
            f.write('#!/bin/sh\nexit 1\n')                                      # pulseaudio-utils is not installed
        os.chmod(os.path.join(stubs, 'pactl'), 0o755)
        wj = os.path.join(d, 'wpctl.json')
        if os.path.exists(wj):
            os.unlink(wj)
        env = {'MELLAGUI_FAKE_WPCTL': wj, 'PATH': stubs + ':' + FAKES + ':' + os.environ['PATH']}
        c = self.start('mellagui-mix7', env)
        info = self.mixer_info(c)
        self.assertEqual((info['backend'], info['volume'], info['mute'], info['output']), ('wpctl', '55', '0', 'PipeWire'))
        g = self.geometry(c)
        sx, sy, sw = g['slider']
        c.click(sx + round(sw * 0.2), sy)
        self.wait_for(lambda: self.state(wj)['vol'] == 20, 'wpctl volume 20')
        c.click(*g['mute'])
        self.wait_for(lambda: self.state(wj)['mute'] is True, 'wpctl muted')
        self.wait_for(lambda: self.mixer_info(c)['mute'] == '1', 'state follows')

    def test_mixer_falls_back_to_alsa_master(self):
        d = os.path.join(tmp, 'mixer-alsa')
        os.makedirs(d, exist_ok=True)
        stubs = os.path.join(d, 'bin')
        os.makedirs(stubs, exist_ok=True)
        for prog in ('pactl', 'wpctl'):                                         # no PulseAudio / PipeWire here
            with open(os.path.join(stubs, prog), 'w') as f:
                f.write('#!/bin/sh\nexit 1\n')
            os.chmod(os.path.join(stubs, prog), 0o755)
        aj = os.path.join(d, 'amixer.json')
        if os.path.exists(aj):
            os.unlink(aj)
        env = {'MELLAGUI_FAKE_AMIXER': aj, 'PATH': stubs + ':' + FAKES + ':' + os.environ['PATH']}
        c = self.start('mellagui-mix4', env)
        info = self.mixer_info(c)
        self.assertEqual((info['backend'], info['volume'], info['mute'], info['streams']), ('alsa', '40', '0', '0'))
        g = self.geometry(c)
        sx, sy, sw = g['slider']
        c.click(sx + round(sw * 0.7), sy)
        self.wait_for(lambda: self.state(aj)['vol'] == 70, 'ALSA volume 70')
        c.click(*g['mute'])
        self.wait_for(lambda: self.state(aj)['mute'] is True, 'ALSA muted')

    def test_mixer_without_any_mixer_program(self):
        d = os.path.join(tmp, 'mixer-none')
        stubs = os.path.join(d, 'bin')
        os.makedirs(stubs, exist_ok=True)
        for prog in ('pactl', 'wpctl', 'amixer'):
            with open(os.path.join(stubs, prog), 'w') as f:
                f.write('#!/bin/sh\nexit 1\n')
            os.chmod(os.path.join(stubs, prog), 0o755)
        c = self.start('mellagui-mix5', {'PATH': stubs + ':' + os.environ['PATH']})
        info = self.mixer_info(c)
        self.assertEqual((info['backend'], info['ready']), ('none', '1'))
        x, y, w, h = self.card(c)                                               # the card is there and says so, no crash
        c.click(x + w // 2, y + 80)
        self.assertEqual(self.mixer_info(c)['backend'], 'none')



class TestSysinfo(Base):
    """The system monitor gadget: CPU / memory / temperature from /proc and /sys (a fake tree through MELLAGUI_SYS_ROOT)."""

    STAT_A = 'cpu  1000 0 500 8000 500 0 0 0 0 0\ncpu0 500 0 250 4000 250 0 0 0 0 0\ncpu1 500 0 250 4000 250 0 0 0 0 0\n'
    STAT_B = 'cpu  1100 0 550 8050 500 0 0 0 0 0\ncpu0 600 0 250 4000 250 0 0 0 0 0\ncpu1 500 0 300 4050 250 0 0 0 0 0\n'   # +150 busy of +200

    def tree(self, tag, with_sensor=True):
        root = os.path.join(tmp, 'sys-' + tag)
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(os.path.join(root, 'proc'))
        self.write(root, 'proc/stat', self.STAT_A)
        self.write(root, 'proc/meminfo', 'MemTotal:        1000000 kB\nMemFree: 100000 kB\nMemAvailable:    250000 kB\nSwapTotal: 2000000 kB\nSwapFree: 1500000 kB\n')
        self.write(root, 'proc/loadavg', '1.25 0.50 0.30 2/300 1234\n')
        self.write(root, 'proc/cpuinfo', 'processor\t: 0\nmodel name\t: Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz\n')
        self.write(root, 'proc/uptime', '93784.55 200000.00\n')                      # 1 day 2 hours
        self.write(root, 'proc/sys/kernel/osrelease', '6.8.0-test\n')
        self.write(root, 'proc/sys/kernel/hostname', 'mellaguibox\n')
        self.write(root, 'etc/os-release', 'NAME="Testix"\nPRETTY_NAME="Testix 1.0 LTS"\n')
        self.write(root, 'sys/class/dmi/id/sys_vendor', 'LENOVO\n')
        self.write(root, 'sys/class/dmi/id/product_name', '20L5CTO1WW\n')
        self.write(root, 'sys/class/dmi/id/product_version', 'ThinkPad T480\n')
        if with_sensor:
            self.write(root, 'sys/class/hwmon/hwmon0/name', 'coretemp\n')
            self.write(root, 'sys/class/hwmon/hwmon0/temp1_input', '66000\n')
            self.write(root, 'sys/class/hwmon/hwmon0/temp1_label', 'Package id 0\n')
            self.write(root, 'sys/class/hwmon/hwmon0/temp2_input', '71000\n')
            self.write(root, 'sys/class/hwmon/hwmon0/temp2_label', 'Core 0\n')
        return root

    @staticmethod
    def write(root, rel, text):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + '.tmp', 'w') as f:
            f.write(text)
        os.replace(path + '.tmp', path)                        # atomic: the reader never sees half a file

    @staticmethod
    def info(c):
        c.cmd('sysinfo')
        return dict(kv.split('=', 1) for kv in c.last.split()[1:])

    def start(self, name, root, *args):
        p, c, stop = self.private_compositor(name, '--sysinfo', *args, env={'MELLAGUI_SYS_ROOT': root})
        for _ in range(100):
            if self.info(c)['ready'] == '1':
                break
            time.sleep(0.05)
        return c

    def test_values_come_from_proc_and_sys(self):
        root = self.tree('values')
        c = self.start('mellagui-sys1', root)
        i = self.info(c)
        self.assertEqual((i['visible'], i['cores'], i['mem'], i['mem_total_kb'], i['swap'], i['temp'], i['load']),
                         ('1', '2', '75', '1000000', '25', '66', '1.25'))         # the package sensor, not the hotter core
        # the load needs two samples: new counters show up as a percentage within a couple of seconds
        self.write(root, 'proc/stat', self.STAT_B)
        seen = False
        end = time.time() + 3.5
        while time.time() < end and not seen:
            seen = self.info(c)['cpu'] == '75'                                    # (150 busy of 200 total jiffies)
            time.sleep(0.05)
        self.assertTrue(seen, 'cpu load was not computed from the counters')
        self.assertGreaterEqual(int(self.info(c)['hist']), 1)
        # memory and temperature follow, too
        self.write(root, 'proc/meminfo', 'MemTotal: 1000000 kB\nMemAvailable: 500000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n')
        self.write(root, 'sys/class/hwmon/hwmon0/temp1_input', '88000\n')
        end = time.time() + 3.5
        while time.time() < end and (self.info(c)['mem'] != '50' or self.info(c)['temp'] != '88'):
            time.sleep(0.05)
        i = self.info(c)
        self.assertEqual((i['mem'], i['swap'], i['temp']), ('50', '0', '88'))

    def test_device_info_comes_from_dmi_cpuinfo_and_os_release(self):
        root = self.tree('device')
        c = self.start('mellagui-sys7', root)
        c.cmd('sysdev')
        got = dict(kv.split('=', 1) for kv in c.last[3:].split('|'))
        self.assertEqual(got['maker'], 'LENOVO')
        self.assertEqual(got['model'], 'ThinkPad T480')                          # product_version: the product name is a type number
        self.assertEqual(got['cpu'], 'Intel Core i5-8250U @ 1.60GHz')
        self.assertEqual((got['os'], got['kernel'], got['host'], got['uptime']), ('Testix 1.0 LTS', '6.8.0-test', 'mellaguibox', '93784'))
        # placeholders of cheap boards are not shown; the board's own name takes over
        self.write(root, 'sys/class/dmi/id/sys_vendor', 'To Be Filled By O.E.M.\n')
        self.write(root, 'sys/class/dmi/id/product_name', 'To Be Filled By O.E.M.\n')
        self.write(root, 'sys/class/dmi/id/product_version', 'Default string\n')
        self.write(root, 'sys/class/dmi/id/board_vendor', 'ASRock\n')
        self.write(root, 'sys/class/dmi/id/board_name', 'B450M Pro4\n')
        c2 = self.start('mellagui-sys8', root)
        c2.cmd('sysdev')
        got = dict(kv.split('=', 1) for kv in c2.last[3:].split('|'))
        self.assertEqual((got['maker'], got['model']), ('ASRock', 'B450M Pro4'))
        # the card draws (the rows that fit): no crash, the default size shows them
        c2.cmd('gadget-rect system')
        self.assertGreaterEqual(int(c2.last.split()[4]), 300)

    def test_no_sensor_and_no_proc_do_not_break_anything(self):
        root = self.tree('nosensor', with_sensor=False)
        c = self.start('mellagui-sys2', root)
        self.assertEqual(self.info(c)['temp'], '-1')
        shutil.rmtree(os.path.join(root, 'proc'))                                # even /proc vanishing is survived
        time.sleep(1.3)
        i = self.info(c)
        self.assertEqual(i['ready'], '1')
        x, y, w, h = map(int, (lambda: (c.cmd('gadget-rect system'), c.last.split()[1:5])[1])())
        self.assertGreater(w, 200)

    def test_toggle_setting_super_s_and_stacking_with_the_other_gadgets(self):
        root = self.tree('stack')
        p, c, stop = self.private_compositor('mellagui-sys3', '--size', '1280x1000', '--mixer',
                                             env={'MELLAGUI_SYS_ROOT': root, 'PATH': FAKES + ':' + os.environ['PATH'],
                                                  'MELLAGUI_FAKE_STATE': os.path.join(tmp, 'sys3-pactl.json')})
        c.cmd('gadget-rect system')
        self.assertIn('hidden', c.last)                                          # hidden by default
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_S)
        c.key(KEY_LEFTMETA, False)
        for _ in range(60):
            c.cmd('gadget-rect system')
            if 'hidden' not in c.last:
                break
            time.sleep(0.05)
        self.assertNotIn('hidden', c.last)
        rects = []
        for name in ('', 'mixer', 'system'):                                     # clipboard, mixer, system: one column, top to bottom
            c.cmd('gadget-rect ' + name)
            rects.append(tuple(map(int, c.last.split()[1:5])))
        for upper, lower in zip(rects, rects[1:]):
            self.assertEqual(upper[0] + upper[2], lower[0] + lower[2])           # same right edge
            self.assertGreaterEqual(lower[1], upper[1] + upper[3])               # no overlap
        c.cmd('settings')
        self.assertIn('sysinfo=1', c.last)
        c.cmd('settings sysinfo 0')
        c.cmd('gadget-rect system')
        self.assertIn('hidden', c.last)
        # on a small screen the cards go into a second column instead of overlapping
        p2, c2, stop2 = self.private_compositor('mellagui-sys4', '--size', '1000x500', '--mixer', '--sysinfo', env={'MELLAGUI_SYS_ROOT': root,
                                                'PATH': FAKES + ':' + os.environ['PATH'], 'MELLAGUI_FAKE_STATE': os.path.join(tmp, 'sys4-pactl.json')})
        boxes = []
        for name in ('', 'mixer', 'system'):
            c2.cmd('gadget-rect ' + name)
            boxes.append(tuple(map(int, c2.last.split()[1:5])))
        for i in range(3):
            for j in range(i + 1, 3):
                a, b = boxes[i], boxes[j]
                overlap = a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]
                self.assertFalse(overlap, 'gadgets %d and %d overlap on a small screen: %r %r' % (i, j, a, b))
            self.assertGreaterEqual(boxes[i][0], 0)

    def test_gadgets_cascade_when_the_screen_is_too_small_for_columns(self):
        root = self.tree('cascade')
        p, c, stop = self.private_compositor('mellagui-sys6', '--mixer', '--sysinfo', env={'MELLAGUI_SYS_ROOT': root,      # 800x500
                                             'PATH': FAKES + ':' + os.environ['PATH'], 'MELLAGUI_FAKE_STATE': os.path.join(tmp, 'sys6-pactl.json')})
        tops = []
        for name in ('', 'mixer', 'system'):
            c.cmd('gadget-rect ' + name)
            x, y, w, h = map(int, c.last.split()[1:5])
            self.assertTrue(0 <= x and x + w <= 800 and 32 <= y and y + h <= 500, (name, x, y, w, h))
            tops.append((x, y))
        self.assertEqual(len(set(tops)), 3, 'two gadgets sit exactly on top of each other: %r' % tops)

    def test_the_card_can_be_moved_and_shows_no_error_when_tiny(self):
        root = self.tree('move')
        c = self.start('mellagui-sys5', root, '--size', '1024x960')
        c.cmd('gadget-rect system')
        x, y, w, h = map(int, c.last.split()[1:5])
        c.abs(x + 80, y + 10)
        c.button(272, True)
        c.abs(x - 60, y + 130)
        c.button(272, False)
        c.cmd('gadget-rect system')
        self.assertEqual(tuple(map(int, c.last.split()[1:5])), (x - 140, y + 120, w, h))
        # resized to the smallest size the card has: still drawn (no crash), the rows that fit are chosen
        c.cmd('gadget-rect system')
        x, y, w, h = map(int, c.last.split()[1:5])
        c.abs(x + w - 3, y + h - 3)
        c.button(272, True)
        c.abs(x - 500, y - 500)
        c.button(272, False)
        c.cmd('gadget-rect system')
        w2, h2 = map(int, c.last.split()[3:5])
        self.assertEqual((w2, h2), (210, 130))
        c.dump(os.path.join(tmp, 'sys5.ppm'))
        self.assertEqual(self.info(c)['ready'], '1')



class Gbm:
    """GPU memory from libgbm through ctypes (a linear buffer object on the render node), or None when it cannot be had."""

    GBM_BO_USE_RENDERING, GBM_BO_USE_LINEAR, GBM_BO_TRANSFER_WRITE = 1 << 2, 1 << 4, 1 << 1
    ARGB8888, XRGB8888 = 0x34325241, 0x34325258

    @classmethod
    def open(cls):
        import ctypes, ctypes.util, glob
        nodes = [n for n in sorted(glob.glob('/dev/dri/renderD*')) if os.access(n, os.R_OK | os.W_OK)]
        lib = ctypes.util.find_library('gbm')
        if not nodes or not lib:
            return None
        try:
            g = cls()
            g.ct = ctypes
            g.lib = ctypes.CDLL(lib)
            g.lib.gbm_create_device.restype = ctypes.c_void_p
            g.lib.gbm_bo_create.restype = ctypes.c_void_p
            g.lib.gbm_bo_create.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
            g.lib.gbm_bo_get_stride.argtypes = [ctypes.c_void_p]
            g.lib.gbm_bo_get_stride.restype = ctypes.c_uint32
            g.lib.gbm_bo_get_fd.argtypes = [ctypes.c_void_p]
            g.lib.gbm_bo_map.restype = ctypes.c_void_p
            g.lib.gbm_bo_map.argtypes = [ctypes.c_void_p] + [ctypes.c_uint32] * 4 + [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]
            g.lib.gbm_bo_unmap.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
            g.node = nodes[0]
            g.fd = os.open(nodes[0], os.O_RDWR)
            g.dev = g.lib.gbm_create_device(g.fd)
            return g if g.dev else None
        except Exception:
            return None

    def bo(self, w, h, fmt=None):
        bo = self.lib.gbm_bo_create(self.dev, w, h, fmt or self.XRGB8888, self.GBM_BO_USE_LINEAR | self.GBM_BO_USE_RENDERING)
        return bo if bo else None

    def bo_with_modifier(self, w, h, fmt, modifier):
        """A buffer object in the given layout (tiled ones too), or None when the driver cannot make it."""
        ct = self.ct
        if not hasattr(self.lib, 'gbm_bo_create_with_modifiers'):
            return None
        self.lib.gbm_bo_create_with_modifiers.restype = ct.c_void_p
        self.lib.gbm_bo_create_with_modifiers.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_uint32, ct.c_uint32, ct.POINTER(ct.c_uint64), ct.c_uint32]
        mods = (ct.c_uint64 * 1)(modifier)
        bo = self.lib.gbm_bo_create_with_modifiers(self.dev, w, h, fmt, mods, 1)
        return bo if bo else None

    def stride(self, bo):
        return self.lib.gbm_bo_get_stride(bo)

    def fd_of(self, bo):
        return self.lib.gbm_bo_get_fd(bo)

    def fill(self, bo, w, h, pixel_of):
        """Write pixel_of(x, y) (a 32-bit value) for every pixel through a CPU mapping."""
        ct = self.ct
        stride, data = ct.c_uint32(), ct.c_void_p()
        m = self.lib.gbm_bo_map(bo, 0, 0, w, h, self.GBM_BO_TRANSFER_WRITE, ct.byref(stride), ct.byref(data))
        if not m:
            raise RuntimeError('gbm_bo_map failed')
        for y in range(h):
            row = b''.join(struct.pack('<I', pixel_of(x, y)) for x in range(w))
            ct.memmove(m + y * stride.value, row, len(row))
        self.lib.gbm_bo_unmap(bo, data)


class TestDmabuf(Base):
    """zwp_linux_dmabuf_v1: clients that render with the GPU hand MellaGUI dmabufs, which are read like shm buffers."""

    DMABUF_ARGB, DMABUF_XRGB = 0x34325241, 0x34325258

    def gbm(self):
        g = Gbm.open()
        if g is None:
            self.skipTest('no render node or libgbm')
        return g

    def setUp(self):
        super().setUp()
        self.c = self.client()
        if 'zwp_linux_dmabuf_v1' not in self.c.wl.globals:
            self.skipTest('compositor has no dmabuf global (no render node)')

    def dmabuf(self):
        return self.c.wl.bind('zwp_linux_dmabuf_v1', 4)

    def import_bo(self, g, bo, w, h, fmt, immed=True, plane=0, modifier=(0, 0), flags=0, offset=0):
        wl = self.c.wl
        dm = self.dmabuf()
        params = wl.new_id('zwp_linux_buffer_params_v1')
        wl.request(dm, 'create_params', params)
        fd = g.fd_of(bo)
        wl.request(params, 'add', plane, offset, g.stride(bo), modifier[0], modifier[1], fds=[fd], **{}) if False else None
        wl.request(params, 'add', plane, offset, g.stride(bo), modifier[0], modifier[1], fds=[fd])
        os.close(fd)
        if immed:
            buf = wl.new_id('wl_buffer')
            wl.request(params, 'create_immed', buf, I(w), I(h), fmt, flags)
            return params, buf
        wl.request(params, 'create', I(w), I(h), fmt, flags)
        return params, None

    def window_with(self, w, h):
        win = Window(self.c, w, h)
        return win

    def show(self, win, buf, w, h):
        wl = self.c.wl
        wl.request(win.surface, 'attach', buf, I(0), I(0))
        wl.request(win.surface, 'damage_buffer', I(0), I(0), I(w), I(h))
        wl.request(win.surface, 'commit')
        wl.roundtrip()

    def window_origin(self):
        r = ctl.windows()['windows'][0]
        return r['x'], r['y']

    def test_global_is_version_4_and_feedback_names_the_render_node(self):
        wl = self.c.wl
        self.assertEqual(wl.globals['zwp_linux_dmabuf_v1'][1], 4)
        dm = self.dmabuf()
        fb = wl.new_id('zwp_linux_dmabuf_feedback_v1')
        wl.request(dm, 'get_default_feedback', fb)
        wl.roundtrip()
        evs = [e for e in wl.queue if e.obj == fb]
        wl.queue = [e for e in wl.queue if e.obj != fb]
        self.assertEqual([e.name for e in evs], ['format_table', 'main_device', 'tranche_target_device', 'tranche_formats',
                                                  'tranche_flags', 'tranche_done', 'done'])
        table_fd, size = evs[0].fds[0], evs[0].args[0]
        data = os.pread(table_fd, size, 0)
        os.close(table_fd)
        entries = [struct.unpack('<IIQ', data[i:i + 16]) for i in range(0, size, 16)]
        pairs = [(f, m) for f, _, m in entries]
        if os.environ.get('MELLAGUI_RENDERER') == 'gl':               # + the layouts the GPU can sample (tiled ones)
            self.assertIn((self.DMABUF_ARGB, 0), pairs)
            self.assertIn((self.DMABUF_XRGB, 0), pairs)
            self.assertTrue({f for f, _ in pairs} <= {self.DMABUF_ARGB, self.DMABUF_XRGB})
        else:
            self.assertEqual(pairs, [(self.DMABUF_ARGB, 0), (self.DMABUF_XRGB, 0)])           # linear ARGB8888 / XRGB8888 only
        dev = struct.unpack('<Q', evs[1].args[0])[0]
        self.assertEqual(evs[1].args[0], evs[2].args[0])
        self.assertEqual(list(struct.unpack('<%dH' % (len(evs[3].args[0]) // 2), evs[3].args[0])), list(range(size // 16)))
        st = os.stat(Gbm.open().node) if Gbm.open() else None
        if st is not None:
            self.assertEqual(os.major(dev), os.major(st.st_rdev))                              # a DRM render node (major 226)
            self.assertEqual(os.major(dev), 226)

    def test_older_versions_get_the_formats_as_events(self):
        wl = self.c.wl
        dm = wl.bind('zwp_linux_dmabuf_v1', 3)
        wl.roundtrip()
        mods = [e.args for e in wl.queue if e.obj == dm and e.name == 'modifier']
        wl.queue = [e for e in wl.queue if e.obj != dm]
        if os.environ.get('MELLAGUI_RENDERER') == 'gl':
            self.assertIn([self.DMABUF_ARGB, 0, 0], mods)
            self.assertIn([self.DMABUF_XRGB, 0, 0], mods)
        else:
            self.assertEqual(mods, [[self.DMABUF_ARGB, 0, 0], [self.DMABUF_XRGB, 0, 0]])

    def test_a_gpu_buffer_is_shown_and_follows_updates(self):
        g = self.gbm()
        w, h = 200, 120
        bo = g.bo(w, h)
        self.assertIsNotNone(bo, 'gbm could not make a linear buffer')
        g.fill(bo, w, h, lambda x, y: 0xff000000 | (0xc02020 if x < 100 else 0x2040c0))
        win = self.window_with(w, h)
        params, buf = self.import_bo(g, bo, w, h, self.DMABUF_XRGB)
        self.show(win, buf, w, h)
        self.c.wl.roundtrip()
        x0, y0 = self.window_origin()
        ppm = ctl.dump(os.path.join(tmp, 'dmabuf1.ppm'))
        self.assertEqual(ppm.px(x0 + 20, y0 + 20), (0xc0, 0x20, 0x20))
        self.assertEqual(ppm.px(x0 + 150, y0 + 100), (0x20, 0x40, 0xc0))
        # the same buffer object repainted by the producer: a new commit shows the new pixels (sync + the kept mapping)
        g.fill(bo, w, h, lambda x, y: 0xff208040)
        self.show(win, buf, w, h)
        self.c.wl.roundtrip()
        ppm = ctl.dump(os.path.join(tmp, 'dmabuf2.ppm'))
        self.assertEqual(ppm.px(x0 + 20, y0 + 20), (0x20, 0x80, 0x40))
        self.assertEqual(ppm.px(x0 + 150, y0 + 100), (0x20, 0x80, 0x40))
        released = any(e.obj == buf and e.name == 'release' for e in self.c.wl.queue)
        if os.environ.get('MELLAGUI_RENDERER') == 'gl':
            self.assertFalse(released)          # zero copy: the GPU samples the client's buffer, which stays with us until replaced
        else:
            self.assertTrue(released)           # the compositor released the buffer once it had copied it

    def test_argb_buffers_blend_with_what_is_behind(self):
        g = self.gbm()
        w, h = 64, 64
        bo = g.bo(w, h, Gbm.ARGB8888)
        g.fill(bo, w, h, lambda x, y: 0xff102030 if x < 32 else 0x00000000)         # opaque left half, transparent right half
        win = self.window_with(w, h)
        params, buf = self.import_bo(g, bo, w, h, self.DMABUF_ARGB)
        self.show(win, buf, w, h)
        x0, y0 = self.window_origin()
        ppm = ctl.dump(os.path.join(tmp, 'dmabuf3.ppm'))
        self.assertEqual(ppm.px(x0 + 5, y0 + 5), (0x10, 0x20, 0x30))
        self.assertNotEqual(ppm.px(x0 + 50, y0 + 5), (0, 0, 0))                          # transparent: the window background shows

    def test_partial_update_with_odd_width_and_hostile_alpha_is_fixed_in_the_copy(self):
        """The alpha fix runs inside the streaming copy: unaligned rect starts, odd widths, colour above alpha."""
        if os.environ.get('MELLAGUI_RENDERER') == 'gl':
            self.skipTest('zero copy: the GPU samples the buffer as it is, nothing repairs invalid premultiplied colours')
        g = self.gbm()
        w, h = 37, 8
        bo = g.bo(w, h, Gbm.ARGB8888)
        g.fill(bo, w, h, lambda x, y: 0xff102030)
        win = self.window_with(w, h)
        params, buf = self.import_bo(g, bo, w, h, self.DMABUF_ARGB)
        self.show(win, buf, w, h)
        x0, y0 = self.window_origin()
        ppm = ctl.dump(os.path.join(tmp, 'dmabuf4.ppm'))
        self.assertEqual(ppm.px(x0 + 1, y0 + 1), (0x10, 0x20, 0x30))
        # alpha 0x80 with red 0xff: not a valid premultiplied pixel; it must be clamped, never overflow the blend
        g.fill(bo, w, h, lambda x, y: 0x80ff0000)
        wl = self.c.wl
        wl.request(win.surface, 'attach', buf, I(0), I(0))
        wl.request(win.surface, 'damage_buffer', I(3), I(1), I(21), I(3))               # starts 12 bytes into a row
        wl.request(win.surface, 'commit')
        wl.roundtrip()
        ppm = ctl.dump(os.path.join(tmp, 'dmabuf5.ppm'))
        for (x, y) in ((3, 1), (4, 2), (23, 3), (13, 2)):                                  # inside the damage
            r, gr, b = ppm.px(x0 + x, y0 + y)
            self.assertLessEqual(abs(r - 136), 1, (x, y, r))
            self.assertLessEqual(abs(gr - 9), 1)
            self.assertLessEqual(abs(b - 11), 1)
        for (x, y) in ((0, 0), (2, 1), (24, 2), (36, 7), (10, 5)):                         # outside: still the old frame
            self.assertEqual(ppm.px(x0 + x, y0 + y), (0x10, 0x20, 0x30), (x, y))

    def test_create_without_immed_answers_created_and_failed(self):
        g = self.gbm()
        w, h = 32, 32
        bo = g.bo(w, h)
        g.fill(bo, w, h, lambda x, y: 0xff808080)
        wl = self.c.wl
        params, _ = self.import_bo(g, bo, w, h, self.DMABUF_XRGB, immed=False)
        ev = wl.wait_for(lambda e: e.obj == params and e.name in ('created', 'failed'), 2.0)
        self.assertIsNotNone(ev)
        self.assertEqual(ev.name, 'created')
        self.assertGreaterEqual(ev.args[0], 0xff000000)                                   # a server-allocated wl_buffer id
        # a memfd is not a dmabuf: the import is refused with `failed`, not a crash
        fd = os.memfd_create('not-a-dmabuf')
        os.ftruncate(fd, 4096 * 4)
        p2 = wl.new_id('zwp_linux_buffer_params_v1')
        wl.request(self.dmabuf(), 'create_params', p2)
        wl.request(p2, 'add', 0, 0, 128, 0, 0, fds=[fd])
        os.close(fd)
        wl.request(p2, 'create', I(32), I(32), self.DMABUF_XRGB, 0)
        ev = wl.wait_for(lambda e: e.obj == p2 and e.name in ('created', 'failed'), 2.0)
        self.assertEqual(ev.name, 'failed')

    def expect_error(self, code, build):
        """Run `build(wl)` on a fresh connection and expect a protocol error with `code` on the params object."""
        c2 = self.client()
        wl = c2.wl
        dm = wl.bind('zwp_linux_dmabuf_v1', 4)
        params = wl.new_id('zwp_linux_buffer_params_v1')
        wl.request(dm, 'create_params', params)
        build(wl, params)
        try:
            wl.roundtrip()
        except Exception:
            pass
        end = time.time() + 2
        while wl.error is None and time.time() < end:
            wl.pump(0.05)
        self.assertIsNotNone(wl.error, 'no protocol error')
        self.assertEqual(wl.error.code, code, wl.error)

    def test_bad_requests_are_protocol_errors(self):
        g = self.gbm()
        bo = g.bo(32, 32)
        stride = g.stride(bo)

        def add(wl, params, plane=0, offset=0):
            fd = g.fd_of(bo)
            wl.request(params, 'add', plane, offset, stride, 0, 0, fds=[fd])
            os.close(fd)
        self.expect_error(1, lambda wl, p: add(wl, p, plane=1))                                # plane_idx
        self.expect_error(2, lambda wl, p: (add(wl, p), add(wl, p)))                           # plane_set
        self.expect_error(3, lambda wl, p: wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 0))  # incomplete
        self.expect_error(4, lambda wl, p: (add(wl, p), wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), 0x34324241, 0)))   # format
        self.expect_error(5, lambda wl, p: (add(wl, p), wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(0), I(32), self.DMABUF_XRGB, 0)))  # dimensions
        self.expect_error(6, lambda wl, p: (add(wl, p), wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(4000), self.DMABUF_XRGB, 0)))  # out of bounds
        self.expect_error(7, lambda wl, p: (add(wl, p), wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 1)))  # flags
        self.expect_error(0, lambda wl, p: (add(wl, p), wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 0),
                                            wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 0)))      # already_used
        # a regular file or memfd with create_immed: refused (invalid_wl_buffer), never mapped
        def memfd(wl, p):
            fd = os.memfd_create('x')
            os.ftruncate(fd, 64 * 1024)
            wl.request(p, 'add', 0, 0, 128, 0, 0, fds=[fd])
            os.close(fd)
            wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 0)
        self.expect_error(7, memfd)
        # a tiled (non-linear) modifier is not offered
        self.expect_error(7, lambda wl, p: (wl.request(p, 'add', 0, 0, stride, 0, 1, fds=[g.fd_of(bo)]),
                                             wl.request(p, 'create_immed', wl.new_id('wl_buffer'), I(32), I(32), self.DMABUF_XRGB, 0)))

    def test_no_dmabuf_option_hides_the_global(self):
        p, c, stop = self.private_compositor('mellagui-dmabuf-off', '--no-dmabuf')
        cl = Client(name='mellagui-dmabuf-off')
        cl.wl.get_registry()
        cl.wl.roundtrip()
        self.assertNotIn('zwp_linux_dmabuf_v1', cl.wl.globals)


class TestDmabufZeroCopy(Base):
    """With the GL renderer a client's dmabuf is sampled in place by the GPU (an EGLImage): no CPU copy, the wl_buffer is held until
    the next commit replaces it."""
    NAME = 'mellagui-zc'
    ARGB, XRGB = 0x34325241, 0x34325258

    def setUp(self):
        super().setUp()
        self.g = Gbm.open()
        if self.g is None:
            self.skipTest('no render node or libgbm')
        tool = os.path.join(ROOT, 'build', 'gl-test')
        if not os.path.exists(tool) or subprocess.run([tool], capture_output=True, timeout=60).returncode == 77:
            self.skipTest('no OpenGL ES here')
        self.shots = os.path.join(tmp, self.NAME + '-shots')
        p, self.c, stop = self.private_compositor(self.NAME, '--screenshot-dir', self.shots,
                                                  env={'MELLAGUI_RENDERER': 'gl'})
        self.cl = Client(self.NAME)
        self.clients.append(self.cl)
        if 'zwp_linux_dmabuf_v1' not in self.cl.wl.globals:
            self.skipTest('no dmabuf global')

    def bo(self, w, h, color, fmt=None):
        bo = self.g.bo(w, h) if fmt is None else self.g.bo(w, h, fmt)
        self.assertIsNotNone(bo)
        self.g.fill(bo, w, h, color if callable(color) else (lambda x, y: color))
        return bo

    def wl_buffer(self, bo, w, h, fmt):
        wl = self.cl.wl
        dm = wl.bind('zwp_linux_dmabuf_v1', 4)
        params = wl.new_id('zwp_linux_buffer_params_v1')
        wl.request(dm, 'create_params', params)
        fd = self.g.fd_of(bo)
        wl.request(params, 'add', 0, 0, self.g.stride(bo), 0, 0, fds=[fd])
        os.close(fd)
        buf = wl.new_id('wl_buffer')
        wl.request(params, 'create_immed', buf, I(w), I(h), fmt, 0)
        return buf

    def show(self, win, buf, w, h):
        wl = self.cl.wl
        wl.request(win.surface, 'attach', buf, I(0), I(0))
        wl.request(win.surface, 'damage_buffer', I(0), I(0), I(w), I(h))
        wl.request(win.surface, 'commit')
        wl.roundtrip()

    def released(self, buf):
        self.cl.wl.pump(0.05)
        return any(e.obj == buf and e.name == 'release' for e in self.cl.wl.queue)

    def origin(self):
        r = self.c.windows()['windows'][0]
        return r['x'], r['y']

    def settle(self, name):
        self.c.abs(790, 490)
        self.c.sync()
        time.sleep(0.15)
        self.c.sync()
        return self.c.dump(os.path.join(tmp, '%s-%s.ppm' % (self.NAME, name)))

    def test_shown_in_place_and_held_until_replaced(self):
        w, h = 120, 80
        win = Window(self.cl, w, h)
        a, b = self.bo(w, h, 0xff2040c0), self.bo(w, h, 0xffc08020)
        ba, bb = self.wl_buffer(a, w, h, self.XRGB), self.wl_buffer(b, w, h, self.XRGB)
        self.show(win, ba, w, h)
        x0, y0 = self.origin()
        self.assertEqual(self.settle('a').px(x0 + 10, y0 + 10), (0x20, 0x40, 0xc0))
        self.assertFalse(self.released(ba), 'the buffer is still being sampled: it must not be released')
        self.show(win, bb, w, h)
        self.assertEqual(self.settle('b').px(x0 + 10, y0 + 10), (0xc0, 0x80, 0x20))
        self.assertTrue(self.released(ba), 'the replaced buffer goes back to the client')
        self.assertFalse(self.released(bb))

    def test_the_producer_can_repaint_the_same_buffer(self):
        w, h = 100, 60
        win = Window(self.cl, w, h)
        bo = self.bo(w, h, 0xff102030)
        buf = self.wl_buffer(bo, w, h, self.XRGB)
        self.show(win, buf, w, h)
        x0, y0 = self.origin()
        self.assertEqual(self.settle('r1').px(x0 + 5, y0 + 5), (0x10, 0x20, 0x30))
        self.g.fill(bo, w, h, lambda x, y: 0xff80a0c0)
        self.show(win, buf, w, h)
        self.assertEqual(self.settle('r2').px(x0 + 5, y0 + 5), (0x80, 0xa0, 0xc0))

    def test_argb_blends_with_what_is_behind(self):
        w, h = 64, 64
        win = Window(self.cl, w, h)
        bo = self.bo(w, h, lambda x, y: 0xff102030 if x < 32 else 0x00000000, Gbm.ARGB8888)
        self.show(win, self.wl_buffer(bo, w, h, self.ARGB), w, h)
        x0, y0 = self.origin()
        ppm = self.settle('argb')
        self.assertEqual(ppm.px(x0 + 5, y0 + 5), (0x10, 0x20, 0x30))
        self.assertNotEqual(ppm.px(x0 + 50, y0 + 5), (0, 0, 0))              # transparent half: the window's background shows

    def test_a_screenshot_of_a_zero_copy_window_has_its_pixels(self):
        w, h = 160, 100
        win = Window(self.cl, w, h)
        self.show(win, self.wl_buffer(self.bo(w, h, 0xff20c060), w, h, self.XRGB), w, h)
        x0, y0 = self.origin()
        self.settle('shot')
        self.c.tap(99)
        for _ in range(100):
            if os.path.isdir(self.shots) and os.listdir(self.shots):
                break
            time.sleep(0.03)
        with open(os.path.join(self.shots, sorted(os.listdir(self.shots))[0]), 'rb') as f:
            pw, ph, rows = TestScreenshot.decode_png(f.read())
        self.assertEqual(tuple(rows[y0 + 10][(x0 + 10) * 3:(x0 + 10) * 3 + 3]), (0x20, 0xc0, 0x60))

    def test_back_to_shm_content_on_the_same_surface(self):
        w, h = 100, 70
        win = Window(self.cl, w, h, color=0xff303030)
        buf = self.wl_buffer(self.bo(w, h, 0xffc02020), w, h, self.XRGB)
        self.show(win, buf, w, h)
        x0, y0 = self.origin()
        self.assertEqual(self.settle('z1').px(x0 + 5, y0 + 5), (0xc0, 0x20, 0x20))
        win.commit_buffer(w, h, color=0xff2020c0)                           # an shm buffer again
        self.cl.wl.roundtrip()
        self.assertEqual(self.settle('z2').px(x0 + 5, y0 + 5), (0x20, 0x20, 0xc0))
        self.assertTrue(self.released(buf))
        buf2 = self.wl_buffer(self.bo(w, h, 0xff20c020), w, h, self.XRGB)    # and zero copy once more
        self.show(win, buf2, w, h)
        self.assertEqual(self.settle('z3').px(x0 + 5, y0 + 5), (0x20, 0xc0, 0x20))

    def offered(self):
        """{(fourcc, modifier)} from the default feedback's format table."""
        wl = self.cl.wl
        dm = wl.bind('zwp_linux_dmabuf_v1', 4)
        fb = wl.new_id('zwp_linux_dmabuf_feedback_v1')
        wl.request(dm, 'get_default_feedback', fb)
        wl.roundtrip()
        evs = [e for e in wl.queue if e.obj == fb]
        wl.queue = [e for e in wl.queue if e.obj != fb]
        fd, size = evs[0].fds[0], evs[0].args[0]
        data = os.pread(fd, size, 0)
        os.close(fd)
        return {(struct.unpack_from('<I', data, i)[0], struct.unpack_from('<Q', data, i + 8)[0]) for i in range(0, size, 16)}

    def wl_buffer_mod(self, bo, w, h, fmt, modifier, immed=True):
        wl = self.cl.wl
        dm = wl.bind('zwp_linux_dmabuf_v1', 4)
        params = wl.new_id('zwp_linux_buffer_params_v1')
        wl.request(dm, 'create_params', params)
        fd = self.g.fd_of(bo)
        wl.request(params, 'add', 0, 0, self.g.stride(bo), modifier >> 32, modifier & 0xffffffff, fds=[fd])
        os.close(fd)
        if immed:
            buf = wl.new_id('wl_buffer')
            wl.request(params, 'create_immed', buf, I(w), I(h), fmt, 0)
            return params, buf
        wl.request(params, 'create', I(w), I(h), fmt, 0)
        return params, None

    def test_the_gpu_layouts_are_offered_and_linear_stays_first(self):
        offered = self.offered()
        self.assertIn((self.XRGB, 0), offered)
        self.assertIn((self.ARGB, 0), offered)
        for f, m in offered:
            self.assertIn(f, (self.XRGB, self.ARGB))
            self.assertIn(m, (0, (1 << 56) | 1, (1 << 56) | 2, (1 << 56) | 3), 'only single plane layouts that are known are offered')

    def test_a_tiled_buffer_is_shown_correctly(self):
        offered = self.offered()
        tiled = sorted(m for f, m in offered if f == self.XRGB and m != 0)
        if not tiled:
            self.skipTest('the GPU samples linear buffers only')
        w, h = 150, 90
        for m in tiled:
            bo = self.g.bo_with_modifier(w, h, self.XRGB, m)
            if bo is None:
                continue
            self.g.fill(bo, w, h, lambda x, y: 0xff000000 | ((x * 255 // w) << 16) | ((y * 255 // h) << 8) | 0x40)
            win = Window(self.cl, w, h)
            params, buf = self.wl_buffer_mod(bo, w, h, self.XRGB, m)
            self.show(win, buf, w, h)
            x0, y0 = self.origin()
            ppm = self.settle('tiled%d' % (m & 0xff))
            for (x, y) in ((3, 3), (140, 5), (5, 80), (140, 85), (75, 45)):             # a gradient: a wrong detiling would scramble it
                want = ((x * 255 // w), (y * 255 // h), 0x40)
                got = ppm.px(x0 + x, y0 + y)
                self.assertTrue(all(abs(a - b) <= 2 for a, b in zip(got, want)), (m, x, y, got, want))
            return
        self.skipTest('gbm could not make a tiled buffer')

    def test_a_layout_that_was_not_offered_is_refused(self):
        w, h = 64, 64
        bo = self.bo(w, h, 0xff102030)
        params, _ = self.wl_buffer_mod(bo, w, h, self.XRGB, (7 << 56) | 99, immed=False)       # some vendor layout we never offered
        self.cl.wl.roundtrip()
        evs = [e for e in self.cl.wl.queue if e.obj == params]
        self.assertEqual([e.name for e in evs], ['failed'])
        self.cl.wl.queue = [e for e in self.cl.wl.queue if e.obj != params]

    def test_a_client_that_leaves_while_its_buffer_is_held(self):
        w, h = 90, 60
        win = Window(self.cl, w, h)
        self.show(win, self.wl_buffer(self.bo(w, h, 0xff808080), w, h, self.XRGB), w, h)
        self.cl.wl.close()
        for _ in range(100):
            if not self.c.windows()['windows']:
                break
            time.sleep(0.03)
        self.assertEqual(self.c.windows()['windows'], [])
        self.settle('gone')
        self.c.cmd('windows')                                                # the compositor still answers


class TestOcclusion(Base):
    """The renderer skips what an opaque surface covers; translucent windows must still blend with what is below."""

    def two_windows(self):
        low = Window(self.client(), 300, 220, title='low', color=0xff00ff00)
        c = self.client()
        top = Window(c, 200, 140, title='top', color=0xffff0000)
        ws = {w['title']: w for w in ctl.windows()['windows']}
        a, b = ws['low'], ws['top']
        if not (b['x'] + 60 < a['x'] + a['w'] and a['x'] + 60 < b['x'] + b['w'] and b['y'] + 60 < a['y'] + a['h'] and a['y'] + 60 < b['y'] + b['h']):
            # (the cascade position depends on how many windows the shared compositor placed before) move `top` over `low`
            dx, dy = a['x'] + 30 - b['x'], a['y'] + 30 - b['y']
            ctl.abs(b['x'] + 50, b['y'] - 14)
            ctl.button(272, True)
            ctl.abs(b['x'] + 50 + dx, b['y'] - 14 + dy)
            ctl.button(272, False)
            ctl.sync()
            ws = {w['title']: w for w in ctl.windows()['windows']}
            a, b = ws['low'], ws['top']
        x0, y0 = max(a['x'], b['x']), max(a['y'], b['y'])
        x1, y1 = min(a['x'] + a['w'], b['x'] + b['w']), min(a['y'] + a['h'], b['y'] + b['h'])
        self.assertTrue(x1 - x0 > 20 and y1 - y0 > 20, 'the windows do not overlap: %r %r' % (a, b))
        return low, top, (x0 + 10, y0 + 10), (a, b)

    def commit_argb(self, win, argb):
        w, h = win.size
        buf = win.shm.buffer(w, h, argb, fmt=0)                               # ARGB8888
        wl = win.c.wl
        wl.request(win.surface, 'attach', buf, I(0), I(0))
        wl.request(win.surface, 'damage_buffer', I(0), I(0), I(w), I(h))
        wl.request(win.surface, 'commit')
        wl.roundtrip()

    def test_an_opaque_argb_window_hides_what_is_below(self):
        low, top, pt, _ = self.two_windows()
        self.commit_argb(top, 0xffff0000)
        ppm = ctl.dump(os.path.join(tmp, 'occ1.ppm'))
        self.assertEqual(ppm.px(*pt), (255, 0, 0))

    def test_a_window_that_becomes_translucent_blends_with_its_frame_background(self):
        low, top, pt, _ = self.two_windows()
        self.commit_argb(top, 0xffff0000)                                     # opaque first: the renderer treats it as solid
        self.assertEqual(ctl.dump(os.path.join(tmp, 'occ2a.ppm')).px(*pt), (255, 0, 0))
        self.commit_argb(top, 0x80800000)                                     # premultiplied: half transparent red
        # A translucent client shows the frame's own background (0x101216), not the window below: 0x80 + 0x10 * 127 / 255 ...
        r, g, b = ctl.dump(os.path.join(tmp, 'occ2b.ppm')).px(*pt)
        self.assertTrue(abs(r - 136) <= 1 and abs(g - 9) <= 1 and abs(b - 11) <= 1, (r, g, b))
        self.commit_argb(top, 0xff0000ff)                                     # and opaque blue again
        self.assertEqual(ctl.dump(os.path.join(tmp, 'occ2c.ppm')).px(*pt), (0, 0, 255))

    def test_transparent_corners_with_an_opaque_region_paint_exactly_right(self):
        low, top, pt, (a, b) = self.two_windows()
        c = top.c
        wl = c.wl
        w, h = top.size
        # an ARGB buffer: opaque blue, except a 12x12 transparent box in each corner (like a rounded GTK window)
        buf = top.shm.buffer(w, h, 0xff0000ff, fmt=0)
        for (cx, cy) in ((0, 0), (w - 12, 0), (0, h - 12), (w - 12, h - 12)):
            for y in range(cy, cy + 12):
                top.shm.map[(y * w + cx) * 4:(y * w + cx + 12) * 4] = b'\0' * 48
        region = wl.new_id('wl_region')
        wl.request(c.compositor, 'create_region', region)
        wl.request(region, 'add', I(0), I(12), I(w), I(h - 24))                  # the opaque region: two overlapping bands
        wl.request(region, 'add', I(12), I(0), I(w - 24), I(h))
        wl.request(top.surface, 'set_opaque_region', region)
        wl.request(top.surface, 'attach', buf, I(0), I(0))
        wl.request(top.surface, 'damage_buffer', I(0), I(0), I(w), I(h))
        wl.request(top.surface, 'commit')
        wl.roundtrip()
        ppm = ctl.dump(os.path.join(tmp, 'occ4.ppm'))
        x, y = b['x'], b['y']
        self.assertEqual(ppm.px(x + w // 2, y + h // 2), (0, 0, 255))              # inside the cover
        self.assertEqual(ppm.px(x + 3, y + 50), (0, 0, 255))                       # the left band of the opaque region
        self.assertEqual(ppm.px(x + w // 2, y + 3), (0, 0, 255))                   # the top band
        self.assertEqual(ppm.px(x + 5, y + 5), (0x10, 0x12, 0x16))                 # a transparent corner: the frame's background
        self.assertEqual(ppm.px(x + w - 6, y + h - 6), (0x10, 0x12, 0x16))
        # and the window below is intact right next to the top window
        self.assertEqual(ppm.px(a['x'] + 5, a['y'] + 5), (0, 255, 0))

    def test_a_window_moved_away_uncovers_what_was_below_it(self):
        low, top, pt, (a, b) = self.two_windows()
        before = ctl.dump(os.path.join(tmp, 'occ3a.ppm')).px(*pt)
        self.assertEqual(before, (255, 0, 0))
        ctl.abs(b['x'] + 50, b['y'] - 14)                                      # drag the top window's title bar to the right
        ctl.button(272, True)
        ctl.abs(b['x'] + 50 + 420, b['y'] - 14)
        ctl.button(272, False)
        ctl.sync()
        after = ctl.dump(os.path.join(tmp, 'occ3b.ppm')).px(*pt)
        self.assertEqual(after, (0, 255, 0))                                   # the window that was below shows again


class TestLock(Base):
    """Screen lock (src/lock.c): password / PIN, delays, idle lock, panel button, nothing reaches clients while locked."""
    ENV = {'MELLAGUI_LOCK_ITER': '1000', 'MELLAGUI_LOCK_DELAY_MS': '1500', 'MELLAGUI_LOCK_UNIT_MS': '1000'}
    CODES = {c: k for k, c in enumerate('1234567890', 2)}
    CODES.update({c: k for c, k in zip('qwertyuiop', range(16, 26))})
    CODES.update({c: k for c, k in zip('asdfghjkl', range(30, 39))})
    CODES.update({c: k for c, k in zip('zxcvbnm', range(44, 51))})
    POWER_BTN = (800 - 4 - 12, 14)      # the panel's round power button on the 800 px wide private compositors
    LOCK_DISC = (348, 242)              # the session dialog's Lock button there (card 264x172 centred on 800x500)

    def start(self, name='lock', *extra, file=None, env=None):
        args = ['--lock-file', file] if file else []
        return self.private_compositor(name, *args, *extra, env=dict(self.ENV, **(env or {})))

    @staticmethod
    def state(c):
        c.cmd('lock')
        head, _, msg = c.last.partition(' message=')                      # the message holds blanks
        st = dict(kv.split('=', 1) for kv in head.split()[1:])
        st['message'] = msg
        return st

    def type(self, c, text, enter=True):
        for ch in text:
            c.tap(self.CODES[ch])
        if enter:
            c.tap(KEY_ENTER)
        c.sync()

    def keyboard_client(self, name):
        cl = Client(os.path.join(tmp, name))
        self.clients.append(cl)
        seat = cl.wl.bind('wl_seat', 5)
        kb = cl.wl.new_id('wl_keyboard')
        cl.wl.request(seat, 'get_keyboard', kb)
        win = Window(cl, 300, 200)
        cl.wl.roundtrip()
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'enter', 2.0), 'no keyboard enter')
        cl.wl.queue.clear()
        return cl, kb, win

    def test_nothing_set_nothing_locks(self):
        p, c, stop = self.start()
        self.assertEqual(self.state(c)['kind'], 'none')
        with self.assertRaises(RuntimeError):
            c.cmd('lock now')
        c.key(KEY_LEFTMETA, True)
        c.tap(38)                                                          # Super+L
        c.key(KEY_LEFTMETA, False)
        self.assertEqual(self.state(c)['locked'], '0')

    def test_choosing_a_secret(self):
        path = os.path.join(tmp, 'lock-choose.conf')
        p, c, stop = self.start('lock-choose', file=path)
        for bad in ('- password short', '- password has\x01ctl12345', '- pin 123', '- pin 12a45', '- pin 1234567890123',
                    '- other whatever1', '- keep - 5'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('lock set ' + bad)
        self.assertFalse(os.path.exists(path))
        c.cmd('lock set - password hunter22x')
        st = self.state(c)
        self.assertEqual((st['kind'], st['status']), ('password', 'ok'))
        self.assertEqual(oct(os.stat(path).st_mode & 0o777), '0o600')
        text = open(path).read()
        self.assertNotIn('hunter22x', text)                                # only a salted hash is kept
        for k in ('kind=password', 'iter=1000', 'salt=', 'hash='):
            self.assertIn(k, text)
        # changing it needs the current secret
        with self.assertRaises(RuntimeError):
            c.cmd('lock set wrongsecret pin 4321')
        self.assertEqual(self.state(c)['status'], 'wrong')
        self.assertEqual(self.state(c)['kind'], 'password')
        c.cmd('lock set hunter22x pin 4321')
        self.assertEqual(self.state(c)['kind'], 'pin')
        self.assertNotIn('4321', open(path).read())
        # turning it off needs it too, and removes the file
        with self.assertRaises(RuntimeError):
            c.cmd('lock set 0000 none -')
        c.cmd('lock set 4321 none -')
        self.assertEqual(self.state(c)['kind'], 'none')
        self.assertFalse(os.path.exists(path))

    def test_locked_screen_shows_nothing_and_sends_nothing(self):
        p, c, stop = self.start()
        c.cmd('lock set - password hunter22x')
        cl, kb, win = self.keyboard_client('lock')
        c.cmd('lock now')
        self.assertEqual(self.state(c)['locked'], '1')
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'leave', 2.0), 'focus not taken away')
        # the window is not in the picture: the lock screen is
        c.cmd('dump ' + os.path.join(tmp, 'locked.ppm'))
        ppm = Ppm(os.path.join(tmp, 'locked.ppm'))
        self.assertEqual(ppm.px(5, 5), (0x0b, 0x0d, 0x12))
        self.assertEqual(ppm.px(790, 490), (0x0b, 0x0d, 0x12))
        # typing, shortcuts, clicks go nowhere
        cl.wl.queue.clear()
        c.tap(KEY_A)
        c.key(KEY_LEFTMETA, True); c.key(KEY_LEFTSHIFT, True); c.tap(18); c.key(KEY_LEFTSHIFT, False); c.key(KEY_LEFTMETA, False)  # Super+Shift+E
        c.key(KEY_LEFTMETA, True); c.tap(KEY_ENTER); c.key(KEY_LEFTMETA, False)                                              # terminal
        c.click(100, 100)
        c.sync()
        self.assertTrue(p.poll() is None, 'Super+Shift+E quit a locked compositor')
        cl.wl.roundtrip()
        self.assertEqual([e.name for e in cl.wl.queue if e.obj == kb and e.name in ('key', 'enter')], [])
        self.assertEqual(self.state(c)['locked'], '1')
        # wrong secret, then the right one
        self.type(c, 'nottherightone')
        st = self.state(c)
        self.assertEqual((st['locked'], st['fails']), ('1', '1'))
        self.assertEqual(st['message'], 'Wrong password.')
        self.type(c, 'hunter22x')
        self.assertEqual(self.state(c)['locked'], '0')
        # the window has the keyboard again and nothing typed while locked arrives
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'enter', 2.0), 'focus not given back')
        keys = [e for e in cl.wl.queue if e.obj == kb and e.name == 'key']
        self.assertEqual(keys, [])
        c.tap(KEY_A)
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'key' and e.args[2] == KEY_A, 2.0))

    def test_a_pin_takes_digits_only_and_the_keypad_works(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 2468')
        c.cmd('lock now')
        self.type(c, 'a2b4c6d8')                                           # letters are not typed into a PIN
        self.assertEqual(self.state(c)['locked'], '0')

    def test_backspace_and_escape(self):
        p, c, stop = self.start()
        c.cmd('lock set - password hunter22x')
        c.cmd('lock now')
        self.type(c, 'hunter22xy', enter=False)
        self.assertEqual(self.state(c)['typed'], '10')
        c.tap(14)                                                           # Backspace
        self.assertEqual(self.state(c)['typed'], '9')
        c.tap(KEY_ESC)
        self.assertEqual(self.state(c)['typed'], '0')
        self.type(c, 'hunter22xy', enter=False)
        c.tap(14)
        c.tap(KEY_ENTER)
        c.sync()
        self.assertEqual(self.state(c)['locked'], '0')

    def test_delay_after_three_wrong_attempts(self):
        p, c, stop = self.start()
        c.cmd('lock set - password hunter22x')
        c.cmd('lock now')
        for _ in range(3):
            self.type(c, 'wrongwrong')
        st = self.state(c)
        self.assertEqual(st['fails'], '3')
        self.assertGreaterEqual(int(st['wait']), 1)
        self.type(c, 'hunter22x')                                           # right, but too early
        self.assertEqual(self.state(c)['locked'], '1')
        # the settings key is held back by the same delay
        with self.assertRaises(RuntimeError):
            c.cmd('lock set hunter22x none -')
        time.sleep(2.2)
        self.type(c, 'hunter22x')
        self.assertEqual(self.state(c)['locked'], '0')

    def test_super_l_and_the_panel_button_lock(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357')
        c.key(KEY_LEFTMETA, True)
        c.tap(38)
        c.key(KEY_LEFTMETA, False)
        c.sync()
        self.assertEqual(self.state(c)['locked'], '1')
        self.type(c, '1357')
        self.assertEqual(self.state(c)['locked'], '0')
        c.click(*self.POWER_BTN)
        c.click(*self.LOCK_DISC)
        c.sync()
        self.assertEqual(self.state(c)['locked'], '1')

    def test_idle_timeout_and_lock_at_start(self):
        path = os.path.join(tmp, 'lock-idle.conf')
        p, c, stop = self.start('lock-idle', file=path)
        c.cmd('lock set - pin 1357 1 1')                                   # 1 "minute" (1 s here), lock at start
        self.assertEqual(self.state(c)['timeout'], '1')
        time.sleep(0.5)
        c.tap(KEY_A)                                                        # activity restarts the idle time
        time.sleep(0.7)
        self.assertEqual(self.state(c)['locked'], '0')
        deadline = time.time() + 5
        while time.time() < deadline and self.state(c)['locked'] != '1':
            time.sleep(0.2)
        self.assertEqual(self.state(c)['locked'], '1', 'idle timeout did not lock')
        stop()
        # a new run with the same file starts locked, and 1357 opens it
        p2, c2, stop2 = self.start('lock-idle2', file=path)
        st = self.state(c2)
        self.assertEqual((st['kind'], st['locked'], st['start'], st['timeout']), ('pin', '1', '1', '1'))
        self.type(c2, '1357')
        self.assertEqual(self.state(c2)['locked'], '0')

    def test_monitors_go_to_standby_while_locked_and_wake_on_input(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357 0 0 2')                                 # standby after 2 "minutes" (2 s here)
        self.assertEqual(self.state(c)['blank'], '2')
        c.cmd('power')
        self.assertEqual(c.last, 'ok on')
        time.sleep(2.5)
        c.cmd('power')
        self.assertEqual(c.last, 'ok on', 'monitors went to standby while the screen was not locked')
        c.cmd('lock now')
        deadline = time.time() + 6
        while time.time() < deadline and self.state(c)['blanked'] != '1':
            time.sleep(0.2)
        self.assertEqual(self.state(c)['blanked'], '1', 'no standby after the timeout')
        c.cmd('power')
        self.assertEqual(c.last, 'ok standby')
        # the first key only wakes the monitors: it is not typed into the PIN
        c.tap(self.CODES['1'])
        c.sync()
        st = self.state(c)
        self.assertEqual((st['blanked'], st['typed'], st['locked']), ('0', '0', '1'))
        c.cmd('power')
        self.assertEqual(c.last, 'ok on')
        self.type(c, '1357')
        self.assertEqual(self.state(c)['locked'], '0')
        # standby again, and unlocking from standby is not possible without waking first; unlock wakes in any case
        c.cmd('lock now')
        deadline = time.time() + 6
        while time.time() < deadline and self.state(c)['blanked'] != '1':
            time.sleep(0.2)
        c.tap(KEY_A)                                                       # wake
        self.type(c, '1357')
        st = self.state(c)
        self.assertEqual((st['locked'], st['blanked']), ('0', '0'))

    def test_standby_can_be_off(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357 0 0 0')
        c.cmd('lock now')
        time.sleep(3.5)
        self.assertEqual(self.state(c)['blanked'], '0')
        self.assertEqual(self.state(c)['blank'], '0')

    def test_console_switch_can_be_turned_off_while_locked(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357')
        st = self.state(c)
        self.assertEqual((st['vt'], st['start']), ('1', '1'))              # defaults: console switch on, lock at start on

        def ctrl_alt_f(n):
            c.key(KEY_LEFTCTRL, True); c.key(KEY_LEFTALT, True)
            c.tap(58 + n)                                                  # KEY_F1 = 59
            c.key(KEY_LEFTALT, False); c.key(KEY_LEFTCTRL, False)
            c.sync()
        c.cmd('lock now')
        ctrl_alt_f(2)
        c.cmd('vt')
        self.assertEqual(c.last, 'ok 2')
        with self.assertRaises(RuntimeError):                              # the lock cannot be changed behind a locked screen
            c.cmd('lock set 1357 keep - - - - 0')
        self.assertEqual(self.state(c)['status'], 'locked')
        self.type(c, '1357')
        c.cmd('lock set 1357 keep - - - - 0')                              # vt = 0
        self.assertEqual(self.state(c)['vt'], '0')
        c.cmd('lock now')
        ctrl_alt_f(5)
        c.cmd('vt')
        self.assertEqual(c.last, 'ok 2', 'the console switched although it is turned off on the lock screen')

    def test_quitting_needs_the_secret(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357')
        # Super+Shift+E, the same as Exit in the session dialog: locks and asks
        c.key(KEY_LEFTMETA, True); c.key(KEY_LEFTSHIFT, True); c.tap(18); c.key(KEY_LEFTSHIFT, False); c.key(KEY_LEFTMETA, False)
        c.sync()
        st = self.state(c)
        self.assertEqual((st['locked'], st['quit']), ('1', '1'))
        self.assertIsNone(p.poll())
        self.assertIn('quit MellaGUI', st['message'])
        self.type(c, '9999')                                               # wrong: still running, still locked
        self.assertIsNone(p.poll())
        self.assertEqual(self.state(c)['locked'], '1')
        c.tap(KEY_ESC)                                                     # clears the field ...
        c.tap(KEY_ESC)                                                     # ... and on an empty one cancels the quit
        c.sync()
        st = self.state(c)
        self.assertEqual((st['locked'], st['quit']), ('1', '0'))
        self.type(c, '1357')                                               # now it is a plain unlock
        self.assertEqual(self.state(c)['locked'], '0')
        self.assertIsNone(p.poll())
        c.cmd('lock quit')
        self.assertEqual(self.state(c)['quit'], '1')
        try:
            self.type(c, '1357')                                           # right secret: MellaGUI quits
        except (ConnectionError, EOFError):
            pass
        self.assertEqual(p.wait(timeout=5), 0)

    def test_quitting_without_a_lock_is_immediate(self):
        p, c, stop = self.start()
        c.key(KEY_LEFTMETA, True)
        c.key(KEY_LEFTSHIFT, True)
        try:
            c.tap(18)                                                      # the compositor quits on this press
        except (ConnectionError, EOFError):
            pass
        self.assertEqual(p.wait(timeout=5), 0)

    def test_spamming_lock_set_does_not_block_the_lock_screen(self):
        p, c, stop = self.start()
        c.cmd('lock set - pin 1357')
        for _ in range(5):                                                 # a program guessing through the settings
            with self.assertRaises(RuntimeError):
                c.cmd('lock set 0000 none -')
        st = self.state(c)
        self.assertEqual(st['fails'], '0')                                 # the lock screen's own counter is untouched
        c.cmd('lock now')
        self.type(c, '1357')
        self.assertEqual(self.state(c)['locked'], '0', 'settings spam delayed the lock screen')

    def test_settings_protocol_lock_set(self):
        p, c, stop = self.start()
        cl = Client(os.path.join(tmp, 'lock'))
        self.clients.append(cl)
        obj = cl.wl.bind('mellagui_settings_v1')
        cl.wl.roundtrip()

        def burst():
            vals = {}
            end = time.time() + 2
            while time.time() < end:
                cl.wl.pump(0.05)
                for e in list(cl.wl.queue):
                    if e.obj != obj:
                        continue
                    cl.wl.queue.remove(e)
                    if e.name == 'value':
                        vals[e.args[0]] = e.args[1]
                    elif e.name == 'done':
                        return vals
            return None
        first = burst()
        self.assertEqual((first['lock-kind'], first['locked'], first['lock-set']), ('none', '0', ''))
        cl.wl.request(obj, 'set', 'lock-set', '\tpassword\thunter22x\t5\t0')
        got = burst()
        self.assertEqual((got['lock-kind'], got['lock-status'], got['lock-timeout'], got['lock-set']), ('password', 'ok', '5', ''))
        cl.wl.request(obj, 'set', 'lock-set', 'nope\tnone\t\t\t')
        self.assertEqual(burst()['lock-status'], 'wrong')
        cl.wl.request(obj, 'set', 'lock-now', '1')
        self.assertEqual(burst()['locked'], '1')
        cl.wl.request(obj, 'set', 'lock-set', 'hunter22x\tnone\t\t\t')      # the settings cannot be used to open the lock,
        got = burst()                                                       # not even with the right secret: no changes behind a locked screen
        self.assertEqual((got['lock-kind'], got['locked'], got['lock-status']), ('password', '1', 'locked'))


class TestAccess(Base):
    """Who may talk to the compositor: the socket is 0600, and with --settings-access own only MellaGUI's programs get the settings."""

    def test_socket_is_private(self):
        mode = os.stat(os.path.join(tmp, 'mellagui-test')).st_mode & 0o777
        self.assertEqual(oct(mode), '0o600')

    def test_settings_stay_closed_to_other_programs(self):
        panel = os.path.join(os.path.dirname(BIN), 'mellagui-settings')
        p, c, stop = self.private_compositor('own', '--settings-access', 'own')
        # a program that is not one of ours (this raw client) binds the interface and gets nothing
        cl = Client(os.path.join(tmp, 'own'))
        self.clients.append(cl)
        obj = cl.wl.bind('mellagui_settings_v1')
        cl.wl.roundtrip()
        cl.wl.pump(0.3)
        self.assertEqual([e.name for e in cl.wl.queue if e.obj == obj], [], 'the settings answered a foreign program')
        cl.wl.request(obj, 'set', 'sound', '0')                             # and its requests do nothing
        cl.wl.roundtrip()
        c.cmd('settings')
        self.assertIn('sound=1', c.last)
        self.assertIsNone(cl.wl.error)
        # MellaGUI's own panel (an executable called mellagui-* next to the compositor) works
        if os.path.exists(panel):
            env = dict(os.environ, WAYLAND_DISPLAY='own')
            r = subprocess.run([panel, '--get', 'sound'], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(r.stdout.strip(), 'sound=1', r.stderr)

    def test_settings_are_open_by_default_on_headless(self):
        p, c, stop = self.private_compositor('open')
        cl = Client(os.path.join(tmp, 'open'))
        self.clients.append(cl)
        obj = cl.wl.bind('mellagui_settings_v1')
        cl.wl.roundtrip()
        self.assertTrue(cl.wl.wait_for(lambda e: e.obj == obj and e.name == 'done', 2.0))

    def test_bad_settings_access_value(self):
        r = subprocess.run([BIN, '--backend', 'headless', '--settings-access', 'everyone'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 2)


class TestFuzz(Base):
    """The coverage-guided fuzzer of the whole compositor (tests/fuzz_server.c): a short run must end without a sanitizer report."""

    def test_short_fuzz_run(self):
        path = os.path.join(ROOT, 'build', 'fuzz-server')
        if not os.path.exists(path):
            self.skipTest('fuzz-server not built (make fuzz)')
        crash = os.path.join(tmp, 'fuzz-current.bin')
        r = subprocess.run([path, '-t', '20', '-s', '1', '-o', crash], capture_output=True, text=True, timeout=120,
                           env=dict(os.environ, XDG_RUNTIME_DIR=tmp))
        self.assertEqual(r.returncode, 0, 'fuzz-server failed; the input that was running is %s\n%s%s' % (crash, r.stdout[-800:], r.stderr[-3000:]))
        self.assertIn('no sanitizer report', r.stdout)


def png_chunk(typ, body):
    import zlib
    return struct.pack('>I', len(body)) + typ + body + struct.pack('>I', zlib.crc32(typ + body))


def make_png(w, h, ctype, depth, pix, palette=None, trns=None, strategy='dynamic', interlace=0):
    """PNG bytes. pix[y][x] = tuple of samples (grey / grey,alpha / r,g,b / r,g,b,a / palette index) in 0..2^depth-1.
    Row filters rotate through all five types; `strategy` picks dynamic / fixed Huffman or stored zlib blocks."""
    import zlib
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, chans * depth // 8)
    rows = []
    for row in pix:
        if depth == 16:
            b = b''.join(struct.pack('>H', v) for px in row for v in px)
        elif depth == 8:
            b = bytes(v for px in row for v in px)
        else:
            bitstr = ''.join(format(px[0], '0%db' % depth) for px in row)
            bitstr += '0' * (-len(bitstr) % 8)
            b = bytes(int(bitstr[i:i + 8], 2) for i in range(0, len(bitstr), 8))
        rows.append(b)
    out = bytearray()
    prev = bytes(len(rows[0]))
    for y, cur in enumerate(rows):
        ft = y % 5
        out.append(ft)
        for i, v in enumerate(cur):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 0:
                pred = 0
            elif ft == 1:
                pred = a
            elif ft == 2:
                pred = b
            elif ft == 3:
                pred = (a + b) // 2
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            out.append((v - pred) & 255)
        prev = cur
    if strategy == 'stored':
        co = zlib.compressobj(0)
    elif strategy == 'fixed':
        co = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_FIXED)
    else:
        co = zlib.compressobj(9)
    z = co.compress(bytes(out)) + co.flush()
    d = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    if palette:
        d += png_chunk(b'PLTE', bytes(v for c in palette for v in c))
    if trns:
        d += png_chunk(b'tRNS', bytes(trns))
    return d + png_chunk(b'IDAT', z) + png_chunk(b'IEND', b'')


def expected_rgb(ctype, depth, px, x, y, palette=None, trns=None):
    """What mellagui-view's decoder must produce (alpha over its grey checkerboard)."""
    mask = (1 << depth) - 1
    hi = (lambda v: v >> 8) if depth == 16 else (lambda v: v)
    a = 255
    if ctype == 0:
        g = hi(px[0]) if depth >= 8 else px[0] * 255 // mask
        r = gg = b = g
    elif ctype == 2:
        r, gg, b = hi(px[0]), hi(px[1]), hi(px[2])
    elif ctype == 3:
        r, gg, b = palette[px[0]]
        a = trns[px[0]] if trns and px[0] < len(trns) else 255
    elif ctype == 4:
        r = gg = b = hi(px[0])
        a = hi(px[1])
    else:
        r, gg, b, a = hi(px[0]), hi(px[1]), hi(px[2]), hi(px[3])
    if a < 255:
        bg = 0x66 if ((x >> 3) ^ (y >> 3)) & 1 else 0x44
        r, gg, b = [(v * a + bg * (255 - a)) // 255 for v in (r, gg, b)]
    return (r, gg, b)


# A 16x16 baseline JPEG (4:4:4, quality 95): red (230,30,30) left half, blue (30,30,230) right half.
JPEG_RED_BLUE = base64.b64decode(
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAQABADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5nr+dz/ZA+a6/6GD/AAvPpSv+ec/3QPmuv+hg/wALz//Z')


class TestImages(unittest.TestCase):
    """mellagui-view's PNG decoder (own inflate) against zlib's output, and the viewer window itself."""
    VIEW = os.environ.get('MELLAGUI_VIEW_BIN', os.path.join(ROOT, 'build', 'mellagui-view'))

    def setUp(self):
        if not os.path.exists(self.VIEW):
            self.skipTest('mellagui-view not built')

    def decode(self, data, name='img.png'):
        d = tempfile.mkdtemp(prefix='mellagui-img-')
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        src, dst = os.path.join(d, name), os.path.join(d, 'out.ppm')
        with open(src, 'wb') as f:
            f.write(data)
        r = subprocess.run([self.VIEW, '--decode', src, dst], capture_output=True, text=True, timeout=20)
        return r, dst

    def test_selftest(self):
        r = subprocess.run([self.VIEW, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_every_colour_type_depth_and_compression(self):
        import random
        rnd = random.Random(7)
        w, h = 37, 23
        pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(16)]
        trns = [rnd.randrange(256) for _ in range(5)]
        cases = [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16), (6, 8), (6, 16)]
        for ctype, depth in cases:
            chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
            top = 1 << depth if ctype != 3 else min(1 << depth, len(pal))
            pix = [[tuple(rnd.randrange(top) for _ in range(chans)) for _ in range(w)] for _ in range(h)]
            palette = pal if ctype == 3 else None
            tr = trns if ctype == 3 else None
            for strategy in ('dynamic', 'fixed', 'stored'):
                with self.subTest(ctype=ctype, depth=depth, strategy=strategy):
                    data = make_png(w, h, ctype, depth, pix, palette, tr, strategy)
                    r, dst = self.decode(data)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    ppm = Ppm(dst)
                    self.assertEqual((ppm.w, ppm.h), (w, h))
                    for y in range(h):
                        for x in range(w):
                            self.assertEqual(ppm.px(x, y), expected_rgb(ctype, depth, pix[y][x], x, y, palette, tr), (x, y))

    def test_large_repetitive_image_uses_long_matches(self):
        w, h = 300, 200
        pix = [[((x // 20) * 10 % 256, (y // 10) * 12 % 256, 99) for x in range(w)] for y in range(h)]
        r, dst = self.decode(make_png(w, h, 2, 8, pix))
        self.assertEqual(r.returncode, 0, r.stderr)
        ppm = Ppm(dst)
        self.assertEqual([ppm.px(x, y) for y in (0, 77, 199) for x in (0, 150, 299)],
                         [pix[y][x] for y in (0, 77, 199) for x in (0, 150, 299)])

    def test_bad_files_are_refused_not_crashed(self):
        good = make_png(8, 8, 2, 8, [[(1, 2, 3)] * 8 for _ in range(8)])
        for name, data in (('interlaced', make_png(8, 8, 2, 8, [[(1, 2, 3)] * 8 for _ in range(8)], interlace=1)),
                           ('truncated', good[:len(good) // 2]),
                           ('flipped', good[:60] + bytes(b ^ 0xff for b in good[60:70]) + good[70:]),
                           ('junk', b'hello world, not an image'),
                           ('empty-idat', good.replace(b'IDAT', b'JUNK'))):
            with self.subTest(name):
                r, _ = self.decode(data)
                self.assertEqual(r.returncode, 1, name)
                self.assertNotIn('Sanitizer', r.stderr)
                self.assertTrue(r.stderr.strip(), 'no message')

    def test_jpeg_baseline(self):
        r, dst = self.decode(JPEG_RED_BLUE, 'x.jpg')
        self.assertEqual(r.returncode, 0, r.stderr)
        ppm = Ppm(dst)
        self.assertEqual((ppm.w, ppm.h), (16, 16))
        for (x, y), want in (((1, 1), (230, 30, 30)), ((14, 14), (30, 30, 230)), ((3, 12), (230, 30, 30))):
            got = ppm.px(x, y)
            self.assertTrue(all(abs(a - b) <= 6 for a, b in zip(got, want)), (x, y, got, want))

    @staticmethod
    def strip_dht(jpg):
        """The same JPEG without its DHT segments: how a camera's MJPEG frames come."""
        out, i = bytearray(jpg[:2]), 2
        while i < len(jpg):
            assert jpg[i] == 0xff, i
            m = jpg[i + 1]
            n = int.from_bytes(jpg[i + 2:i + 4], 'big')
            if m == 0xda:                                                   # the scan: everything else is entropy data
                out += jpg[i:]
                break
            if m != 0xc4:
                out += jpg[i:i + 2 + n]
            i += 2 + n
        return bytes(out)

    def test_jpeg_without_huffman_tables_uses_the_standard_ones(self):
        """UVC cameras leave the DHT segment out (ITU T.81 Annex K tables are meant): colour and grey, 4:4:4 / 4:2:0 / 4:2:2."""
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        import math
        im = Image.new('RGB', (64, 48))
        for x in range(64):
            for y in range(48):
                im.putpixel((x, y), (int(127 + 120 * math.sin(x / 9.0 + y / 17.0)), int(127 + 120 * math.sin(y / 7.0)),
                                     int(127 + 120 * math.cos((x + y) / 13.0))))
        for name, img, kw in (('444', im, dict(subsampling=0, quality=90)), ('420', im, dict(subsampling=2, quality=85)),
                              ('422', im, dict(subsampling=1, quality=85)), ('grey', im.convert('L'), dict(quality=88))):
            with self.subTest(name):
                buf = io.BytesIO()
                img.save(buf, 'JPEG', **kw)
                bare = self.strip_dht(buf.getvalue())
                self.assertNotIn(b'\xff\xc4', bare[:bare.index(b'\xff\xda')])
                self.assertLess(len(bare), len(buf.getvalue()))
                r, dst = self.decode(bare, name + '.jpg')
                self.assertEqual(r.returncode, 0, r.stderr)
                ref = Image.open(io.BytesIO(buf.getvalue())).convert('RGB')
                diffs = [abs(a - b) for a, b in zip(open(dst, 'rb').read().split(b'\n', 3)[3], ref.tobytes())]
                self.assertLessEqual(sum(diffs) / len(diffs), 3.0)
                self.assertLessEqual(max(diffs), 60)

    def test_jpeg_without_tables_and_progressive_is_refused(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        buf = io.BytesIO()
        Image.new('RGB', (16, 16), (9, 99, 199)).save(buf, 'JPEG', progressive=True)
        r, _ = self.decode(self.strip_dht(buf.getvalue()), 'p.jpg')
        self.assertEqual(r.returncode, 1)
        self.assertIn('Huffman', r.stderr)

    def test_jpeg_against_pillow(self):
        """Baseline, progressive, subsampled, grey, with restart markers and EXIF orientation: the same pixels as Pillow gives."""
        try:
            from PIL import Image, ImageOps
        except ImportError:
            self.skipTest('Pillow not installed')
        import math
        im = Image.new('RGB', (75, 41))
        for x in range(75):
            for y in range(41):
                im.putpixel((x, y), (int(127 + 120 * math.sin(x / 9.0 + y / 17.0)), int(127 + 120 * math.sin(y / 7.0)),
                                     int(127 + 120 * math.cos((x + y) / 13.0))))
        cases = (('444', dict(subsampling=0, quality=92), 0.7, 5), ('420', dict(subsampling=2, quality=85), 3.0, 60),
                 ('422', dict(subsampling=1, quality=85), 3.0, 60), ('prog420', dict(subsampling=2, quality=85, progressive=True), 3.0, 60),
                 ('prog444', dict(subsampling=0, quality=92, progressive=True), 0.7, 5), ('restart', dict(subsampling=2, quality=80, restart_marker_blocks=3), 3.0, 60),
                 ('optimized', dict(subsampling=0, quality=75, optimize=True), 0.7, 5))
        for name, kw, mean_limit, max_limit in cases:
            with self.subTest(name):
                buf = io.BytesIO()
                im.save(buf, 'JPEG', **kw)
                r, dst = self.decode(buf.getvalue(), name + '.jpg')
                self.assertEqual(r.returncode, 0, r.stderr)
                ppm = Ppm(dst)
                ref = Image.open(io.BytesIO(buf.getvalue())).convert('RGB')
                self.assertEqual((ppm.w, ppm.h), ref.size)
                diffs = [abs(a - b) for a, b in zip(open(dst, 'rb').read().split(b'\n', 3)[3], ref.tobytes())]
                self.assertLessEqual(sum(diffs) / len(diffs), mean_limit)
                self.assertLessEqual(max(diffs), max_limit)
        grey = im.convert('L')
        for kw in (dict(quality=88), dict(quality=88, progressive=True)):
            buf = io.BytesIO()
            grey.save(buf, 'JPEG', **kw)
            r, dst = self.decode(buf.getvalue(), 'grey.jpg')
            self.assertEqual(r.returncode, 0, r.stderr)
            ref = Image.open(io.BytesIO(buf.getvalue())).convert('RGB')
            diffs = [abs(a - b) for a, b in zip(open(dst, 'rb').read().split(b'\n', 3)[3], ref.tobytes())]
            self.assertLessEqual(max(diffs), 3)
        small = Image.new('RGB', (40, 20))
        for x in range(40):
            for y in range(20):
                small.putpixel((x, y), (x * 6, y * 12, 128))
        for o in range(1, 9):
            with self.subTest(orientation=o):
                ex = Image.Exif()
                ex[0x0112] = o
                buf = io.BytesIO()
                small.save(buf, 'JPEG', quality=95, subsampling=0, exif=ex)
                r, dst = self.decode(buf.getvalue(), 'o%d.jpg' % o)
                self.assertEqual(r.returncode, 0, r.stderr)
                ref = ImageOps.exif_transpose(Image.open(io.BytesIO(buf.getvalue()))).convert('RGB')
                ppm = Ppm(dst)
                self.assertEqual((ppm.w, ppm.h), ref.size)
                diffs = [abs(a - b) for a, b in zip(open(dst, 'rb').read().split(b'\n', 3)[3], ref.tobytes())]
                self.assertLessEqual(max(diffs), 6)

    def test_damaged_jpegs_are_refused_not_crashed(self):
        good = JPEG_RED_BLUE
        rnd = random.Random(3)
        cases = [('junk-after-soi', b'\xff\xd8\xff\xe0\x00\x02'), ('truncated-half', good[:len(good) // 2]), ('truncated-header', good[:30]),
                 ('no-scan', good[:good.index(b'\xff\xda')]), ('cmyk-like', good.replace(b'\x03\x01\x11\x00', b'\x04\x01\x11\x00', 1))]
        for i in range(40):
            d = bytearray(good)
            for _ in range(rnd.randrange(1, 5)):
                d[rnd.randrange(len(d))] = rnd.randrange(256)
            cases.append(('flipped%d' % i, bytes(d)))
        for name, data in cases:
            with self.subTest(name):
                r, _ = self.decode(data, 'bad.jpg')
                self.assertIn(r.returncode, (0, 1), name)                 # an error or (for harmless flips) a picture, never a crash
                self.assertNotIn('Sanitizer', r.stderr)
                if r.returncode:
                    self.assertTrue(r.stderr.strip(), 'no message')

    def test_ppm(self):
        data = b'P6\n# comment\n3 2\n255\n' + bytes(range(18))
        r, dst = self.decode(data, 'x.ppm')
        self.assertEqual(r.returncode, 0, r.stderr)
        ppm = Ppm(dst)
        self.assertEqual((ppm.w, ppm.h, ppm.px(2, 1)), (3, 2, (15, 16, 17)))


class TestCamera(Base):
    """mellagui-cam: command line and window, with MELLAGUI_CAM_FAKE=1 (a moving test pattern) instead of a real webcam."""
    CAM = os.environ.get('MELLAGUI_CAM_BIN', os.path.join(ROOT, 'build', 'mellagui-cam'))

    def setUp(self):
        if not os.path.exists(self.CAM):
            self.skipTest('mellagui-cam not built')
        super().setUp()

    def run_cam(self, *args, **env):
        return subprocess.run([self.CAM] + list(args), env=dict(os.environ, MELLAGUI_CAM_FAKE='1', **env), capture_output=True, text=True, timeout=20)

    @staticmethod
    def png_size(path):
        with open(path, 'rb') as f:
            d = f.read(24)
        assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
        return int.from_bytes(d[16:20], 'big'), int.from_bytes(d[20:24], 'big')

    def start(self, **env):
        d = tempfile.mkdtemp(prefix='mellagui-cam-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        p = subprocess.Popen([self.CAM, *env.pop('args', [])], env=dict(os.environ, MELLAGUI_CAM_FAKE='1', MELLAGUI_CAM_DIR=d, **env),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.cam')
        self.assertIsNotNone(win, 'the camera window did not appear')
        for _ in range(100):                                        # it is listed before it is mapped (size 0)
            win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.cam'][0]
            if win['w'] > 0:
                break
            time.sleep(0.03)
        ctl.abs(win['x'] + 100, win['y'] + 100)                     # the pointer inside: the keyboard goes here
        time.sleep(0.4)
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.cam'][0]     # the size is settled now (the first one was a guess)
        return p, win, d

    def shot(self, name):
        return ctl.dump(os.path.join(tmp, name))

    def test_selftest(self):
        r = self.run_cam('--selftest')
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_list_and_bad_arguments(self):
        r = self.run_cam('--list')
        self.assertEqual(r.stdout.strip(), 'fake|Test pattern|MJPG|320x240,640x480')
        self.assertEqual(self.run_cam('--size', 'big').returncode, 2)
        self.assertEqual(self.run_cam('--bogus').returncode, 2)

    def test_snapshot_without_a_window(self):
        d = tempfile.mkdtemp(prefix='mellagui-cam-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        out = os.path.join(d, 'a.png')
        self.assertEqual(self.run_cam('--snapshot', out, '--size', '320x240').returncode, 0)
        self.assertEqual(self.png_size(out), (320, 240))
        self.assertEqual(self.run_cam('--snapshot', out).returncode, 0)                  # default: 640x480 (replaces the file)
        self.assertEqual(self.png_size(out), (640, 480))
        r = self.run_cam('--snapshot', os.path.join(d, 'no', 'such', 'dir.png'))
        self.assertEqual(r.returncode, 1)
        self.assertIn('cannot write', r.stderr)

    def test_window_shows_a_moving_mirrored_picture_and_pauses(self):
        p, win, d = self.start()
        x0, y0 = win['x'], win['y']
        a = self.shot('cam_a.ppm')
        self.assertEqual(a.px(x0 + 30, y0 + 8), (0, 0, 192))                 # mirrored: the last bar (blue) is on the left
        box = lambda ppm: ppm.count((255, 255, 255), x0, y0, x0 + 680, y0 + 400)
        self.assertGreater(box(a), 1000)                                      # the white square
        time.sleep(0.5)
        b = self.shot('cam_b.ppm')
        self.assertNotEqual(a.data, b.data, 'the picture does not move')
        ctl.tap(50)                                                           # M: no mirror, silver on the left
        time.sleep(0.4)
        self.assertEqual(self.shot("cam_c.ppm").px(x0 + 30, y0 + 8), (192, 192, 192))
        ctl.tap(25)                                                           # P: pause
        time.sleep(0.4)
        c1 = self.shot('cam_d.ppm')
        time.sleep(0.6)
        c2 = self.shot('cam_e.ppm')
        self.assertEqual(c1.data, c2.data, 'a paused picture moves')
        ctl.tap(25)
        time.sleep(0.6)
        self.assertNotEqual(self.shot('cam_f.ppm').data, c2.data, 'does not go on after the pause')
        ctl.tap(16)                                                           # Q
        for _ in range(60):
            if p.poll() is not None:
                break
            time.sleep(0.05)
        self.assertIsNotNone(p.poll(), 'Q did not close the camera window')
        self.assertEqual(p.returncode, 0)

    def test_snapshot_by_key_and_by_button_and_size_change(self):
        p, win, d = self.start()
        ctl.tap(57)                                                           # Space
        time.sleep(0.5)
        files = sorted(os.listdir(d))
        self.assertEqual(len(files), 1)
        self.assertRegex(files[0], r'^camera-\d{8}-\d{6}\.png$')
        self.assertEqual(self.png_size(os.path.join(d, files[0])), (640, 480))
        if win['x'] + 30 > 150:                                               # the dock floats over the bottom of the screen from x ~ 197 on and
            ctl.abs(win['x'] + 200, win['y'] - 12)                            # (the window's place depends on the tests before) would take the click:
            ctl.button(BTN_LEFT, True)                                        # drag the window to the left edge first
            ctl.abs(win['x'] + 200 - (win['x'] - 40), win['y'] - 12)
            ctl.button(BTN_LEFT, False)
            time.sleep(0.2)
            win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.cam'][0]
            self.assertLess(win['x'], 100)
        ctl.click(win['x'] + 30, win['y'] + win['h'] - 22)                    # the Snapshot button (left end of the bar)
        time.sleep(0.5)
        files = sorted(os.listdir(d))
        self.assertEqual(len(files), 2, files)                                # the same second: "-1" is added, nothing is overwritten
        ctl.tap(31)                                                           # S: the next size
        time.sleep(0.6)
        ctl.tap(28)                                                           # Enter
        time.sleep(0.5)
        sizes = sorted(self.png_size(os.path.join(d, f)) for f in os.listdir(d))
        self.assertEqual(sizes[0], (320, 240), sizes)                         # 640x480 -> next in the list wraps to 320x240
        self.assertEqual(len(sizes), 3)
        self.assertTrue(alive_p(p))

    def test_no_camera_keeps_the_window_open_with_a_message(self):
        d = tempfile.mkdtemp(prefix='mellagui-cam-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        env = {k: v for k, v in os.environ.items() if k != 'MELLAGUI_CAM_FAKE'}
        p = subprocess.Popen([self.CAM, '--device', '/dev/video-not-here'], env=dict(env, MELLAGUI_CAM_DIR=d),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.cam')
        self.assertIsNotNone(win)
        time.sleep(0.5)
        self.assertTrue(alive_p(p))
        ctl.abs(win['x'] + 100, win['y'] + 100)
        ctl.tap(57)                                                           # a snapshot without a picture is refused politely
        time.sleep(0.3)
        self.assertEqual(os.listdir(d), [])
        self.assertTrue(alive_p(p))

    def test_dock_has_a_camera_button_after_images(self):
        names = [l.split('"')[1] for l in ctl.cmd('dock') if l.startswith('icon ')]
        self.assertEqual(names[:7], ['Launcher', 'Terminal', 'Files', 'Editor', 'Calculator', 'Images', 'Camera'])


class TestRecord(Base):
    """The screen recorder: mellagui_capture_v1 (src/capture.c) and mellagui-record (client/mellagui-record.c, src/jpegenc.c, src/mp4.c)."""
    REC = os.path.join(os.path.dirname(BIN), 'mellagui-record')
    LOCK_ENV = {'MELLAGUI_LOCK_ITER': '1000', 'MELLAGUI_LOCK_DELAY_MS': '1500', 'MELLAGUI_LOCK_UNIT_MS': '1000'}

    def setUp(self):
        if not os.path.exists(self.REC):
            self.skipTest('mellagui-record not built')
        super().setUp()

    def outdir(self):
        d = tempfile.mkdtemp(prefix='mellagui-rec-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        return d

    def run_rec(self, *args, display=None, timeout=30):
        env = dict(os.environ)
        if display:
            env['WAYLAND_DISPLAY'] = display
        return subprocess.run([self.REC] + list(args), env=env, capture_output=True, text=True, timeout=timeout)

    @staticmethod
    def parse_mp4(path):
        """The top level boxes and the sample table of the file: {'w', 'h', 'duration_ms', 'samples': [bytes, ...]}."""
        with open(path, 'rb') as f:
            d = f.read()
        at, boxes = 0, {}
        while at + 8 <= len(d):
            size = int.from_bytes(d[at:at + 4], 'big')
            hdr = 8
            if size == 1:
                size, hdr = int.from_bytes(d[at + 8:at + 16], 'big'), 16
            assert size >= hdr and at + size <= len(d), 'bad box at %d' % at
            boxes[d[at + 4:at + 8]] = (at + hdr, at + size)
            at += size
        assert at == len(d), 'junk after the last box'
        assert set(boxes) == {b'ftyp', b'mdat', b'moov'}, boxes.keys()
        a, b = boxes[b'moov']
        moov = d[a:b]

        def find(tag, start=0):
            i = moov.index(tag, start)
            return i + 4
        i = find(b'mvhd')
        timescale, dur = int.from_bytes(moov[i + 12:i + 16], 'big'), int.from_bytes(moov[i + 16:i + 20], 'big')
        assert timescale == 1000
        i = find(b'stsz')
        count = int.from_bytes(moov[i + 8:i + 12], 'big')
        sizes = [int.from_bytes(moov[i + 12 + 4 * k:i + 16 + 4 * k], 'big') for k in range(count)]
        i = find(b'stco')
        off = int.from_bytes(moov[i + 8:i + 12], 'big')
        i = find(b'tkhd')
        w, h = int.from_bytes(moov[i + 76:i + 80], 'big') >> 16, int.from_bytes(moov[i + 80:i + 84], 'big') >> 16
        samples = []
        for sz in sizes:
            samples.append(d[off:off + sz])
            off += sz
        assert off <= boxes[b'mdat'][1], 'samples run out of the mdat box'
        return dict(w=w, h=h, duration_ms=dur, samples=samples)

    def test_the_pointer_is_in_the_recording(self):
        view = os.path.join(os.path.dirname(BIN), 'mellagui-view')
        if not os.path.exists(view):
            self.skipTest('mellagui-view not built')
        p, c, stop = self.private_compositor('mellagui-rec-cur', '--config', 'none')
        c.sync()
        d = self.outdir()
        c.abs(300, 250)                                           # over the bare wallpaper
        r = self.run_rec('--start', '--duration', '1', '--codec', 'mjpeg', '--dir', d, '--name', 'cur.mp4', display='mellagui-rec-cur')
        self.assertEqual(r.returncode, 0, r.stderr)
        path = os.path.join(d, 'cur.mp4')
        out = os.path.join(d, 'cur.ppm')
        self.assertEqual(subprocess.run([view, '--decode', path, out, '0'], capture_output=True, timeout=20).returncode, 0)
        ppm = Ppm(out)
        white = sum(1 for y in range(250, 272) for x in range(300, 316) if all(v > 200 for v in ppm.px(x, y)))
        self.assertGreater(white, 15, 'the pointer is not in the picture')
        far = sum(1 for y in range(100, 122) for x in range(100, 116) if all(v > 200 for v in ppm.px(x, y)))
        self.assertEqual(far, 0, 'something white where the pointer is not')

    def test_selftest(self):
        r = self.run_rec('--selftest')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('ok', r.stdout)

    def test_list_and_bad_arguments(self):
        r = self.run_rec('--list')
        self.assertEqual(r.stdout.strip(), '0 headless-0 1024x600+0+0', r.stderr)
        self.assertEqual(self.run_rec('--bogus').returncode, 2)
        r = self.run_rec('--start', '--output', '5', '--dir', self.outdir())
        self.assertEqual(r.returncode, 1)
        self.assertIn("no such output", r.stderr)

    def test_the_protocol(self):
        cl = Client()
        self.clients.append(cl)
        wl = cl.wl
        cap = wl.bind('mellagui_capture_v1')
        wl.request(cap, 'list')
        wl.roundtrip()
        outs = [e.args for e in wl.queue if e.obj == cap and e.name == 'output']
        self.assertEqual(outs, [[0, 'headless-0', 0, 0, 1024, 600]])
        wl.queue.clear()
        win_client, win = self.window(300, 200, color=0xffff00ff)           # something unmistakable on the screen
        ctl.sync()
        shm = Shm(wl, 1024 * 600 * 4)
        buf = shm.buffer(1024, 600, argb=0xff010203)
        wl.request(cap, 'grab', I(0), buf)
        ev = wl.wait_for(lambda e: e.obj == cap and e.name in ('ready', 'failed'), 5.0)
        self.assertIsNotNone(ev)
        self.assertEqual((ev.name, ev.args), ('ready', [0, 1024, 600]))
        # the buffer holds what dump shows (away from the pointer, which a capture leaves out)
        ppm = ctl.dump(os.path.join(tmp, 'rec-protocol.ppm'))
        same = total = magenta = 0
        for y in range(0, 600, 13):
            for x in range(0, 1024, 11):
                if abs(x - 900) < 20 and abs(y - 500) < 30:
                    continue
                v = struct.unpack_from('<I', shm.map, (y * 1024 + x) * 4)[0]
                total += 1
                same += ((v >> 16) & 255, (v >> 8) & 255, v & 255) == ppm.px(x, y)
                magenta += v == 0xffff00ff
        self.assertGreater(same, total * 0.97, '%d of %d samples equal' % (same, total))
        self.assertGreater(magenta, 0, 'the magenta window is not in the picture')
        # mistakes are answered, not punished
        small = shm.buffer(100, 100)
        for args, why in (((I(0), small), 'size'), ((I(3), buf), 'no-output'), ((I(-1), buf), 'no-output')):
            wl.request(cap, 'grab', *args)
            ev = wl.wait_for(lambda e: e.obj == cap and e.name in ('ready', 'failed'), 5.0)
            self.assertEqual((ev.name, ev.args[1]), ('failed', why))
        self.assertIsNone(wl.error)
        self.assertTrue(alive())

    def test_a_recording(self):
        d = self.outdir()
        win_client = self.client()
        r = subprocess.Popen([self.REC, '--start', '--duration', '2.5', '--dir', d, '--fps', '15', '--codec', 'mjpeg'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (r.poll() is None) and (r.kill(), r.wait()))
        time.sleep(0.9)
        self.assertEqual(ctl.cmd('tray')[0], 'tray n=1', 'the bar has no item while recording')
        win = Window(win_client, 300, 200, color=0xffff00ff)                # appears while recording
        win_client.wl.roundtrip()
        out, err = r.communicate(timeout=20)
        self.assertEqual(r.returncode, 0, err)
        self.assertEqual(ctl.cmd('tray')[0], 'tray n=0', 'the item is still in the bar after the recording')
        files = [f for f in os.listdir(d) if f.endswith('.mp4')]
        self.assertEqual(len(files), 1, files)
        path = os.path.join(d, files[0])
        self.assertEqual(out.strip(), path)
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        mp4 = self.parse_mp4(path)
        self.assertEqual((mp4['w'], mp4['h']), (1024, 600))
        self.assertTrue(2300 <= mp4['duration_ms'] <= 3500, mp4['duration_ms'])
        self.assertGreaterEqual(len(mp4['samples']), 2)
        self.assertLess(len(mp4['samples']), 30, 'identical pictures are not dropped')
        for s in mp4['samples']:
            self.assertEqual((s[:2], s[-2:]), (b'\xff\xd8', b'\xff\xd9'))
        self.assertIn('unchanged skipped', err)
        try:
            from PIL import Image
        except ImportError:
            return
        first = Image.open(io.BytesIO(mp4['samples'][0])).convert('RGB')
        last = Image.open(io.BytesIO(mp4['samples'][-1])).convert('RGB')
        self.assertEqual(first.size, (1024, 600))
        w = [x for x in ctl.windows()['windows'] if x['title'] == 'test'][0]
        cx, cy = w['x'] + w['w'] // 2, w['y'] + w['h'] // 2

        def magenta(im):
            p = im.getpixel((cx, cy))
            return p[0] > 220 and p[1] < 40 and p[2] > 220
        self.assertFalse(magenta(first), 'the window is in the first picture')
        self.assertTrue(magenta(last), 'the window is not in the last picture: %r' % (last.getpixel((cx, cy)),))
        ffprobe = shutil.which('ffprobe')
        if ffprobe:
            r2 = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'stream=codec_name,width,height', '-of', 'csv=p=0', path],
                                capture_output=True, text=True, timeout=20)
            self.assertEqual(r2.stdout.strip(), 'mjpeg,1024,600', r2.stderr)

    def test_sigterm_finishes_the_file(self):
        d = self.outdir()
        r = subprocess.Popen([self.REC, '--start', '--dir', d, '--codec', 'mjpeg'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (r.poll() is None) and (r.kill(), r.wait()))
        for _ in range(100):
            if any(f.endswith('.mp4') for f in os.listdir(d)):
                break
            time.sleep(0.05)
        time.sleep(0.7)
        r.send_signal(signal.SIGTERM)
        out, err = r.communicate(timeout=20)
        self.assertEqual(r.returncode, 0, err)
        mp4 = self.parse_mp4(out.strip())
        self.assertGreaterEqual(len(mp4['samples']), 1)

    def test_names_are_never_reused(self):
        d = self.outdir()
        args = ('--start', '--duration', '0.4', '--dir', d, '--name', 'one.mp4', '--codec', 'mjpeg')
        self.assertEqual(self.run_rec(*args).returncode, 0)
        before = os.stat(os.path.join(d, 'one.mp4')).st_size
        r = self.run_rec(*args)
        self.assertEqual(r.returncode, 1)
        self.assertIn('cannot write', r.stderr)
        self.assertEqual(os.stat(os.path.join(d, 'one.mp4')).st_size, before)
        r = self.run_rec('--start', '--duration', '0.2', '--dir', os.path.join(d, 'no', 'such'), '--name', 'x.mp4')
        self.assertEqual(r.returncode, 1)                                    # only the last directory is created
        self.assertIn('cannot create', r.stderr)

    def test_the_window(self):
        """Space starts, the window goes out of the way and the bar's item shows the recording; a click on the item ends it."""
        d = self.outdir()
        p = subprocess.Popen([self.REC, '--dir', d, '--codec', 'mjpeg'], env=dict(os.environ), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.record')
        self.assertIsNotNone(win, 'the recorder window did not appear')
        for _ in range(100):
            win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.record'][0]
            if win['w'] > 0:
                break
            time.sleep(0.03)
        ctl.abs(win['x'] + 100, win['y'] + 100)                              # the pointer inside: the keyboard goes here
        time.sleep(0.3)
        self.assertEqual(ctl.cmd('tray')[0], 'tray n=0')
        ctl.tap(KEY_SPACE)                                                   # record
        item = None
        for _ in range(100):
            lines = ctl.cmd('tray')
            if lines[0] == 'tray n=1':
                item = lines[1]
                break
            time.sleep(0.05)
        self.assertIsNotNone(item, 'Space did not start a recording')
        for _ in range(100):
            win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.record'][0]
            if 'minimized' in win['flags']:
                break
            time.sleep(0.05)
        self.assertIn('minimized', win['flags'], 'the window stays in the way of the recording')
        time.sleep(0.8)
        m = re.match(r'item (\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) icon=(\d+)x(\d+) text=(\d\d:\d\d)$', ctl.cmd('tray')[1])
        self.assertIsNotNone(m, ctl.cmd('tray'))
        x, y, w, h = (int(m[i]) for i in (2, 3, 4, 5))
        ctl.click(x + w // 2, y + h // 2)                                    # the item stops it, and with it the program (nobody sees the window)
        self.assertEqual(p.wait(timeout=15), 0)
        files = [f for f in os.listdir(d) if f.endswith('.mp4')]
        self.assertEqual(len(files), 1)
        self.assertGreaterEqual(len(self.parse_mp4(os.path.join(d, files[0]))['samples']), 1)
        self.assertEqual(ctl.cmd('tray')[0], 'tray n=0')

    VIEW = os.path.join(os.path.dirname(BIN), 'mellagui-view')

    def view_info(self, path):
        r = subprocess.run([self.VIEW, '--video-info', path], capture_output=True, text=True, timeout=20)
        return r.stdout.strip()

    def view_frame(self, path, n):
        """Picture n of a recording as a PIL image, decoded by the viewer's own decoders."""
        from PIL import Image
        out = path + '.%d.ppm' % n
        r = subprocess.run([self.VIEW, '--decode', path, out, str(n)], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return Image.open(out).convert('RGB')

    def test_our_own_h264(self):
        """--codec h264: no ffmpeg involved; the file is H.264 (constrained baseline) that ffmpeg decodes, and the viewer decodes it by itself."""
        if not os.path.exists(self.VIEW):
            self.skipTest('mellagui-view not built')
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        d = self.outdir()
        win_client = self.client()
        r = subprocess.Popen([self.REC, '--start', '--duration', '3', '--dir', d, '--codec', 'h264', '--name', 'own.mp4'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=dict(os.environ, MELLAGUI_FFMPEG='none'))
        self.addCleanup(lambda: (r.poll() is None) and (r.kill(), r.wait()))
        time.sleep(1.2)
        win = Window(win_client, 300, 200, color=0xffff00ff)                # appears while recording
        win_client.wl.roundtrip()
        out, err = r.communicate(timeout=30)
        self.assertEqual(r.returncode, 0, err)
        self.assertIn('H.264, built in', err)
        path = os.path.join(d, 'own.mp4')
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        info = self.view_info(path)
        self.assertRegex(info, r'^1024x600 codec=avc1 mjpeg=0 pictures=\d+ duration_ms=[23]\d\d\d$')
        n = int(re.search(r'pictures=(\d+)', info)[1])
        self.assertGreaterEqual(n, 2)
        w = [x for x in ctl.windows()['windows'] if x['title'] == 'test'][0]
        cx, cy = w['x'] + w['w'] // 2, w['y'] + w['h'] // 2

        def magenta(p):
            return p[0] > 200 and p[1] < 60 and p[2] > 200
        self.assertFalse(magenta(self.view_frame(path, 0).getpixel((cx, cy))), 'the window is in the first picture')
        last = self.view_frame(path, n - 1)
        self.assertTrue(magenta(last.getpixel((cx, cy))), 'the window is not in the last picture: %r' % (last.getpixel((cx, cy)),))
        # an independent decoder agrees (ffmpeg, when it is there)
        ffmpeg = shutil.which('ffmpeg')
        if ffmpeg:
            r2 = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=codec_name,profile,pix_fmt,width,height',
                                 '-of', 'csv=p=0', path], capture_output=True, text=True, timeout=20)
            self.assertEqual(r2.stdout.strip(), 'h264,Constrained Baseline,1024,600,yuv420p', r2.stderr)
            png = os.path.join(d, 'ff.png')
            subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', path, '-vf', 'select=eq(n\\,%d)' % (n - 1), '-frames:v', '1', png], check=True, timeout=30)
            ref = Image.open(png).convert('RGB')
            diff = sum(abs(a - b) for x in range(0, 1024, 7) for y in range(0, 600, 5)
                       for a, b in zip(ref.getpixel((x, y)), last.getpixel((x, y)))) / (len(range(0, 1024, 7)) * len(range(0, 600, 5)) * 3)
            self.assertLess(diff, 3.0, 'our decoder and ffmpeg disagree about the last picture')

    def test_our_own_h264_with_an_odd_sized_screen(self):
        if not os.path.exists(self.VIEW):
            self.skipTest('mellagui-view not built')
        p, c, stop = self.private_compositor('rec-odd', '--size', '1001x601')
        d = self.outdir()
        r = self.run_rec('--start', '--duration', '0.8', '--dir', d, '--codec', 'h264', '--name', 'odd.mp4', display='rec-odd')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(self.view_info(os.path.join(d, 'odd.mp4')), r'^1000x600 codec=avc1 ')            # 4:2:0 wants even sides
        if shutil.which('ffprobe'):
            self.assertEqual(self.probe(os.path.join(d, 'odd.mp4'))['width'], '1000')

    @staticmethod
    def have_x264():
        ff = shutil.which('ffmpeg')
        if not ff:
            return False
        r = subprocess.run([ff, '-hide_banner', '-encoders'], capture_output=True, text=True, timeout=20)
        return 'libx264' in r.stdout

    def probe(self, path):
        r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=codec_name,pix_fmt,width,height',
                            '-show_entries', 'format=duration', '-of', 'default=nw=1', path], capture_output=True, text=True, timeout=20)
        return dict(l.split('=', 1) for l in r.stdout.split())

    def test_h264_through_ffmpeg(self):
        if not self.have_x264():
            self.skipTest('ffmpeg with libx264 not installed')
        d = self.outdir()
        win_client = self.client()
        r = subprocess.Popen([self.REC, '--start', '--duration', '3', '--dir', d, '--codec', 'ffmpeg'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (r.poll() is None) and (r.kill(), r.wait()))
        time.sleep(1.2)
        win = Window(win_client, 300, 200, color=0xffff00ff)                # appears while recording
        win_client.wl.roundtrip()
        out, err = r.communicate(timeout=30)
        self.assertEqual(r.returncode, 0, err)
        self.assertIn('H.264 through ffmpeg', err)
        files = [f for f in os.listdir(d) if f.endswith('.mp4')]
        self.assertEqual(len(files), 1, files)
        path = os.path.join(d, files[0])
        self.assertEqual(out.strip(), path)
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        info = self.probe(path)
        self.assertEqual((info['codec_name'], info['pix_fmt'], info['width'], info['height']), ('h264', 'yuv420p', '1024', '600'))
        self.assertTrue(2.2 <= float(info['duration']) <= 4.0, info)
        try:
            from PIL import Image
        except ImportError:
            return
        png = os.path.join(d, 'last.png')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-sseof', '-0.3', '-i', path, '-frames:v', '1', png], check=True, timeout=30)
        w = [x for x in ctl.windows()['windows'] if x['title'] == 'test'][0]
        p = Image.open(png).convert('RGB').getpixel((w['x'] + w['w'] // 2, w['y'] + w['h'] // 2))
        self.assertTrue(p[0] > 200 and p[1] < 60 and p[2] > 200, 'the window is not in the last picture: %r' % (p,))

    def test_codec_choice_without_ffmpeg(self):
        d = self.outdir()
        env = dict(os.environ, MELLAGUI_FFMPEG='none')
        r = subprocess.run([self.REC, '--start', '--duration', '0.3', '--dir', d, '--codec', 'ffmpeg'], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(r.returncode, 1)
        self.assertIn('not installed', r.stderr)
        self.assertEqual([f for f in os.listdir(d)], [], 'a file was left behind')
        r = subprocess.run([self.REC, '--start', '--duration', '0.4', '--dir', d], env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(r.returncode, 0, r.stderr)                          # auto falls back on our own H.264
        self.assertIn('H.264, built in', r.stderr)
        self.assertEqual(self.run_rec('--codec', 'x264?').returncode, 2)

    def test_closed_to_other_programs(self):
        p, c, stop = self.private_compositor('rec-own', '--settings-access', 'own')
        cl = Client(os.path.join(tmp, 'rec-own'))
        self.clients.append(cl)
        cap = cl.wl.bind('mellagui_capture_v1')
        cl.wl.request(cap, 'list')
        cl.wl.roundtrip()
        self.assertEqual([e.name for e in cl.wl.queue if e.obj == cap], ['done'], 'a foreign program was told about the outputs')
        cl.wl.queue.clear()
        shm = Shm(cl.wl, 800 * 500 * 4)
        buf = shm.buffer(800, 500, argb=0xff010203)
        cl.wl.request(cap, 'grab', I(0), buf)
        ev = cl.wl.wait_for(lambda e: e.obj == cap and e.name in ('ready', 'failed'), 3.0)
        self.assertEqual((ev.name, ev.args[1]), ('failed', 'no-output'))
        self.assertEqual(struct.unpack_from('<I', shm.map, 0)[0], 0xff010203, 'a foreign program got the screen')
        # MellaGUI's own recorder (an executable called mellagui-* next to the compositor) is served
        r = self.run_rec('--list', display='rec-own')
        self.assertEqual(r.stdout.strip(), '0 headless-0 800x500+0+0', r.stderr)

    def test_a_locked_screen_is_not_captured(self):
        path = os.path.join(tmp, 'rec-lock.conf')
        p, c, stop = self.private_compositor('rec-lock', '--lock-file', path, env=self.LOCK_ENV)
        c.cmd('lock set - password hunter22x')
        cl = Client(os.path.join(tmp, 'rec-lock'))
        self.clients.append(cl)
        cap = cl.wl.bind('mellagui_capture_v1')
        shm = Shm(cl.wl, 800 * 500 * 4)
        buf = shm.buffer(800, 500, argb=0xff010203)
        cl.wl.request(cap, 'grab', I(0), buf)
        ev = cl.wl.wait_for(lambda e: e.obj == cap and e.name in ('ready', 'failed'), 3.0)
        self.assertEqual(ev.name, 'ready')
        c.cmd('lock now')
        shm.map[0:4] = struct.pack('<I', 0xff010203)
        cl.wl.request(cap, 'grab', I(0), buf)
        ev = cl.wl.wait_for(lambda e: e.obj == cap and e.name in ('ready', 'failed'), 3.0)
        self.assertEqual((ev.name, ev.args[1]), ('failed', 'locked'))
        self.assertEqual(struct.unpack_from('<I', shm.map, 0)[0], 0xff010203)


class TestTray(Base):
    """The panel's tray (src/tray.c): programs put icons / labels into the top bar through mellagui_tray_v1."""

    def item(self, cl, tray=None):
        wl = cl.wl
        tray = tray or wl.bind('mellagui_tray_v1')
        it = wl.new_id('mellagui_tray_item_v1')
        wl.request(tray, 'create_item', it)
        wl.roundtrip()
        return tray, it

    @staticmethod
    def items():
        lines = ctl.cmd('tray')
        n = int(re.match(r'tray n=(\d+)', lines[0])[1])
        out = []
        for l in lines[1:]:
            m = re.match(r'item (\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) icon=(\S+) text=(.*)$', l)
            out.append(dict(id=int(m[1]), x=int(m[2]), y=int(m[3]), w=int(m[4]), h=int(m[5]), icon=m[6], text=m[7]))
        assert n == len(out)
        return out

    def test_an_item_shows_icon_and_text(self):
        cl = self.client()
        wl = cl.wl
        self.assertEqual(self.items(), [])
        tray, it = self.item(cl)
        ev = wl.wait_for(lambda e: e.obj == it and e.name == 'size', 2.0)
        self.assertIsNotNone(ev, 'no icon size was announced')
        size = ev.args[0]
        self.assertTrue(8 <= size <= 64, size)
        shm = Shm(wl, 64 * 64 * 4)
        buf = shm.buffer(size, size, argb=0xffff00ff)
        wl.request(it, 'set_icon', buf)
        wl.request(it, 'set_text', 'rec 01:02')
        wl.roundtrip()
        ctl.sync()
        (item,) = self.items()
        self.assertEqual((item['icon'], item['text']), ('%dx%d' % (size, size), 'rec 01:02'))
        ppm = ctl.dump(os.path.join(tmp, 'tray1.ppm'))
        ix, iy = item['x'] + 6 + size // 2, item['y'] + item['h'] // 2
        self.assertEqual(ppm.px(ix, iy), (255, 0, 255), 'the icon is not in the bar')
        # the label changes, the icon goes
        wl.request(it, 'set_text', 'x')
        wl.request(it, 'set_icon', 0)
        wl.roundtrip()
        (item,) = self.items()
        self.assertEqual((item['icon'], item['text']), ('-', 'x'))
        # the buffer was released after the copy: it can be drawn into again
        self.assertIsNone(wl.error)

    def test_clicks(self):
        cl = self.client()
        wl = cl.wl
        tray, it = self.item(cl)
        wl.request(it, 'set_text', 'hello')
        wl.roundtrip()
        ctl.sync()
        (item,) = self.items()
        cx, cy = item['x'] + item['w'] // 2, item['y'] + item['h'] // 2
        wl.queue.clear()
        ctl.click(cx, cy)
        ev = wl.wait_for(lambda e: e.obj == it and e.name == 'clicked', 2.0)
        self.assertEqual(ev.args, [BTN_LEFT])
        ctl.click(cx, cy, BTN_RIGHT)
        ev = wl.wait_for(lambda e: e.obj == it and e.name == 'clicked', 2.0)
        self.assertEqual(ev.args, [BTN_RIGHT])
        ctl.abs(cx, cy)                                                       # pressed on it, let go elsewhere: no click
        ctl.button(BTN_LEFT, True)
        ctl.abs(cx - item['w'] - 40, cy)
        ctl.button(BTN_LEFT, False)
        wl.pump(0.3)
        self.assertEqual([e for e in wl.queue if e.obj == it and e.name == 'clicked'], [])

    def test_items_go_with_their_client(self):
        cl = self.client()
        tray, it = self.item(cl)
        cl2 = self.client()
        self.item(cl2)
        self.assertEqual(len(self.items()), 2)
        cl.wl.request(it, 'destroy')
        cl.wl.roundtrip()
        ctl.sync()
        self.assertEqual(len(self.items()), 1)
        cl2.wl.close()
        for _ in range(50):
            ctl.sync()
            if not self.items():
                break
            time.sleep(0.05)
        self.assertEqual(self.items(), [])

    def test_the_first_item_is_the_rightmost_and_they_do_not_overlap(self):
        cl = self.client()
        tray, a = self.item(cl)
        _, b = self.item(cl, tray)
        cl.wl.request(a, 'set_text', 'one')
        cl.wl.request(b, 'set_text', 'two two')
        cl.wl.roundtrip()
        ctl.sync()
        ia, ib = self.items()
        self.assertLess(ib['x'] + ib['w'], ia['x'])
        self.assertEqual((ia['y'], ia['h']), (ib['y'], ib['h']))

    def test_limits_and_bad_icons(self):
        cl = self.client()
        wl = cl.wl
        tray = wl.bind('mellagui_tray_v1')
        its = [self.item(cl, tray)[1] for _ in range(6)]
        self.assertEqual(len(self.items()), 4, 'more than 4 items of one client are shown')
        shm = Shm(wl, 200 * 200 * 4)
        big = shm.buffer(100, 100, argb=0xffff00ff)                           # too big: ignored, the program is not punished
        wl.request(its[0], 'set_icon', big)
        wl.roundtrip()
        self.assertEqual(self.items()[0]['icon'], '-')
        self.assertIsNone(wl.error)
        wl.request(its[0], 'set_text', 'a' * 100 + '\x01\x02')                 # cut to 31 bytes, control characters are blanks
        wl.roundtrip()
        ctl.sync()
        text = self.items()[0]['text']
        self.assertEqual(len(text), 31)
        self.assertNotIn('\x01', text)

    def test_closed_to_other_programs(self):
        p, c, stop = self.private_compositor('tray-own', '--settings-access', 'own')
        cl = Client(os.path.join(tmp, 'tray-own'))
        self.clients.append(cl)
        tray, it = self.item(cl)
        self.assertEqual([e.name for e in cl.wl.queue if e.obj == it], [], 'a foreign program was answered')
        cl.wl.request(it, 'set_text', 'spoof')
        cl.wl.roundtrip()
        self.assertEqual(c.cmd('tray')[0], 'tray n=0')
        self.assertIsNone(cl.wl.error)


class TestViewer(Base):
    """mellagui-view in a window: browsing, zoom, title."""
    VIEW = os.environ.get('MELLAGUI_VIEW_BIN', os.path.join(ROOT, 'build', 'mellagui-view'))

    def solid(self, rgb, w=60, h=40):
        return make_png(w, h, 2, 8, [[rgb] * w for _ in range(h)])

    def start(self, directory, *args, env=None):
        if not os.path.exists(self.VIEW):
            self.skipTest('mellagui-view not built')
        p = subprocess.Popen([self.VIEW] + list(args or [directory]), env=dict(os.environ, HOME=directory, **(env or {})),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.view')
        self.assertIsNotNone(win, 'the viewer window did not appear')
        time.sleep(0.3)
        return p, win

    def centre(self, name):
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.view'][0]
        ctl.abs(900, 590)
        time.sleep(0.1)
        ppm = ctl.dump(os.path.join(tmp, name))
        return ppm, win

    def title(self):
        return [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.view'][0]['title']

    def wait_title(self, text):
        for _ in range(100):
            if self.title() == text:
                return True
            time.sleep(0.03)
        return False

    def test_browse_zoom_and_quit(self):
        d = tempfile.mkdtemp(prefix='mellagui-viewer-')
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        colours = {'20261107-100000.png': (255, 0, 0), '20261107-100001.png': (0, 255, 0), '20261107-100002.png': (0, 0, 255)}
        for n, rgb in colours.items():
            with open(os.path.join(d, n), 'wb') as f:
                f.write(self.solid(rgb))
        with open(os.path.join(d, 'notes.txt'), 'w') as f:
            f.write('not an image')
        p, win = self.start(d)
        self.assertTrue(self.wait_title('20261107-100002.png - Images'))          # a directory opens at the newest image
        ppm, win = self.centre('view1.ppm')
        cx, cy = win['x'] + win['w'] // 2, win['y'] + (win['h'] - 24) // 2
        self.assertEqual(ppm.px(cx, cy), (0, 0, 255))
        small = ppm.count((0, 0, 255), win['x'], win['y'], win['x'] + win['w'], win['y'] + win['h'])
        self.assertEqual(small, 60 * 40)                                          # small images are shown at 100 %, not stretched
        ctl.tap(105)                                                              # Left
        self.assertTrue(self.wait_title('20261107-100001.png - Images'))
        ppm, win = self.centre('view2.ppm')
        self.assertEqual(ppm.px(cx, cy), (0, 255, 0))
        ctl.tap(102)                                                              # Home
        self.assertTrue(self.wait_title('20261107-100000.png - Images'))
        ctl.tap(107)                                                              # End
        self.assertTrue(self.wait_title('20261107-100002.png - Images'))
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(13)                                                               # Shift+= is "+": zoom in
        ctl.key(KEY_LEFTSHIFT, False)
        time.sleep(0.2)
        ppm, win = self.centre('view3.ppm')
        big = ppm.count((0, 0, 255), win['x'], win['y'], win['x'] + win['w'], win['y'] + win['h'])
        self.assertAlmostEqual(big / small, 1.25 * 1.25, delta=0.25)
        ctl.tap(11)                                                               # 0: back to 100 %
        time.sleep(0.2)
        ppm, win = self.centre('view4.ppm')
        self.assertEqual(ppm.count((0, 0, 255), win['x'], win['y'], win['x'] + win['w'], win['y'] + win['h']), small)
        ctl.tap(KEY_ESC)
        for _ in range(100):
            if p.poll() is not None:
                break
            time.sleep(0.05)
        self.assertEqual(p.poll(), 0)

    def test_a_file_argument_starts_at_that_file_and_a_broken_one_says_so(self):
        d = tempfile.mkdtemp(prefix='mellagui-viewer-')
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        with open(os.path.join(d, 'a.png'), 'wb') as f:
            f.write(self.solid((255, 0, 0)))
        with open(os.path.join(d, 'b.png'), 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\nbroken')
        p, win = self.start(d, os.path.join(d, 'a.png'))
        self.assertTrue(self.wait_title('a.png - Images'))
        ctl.tap(106)                                                              # Right: the broken one
        self.assertTrue(self.wait_title('b.png - Images'))
        time.sleep(0.2)
        self.assertTrue(alive())
        ctl.tap(105)
        self.assertTrue(self.wait_title('a.png - Images'))

    def test_an_empty_directory_is_not_an_error(self):
        d = tempfile.mkdtemp(prefix='mellagui-viewer-')
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        p, win = self.start(d)
        self.assertTrue(self.wait_title('no image - Images'))
        ctl.tap(106)
        ctl.tap(107)
        time.sleep(0.2)
        self.assertIsNone(p.poll())

    # ---- videos -----------------------------------------------------------------------------------------------------------

    def make_video(self, d, name='clip.mp4', seconds=3, rate=10, codec='mjpeg'):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            self.skipTest('ffmpeg not installed')
        path = os.path.join(d, name)
        args = [ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=duration=%d:size=320x240:rate=%d' % (seconds, rate), '-c:v', codec]
        if codec == 'mjpeg':
            args += ['-pix_fmt', 'yuvj420p']
        subprocess.run(args + [path], check=True, timeout=60)
        return path

    def video_dir(self):
        d = tempfile.mkdtemp(prefix='mellagui-viewer-')
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        return d

    def info(self, path):
        return subprocess.run([self.VIEW, '--video-info', path], capture_output=True, text=True, timeout=10)

    def window_pixels(self, name):
        """The viewer's window and the bytes of its picture area (without the bar) in a dump."""
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.view'][0]
        ppm = ctl.dump(os.path.join(tmp, name))
        rows = []
        for y in range(win['y'], win['y'] + win['h'] - 28):
            o = (y * ppm.w + win['x']) * 3
            rows.append(ppm.data[o:o + win['w'] * 3])
        return win, ppm, b''.join(rows)

    def test_video_info_and_decode(self):
        if not os.path.exists(self.VIEW):
            self.skipTest('mellagui-view not built')
        d = self.video_dir()
        path = self.make_video(d)
        r = self.info(path)
        self.assertEqual(r.stdout.strip(), '320x240 codec=mp4v mjpeg=1 pictures=30 duration_ms=3000', r.stderr)
        out = os.path.join(d, 'f.ppm')
        self.assertEqual(subprocess.run([self.VIEW, '--decode', path, out, '12'], capture_output=True, timeout=10).returncode, 0)
        ppm = Ppm(out)
        self.assertEqual((ppm.w, ppm.h), (320, 240))
        self.assertEqual(subprocess.run([self.VIEW, '--decode', path, out, '99'], capture_output=True, timeout=10).returncode, 1)
        # a video that is not Motion JPEG is recognised, not shown
        mp4 = self.make_video(d, 'm.mp4', seconds=1, codec='mpeg4')
        self.assertIn('mjpeg=0', self.info(mp4).stdout)
        self.assertEqual(subprocess.run([self.VIEW, '--decode', mp4, out], capture_output=True, timeout=10).returncode, 1)
        # damaged: cut off (no index), junk, empty, missing
        with open(path, 'rb') as f:
            data = f.read()
        for name, blob in (('cut.mp4', data[:len(data) // 2]), ('junk.mp4', os.urandom(300)), ('empty.mp4', b'')):
            with open(os.path.join(d, name), 'wb') as f:
                f.write(blob)
            r = self.info(os.path.join(d, name))
            self.assertEqual(r.returncode, 1, name)
            self.assertNotIn('Sanitizer', r.stderr)
        self.assertEqual(self.info(os.path.join(d, 'nothing.mp4')).returncode, 1)

    def test_a_recording_of_ours_can_be_read(self):
        rec = os.path.join(os.path.dirname(BIN), 'mellagui-record')
        if not os.path.exists(rec) or not os.path.exists(self.VIEW):
            self.skipTest('mellagui-record / mellagui-view not built')
        d = self.video_dir()
        r = subprocess.run([rec, '--start', '--duration', '1.2', '--dir', d, '--name', 'ours.mp4', '--codec', 'mjpeg'], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        info = self.info(os.path.join(d, 'ours.mp4')).stdout
        self.assertRegex(info, r'^1024x600 codec=mp4v mjpeg=1 pictures=\d+ duration_ms=1[2-3]\d\d$')
        out = os.path.join(d, 'f.ppm')
        self.assertEqual(subprocess.run([self.VIEW, '--decode', os.path.join(d, 'ours.mp4'), out], capture_output=True, timeout=10).returncode, 0)
        ppm = Ppm(out)
        self.assertEqual((ppm.w, ppm.h), (1024, 600))
        near = sum(1 for x in range(300, 400) for y in range(0, 30)
                   if all(abs(a - b) <= 8 for a, b in zip(ppm.px(x, y), (0x14, 0x17, 0x1f))))
        self.assertGreater(near, 500, 'the panel of the recorded screen is not in the picture')

    def test_a_video_plays_pauses_and_seeks(self):
        d = self.video_dir()
        self.make_video(d)
        p, win = self.start(d, os.path.join(d, 'clip.mp4'))
        self.assertTrue(self.wait_title('clip.mp4 - Images'))
        ctl.abs(900, 590)
        time.sleep(0.2)
        _, _, a = self.window_pixels('vid1.ppm')
        time.sleep(0.6)
        _, _, b = self.window_pixels('vid2.ppm')
        self.assertNotEqual(a, b, 'the picture does not change: the video is not playing')
        ctl.tap(KEY_SPACE)                                                        # pause
        time.sleep(0.3)
        _, _, c = self.window_pixels('vid3.ppm')
        time.sleep(0.6)
        _, _, e = self.window_pixels('vid4.ppm')
        self.assertEqual(c, e, 'the video moves while it is paused')
        ctl.tap(KEY_SPACE)                                                        # on again
        time.sleep(0.6)
        _, _, f = self.window_pixels('vid5.ppm')
        self.assertNotEqual(e, f)
        ctl.tap(107)                                                              # End: the last picture, shown as it was decoded
        time.sleep(0.5)
        win, ppm, g = self.window_pixels('vid6.ppm')
        out = os.path.join(d, 'last.ppm')
        subprocess.run([self.VIEW, '--decode', os.path.join(d, 'clip.mp4'), out, '29'], check=True, timeout=10)
        last = Ppm(out)
        ox, oy = win['x'] + (win['w'] - 320) // 2, win['y'] + (win['h'] - 24 - 240) // 2
        xs, ys = range(0, 320, 5), range(0, 240, 7)
        same = sum(1 for y in ys for x in xs if ppm.px(ox + x, oy + y) == last.px(x, y))
        self.assertEqual(same, len(xs) * len(ys), 'the last picture is not on the screen')
        time.sleep(0.5)
        _, _, h = self.window_pixels('vid7.ppm')
        self.assertEqual(g, h, 'the video does not stop at its end')
        ctl.tap(KEY_SPACE)                                                        # at the end: plays again from the start
        time.sleep(0.5)
        _, _, i = self.window_pixels('vid8.ppm')
        self.assertNotEqual(h, i)

    def test_h264_plays_through_ffmpeg(self):
        d = self.video_dir()
        self.make_video(d, codec='libx264')
        p, win = self.start(d, os.path.join(d, 'clip.mp4'))
        self.assertTrue(self.wait_title('clip.mp4 - Images'))
        ctl.abs(900, 590)
        time.sleep(0.8)
        _, _, a = self.window_pixels('h264a.ppm')
        time.sleep(0.6)
        _, _, b = self.window_pixels('h264b.ppm')
        self.assertNotEqual(a, b, 'the picture does not change: the video is not playing')
        ctl.tap(KEY_SPACE)                                                        # pause
        time.sleep(0.4)
        _, _, c = self.window_pixels('h264c.ppm')
        time.sleep(0.6)
        _, _, e = self.window_pixels('h264d.ppm')
        self.assertEqual(c, e, 'the video moves while it is paused')
        ctl.tap(107)                                                              # End
        time.sleep(1.0)
        _, _, f = self.window_pixels('h264e.ppm')
        time.sleep(0.6)
        _, _, g = self.window_pixels('h264f.ppm')
        self.assertEqual(f, g, 'the video does not stop at its end')
        ctl.tap(KEY_SPACE)                                                        # at the end: again from the start
        time.sleep(0.8)
        _, _, h = self.window_pixels('h264g.ppm')
        self.assertNotEqual(g, h)
        self.assertIsNone(p.poll())

    def record_changing(self, d, name='own.mp4', seconds=3):
        """A recording of the test screen (our own H.264) in which a window changes its colour every quarter of a second."""
        rec = os.path.join(os.path.dirname(BIN), 'mellagui-record')
        if not os.path.exists(rec):
            self.skipTest('mellagui-record not built')
        cl = Client()
        self.addCleanup(cl.wl.close)
        win = Window(cl, 400, 300, color=0xffff0000)
        cl.wl.roundtrip()
        r = subprocess.Popen([rec, '--start', '--duration', str(seconds), '--dir', d, '--codec', 'h264', '--name', name],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=dict(os.environ, MELLAGUI_FFMPEG='none'))
        self.addCleanup(lambda: (r.poll() is None) and (r.kill(), r.wait()))
        colours = [0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff, 0xff202020]
        t0 = time.time()
        i = 0
        while r.poll() is None and time.time() - t0 < seconds + 2:
            win.commit_buffer(400, 300, color=colours[i % len(colours)])
            cl.wl.roundtrip()
            i += 1
            time.sleep(0.25)
        out, err = r.communicate(timeout=30)
        self.assertEqual(r.returncode, 0, err)
        cl.wl.close()
        for _ in range(100):                                                      # the test window must be gone before the viewer starts
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.test']:
                break
            time.sleep(0.03)
        return os.path.join(d, name)

    def test_our_own_h264_plays_without_ffmpeg(self):
        d = self.video_dir()
        path = self.record_changing(d)
        p, win = self.start(d, path, env={'MELLAGUI_FFMPEG': 'none'})              # no ffmpeg: the viewer's own decoder
        self.assertTrue(self.wait_title('own.mp4 - Images'))
        ctl.abs(900, 590)
        time.sleep(0.3)
        _, _, a = self.window_pixels('own1.ppm')
        time.sleep(0.7)
        _, _, b = self.window_pixels('own2.ppm')
        self.assertNotEqual(a, b, 'the picture does not change: the video is not playing')
        ctl.tap(KEY_SPACE)                                                        # pause
        time.sleep(0.3)
        _, _, c = self.window_pixels('own3.ppm')
        time.sleep(0.6)
        _, _, e = self.window_pixels('own4.ppm')
        self.assertEqual(c, e, 'the video moves while it is paused')
        ctl.tap(107)                                                              # End
        time.sleep(0.5)
        _, _, f = self.window_pixels('own5.ppm')
        time.sleep(0.5)
        _, _, g = self.window_pixels('own6.ppm')
        self.assertEqual(f, g, 'the video does not stop at its end')
        ctl.tap(102)                                                              # Home: the first picture again (and seeking backwards works)
        time.sleep(0.5)
        _, _, h = self.window_pixels('own7.ppm')
        self.assertNotEqual(g, h)
        ctl.tap(KEY_SPACE)                                                        # plays on from the start
        time.sleep(0.8)
        _, _, i = self.window_pixels('own8.ppm')
        self.assertNotEqual(h, i)
        self.assertIsNone(p.poll())

    def test_h264_without_ffmpeg_is_refused_politely(self):
        d = self.video_dir()
        self.make_video(d, codec='libx264')
        p, win = self.start(d, os.path.join(d, 'clip.mp4'), env={'MELLAGUI_FFMPEG': 'none'})
        self.assertTrue(self.wait_title('clip.mp4 - Images'))
        time.sleep(0.4)
        self.assertIsNone(p.poll())

    def test_the_buttons_in_the_bar(self):
        """Back 5 s, play / pause, stop, on 5 s: the buttons at the left of the bar of a video."""
        d = self.video_dir()
        self.make_video(d)
        p, win = self.start(d, os.path.join(d, 'clip.mp4'))
        self.assertTrue(self.wait_title('clip.mp4 - Images'))
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.view'][0]
        by = win['y'] + win['h'] - 24 + 14

        def press(i):
            ctl.click(win['x'] + 6 + i * 28 + 13, by)
            ctl.abs(900, 590)
            time.sleep(0.4)

        def frame_is(n):
            out = os.path.join(d, 'f%d.ppm' % n)
            subprocess.run([self.VIEW, '--decode', os.path.join(d, 'clip.mp4'), out, str(n)], check=True, timeout=10)
            ref = Ppm(out)
            w2, ppm, _ = self.window_pixels('btn-%d.ppm' % n)
            ox, oy = w2['x'] + (w2['w'] - 320) // 2, w2['y'] + (w2['h'] - 24 - 240) // 2
            xs, ys = range(0, 320, 5), range(0, 240, 7)
            return sum(1 for y in ys for x in xs if ppm.px(ox + x, oy + y) == ref.px(x, y)) == len(xs) * len(ys)

        ctl.abs(900, 590)
        time.sleep(0.5)
        _, _, a = self.window_pixels('btn-a.ppm')
        time.sleep(0.5)
        _, _, b = self.window_pixels('btn-b.ppm')
        self.assertNotEqual(a, b, 'it is not playing')
        press(1)                                                                  # pause
        _, _, c = self.window_pixels('btn-c.ppm')
        time.sleep(0.6)
        _, _, e = self.window_pixels('btn-d.ppm')
        self.assertEqual(c, e, 'the pause button did not pause')
        press(1)                                                                  # play again
        time.sleep(0.4)
        _, _, f = self.window_pixels('btn-e.ppm')
        self.assertNotEqual(e, f, 'the play button did not play')
        press(2)                                                                  # stop: the first picture, paused
        self.assertTrue(frame_is(0), 'stop did not show the first picture')
        time.sleep(0.5)
        self.assertTrue(frame_is(0), 'stop did not keep it paused')
        press(3)                                                                  # on 5 s: past the end of this 3 s video: the last picture
        self.assertTrue(frame_is(29), 'the forward button did not go to the end')
        press(0)                                                                  # back 5 s: the start
        self.assertTrue(frame_is(0), 'the back button did not go to the start')

    def test_a_video_that_cannot_be_shown_does_not_hurt(self):
        d = self.video_dir()
        self.make_video(d, 'm.mp4', seconds=1, codec='mpeg4')
        with open(os.path.join(d, 'z-junk.mp4'), 'wb') as f:
            f.write(os.urandom(500))
        p, win = self.start(d, os.path.join(d, 'm.mp4'))
        self.assertTrue(self.wait_title('m.mp4 - Images'))
        ctl.tap(109)                                                              # PgDn: the junk file after it (Right seeks in a video that plays)
        self.assertTrue(self.wait_title('z-junk.mp4 - Images'))
        self.assertIsNone(p.poll())

    def test_a_playing_video_keeps_the_screen_awake(self):
        """zwp_idle_inhibit_manager_v1: the viewer holds an inhibitor while a video plays, not while it is paused."""
        d = self.video_dir()
        self.make_video(d, seconds=4)
        self.assertEqual(ctl.cmd('idle')[0], 'idle inhibitors=0 inhibited=0')
        p, win = self.start(d, os.path.join(d, 'clip.mp4'))
        self.assertTrue(self.wait_title('clip.mp4 - Images'))
        ctl.abs(900, 590)
        deadline = time.time() + 3
        while time.time() < deadline and ctl.cmd('idle')[0] != 'idle inhibitors=1 inhibited=1':
            time.sleep(0.05)
        self.assertEqual(ctl.cmd('idle')[0], 'idle inhibitors=1 inhibited=1')
        ctl.tap(KEY_SPACE)                                                        # pause
        deadline = time.time() + 3
        while time.time() < deadline and ctl.cmd('idle')[0] != 'idle inhibitors=0 inhibited=0':
            time.sleep(0.05)
        self.assertEqual(ctl.cmd('idle')[0], 'idle inhibitors=0 inhibited=0')
        ctl.tap(KEY_SPACE)                                                        # on again
        deadline = time.time() + 3
        while time.time() < deadline and ctl.cmd('idle')[0] != 'idle inhibitors=1 inhibited=1':
            time.sleep(0.05)
        self.assertEqual(ctl.cmd('idle')[0], 'idle inhibitors=1 inhibited=1')


class TestIdleInhibit(Base):
    """src/idleinhibit.c: zwp_idle_inhibit_manager_v1. A visible surface with an inhibitor keeps the screensaver and the auto lock away."""
    UNIT = {'MELLAGUI_LOCK_UNIT_MS': '250'}

    def start(self, name):
        p, c, stop = self.private_compositor(name, '--config', 'none', env=self.UNIT)
        c.sync()
        cl = Client(name)
        mgr = cl.wl.bind('zwp_idle_inhibit_manager_v1')
        return c, cl, mgr

    @staticmethod
    def idle(c):
        return dict(kv.split('=') for kv in c.cmd('idle')[0].split()[1:])

    @staticmethod
    def workspace(c, n):
        c.key(KEY_LEFTMETA, True)
        c.tap(1 + n)                                                 # KEY_1 = 2
        c.key(KEY_LEFTMETA, False)
        c.sync()

    @staticmethod
    def inhibit(cl, mgr, surface):
        i = cl.wl.new_id('zwp_idle_inhibitor_v1')
        cl.wl.request(mgr, 'create_inhibitor', i, surface)
        cl.wl.roundtrip()
        return i

    def test_it_counts_only_while_the_window_is_visible(self):
        c, cl, mgr = self.start('mellagui-ih1')
        self.assertEqual(self.idle(c), {'inhibitors': '0', 'inhibited': '0'})
        win = Window(cl)
        win.wait_configure()
        i = self.inhibit(cl, mgr, win.surface)
        c.sync()
        self.assertEqual(self.idle(c), {'inhibitors': '1', 'inhibited': '1'})
        cl.wl.request(win.toplevel, 'set_minimized')                 # hidden: nobody sees it
        cl.wl.roundtrip()
        c.sync()
        self.assertEqual(self.idle(c), {'inhibitors': '1', 'inhibited': '0'})
        cl.wl.request(i, 'destroy')
        cl.wl.roundtrip()
        c.sync()
        self.assertEqual(self.idle(c), {'inhibitors': '0', 'inhibited': '0'})

    def test_another_workspace_and_a_destroyed_surface_do_not_inhibit(self):
        c, cl, mgr = self.start('mellagui-ih2')
        win = Window(cl)
        win.wait_configure()
        self.inhibit(cl, mgr, win.surface)
        c.sync()
        self.assertEqual(self.idle(c)['inhibited'], '1')
        self.workspace(c, 2)
        self.assertEqual(self.idle(c), {'inhibitors': '1', 'inhibited': '0'})
        self.workspace(c, 1)
        self.assertEqual(self.idle(c)['inhibited'], '1')
        cl.wl.request(win.toplevel, 'destroy')                       # the window goes; the inhibitor object stays but does nothing
        cl.wl.request(win.xdg, 'destroy')
        cl.wl.request(win.surface, 'destroy')
        cl.wl.roundtrip()
        c.sync()
        self.assertEqual(self.idle(c), {'inhibitors': '1', 'inhibited': '0'})
        cl.wl.close()
        time.sleep(0.2)
        c.sync()
        self.assertEqual(self.idle(c), {'inhibitors': '0', 'inhibited': '0'})

    def test_the_screensaver_waits_for_the_inhibitor(self):
        c, cl, mgr = self.start('mellagui-ih3')
        win = Window(cl)
        win.wait_configure()
        i = self.inhibit(cl, mgr, win.surface)
        c.cmd('settings saver-minutes 2')                            # 2 * 250 ms
        time.sleep(1.6)
        self.assertEqual(dict(kv.split('=') for kv in c.cmd('saver')[0].split()[1:])['running'], '0', 'the screensaver started under an inhibitor')
        cl.wl.request(i, 'destroy')
        cl.wl.roundtrip()
        t0 = time.time()
        while time.time() - t0 < 1.0:                                # the idle time starts over when the inhibitor goes
            self.assertEqual(dict(kv.split('=') for kv in c.cmd('saver')[0].split()[1:])['running'], '0', 'the idle time was not restarted')
            time.sleep(0.1)
            if time.time() - t0 > 0.3:
                break
        end = time.time() + 6
        while time.time() < end:
            if dict(kv.split('=') for kv in c.cmd('saver')[0].split()[1:])['running'] == '1':
                break
            time.sleep(0.05)
        else:
            self.fail('the screensaver never started after the inhibitor was gone')


class TestCalendar(Base):
    """The clock settings (src/clockfmt.c), the panel's clock and mellagui-calendar, which a click on the clock shows."""
    CAL = os.path.join(ROOT, 'build', 'mellagui-calendar')

    def start(self, name, *args, env=None):
        if not os.path.exists(self.CAL):
            self.skipTest('mellagui-calendar not built')
        p, c, stop = self.private_compositor(name, '--config', 'none', *args, env=env)
        c.sync()
        return c

    @staticmethod
    def clock(c):
        line = c.cmd('clock')[0]
        m = re.match(r'clock x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) tasks=(\d+) calendar=(\d) text=(.*)$', line)
        return dict(x=int(m[1]), y=int(m[2]), w=int(m[3]), h=int(m[4]), tasks=int(m[5]), cal=int(m[6]), text=m[7])

    def wait_cal(self, c, state, what, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.clock(c)['cal'] == state:
                return
            time.sleep(0.05)
        self.fail('timeout: ' + what + ' (calendar=%d)' % self.clock(c)['cal'])

    @staticmethod
    def click_clock(c):
        k = TestCalendar.clock(c)
        c.click(k['x'] + k['w'] // 2, k['y'] + k['h'] // 2)

    @staticmethod
    def cal_window(c):
        ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.calendar']
        return ws[0] if ws else None

    def test_selftest(self):
        if not os.path.exists(self.CAL):
            self.skipTest('mellagui-calendar not built')
        r = subprocess.run([self.CAL, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('selftest ok', r.stdout)

    def test_the_clock_follows_its_settings(self):
        c = self.start('mellagui-cal1')
        k = self.clock(c)
        self.assertRegex(k['text'], r'^\d\d\.\d\d\.\d{4} \d\d:\d\d$')
        self.assertEqual(k['w'], 136)                               # the width it always had
        c.cmd('settings clock-seconds 1')
        k2 = self.clock(c)
        self.assertRegex(k2['text'], r'^\d\d\.\d\d\.\d{4} \d\d:\d\d:\d\d$')
        self.assertGreater(k2['w'], k['w'])                         # the clock grows to the left
        self.assertEqual(k2['x'] + k2['w'], k['x'] + k['w'])
        c.cmd('settings clock-hours 12')
        self.assertRegex(self.clock(c)['text'], r'^\d\d\.\d\d\.\d{4} \d{1,2}:\d\d:\d\d [AP]M$')
        c.cmd('settings clock-seconds 0')
        c.cmd('settings date-format ymd')
        self.assertRegex(self.clock(c)['text'], r'^\d{4}-\d\d-\d\d \d{1,2}:\d\d [AP]M$')
        c.cmd('settings date-format mdy')
        self.assertRegex(self.clock(c)['text'], r'^\d\d/\d\d/\d{4} ')
        c.cmd('settings date-format long')
        c.cmd('settings clock-weekday 1')
        self.assertRegex(self.clock(c)['text'], r'^(Mon|Tue|Wed|Thu|Fri|Sat|Sun) \d{1,2} [A-Z][a-z]{2} \d{4} \d{1,2}:\d\d [AP]M$')
        c.cmd('settings clock-date 0')
        self.assertRegex(self.clock(c)['text'], r'^\d{1,2}:\d\d [AP]M$')
        c.cmd('settings clock-hours 24')
        self.assertRegex(self.clock(c)['text'], r'^\d\d:\d\d$')
        for bad in ('clock-hours 7', 'date-format iso', 'cal-week-start tue'):
            with self.assertRaises(RuntimeError):
                c.cmd('settings ' + bad)

    def test_the_clock_settings_are_saved(self):
        path = os.path.join(tmp, 'cal-settings.conf')
        if os.path.exists(path):
            os.unlink(path)
        c = self.start('mellagui-cal2', '--settings', path)
        for kv in ('clock-hours 12', 'clock-seconds 1', 'date-format long', 'cal-week-start sun', 'cal-weeks 1'):
            c.cmd('settings ' + kv)
        time.sleep(0.6)
        with open(path) as f:
            text = f.read()
        for line in ('clock-hours=12', 'clock-seconds=1', 'date-format=long', 'cal-week-start=sun', 'cal-weeks=1', 'clock-date=1', 'cal-enabled=1'):
            self.assertIn(line, text.split('\n'))
        c2 = self.start('mellagui-cal2b', '--settings', path)
        self.assertRegex(self.clock(c2)['text'], r'^\d{1,2} [A-Z][a-z]{2} \d{4} \d{1,2}:\d\d:\d\d [AP]M$')

    def test_a_click_on_the_clock_shows_the_calendar_and_the_next_one_hides_it(self):
        c = self.start('mellagui-cal3')
        self.assertIsNone(self.cal_window(c))                       # (headless: it is started by the first click)
        self.click_clock(c)
        self.wait_cal(c, 2, 'the calendar shows')
        k, w = self.clock(c), self.cal_window(c)
        self.assertNotIn('minimized', w['flags'])
        self.assertGreaterEqual(w['y'], k['y'] + k['h'])             # under the bar
        self.assertLessEqual(abs((w['x'] + w['w']) - (k['x'] + k['w'])), 8)   # its right edge is the clock's
        self.assertEqual(k['tasks'], 0, 'the calendar is in the taskbar')
        time.sleep(0.3)
        ppm = c.dump(os.path.join(tmp, 'cal3.ppm'))
        accent = sum(1 for yy in range(w['y'], w['y'] + w['h'], 2) for xx in range(w['x'], w['x'] + w['w'], 2)
                     if ppm.px(xx, yy) == (0x6f, 0xa8, 0xff))
        self.assertGreater(accent, 100, 'today is not marked in the calendar')
        self.click_clock(c)
        self.wait_cal(c, 1, 'the calendar hides')
        self.assertIn('minimized', self.cal_window(c)['flags'])
        self.click_clock(c)                                          # and again: no new process, it is just shown
        self.wait_cal(c, 2, 'the calendar shows again')
        self.assertEqual(len([w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.calendar']), 1)

    def test_it_hides_when_something_else_is_clicked_and_stays_out_of_the_taskbar(self):
        c = self.start('mellagui-cal4')
        cl = Client('mellagui-cal4')
        win = Window(cl, 300, 200, color=0xffff0000)
        win.wait_configure()
        c.sync()
        self.assertEqual(self.clock(c)['tasks'], 1)
        self.click_clock(c)
        self.wait_cal(c, 2, 'the calendar shows')
        self.assertEqual(self.clock(c)['tasks'], 1)
        r = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.test'][0]
        c.click(r['x'] + 50, r['y'] + 50)                            # a click in the other window
        self.wait_cal(c, 1, 'the calendar goes away')
        self.assertEqual(self.clock(c)['cal'], 1)

    def test_calendars_can_be_turned_off(self):
        c = self.start('mellagui-cal5')
        c.cmd('settings cal-enabled 0')
        self.click_clock(c)
        time.sleep(0.8)
        self.assertIsNone(self.cal_window(c))
        c.cmd('settings cal-enabled 1')
        self.click_clock(c)
        self.wait_cal(c, 2, 'the calendar shows after it was turned on')

    def test_preloaded_it_is_there_hidden_and_shows_at_once(self):
        c = self.start('mellagui-cal6', env={'MELLAGUI_CALENDAR_PRELOAD': '1'})
        self.wait_cal(c, 1, 'the calendar waits hidden')
        self.assertEqual(self.clock(c)['tasks'], 0)
        t0 = time.time()
        self.click_clock(c)
        self.wait_cal(c, 2, 'the calendar shows')
        self.assertLess(time.time() - t0, 1.0)

    def test_escape_hides_it_and_the_close_button_does_not_end_it(self):
        c = self.start('mellagui-cal7')
        self.click_clock(c)
        self.wait_cal(c, 2, 'the calendar shows')
        c.tap(1)                                                     # Esc
        self.wait_cal(c, 1, 'Esc hides it')
        self.click_clock(c)
        self.wait_cal(c, 2, 'shown again')
        w = self.cal_window(c)
        c.click(w['x'] + w['w'] - 12, w['y'] - 12)                   # the close button of the title bar
        time.sleep(0.5)
        self.assertIsNotNone(self.cal_window(c), 'the close button ended the calendar')


class TestGL(unittest.TestCase):
    """src/gl.c: the dlopen()ed EGL / OpenGL ES layer (needs a GPU or Mesa's software driver; skipped without)."""
    def test_gl_layer(self):
        tool = os.path.join(ROOT, 'build', 'gl-test')
        if not os.path.exists(tool):
            self.skipTest('gl-test not built')
        r = subprocess.run([tool], capture_output=True, text=True, timeout=60)
        if r.returncode == 77:
            self.skipTest(r.stdout.strip())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('gl-test ok', r.stdout)

    def run_gbm(self, device, **env):
        tool = os.path.join(ROOT, 'build', 'gl-test')
        if not os.path.exists(tool):
            self.skipTest('gl-test not built')
        if not os.access(device, os.R_OK | os.W_OK):
            self.skipTest('%s is not accessible' % device)
        r = subprocess.run([tool, 'gbm'], capture_output=True, text=True, timeout=60,
                           env=dict(os.environ, GL_TEST_DEVICE=device, **env))
        if r.returncode == 77:
            self.skipTest(r.stdout.strip())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('gl-test gbm ok', r.stdout)

    def test_gbm_window_on_a_render_node(self):
        import glob
        nodes = sorted(glob.glob('/dev/dri/renderD*'))
        if not nodes:
            self.skipTest('no render node')
        self.run_gbm(nodes[0])

    def test_gbm_scanout_buffers_get_a_kms_framebuffer(self):
        """What the DRM backend does per output, minus the mode set: a scanout GBM window, swaps, drmModeAddFB2 on its buffers."""
        import glob
        cards = sorted(glob.glob('/dev/dri/card*'))
        if not cards:
            self.skipTest('no DRM card')
        self.run_gbm(cards[0], GL_TEST_SCANOUT='1', GL_TEST_ADDFB='1')


class TestLogind(unittest.TestCase):
    """src/logind.c (a D-Bus client of its own) against tests/fake_logind.py; the DRM / evdev wiring cannot run headless."""
    TOOL = os.environ.get('MELLAGUI_LOGIND_TEST', os.path.join(ROOT, 'build', 'logind-test'))

    def setUp(self):
        if not os.path.exists(self.TOOL):
            self.skipTest('logind-test not built')
        from fake_logind import FakeLogind
        self.Fake = FakeLogind

    def run_tool(self, fake, *args):
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + fake.path, XDG_SESSION_ID='7')
        p = subprocess.Popen([self.TOOL] + list(args), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        return p

    def lines_until(self, p, word, timeout=8.0):
        out = []
        end = time.time() + timeout
        while time.time() < end:
            line = p.stdout.readline()
            if not line:
                break
            if line[:2].isdigit() and line[2] == ':':           # a log line of the tool
                continue
            out.append(line.strip())
            if line.startswith(word):
                break
        return out

    def test_session_take_device_pause_resume_and_switch(self):
        fake = self.Fake().start()
        self.addCleanup(fake.stop)
        p = self.run_tool(fake, 'session', '/dev/null')                       # /dev/null is the character device 1:3
        out = self.lines_until(p, 'READY')
        self.assertEqual(out, ['take fd=ok inactive=0', 'wrote 5', 'READY'])
        self.assertIn(('GetSession', '7'), fake.events)                       # XDG_SESSION_ID is used
        self.assertIn(('TakeControl', 0), fake.events)
        self.assertIn(('TakeDevice', 1, 3), fake.events)
        with open(fake.devfiles[(1, 3)], 'rb') as f:
            self.assertEqual(f.read(), b'hello')                              # the fd really is the one logind passed
        fake.emit_pause(4, 5, 'pause')                                        # another device: acknowledged by logind.c itself
        self.assertTrue(fake.wait(('PauseDeviceComplete', 4, 5)))
        fake.emit_pause(1, 3, 'pause')                                        # the watched one: the owner decides, then acks
        self.assertTrue(fake.wait(('PauseDeviceComplete', 1, 3)))
        fake.emit_pause(1, 3, 'force')                                        # "force": no ack is owed
        fake.emit_resume(1, 3)
        out = self.lines_until(p, 'DONE')
        self.assertEqual(out, ['pause pause', 'pause force', 'resume fd=ok data=resumed', 'switch 0', 'DONE pauses=2 resumes=1'], out)
        self.assertEqual(p.wait(timeout=5), 0)
        self.assertEqual([e for e in fake.events if e[0] == 'PauseDeviceComplete'], [('PauseDeviceComplete', 4, 5), ('PauseDeviceComplete', 1, 3)])
        self.assertIn(('Get', 'org.freedesktop.login1.Session', 'Seat'), fake.events)
        self.assertIn(('SwitchTo', 2), fake.events)         # on the seat object the session names, not on a made-up path
        self.assertIn(('ReleaseDevice', 1, 3), fake.events)
        self.assertIn(('ReleaseControl',), fake.events)

    def test_an_inactive_session_is_reported(self):
        fake = self.Fake(inactive=True).start()
        self.addCleanup(fake.stop)
        p = self.run_tool(fake, 'session', '/dev/null')
        self.assertEqual(self.lines_until(p, 'take')[0], 'take fd=ok inactive=1')

    def test_without_a_session_or_bus_open_fails_with_a_reason(self):
        fake = self.Fake(get_session_error=True).start()
        self.addCleanup(fake.stop)
        out = self.lines_until(self.run_tool(fake, 'fail'), 'open failed')
        self.assertTrue(out[0].startswith('open failed: this process is not in a logind session'), out)
        fake2 = self.Fake(take_control_error=True).start()
        self.addCleanup(fake2.stop)
        out = self.lines_until(self.run_tool(fake2, 'fail'), 'open failed')
        self.assertIn('cannot take control', out[0])
        self.assertIn('AccessDenied', out[0])
        gone = self.Fake()
        path = gone.path
        gone.stop()
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + path)
        r = subprocess.run([self.TOOL, 'fail'], env=env, capture_output=True, text=True, timeout=10)
        self.assertIn('cannot connect to the system bus', r.stdout)

    def test_without_logind_a_device_is_a_plain_open(self):
        r = subprocess.run([self.TOOL, 'plain', '/dev/null'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.stdout.strip(), 'plain fd=ok')



class TestGLParity(Base):
    """The GL renderer (render_gl.inc) against the software one: the same scene in two private compositors must give the same
    pixels (translucency may differ by one level: float blending against integer rounding)."""
    NAME = {'software': 'mellagui-par-sw', 'gl': 'mellagui-par-gl'}

    def setUp(self):
        super().setUp()
        tool = os.path.join(ROOT, 'build', 'gl-test')
        if not os.path.exists(tool):
            self.skipTest('gl-test not built')
        r = subprocess.run([tool], capture_output=True, text=True, timeout=60)
        if r.returncode == 77:
            self.skipTest(r.stdout.strip())

    def scene(self, renderer):
        """Runs the scene on a fresh compositor with `renderer`; returns {step: Ppm}."""
        name = self.NAME[renderer]
        lock = os.path.join(tmp, name + '.lock')
        p, c, stop = self.private_compositor(name, '--lock-file', lock,
                                             env={'MELLAGUI_RENDERER': renderer, 'MELLAGUI_TOAST_MS': '600000', 'MELLAGUI_ABOUT_STATIC': '1',
                                                  'MELLAGUI_LOCK_ITER': '1000'})
        shots = {}
        cl = Client(name)
        self.clients.append(cl)
        a = Window(cl, 260, 160, title='A', color=0xff336699)
        b = Window(cl, 220, 140, title='B', color=0x80402010)               # translucent, premultiplied
        cl.wl.roundtrip()
        c.abs(20, 480)
        c.sync()
        time.sleep(0.3)
        c.sync()
        shots['two windows'] = c.dump(os.path.join(tmp, name + '-1.ppm'))
        # a window of its own with a pattern, then a change inside it that only damages a rectangle
        w = Window(cl, 200, 120, title='C', color=0xff202020)
        cl.wl.roundtrip()
        stride = 200 * 4
        for y in range(120):
            w.shm.map[y * stride:(y + 1) * stride] = b''.join(struct.pack('<I', 0xff000000 | (x * 255 // 200) << 16 | (y * 255 // 120) << 8 | 0x40) for x in range(200))
        w.commit_buffer(200, 120)
        cl.wl.roundtrip()
        for y in range(30, 60):
            w.shm.map[y * stride + 40 * 4:y * stride + 90 * 4] = struct.pack('<I', 0xffff00ff) * 50
        cl.wl.request(w.surface, 'attach', w.buffer, I(0), I(0))
        cl.wl.request(w.surface, 'damage', I(40), I(30), I(50), I(30))
        cl.wl.request(w.surface, 'commit')
        cl.wl.roundtrip()
        c.sync()
        time.sleep(0.2)
        c.sync()
        shots['partial damage'] = c.dump(os.path.join(tmp, name + '-2.ppm'))
        c.abs(300, 220)                                                      # the cursor over the windows
        c.sync()
        time.sleep(0.2)
        c.sync()
        shots['cursor'] = c.dump(os.path.join(tmp, name + '-3.ppm'))
        c.click(16, 14)                                                      # the About dialog (round logo button)
        c.abs(40, 60)
        c.sync()
        time.sleep(0.2)
        c.sync()
        shots['about dialog'] = c.dump(os.path.join(tmp, name + '-4.ppm'))
        c.tap(KEY_ESC)
        c.click(784, 14)                                                     # the session dialog (round power button)
        c.abs(480, 270)                                                      # highlights Lock
        c.sync()
        time.sleep(0.2)
        c.sync()
        shots['session dialog'] = c.dump(os.path.join(tmp, name + '-4a.ppm'))
        c.tap(KEY_ESC)
        c.cmd('notify 0 A notification with a longer body that wraps over several lines of the card')
        c.abs(40, 480)
        c.sync()
        time.sleep(0.2)
        c.sync()
        shots['notification'] = c.dump(os.path.join(tmp, name + '-4b.ppm'))
        c.abs(700, 450)
        c.tap(99)                                                            # Print: flash, then the card stays
        time.sleep(0.5)
        c.sync()
        shots['card'] = c.dump(os.path.join(tmp, name + '-5.ppm'))
        c.cmd('lock set - password hunter22x')
        c.cmd('lock now')
        time.sleep(0.3)
        c.sync()
        shots['lock'] = c.dump(os.path.join(tmp, name + '-6.ppm'))
        return shots

    def test_scene_gives_the_same_pixels(self):
        sw = self.scene('software')
        gl = self.scene('gl')
        problems = []
        for step in sw:
            a, b = sw[step], gl[step]
            self.assertEqual((a.w, a.h), (b.w, b.h))
            worst, n = 0, 0
            where = None
            for y in range(30, a.h):                                         # the top rows hold the clock
                if step == 'card' and 300 <= y < 314:                         # ... and so does the card's thumbnail (the panel in it)
                    continue
                if step == 'lock' and 100 <= y < 230:                         # ... and the lock screen's big clock
                    continue
                ra = a.data[(y * a.w) * 3:((y + 1) * a.w) * 3]
                rb = b.data[(y * b.w) * 3:((y + 1) * b.w) * 3]
                if ra == rb:
                    continue
                for x in range(a.w):
                    pa, pb = ra[x * 3:x * 3 + 3], rb[x * 3:x * 3 + 3]
                    if pa != pb:
                        d = max(abs(i - j) for i, j in zip(pa, pb))
                        n += 1
                        if d > worst:
                            worst, where = d, (x, y, tuple(pa), tuple(pb))
            if worst > 1:
                problems.append('%s: %d pixels differ, worst by %d at %s' % (step, n, worst, where))
            for y in range(0, 32):                                           # panel: everything left of the clock
                for x in range(0, 330):
                    if a.px(x, y) != b.px(x, y):
                        problems.append('%s: panel pixel %d,%d differs %s %s' % (step, x, y, a.px(x, y), b.px(x, y)))
                        break
                else:
                    continue
                break
        self.assertEqual(problems, [])


class TestAltTab(Base):
    """Alt+Tab: hold Alt, Tab walks the windows in the order they were used, releasing Alt goes there, Esc stays."""
    ACCENT = (0x6f, 0xa8, 0xff)                           # the list's border

    def setUp(self):
        super().setUp()
        self.titles = ['A', 'B', 'C']
        for t in self.titles:                              # created in this order: C is focused, then B, then A in use order
            self.window(200, 150, title=t)
        self.assertEqual(self.focused(), 'C')
        ctl.abs(900, 590)

    def focused(self):
        r = ctl.windows()['windows']
        f = [x['title'] for x in r if 'focused' in x['flags']]
        return f[0] if f else None

    def alt(self, down):
        ctl.key(KEY_LEFTALT, down)

    def list_visible(self):
        ctl.sync()
        time.sleep(0.1)
        ppm = ctl.dump(os.path.join(tmp, 'alttab.ppm'))
        return ppm.count(self.ACCENT, 250, 150, 780, 450) > 100            # the border of a box in the middle of the 1024x600 screen

    def test_one_tab_goes_to_the_window_used_before(self):
        self.alt(True)
        ctl.tap(KEY_TAB)
        self.assertTrue(self.list_visible())
        self.assertEqual(self.focused(), 'C')                                 # nothing changes while Alt is held
        self.alt(False)
        self.assertEqual(self.focused(), 'B')
        self.assertFalse(self.list_visible())

    def test_more_tabs_walk_the_list_and_wrap(self):
        self.alt(True)
        ctl.tap(KEY_TAB)
        ctl.tap(KEY_TAB)
        self.alt(False)
        self.assertEqual(self.focused(), 'A')
        self.alt(True)                                                         # use order is now A, B, C
        for _ in range(3):
            ctl.tap(KEY_TAB)                                                   # B, C, then back to A (wrapped)
        self.alt(False)
        self.assertEqual(self.focused(), 'A')

    def test_shift_goes_backwards(self):
        self.alt(True)
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(KEY_TAB)
        ctl.key(KEY_LEFTSHIFT, False)
        self.alt(False)
        self.assertEqual(self.focused(), 'A')                                 # the least recently used one

    def test_escape_keeps_the_window_and_closes_the_list(self):
        self.alt(True)
        ctl.tap(KEY_TAB)
        self.assertTrue(self.list_visible())
        ctl.tap(KEY_ESC)
        self.assertFalse(self.list_visible())
        self.alt(False)
        self.assertEqual(self.focused(), 'C')

    def test_a_window_on_another_workspace_brings_its_workspace(self):
        ctl.key(KEY_LEFTMETA, True)
        ctl.key(KEY_LEFTSHIFT, True)
        ctl.tap(KEY_2)                                                         # C moves to workspace 2
        ctl.key(KEY_LEFTSHIFT, False)
        ctl.key(KEY_LEFTMETA, False)
        time.sleep(0.1)
        self.assertEqual(ctl.windows()['workspace'], 0)
        self.assertEqual(self.focused(), 'B')
        self.alt(True)
        ctl.tap(KEY_TAB)                                                       # list: B, C, A - C (on workspace 2) was used just before B
        self.alt(False)
        self.assertEqual(self.focused(), 'C')
        self.assertEqual(ctl.windows()['workspace'], 1)

    def test_a_single_window_gets_no_list(self):
        ctl.key(KEY_LEFTALT, False)
        for c in self.clients[:2]:
            c.wl.close()
        for _ in range(100):
            if len(ctl.windows()['windows']) == 1:
                break
            time.sleep(0.03)
        self.alt(True)
        ctl.tap(KEY_TAB)
        self.assertFalse(self.list_visible())
        self.alt(False)
        self.assertEqual(self.focused(), 'C')

    def test_tab_is_not_forwarded_to_the_application(self):
        c, win = self.clients[-1], None
        seat = c.wl.bind('wl_seat', 5)
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        c.wl.roundtrip()
        c.wl.queue.clear()
        self.alt(True)
        ctl.tap(KEY_TAB)
        self.alt(False)
        ctl.sync()
        c.wl.roundtrip()
        self.assertEqual([e for e in c.wl.queue if e.obj == kb and e.name == 'key' and e.args[2] == KEY_TAB], [])

    def test_a_window_that_goes_while_the_list_is_open(self):
        self.alt(True)
        ctl.tap(KEY_TAB)
        self.clients[1].wl.close()                                              # B leaves
        for _ in range(100):
            if len(ctl.windows()['windows']) == 2:
                break
            time.sleep(0.03)
        self.alt(False)
        self.assertTrue(alive())
        self.assertIn(self.focused(), ('A', 'C'))


class TestWallpaper(Base):
    """settings wallpaper / wallpaper-mode: a PNG or PPM as the desktop background (fill, fit, stretch, center)."""
    W, H = 800, 500

    def png(self, name, w=80, h=40, left=(255, 0, 0), right=(0, 0, 255)):
        pix = [[left if x < w // 2 else right for x in range(w)] for _ in range(h)]
        path = os.path.join(tmp, name)
        with open(path, 'wb') as f:
            f.write(make_png(w, h, 2, 8, pix))
        return path

    def start(self, name, *extra, env=None):
        p, c, stop = self.private_compositor(name, *extra, env=env)
        c.abs(790, 490)
        c.sync()
        time.sleep(0.15)
        c.sync()
        return c

    def shot(self, c, tag):
        c.abs(790, 490)
        c.sync()
        time.sleep(0.15)
        c.sync()
        return c.dump(os.path.join(tmp, 'wall-%s.ppm' % tag))

    def get(self, c, key):
        c.cmd('setting ' + key)
        return c.last[3:]

    def test_a_jpeg_works_as_the_wallpaper(self):
        c = self.start('mellagui-wall9')
        path = os.path.join(tmp, 'wall-j.jpg')
        with open(path, 'wb') as f:
            f.write(JPEG_RED_BLUE)
        c.cmd('settings wallpaper-mode stretch')
        c.cmd('settings wallpaper ' + path)
        self.assertEqual(self.get(c, 'wallpaper-status'), 'ok')
        st = self.shot(c, 'jpeg')
        left, right = st.px(10, 250), st.px(790, 400)
        self.assertTrue(all(abs(a - b) <= 8 for a, b in zip(left, (230, 30, 30))), left)
        self.assertTrue(all(abs(a - b) <= 8 for a, b in zip(right, (30, 30, 230))), right)

    def test_modes_place_the_picture(self):
        c = self.start('mellagui-wall1')
        grad = self.shot(c, 'grad')
        img = self.png('wall-a.png')                                         # 80x40: red left half, blue right half
        c.cmd('settings wallpaper-mode stretch')
        c.cmd('settings wallpaper ' + img)
        self.assertEqual(self.get(c, 'wallpaper-status'), 'ok')
        st = self.shot(c, 'stretch')
        self.assertEqual(st.px(10, 250), (255, 0, 0))
        self.assertEqual(st.px(790, 400), (0, 0, 255))                        # (the clipboard gadget sits top right)
        self.assertEqual(st.px(10, 60), (255, 0, 0))                          # the whole screen is picture, the panel (and its shadow) on top
        c.cmd('settings wallpaper-mode fill')                                # 2:1 onto 1.6:1: cropped at the sides, centred
        fl = self.shot(c, 'fill')
        self.assertEqual(fl.px(5, 250), (255, 0, 0))
        self.assertEqual(fl.px(795, 400), (0, 0, 255))
        self.assertEqual(fl.px(380, 250), (255, 0, 0))                        # the picture's red / blue edge is at 400, smoothed
        self.assertEqual(fl.px(420, 250), (0, 0, 255))
        mid = fl.px(400, 250)
        self.assertTrue(0 < mid[0] < 255 and 0 < mid[2] < 255, mid)
        c.cmd('settings wallpaper-mode fit')                                 # whole picture, bars: 800x400 centred
        ft = self.shot(c, 'fit')
        self.assertEqual(ft.px(5, 250), (255, 0, 0))
        self.assertEqual(ft.px(400, 40), grad.px(400, 40))                    # above the picture (it spans y 50..450): the gradient
        self.assertEqual(ft.px(400, 470), grad.px(400, 470))                  # below it
        self.assertNotEqual(ft.px(400, 100), grad.px(400, 100))               # y 50..450 is the picture
        c.cmd('settings wallpaper-mode center')                              # 80x40 unscaled in the middle
        ce = self.shot(c, 'center')
        self.assertEqual(ce.px(380, 250), (255, 0, 0))
        self.assertEqual(ce.px(420, 250), (0, 0, 255))
        self.assertEqual(ce.px(100, 250), grad.px(100, 250))
        self.assertEqual(ce.px(400, 200), grad.px(400, 200))                  # 20 px above the picture's top edge (230)

    def test_clearing_and_bad_files(self):
        c = self.start('mellagui-wall2')
        grad = self.shot(c, 'g2')
        c.cmd('settings wallpaper-mode stretch')
        c.cmd('settings wallpaper ' + self.png('wall-b.png'))
        self.assertEqual(self.shot(c, 'w2').px(10, 250), (255, 0, 0))
        c.cmd('settings wallpaper /nonexistent/picture.png')
        self.assertTrue(self.get(c, 'wallpaper-status').startswith('error:'), self.get(c, 'wallpaper-status'))
        self.assertEqual(self.shot(c, 'e2').px(10, 250), grad.px(10, 250))   # no picture: the gradient
        junk = os.path.join(tmp, 'wall-junk.png')
        with open(junk, 'wb') as f:
            f.write(b'not a picture')
        c.cmd('settings wallpaper ' + junk)
        self.assertTrue(self.get(c, 'wallpaper-status').startswith('error:'))
        c.cmd('settings wallpaper ' + self.png('wall-c.png', left=(0, 255, 0)))
        self.assertEqual(self.shot(c, 'ok2').px(10, 250), (0, 255, 0))
        c.cmd('settings wallpaper ""')                                        # empty: back to the gradient
        self.assertEqual((self.get(c, 'wallpaper'), self.get(c, 'wallpaper-status')), ('', ''))
        self.assertEqual(self.shot(c, 'g3').px(10, 250), grad.px(10, 250))

    def test_refused_values(self):
        c = self.start('mellagui-wall3')
        for line in ('settings wallpaper-mode sideways', 'settings wallpaper-status ok', 'settings wallpaper ' + 'x' * 300):
            with self.assertRaises(RuntimeError, msg=line):
                c.cmd(line)

    def test_the_picture_survives_a_restart_and_home_is_expanded(self):
        home = os.path.join(tmp, 'wall-home')
        os.makedirs(home, exist_ok=True)
        shutil.copy(self.png('wall-d.png', left=(255, 255, 0)), os.path.join(home, 'bg.png'))
        conf = os.path.join(tmp, 'wall-conf', 'settings.conf')
        c = self.start('mellagui-wall4', '--settings', conf, env={'HOME': home})
        c.cmd('settings wallpaper-mode stretch')
        c.cmd('settings wallpaper ~/bg.png')
        self.assertEqual(self.get(c, 'wallpaper-status'), 'ok')
        self.assertEqual(self.shot(c, 'h1').px(10, 250), (255, 255, 0))
        c.cmd('quit')
        time.sleep(0.5)
        text = open(conf).read().split('\n')
        self.assertIn('wallpaper=~/bg.png', text)
        self.assertIn('wallpaper-mode=stretch', text)
        c2 = self.start('mellagui-wall5', '--settings', conf, env={'HOME': home})
        self.assertEqual(self.get(c2, 'wallpaper-status'), 'ok')
        self.assertEqual(self.shot(c2, 'h2').px(10, 250), (255, 255, 0))


class TestNotify(Base):
    """Notification cards: top right below the panel, newest on top, at most five, a click dismisses, a timeout removes."""
    ACCENT = (0x6f, 0xa8, 0xff)
    CARD_X = 800 - 320 - 12                       # private compositors are 800x500: the cards start here, 36 px from the top

    def start(self, name='mellagui-note', **kw):
        p, c, stop = self.private_compositor(name, **kw)
        c.abs(20, 480)
        c.sync()
        return c

    def stripes(self, c, tag):
        """The accent stripes down the left edge of the cards: [(y0, y1)] top to bottom."""
        c.abs(20, 480)
        c.sync()
        time.sleep(0.15)
        c.sync()
        ppm = c.dump(os.path.join(tmp, 'note-%s.ppm' % tag))
        runs, start = [], None
        for y in range(28, 500):
            hit = ppm.px(self.CARD_X + 2, y) == self.ACCENT
            if hit and start is None:
                start = y
            if not hit and start is not None:
                if y - start > 15:                                                  # (the lock screen has small accent marks, too)
                    runs.append((start, y))
                start = None
        return runs, ppm

    def test_a_card_shows_with_its_text_and_goes_when_clicked(self):
        c = self.start()
        c.cmd('notify 0 Backup finished 3 files copied')
        runs, ppm = self.stripes(c, 'one')
        self.assertEqual(len(runs), 1, runs)
        self.assertEqual(runs[0][0], 41)                                           # 32 panel + 8 gap + the border
        self.assertGreater(ppm.count((0xe6, 0xe9, 0xef), self.CARD_X, 60, self.CARD_X + 320, 120), 20)       # body text
        c.click(self.CARD_X + 100, 50)
        runs, _ = self.stripes(c, 'one-gone')
        self.assertEqual(runs, [])

    def test_newest_on_top_and_at_most_five(self):
        c = self.start()
        for i in range(7):
            c.cmd('notify 0 Note %d some words' % i)
        runs, ppm = self.stripes(c, 'many')
        self.assertEqual(len(runs), 5, runs)
        h = runs[0][1] - runs[0][0]
        self.assertTrue(all(abs((b - a) - h) <= 1 for a, b in runs))               # one-line bodies: the same size
        c.click(self.CARD_X + 100, runs[1][0] + 10)                                 # the second card goes, the others close up
        runs2, _ = self.stripes(c, 'many2')
        self.assertEqual(len(runs2), 4)
        self.assertEqual(runs2[0], runs[0])
        self.assertEqual(runs2[1][0], runs[2][0] - (runs[2][0] - runs[1][0]) + 0)  # the third one moved up to where the second was

    def test_a_timeout_removes_it(self):
        c = self.start()
        c.cmd('notify 400 Short lived')
        self.assertEqual(len(self.stripes(c, 'tmo')[0]), 1)
        time.sleep(0.6)
        self.assertEqual(self.stripes(c, 'tmo2')[0], [])

    def test_long_text_is_wrapped_and_cut(self):
        c = self.start()
        c.cmd('notify 0 Long ' + 'word ' * 60)
        runs, _ = self.stripes(c, 'long')
        self.assertEqual(len(runs), 1)
        self.assertLessEqual(runs[0][1] - runs[0][0], 8 + 20 + 4 * 16 + 6 + 6)      # four lines at most
        self.assertGreaterEqual(runs[0][1] - runs[0][0], 8 + 20 + 3 * 16)

    def test_the_click_does_not_reach_the_window_underneath(self):
        c = self.start()
        cl = Client('mellagui-note')
        self.clients.append(cl)
        seat = cl.wl.bind('wl_seat', 5)
        ptr = cl.wl.new_id('wl_pointer')
        cl.wl.request(seat, 'get_pointer', ptr)
        Window(cl, 780, 440, title='big')
        cl.wl.roundtrip()
        c.cmd('notify 0 On top')
        self.stripes(c, 'under')
        cl.wl.queue.clear()
        c.click(self.CARD_X + 100, 50)
        c.sync()
        cl.wl.roundtrip()
        self.assertEqual([e for e in cl.wl.queue if e.obj == ptr and e.name == 'button'], [])
        self.assertEqual(self.stripes(c, 'under2')[0], [])

    def test_the_command_line_tool(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-notify')
        if not os.path.exists(exe):
            self.skipTest('mellagui-notify not built')
        c = self.start('mellagui-note2')
        env = dict(os.environ, WAYLAND_DISPLAY='mellagui-note2')
        r = subprocess.run([exe, '-t', '0', 'From a script', 'some', 'words'], env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.stripes(c, 'cli')[0]), 1)
        self.assertEqual(subprocess.run([exe], env=env, capture_output=True, timeout=10).returncode, 2)       # usage
        self.assertEqual(subprocess.run([exe, '-t', '-5', 'x'], env=env, capture_output=True, timeout=10).returncode, 2)

    def test_nothing_shows_on_the_lock_screen(self):
        c = self.start('mellagui-note3', env={'MELLAGUI_LOCK_ITER': '1000'})
        c.cmd('notify 0 Secret thing')
        self.assertEqual(len(self.stripes(c, 'pre')[0]), 1)
        c.cmd('lock set - password hunter22x')
        c.cmd('lock now')
        time.sleep(0.2)
        self.assertEqual(self.stripes(c, 'locked')[0], [])

    def test_bad_values_are_refused(self):
        c = self.start('mellagui-note4')
        for line in ('settings notify nothing-with-tabs', 'notify 999999999 Title'):
            with self.assertRaises(RuntimeError, msg=line):
                c.cmd(line)


class TestMonitorComesBack(Base):
    """An output that goes away and returns (KVM switch, cable): windows that fill the screen are fitted to it again."""

    @staticmethod
    def settle(cl, win, c, state=None, timeout=3.0):
        """Answer configures until one with `state` (xdg_toplevel state number) arrives; returns the last one."""
        end = time.time() + timeout
        cfg = None
        while time.time() < end:
            cl.wl.roundtrip()
            got = win.handle_configure()
            if got is not None:
                cfg = got
                if state is None or state in got[2]:
                    break
            time.sleep(0.05)
        c.sync()
        return cfg

    def test_maximized_and_fullscreen_windows_follow(self):
        p, c, stop = self.private_compositor('mellagui-hp1')
        cl = Client(os.path.join(tmp, 'mellagui-hp1'))
        self.clients.append(cl)
        win = Window(cl, 300, 200)
        cl.wl.roundtrip()
        c.sync()
        cl.wl.request(win.toplevel, 'set_maximized')
        cfg = self.settle(cl, win, c, XDG_STATE_MAXIMIZED)
        self.assertIn(XDG_STATE_MAXIMIZED, cfg[2])
        w0 = c.windows()['windows'][0]
        self.assertIn('maximized', w0['flags'])
        self.assertGreater(w0['w'], 700)
        c.cmd('unplug')
        cl.wl.roundtrip()
        c.sync()
        self.assertEqual(c.cmd('outputs'), [])
        c.cmd('replug 1000x600')
        cfg = self.settle(cl, win, c, XDG_STATE_MAXIMIZED)
        w1 = c.windows()['windows'][0]
        self.assertEqual((w1['w'] - w0['w'], w1['h'] - w0['h']), (200, 100), 'the maximized window was not fitted to the monitor that came back')
        # full screen: the same
        cl.wl.request(win.toplevel, 'unset_maximized')
        self.settle(cl, win, c)
        cl.wl.request(win.toplevel, 'set_fullscreen', 0)
        self.settle(cl, win, c, XDG_STATE_FULLSCREEN)
        w2 = c.windows()['windows'][0]
        self.assertEqual((w2['x'], w2['y'], w2['w'], w2['h']), (0, 0, 1000, 600))
        c.cmd('unplug')
        c.cmd('replug 700x400')
        self.settle(cl, win, c, XDG_STATE_FULLSCREEN)
        w3 = c.windows()['windows'][0]
        self.assertEqual((w3['x'], w3['y'], w3['w'], w3['h']), (0, 0, 700, 400))
        self.assertIsNone(cl.wl.error)


class TestDoubleClickTime(Base):
    """[windows] double-click-ms: how fast the second click must follow the first (compositor, file manager ...)."""

    def conf(self, name, ms):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write('' if ms is None else '[windows]\ndouble-click-ms = %d\n' % ms)
        return path

    def title_double_click(self, name, ms, gap):
        """A window, a double click with `gap` seconds between the clicks: is it maximized afterwards?"""
        p, c, stop = self.private_compositor(name, '--config', self.conf(name, ms))
        cl = Client(os.path.join(tmp, name))
        self.clients.append(cl)
        win = Window(cl, 300, 200)
        cl.wl.roundtrip()
        c.sync()
        w = c.windows()['windows'][0]
        tx, ty = w['x'] + 60, w['y'] - 12                       # the title bar
        c.abs(tx, ty)
        c.button(BTN_LEFT, True)
        c.button(BTN_LEFT, False)
        time.sleep(gap)
        c.button(BTN_LEFT, True)
        c.button(BTN_LEFT, False)
        time.sleep(0.2)
        c.sync()
        return 'maximized' in c.windows()['windows'][0]['flags'] or any(
            st == XDG_STATE_MAXIMIZED for cfg in [win.wait_configure(0.5)] if cfg for st in cfg[2])

    def test_the_title_bar_follows_the_time(self):
        self.assertTrue(self.title_double_click('mellagui-dc1', None, 0.25), 'the default time (400 ms) did not take 250 ms as a double click')
        self.assertFalse(self.title_double_click('mellagui-dc2', 150, 0.30), 'a double click time of 150 ms took 300 ms')
        self.assertTrue(self.title_double_click('mellagui-dc3', 1000, 0.70), 'a double click time of 1000 ms did not take 700 ms')

    def files_double_click(self, ms, gap):
        """The file manager started with that time; a.txt double clicked with `gap` seconds between the clicks: did the editor open?"""
        exe = os.path.join(ROOT, 'build', 'mellagui-files')
        if not os.path.exists(exe) or not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-edit')):
            self.skipTest('mellagui-files / mellagui-edit not built')
        d = tempfile.mkdtemp(prefix='files-dc-', dir=tmp)
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        with open(os.path.join(d, 'a.txt'), 'w') as f:
            f.write('hello\n')
        for w in ctl.windows()['windows']:
            self.assertNotIn(w['app_id'], ('mellagui.files', 'mellagui.edit'), 'a window of an earlier test is still open')
        conf = self.conf('dc-files-%d' % ms if ms else 'dc-files-default', ms)
        p = subprocess.Popen([exe, d], env=dict(os.environ, MELLAGUI_CONFIG=conf), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('mellagui.files')
        self.assertIsNotNone(win)
        time.sleep(0.4)
        win = [w for w in ctl.windows()['windows'] if w['app_id'] == 'mellagui.files'][0]
        x, y = win['x'] + 100, win['y'] + 30 + 22 + 18 * 1 + 9        # tool bar + header, the row of a.txt (after "..")
        ctl.abs(x, y)
        ctl.button(BTN_LEFT, True)
        ctl.button(BTN_LEFT, False)
        time.sleep(gap)
        ctl.button(BTN_LEFT, True)
        ctl.button(BTN_LEFT, False)
        time.sleep(0.8)
        opened = any(w['app_id'] == 'mellagui.edit' for w in ctl.windows()['windows'])
        for pid in os.listdir('/proc'):                                       # the editor is a child of the file manager: both go
            if not pid.isdigit():
                continue
            try:
                with open('/proc/%s/stat' % pid) as f:
                    ppid = int(f.read().rsplit(')', 1)[1].split()[1])
                if ppid == p.pid:
                    os.kill(int(pid), 9)
            except (OSError, ValueError, IndexError):
                pass
        p.kill()
        p.wait()
        for _ in range(100):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] in ('mellagui.files', 'mellagui.edit')]:
                break
            time.sleep(0.05)
        return opened

    def test_the_file_manager_follows_the_time(self):
        self.assertTrue(self.files_double_click(None, 0.25), 'the default time did not take 250 ms as a double click')
        self.assertFalse(self.files_double_click(150, 0.40), 'a double click time of 150 ms took 400 ms')


    def test_the_pointer_page_sets_it_and_lets_you_try_it(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        path = os.path.join(tmp, 'dc-panel.conf')
        with open(path, 'w') as f:
            f.write('[dock]\nenabled = no\n')                                 # the dock would float over the tabs of the small screen
        p, c, stop = self.private_compositor('mellagui-dc4', '--config', path)
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-dc4'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.5)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 32 + 16)                                    # the Pointer tab
        time.sleep(0.3)

        def current():
            c.cmd('setting metrics')
            return int(re.search(r'windows\.double-click-ms=(\d+):', c.last)[1])

        def box_is_green():
            ppm = c.dump(os.path.join(tmp, 'dc-panel.ppm'))
            self.last_px = ppm.px(x0 + 200, y0 + 200)             # (away from the text and the pointer)
            return self.last_px == (0x5a, 0xc8, 0x8c)

        def two_clicks(gap):
            c.abs(x0 + 330, y0 + 190)
            c.button(BTN_LEFT, True)
            c.button(BTN_LEFT, False)
            time.sleep(gap)
            c.button(BTN_LEFT, True)
            c.button(BTN_LEFT, False)
            time.sleep(0.3)

        self.assertEqual(current(), 400)
        c.click(x0 + 528, y0 + 149)                                            # the far right end of the slider
        for _ in range(60):
            if current() >= 900:
                break
            time.sleep(0.05)
        self.assertGreaterEqual(current(), 900)
        self.assertIn('double-click-ms', open(path).read())
        two_clicks(0.2)
        self.assertTrue(box_is_green(), 'two clicks 200 ms apart are not a double click at 1000 ms (pixel %r, ms %d)' % (self.last_px, current()))
        c.click(x0 + 312, y0 + 149)                                            # the far left end
        for _ in range(60):
            if current() <= 150:
                break
            time.sleep(0.05)
        self.assertLessEqual(current(), 150)
        time.sleep(0.5)
        two_clicks(0.35)
        self.assertFalse(box_is_green(), 'two clicks 350 ms apart are a double click at 100 ms')


class TestOutputLayout(Base):
    """Several monitors: settings output-order puts them left to right, windows move with their monitor, wl_output clients are told."""

    def start(self, name, *extra, env=None):
        p, c, stop = self.private_compositor(name, '--output', '400x300', *extra, env=env)
        c.sync()
        return c

    @staticmethod
    def places(c):
        out = {}
        for l in c.cmd('outputs'):
            f = l.split()
            kv = dict(x.split('=') for x in f[2:])
            out[f[1]] = tuple(int(kv[k]) for k in ('x', 'y', 'w', 'h'))
        return out

    def test_connection_order_by_default_and_the_setting_reorders(self):
        c = self.start('mellagui-lay1')
        self.assertEqual(self.places(c), {'headless-0': (0, 0, 800, 500), 'headless-1': (800, 0, 400, 300)})
        c.cmd('settings output-order headless-1,headless-0')
        self.assertEqual(self.places(c), {'headless-1': (0, 0, 400, 300), 'headless-0': (400, 0, 800, 500)})
        c.cmd('setting outputs')
        self.assertEqual(c.last, 'ok headless-1|400x300|0,0;headless-0|800x500|400,0')
        c.cmd('settings output-order nosuch,headless-1')                 # names that do not exist are ignored; unnamed ones follow
        self.assertEqual(self.places(c)['headless-1'][0], 0)
        c.cmd('settings output-order ""')
        self.assertEqual(self.places(c)['headless-0'][0], 0)

    def test_windows_move_with_their_monitor(self):
        c = self.start('mellagui-lay2')
        cl = Client('mellagui-lay2')
        self.clients.append(cl)
        Window(cl, 200, 120, title='w')
        cl.wl.roundtrip()
        before = c.windows()['windows'][0]
        self.assertLess(before['x'], 400)                                # on the first monitor
        c.cmd('settings output-order headless-1,headless-0')              # it moves 400 px to the right with headless-0
        after = c.windows()['windows'][0]
        self.assertEqual((after['x'] - before['x'], after['y'] - before['y']), (400, 0))
        c.cmd('settings output-order ""')
        back = c.windows()['windows'][0]
        self.assertEqual((back['x'], back['y']), (before['x'], before['y']))

    def test_maximized_windows_follow_the_new_geometry(self):
        c = self.start('mellagui-lay3')
        cl = Client('mellagui-lay3')
        self.clients.append(cl)
        win = Window(cl, 200, 120, title='w')
        cl.wl.roundtrip()
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_F)                                                      # maximize
        c.key(KEY_LEFTMETA, False)
        time.sleep(0.2)
        win.handle_configure()
        cl.wl.roundtrip()
        w0 = c.windows()['windows'][0]
        self.assertGreater(w0['w'], 700)
        c.cmd('settings output-order headless-1,headless-0')
        win.handle_configure()
        cl.wl.roundtrip()
        w1 = c.windows()['windows'][0]
        self.assertEqual(w1['x'], w0['x'] + 400)
        self.assertEqual(w1['w'], w0['w'])

    def test_wl_output_clients_hear_about_the_move(self):
        c = self.start('mellagui-lay4')
        cl = Client('mellagui-lay4')
        self.clients.append(cl)
        outs = [cl.wl.bind('wl_output', 3) for _ in range(1)]
        cl.wl.roundtrip()
        names = [n for n, v in cl.wl.globals.items() if n == 'wl_output']
        cl.wl.queue.clear()
        c.cmd('settings output-order headless-1,headless-0')
        cl.wl.roundtrip()
        geos = [e.args[0] for e in cl.wl.queue if e.name == 'geometry']
        self.assertTrue(geos, 'no wl_output.geometry after the move')

    def test_both_outputs_are_drawn_and_the_gadget_is_on_the_leftmost(self):
        c = self.start('mellagui-lay5')
        c.abs(5, 5)
        c.sync()
        time.sleep(0.15)
        c.sync()
        c.cmd('dump %s 1' % os.path.join(tmp, 'lay5-1.ppm'))
        ppm1 = Ppm(os.path.join(tmp, 'lay5-1.ppm'))
        self.assertEqual((ppm1.w, ppm1.h), (400, 300))
        self.assertEqual(ppm1.px(170, 14), ppm1.px(171, 14))                # a panel row (not the wallpaper gradient)
        self.assertNotEqual(ppm1.px(2, 250), ppm1.px(2, 5))                 # the wallpaper below the panel differs from the panel
        c.cmd('gadget-rect')
        gx = int(c.last.split()[1])
        self.assertGreaterEqual(gx, 0)
        self.assertLess(gx, 800)                                             # docked on headless-0 (the leftmost by default)
        c.cmd('settings output-order headless-1,headless-0')
        c.cmd('gadget-rect')
        gx = int(c.last.split()[1])
        self.assertLess(gx, 400)                                             # now the leftmost is headless-1: the gadget moved there
        c.cmd('notify 0 Hello there')
        c.sync()
        time.sleep(0.2)
        c.cmd('dump %s 1' % os.path.join(tmp, 'lay5-2.ppm'))
        ppm2 = Ppm(os.path.join(tmp, 'lay5-2.ppm'))
        self.assertGreater(ppm2.count((0x6f, 0xa8, 0xff), 60, 30, 400, 120), 20)    # the notification card is on that monitor

    def test_the_settings_panel_reorders_the_monitors(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-lay8')
        p = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-lay8'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 3 * 32 + 16)                         # the Display tab
        time.sleep(0.3)
        # "Monitors, left to right": headless-0 then headless-1, each row with < and > at the right edge
        row0 = y0 + 14 + 28 + 14                                        # first monitor row
        c.click(x0 + 168 + 440 - 16, row0)                              # ">" of the first row: headless-0 moves right
        time.sleep(0.3)
        c.cmd('setting output-order')
        self.assertEqual(c.last, 'ok headless-1,headless-0')
        self.assertEqual(self.places(c)['headless-1'][0], 0)

    def test_refused_values_and_persistence(self):
        conf = os.path.join(tmp, 'lay-conf', 'settings.conf')
        c = self.start('mellagui-lay6', '--settings', conf)
        for bad in ('settings output-order a/b', 'settings output-order ' + 'x' * 300, 'settings outputs foo'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd(bad)
        c.cmd('settings output-order headless-1,headless-0')
        c.cmd('quit')
        time.sleep(0.5)
        self.assertIn('output-order=headless-1,headless-0', open(conf).read().split('\n'))
        c2 = self.start('mellagui-lay7', '--settings', conf)
        self.assertEqual(self.places(c2)['headless-1'][0], 0)                # the saved order is used from the start


class TestConfig(Base):
    """mellagui.conf: the dock's buttons and the program commands come from a hand-written file, the command line wins."""

    def start(self, name, text, *extra, env=None):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', path, *extra, env=env)
        c.sync()
        return p, c

    @staticmethod
    def buttons(c):
        out = []
        for l in c.cmd('dock'):
            m = re.match(r'icon \d+ "(.*)" x=-?\d+ y=-?\d+ w=\d+ h=\d+ running=\d launching=\d cmd=(.*)', l)
            if m:
                out.append([m[1], m[2]])
        return out

    def test_default_dock_when_there_is_no_file(self):
        p, c, stop = self.private_compositor('mellagui-cfg0', '--config', 'none')
        names = [n for n, _ in self.buttons(c)]
        self.assertEqual(names[:6], ['Launcher', 'Terminal', 'Files', 'Editor', 'Calculator', 'Images'])
        self.assertEqual(names[-1], 'Settings')

    def test_dock_apps_section_replaces_the_default_dock(self):
        p, c = self.start('mellagui-cfg1', '# my dock\n[Dock-Apps]\nHello = echo hi\nText Editor = sh -c "a=1; echo $a"\n')
        self.assertEqual(self.buttons(c), [['Hello', 'echo hi'], ['Text Editor', 'sh -c "a=1; echo $a"']])

    def test_missing_programs_are_left_out_and_alternatives_pick_the_first_that_exists(self):
        p, c = self.start('mellagui-cfg2',
                          '[dock-apps]\nGone = /no/such/prog --x\nAlt = /no/such/prog -a || echo second || echo third\n'
                          'Env = env A=1 /no/such/prog\nQuoted = sh -c \'x || y\'\n')
        self.assertEqual(self.buttons(c), [['Alt', 'echo second'], ['Quoted', "sh -c 'x || y'"]])

    def test_tokens_and_own_programs(self):
        p, c = self.start('mellagui-cfg3', '[commands]\nrun = echo run-cmd\n[dock-apps]\nR = @run --flag\nMine = mellagui-nosuchprogram\n',
                          '--terminal', 'echo term')
        a = dict((n, cmd) for n, cmd in self.buttons(c))
        self.assertEqual(a['R'], 'echo run-cmd --flag')
        self.assertEqual(a['Mine'], 'mellagui-nosuchprogram')        # own programs always stay
        out = dict(l.split('=', 1) for l in c.cmd('commands'))
        self.assertEqual(out['run'], 'echo run-cmd')
        self.assertEqual(out['terminal'], 'echo term')               # the command line wins over the file

    def test_sibling_lookup_for_mellagui_programs(self):
        p, c = self.start('mellagui-cfg4', '[dock-apps]\nCalc = mellagui-calc --x\n')
        (name, cmd), = self.buttons(c)
        if os.path.exists(os.path.join(ROOT, 'build', 'mellagui-calc')):
            self.assertRegex(cmd, r"^'.*/mellagui-calc' --x$")
        self.assertIn('mellagui-calc', cmd)

    def test_bad_lines_are_reported_and_ignored(self):
        p, c = self.start('mellagui-cfg5', '[nonsense]\nx = 1\n[dock-apps]\nno equals sign\nOk = echo ok\n[bad\nAlso = echo no\n')
        self.assertEqual(self.buttons(c), [['Ok', 'echo ok']])
        self.assertTrue(alive_p(p))

    def test_an_old_menu_section_is_ignored_quietly(self):
        p, c = self.start('mellagui-cfg6', '[menu]\nOne = echo 1\n[dock-apps]\nTwo = echo 2\n')
        self.assertEqual(self.buttons(c), [['Two', 'echo 2']])
        c.cmd('reload')
        self.assertEqual(c.last, 'ok 0')                              # no rejected line: the section is simply not used any more


def alive_p(p):
    return p.poll() is None


def dock_apps(c):
    """['name|command', ...] of the dock's buttons (control socket `dock`)."""
    out = []
    for l in c.cmd('dock'):
        m = re.match(r'icon \d+ "(.*)" x=-?\d+ y=-?\d+ w=\d+ h=\d+ running=\d launching=\d cmd=(.*)', l)
        if m:
            out.append('%s|%s' % (m[1], m[2]))
    return out


class TestTheme(Base):
    """[theme] in mellagui.conf: the compositor's and the programs' colours."""

    def start(self, name, text, *extra):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', path, *extra)
        c.sync()
        return p, c

    def test_panel_dialog_colour_comes_from_the_file(self):
        p, c = self.start('mellagui-th1', '[theme]\nbackground = #102030\naccent = 0xff0000\n')
        c.click(16, 14)                                                      # the About dialog's card is painted in the panel colour
        c.abs(10, 10)
        c.sync()
        ppm = c.dump(os.path.join(tmp, 'th1.ppm'))
        self.assertEqual(ppm.px(150, 250), (0x10, 0x20, 0x30))

    def test_defaults_without_a_theme(self):
        p, c, stop = self.private_compositor('mellagui-th2', '--config', 'none')
        c.click(16, 14)
        c.abs(10, 10)
        c.sync()
        self.assertEqual(c.dump(os.path.join(tmp, 'th2.ppm')).px(150, 250), (0x14, 0x17, 0x1f))

    def test_short_colours_and_bad_values(self):
        p, c = self.start('mellagui-th3', '[theme]\nbackground = #abc\naccent = nothex\nnosuchcolour = #000000\n')
        c.click(16, 14)
        c.abs(10, 10)
        c.sync()
        self.assertEqual(c.dump(os.path.join(tmp, 'th3.ppm')).px(150, 250), (0xaa, 0xbb, 0xcc))
        self.assertTrue(alive_p(p))

    def test_programs_started_by_the_compositor_use_the_same_theme(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-calc')):
            self.skipTest('mellagui-calc not built')
        p, c = self.start('mellagui-th4', '[theme]\nbackground = #102030\n[dock-apps]\nCalc = mellagui-calc\n')
        c.click(400, 460)                                                    # its dock button
        for _ in range(100):
            wins = c.windows()['windows']
            if wins:
                break
            time.sleep(0.05)
        w = wins[0]
        c.abs(900, 500)
        c.sync()
        time.sleep(0.3)
        ppm = c.dump(os.path.join(tmp, 'th4.ppm'))
        # the calculator's window background shows in the margin of its client area
        seen = {ppm.px(w['x'] + 2, w['y'] + 2), ppm.px(w['x'] + w['w'] - 3, w['y'] + w['h'] - 3)}
        self.assertIn((0x10, 0x20, 0x30), seen)

    def test_print_config_lists_every_colour(self):
        r = subprocess.run([BIN, '--print-config'], capture_output=True, text=True, timeout=10)
        self.assertIn('[theme]', r.stdout)
        self.assertRegex(r.stdout, r'accent\s+= #6fa8ff')
        self.assertIn('[dock-apps]', r.stdout)


class TestKeys(Base):
    """[keys] in mellagui.conf: the window manager's shortcut table."""

    KEY_T, KEY_X, KEY_F4 = 20, 45, 62

    def start(self, name, text, *extra):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', path, *extra)
        c.sync()
        return p, c

    @staticmethod
    def table(c):
        return sorted(l[4:] for l in c.cmd('keys'))

    @staticmethod
    def combo(c, mods, code):
        for m in mods:
            c.key(m, True)
        c.tap(code)
        for m in reversed(mods):
            c.key(m, False)
        c.sync()

    def open_window(self, sock):
        cl = Client(sock)
        self.clients.append(cl)
        w = Window(cl, 200, 120, title='w')
        cl.wl.roundtrip()
        return cl, w

    @staticmethod
    def closed(cl, w, timeout=1.0):
        return cl.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', timeout) is not None

    def test_defaults_are_the_old_shortcuts(self):
        p, c, stop = self.private_compositor('mellagui-key0', '--config', 'none')
        t = self.table(c)
        for want in ('Super+Enter = terminal', 'Super+R = run', 'Super+Q = close', 'Super+Shift+E = quit',
                     'Super+Shift+3 = move-to-workspace 3', 'Super+4 = workspace 4', 'Super+Shift+Tab = focus-prev',
                     'Super+Down = restore', 'Super+Shift+Space = layout-prev'):
            self.assertIn(want, t)
        self.assertIn('Super+Shift+R = reload-config', t)
        self.assertIn('Super+Slash = exec mellagui-keys', t)
        self.assertEqual(len(t), 32)

    def test_exec_binding_runs_a_command(self):
        mark = os.path.join(tmp, 'key-exec-mark')
        if os.path.exists(mark):
            os.unlink(mark)
        p, c = self.start('mellagui-key1', '[keys]\nSuper+T = exec touch %s\n' % mark)
        self.combo(c, [KEY_LEFTMETA], self.KEY_T)
        for _ in range(100):
            if os.path.exists(mark):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(mark))

    def test_rebinding_and_removing(self):
        p, c = self.start('mellagui-key2', '[keys]\nalt+f4 = close\nSuper+Q = none\nsuper+x = close\n')
        t = self.table(c)
        self.assertIn('Alt+F4 = close', t)
        self.assertIn('Super+X = close', t)
        self.assertNotIn('Super+Q = close', t)
        cl, w = self.open_window('mellagui-key2')
        self.combo(c, [KEY_LEFTMETA], KEY_Q)                    # removed: the key goes to the client, no close request
        self.assertFalse(self.closed(cl, w, 0.5))
        self.combo(c, [KEY_LEFTALT], self.KEY_F4)               # new binding without Super
        self.assertTrue(self.closed(cl, w, 2.0))

    def test_modifiers_must_match_exactly(self):
        p, c = self.start('mellagui-key3', '[keys]\nSuper+Shift+X = close\n')
        cl, w = self.open_window('mellagui-key3')
        self.combo(c, [KEY_LEFTMETA], self.KEY_X)               # Super+X alone is not bound
        self.assertFalse(self.closed(cl, w, 0.5))
        self.combo(c, [KEY_LEFTMETA, KEY_LEFTSHIFT], self.KEY_X)
        self.assertTrue(self.closed(cl, w, 2.0))

    def test_workspace_binding_with_a_number(self):
        p, c = self.start('mellagui-key4', '[keys]\nAlt+2 = workspace 3\n')
        self.combo(c, [KEY_LEFTALT], KEY_2)
        self.assertEqual(c.windows()['workspace'], 2)

    def test_bad_lines_are_ignored(self):
        p, c = self.start('mellagui-key5', '[keys]\nSuper+ = close\nSuper+Q = nosuchaction\nSuper+Q = workspace 9\n'
                          'Super+Foo = close\nSuper+Y = close now\nSuper+Z\nCtrl+Alt+T = terminal\n')
        t = self.table(c)
        self.assertIn('Super+Q = close', t)                     # the default survives every rejected line
        self.assertIn('Ctrl+Alt+T = terminal', t)
        self.assertEqual(len(t), 33)
        self.assertTrue(alive_p(p))


class TestMetrics(Base):
    """[windows] / [panel] / [gadgets] / [notifications] in mellagui.conf: the sizes of the shell."""

    def start(self, name, text):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', path)
        c.sync()
        return p, c

    def maximized(self, c, sock):
        cl = Client(sock)
        self.clients.append(cl)
        w = Window(cl, 300, 200, title='m')
        cl.wl.roundtrip()
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_M)
        c.key(KEY_LEFTMETA, False)
        w.handle_configure()
        cl.wl.roundtrip()
        return c.windows()['windows'][0]

    def test_defaults_without_a_file(self):
        p, c, stop = self.private_compositor('mellagui-met0', '--config', 'none')
        r = self.maximized(c, 'mellagui-met0')
        self.assertEqual((r['w'], r['h']), (800 - 8, 500 - 32 - 28 - 4))

    def test_panel_and_frame_sizes_change_the_maximized_window(self):
        p, c = self.start('mellagui-met1', '[panel]\nheight = 40\n[windows]\ntitlebar-height = 32\nborder-width = 6\n')
        r = self.maximized(c, 'mellagui-met1')
        self.assertEqual((r['w'], r['h']), (800 - 12, 500 - 40 - 32 - 6))

    def test_out_of_range_and_bad_values_keep_the_default(self):
        p, c = self.start('mellagui-met3', '[panel]\nheight = 5\nrun-width = wide\n[windows]\nnosuch = 3\n[gadgets]\nwidth = 99999\n')
        r = self.maximized(c, 'mellagui-met3')
        self.assertEqual((r['w'], r['h']), (800 - 8, 500 - 32 - 28 - 4))
        self.assertTrue(alive_p(p))

    def test_buttons_stay_inside_a_low_title_bar(self):
        p, c = self.start('mellagui-met4', '[windows]\ntitlebar-height = 22\nbutton-size = 40\n')
        r = self.maximized(c, 'mellagui-met4')
        self.assertEqual(r['h'], 500 - 32 - 22 - 4)
        self.assertTrue(alive_p(p))

    def test_print_config_has_every_section(self):
        out = subprocess.run([BIN, '--print-config'], capture_output=True, text=True, timeout=10).stdout
        for sec in ('[windows]', '[panel]', '[gadgets]', '[notifications]', '[keys]', '[theme]', '[dock]', '[dock-apps]'):
            self.assertIn(sec, out)
        self.assertRegex(out, r'run-width = 44')
        self.assertRegex(out, r'scrollbar-width = 14')


class TestReload(Base):
    """Reading mellagui.conf again while MellaGUI runs: control `reload`, SIGUSR2 and the reload-config shortcut."""

    KEY_R = 19

    def start(self, name, text, *extra):
        self.path = os.path.join(tmp, name + '.conf')
        self.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path, *extra)
        c.sync()
        return p, c

    def write(self, text):
        with open(self.path, 'w') as f:
            f.write(text)

    @staticmethod
    def menu_px(c, name):                                                    # the panel colour, seen in the About dialog's card
        c.click(16, 14)
        c.abs(10, 10)
        c.sync()
        px = c.dump(os.path.join(tmp, name + '.ppm')).px(150, 250)
        c.tap(KEY_ESC)
        c.sync()
        return px

    def test_theme_dock_keys_and_sizes_change_without_a_restart(self):
        p, c = self.start('mellagui-rl1', '[theme]\nbackground = #102030\n[keys]\nSuper+T = workspace 2\n'
                          '[dock-apps]\nOne = echo 1\n')
        self.assertEqual(self.menu_px(c, 'rl1a'), (0x10, 0x20, 0x30))
        self.write('[theme]\nbackground = #304050\n[keys]\nSuper+U = workspace 3\n[dock-apps]\nTwo = echo 2\nThree = echo 3\n'
                   '[dock]\nicon-size = 64\n')
        c.cmd('reload')
        self.assertEqual(c.last, 'ok 0')
        self.assertEqual(self.menu_px(c, 'rl1b'), (0x30, 0x40, 0x50))
        keys = [l[4:] for l in c.cmd('keys')]
        self.assertIn('Super+U = workspace 3', keys)
        self.assertNotIn('Super+T = workspace 2', keys)
        self.assertIn('Super+Q = close', keys)                               # defaults are back under the file
        self.assertEqual(dock_apps(c), ['Two|echo 2', 'Three|echo 3'])

    def test_maximized_windows_follow_the_new_panel_height(self):
        p, c = self.start('mellagui-rl2', '')
        cl = Client('mellagui-rl2')
        self.clients.append(cl)
        w = Window(cl, 300, 200, title='m')
        cl.wl.roundtrip()
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_M)
        c.key(KEY_LEFTMETA, False)
        w.handle_configure()
        cl.wl.roundtrip()
        self.assertEqual(c.windows()['windows'][0]['h'], 500 - 32 - 28 - 4)
        self.write('[panel]\nheight = 40\n')
        c.cmd('reload')
        cfg = w.handle_configure()
        self.assertIsNotNone(cfg)
        cl.wl.roundtrip()
        self.assertEqual(c.windows()['windows'][0]['h'], 500 - 40 - 28 - 4)

    def test_command_line_stays_and_a_removed_section_returns_to_the_defaults(self):
        p, c = self.start('mellagui-rl3', '[dock-apps]\nFile = echo f\n[theme]\nbackground = #102030\n', '--terminal', 'echo t')
        self.assertEqual(dock_apps(c), ['File|echo f'])
        self.write('# nothing\n')
        c.cmd('reload')
        self.assertEqual(dock_apps(c)[0].split('|')[0], 'Launcher')                # the built-in dock is back
        self.assertEqual(dict(l.split('=', 1) for l in c.cmd('commands'))['terminal'], 'echo t')      # what the command line set stays
        self.assertEqual(self.menu_px(c, 'rl3'), (0x14, 0x17, 0x1f))

    def test_bad_lines_are_counted_and_no_file_is_an_error(self):
        p, c = self.start('mellagui-rl4', '')
        self.write('[theme]\nnosuch = 1\naccent = zzz\n')
        c.cmd('reload')
        self.assertEqual(c.last, 'ok 2')
        p2, c2, stop = self.private_compositor('mellagui-rl5', '--config', 'none')
        with self.assertRaises(RuntimeError):
            c2.cmd('reload')
        self.assertTrue(alive_p(p2))

    def test_sigusr2_and_the_shortcut_reload(self):
        p, c = self.start('mellagui-rl6', '[dock-apps]\nA = echo a\n')
        self.write('[dock-apps]\nB = echo b\n')
        p.send_signal(signal.SIGUSR2)
        for _ in range(100):
            if dock_apps(c) == ['B|echo b']:
                break
            time.sleep(0.05)
        self.assertEqual(dock_apps(c), ['B|echo b'])
        self.write('[dock-apps]\nC = echo c\n')
        c.key(KEY_LEFTMETA, True)
        c.key(KEY_LEFTSHIFT, True)
        c.tap(self.KEY_R)
        c.key(KEY_LEFTSHIFT, False)
        c.key(KEY_LEFTMETA, False)
        c.sync()
        self.assertEqual(dock_apps(c), ['C|echo c'])


class TestConfigSettings(Base):
    """The settings keys behind the panel's Config page: config-file, config-status, config-reload, config-create."""

    def get(self, c, key):
        c.cmd('setting ' + key)
        return c.last[3:]

    def test_no_file(self):
        p, c, stop = self.private_compositor('mellagui-cs1', '--config', 'none')
        self.assertEqual(self.get(c, 'config-file'), '')
        self.assertEqual(self.get(c, 'config-status'), 'none')
        with self.assertRaises(RuntimeError):
            c.cmd('settings config-create 1')
        with self.assertRaises(RuntimeError):
            c.cmd('settings config-reload 1')

    def test_create_the_default_file_then_reload_it(self):
        path = os.path.join(tmp, 'cs-new', 'dir', 'mellagui.conf')
        p, c, stop = self.private_compositor('mellagui-cs2', '--config', path)
        self.assertEqual(self.get(c, 'config-file'), path)
        self.assertEqual(self.get(c, 'config-status'), 'missing')
        c.cmd('settings config-create 1')
        self.assertEqual(self.get(c, 'config-status'), 'ok')
        text = open(path).read()
        for want in ('[dock-apps]', '[keys]', '[theme]', '[windows]'):
            self.assertIn(want, text)
        self.assertRegex(text, r'Super\+Shift\+R\s+= reload-config')
        with self.assertRaises(RuntimeError):                                  # never over an existing file
            c.cmd('settings config-create 1')
        with open(path, 'a') as f:
            f.write('\n[theme]\nbogus = 1\n')
        c.cmd('settings config-reload 1')
        self.assertEqual(self.get(c, 'config-status'), 'ignored:1')
        with open(path, 'w') as f:
            f.write('')
        c.cmd('settings config-reload 1')
        self.assertEqual(self.get(c, 'config-status'), 'ok')

    def test_the_created_file_is_the_default_configuration(self):
        path = os.path.join(tmp, 'cs3.conf')
        p, c, stop = self.private_compositor('mellagui-cs3', '--config', path)
        c.cmd('settings config-create 1')
        before = [l for l in c.cmd('keys')] + dock_apps(c)
        c.cmd('reload')
        self.assertEqual(c.last, 'ok 0')
        self.assertEqual(sorted(before), sorted([l for l in c.cmd('keys')] + dock_apps(c)))

    def test_the_settings_panel_config_page_creates_and_reloads(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        path = os.path.join(tmp, 'cs4.conf')
        p, c, stop = self.private_compositor('mellagui-cs4', '--config', path)
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-cs4'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 6 * 32 + 16)                         # the Config tab (7th)
        time.sleep(0.3)
        button = (x0 + 160 + 100, y0 + 14 + 3 * 28 + 6 + 12)            # first button below the three lines
        self.assertFalse(os.path.exists(path))
        c.click(*button)                                                # "Create it with the defaults"
        for _ in range(60):
            if os.path.exists(path):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(path))
        time.sleep(0.3)
        with open(path, 'w') as f:
            f.write('[dock-apps]\nOnly = echo only\n')
        c.click(x0 + 160 + 100, y0 + 14 + 3 * 28 + 6 + 28 + 4 + 12)     # "Reload" (the Edit button is above it now)
        for _ in range(60):
            if dock_apps(c) == ['Only|echo only']:
                break
            time.sleep(0.05)
        self.assertEqual(dock_apps(c), ['Only|echo only'])


class TestSaver(Base):
    """The screensaver: after idle time the compositor starts mellagui-matrix (a full screen client); the first input ends it."""

    UNIT = {'MELLAGUI_LOCK_UNIT_MS': '250'}                  # a "minute" is a quarter of a second

    def start(self, name, *args, env=None):
        exe = os.path.join(ROOT, 'build', 'mellagui-matrix')
        if not os.path.exists(exe):
            self.skipTest('mellagui-matrix not built')
        p, c, stop = self.private_compositor(name, *args, env=dict(self.UNIT, **(env or {})))
        c.sync()
        return p, c

    @staticmethod
    def saver(c):
        return dict(kv.split('=') for kv in c.cmd('saver')[0].split()[1:])

    def wait(self, pred, what, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return
            time.sleep(0.05)
        self.fail('timeout: ' + what)

    def matrix_windows(self, c):
        return [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.matrix']

    def test_defaults(self):
        p, c = self.start('mellagui-sv1', '--config', 'none')
        self.assertEqual(self.saver(c), {'running': '0', 'enabled': '1', 'minutes': '10'})

    def test_it_starts_after_the_idle_time_full_screen_and_the_first_input_ends_it(self):
        p, c = self.start('mellagui-sv2', '--config', 'none')
        c.cmd('settings saver-minutes 2')                       # 2 * 250 ms of silence
        self.wait(lambda: self.saver(c)['running'] == '1', 'the screensaver starts')
        self.wait(lambda: self.matrix_windows(c), 'its window')
        w = self.matrix_windows(c)[0]
        self.assertIn('fullscreen', w['flags'])
        self.assertEqual((w['x'], w['y'], w['w'], w['h']), (0, 0, 800, 500))
        time.sleep(0.5)
        a = c.dump(os.path.join(tmp, 'sv2a.ppm'))
        time.sleep(0.5)
        b = c.dump(os.path.join(tmp, 'sv2b.ppm'))
        greens = [(x, y) for x in range(0, 800, 3) for y in range(60, 440, 3) if a.px(x, y)[1] > a.px(x, y)[0] + 30]
        self.assertGreater(len(greens), 40, 'no green code on the screen')
        self.assertNotEqual(a.data, b.data, 'the rain does not move')
        self.assertEqual(a.px(2, 2), (0, 0, 0))                  # no panel, no dock over it
        c.tap(KEY_A)                                             # any key
        self.wait(lambda: self.saver(c)['running'] == '0' and not self.matrix_windows(c), 'it ends')

    def test_the_screensaver_follows_a_monitor_that_goes_away_and_comes_back(self):
        """A KVM switch: the monitor leaves, comes back, maybe in another mode; the screensaver must fill it again."""
        p, c = self.start('mellagui-sv-kvm1', '--config', 'none')
        c.cmd('saver start')
        self.wait(lambda: self.matrix_windows(c), 'its window')
        c.cmd('unplug')
        self.assertEqual(c.cmd('outputs'), [])
        time.sleep(0.3)
        self.assertEqual(len(self.matrix_windows(c)), 1, 'the screensaver went away with the monitor')
        c.cmd('replug 1000x600')
        self.wait(lambda: [(w['x'], w['y'], w['w'], w['h']) for w in self.matrix_windows(c)] == [(0, 0, 1000, 600)], 'the saver fits the bigger monitor')
        time.sleep(0.5)
        a = c.dump(os.path.join(tmp, 'svkvm1.ppm'))
        self.assertEqual((a.w, a.h), (1000, 600))
        right = [(x, y) for x in range(850, 1000, 3) for y in range(0, 600, 3) if a.px(x, y)[1] > a.px(x, y)[0] + 30]
        self.assertGreater(len(right), 10, 'no green code on the new part of the screen')
        c.cmd('unplug')
        c.cmd('replug 640x480')                                  # and a smaller one
        self.wait(lambda: [(w['x'], w['y'], w['w'], w['h']) for w in self.matrix_windows(c)] == [(0, 0, 640, 480)], 'the saver fits the smaller monitor')
        self.assertIn('fullscreen', self.matrix_windows(c)[0]['flags'])
        self.assertTrue(alive())

    def test_a_screensaver_that_starts_while_the_monitor_is_away(self):
        """The saver starts on a computer nobody looks at: no output, so no size. The monitor coming back gives it one."""
        p, c = self.start('mellagui-sv-kvm2', '--config', 'none')
        c.cmd('unplug')
        c.cmd('saver start')
        self.wait(lambda: self.matrix_windows(c), 'its window')
        time.sleep(0.4)
        c.cmd('replug 900x540')
        self.wait(lambda: [(w['x'], w['y'], w['w'], w['h']) for w in self.matrix_windows(c)] == [(0, 0, 900, 540)], 'the saver fills the monitor that came back')
        time.sleep(0.5)
        a = c.dump(os.path.join(tmp, 'svkvm2.ppm'))
        self.assertEqual(a.px(2, 2), (0, 0, 0))                  # no panel over it
        c.tap(KEY_A)
        self.wait(lambda: self.saver(c)['running'] == '0' and not self.matrix_windows(c), 'it ends')

    def test_the_first_input_is_not_passed_on(self):
        p, c = self.start('mellagui-sv3', '--config', 'none')
        cl = Client('mellagui-sv3')
        self.clients.append(cl)
        seat = cl.wl.bind('wl_seat', 5)
        kb = cl.wl.new_id('wl_keyboard')
        cl.wl.request(seat, 'get_keyboard', kb)
        win = Window(cl, 200, 100, app_id='below')
        cl.wl.roundtrip()
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'enter', 2.0), 'no keyboard focus before')
        c.sync()
        c.cmd('saver start')
        self.wait(lambda: self.matrix_windows(c), 'its window')
        cl.wl.queue.clear()
        c.tap(KEY_A)                                             # ends the screensaver ...
        self.wait(lambda: not self.matrix_windows(c), 'it ends')
        cl.wl.roundtrip()
        self.assertEqual([e for e in cl.wl.queue if e.obj == kb and e.name == 'key'], [], 'the key that ended it reached the window below')
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'enter', 2.0), 'the window below gets the focus back')
        cl.wl.queue.clear()
        c.tap(KEY_A)                                             # ... the next one is typed as usual
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == kb and e.name == 'key' and e.args[3] == 1, 2.0))

    def test_a_pointer_movement_and_a_click_end_it_too(self):
        p, c = self.start('mellagui-sv4', '--config', 'none')
        for how in ('abs', 'click'):
            c.cmd('saver start')
            self.wait(lambda: self.matrix_windows(c), 'its window')
            time.sleep(0.3)
            if how == 'abs':
                c.abs(100, 100)
                c.abs(300, 200)
            else:
                c.cmd('button 272 1')
                c.cmd('button 272 0')
            self.wait(lambda: self.saver(c)['running'] == '0', 'it ends after %s' % how)

    def test_switched_off_it_never_starts_and_zero_minutes_is_never(self):
        p, c = self.start('mellagui-sv5', '--config', 'none')
        c.cmd('settings saver 0')
        c.cmd('settings saver-minutes 1')
        time.sleep(2.2)
        self.assertEqual(self.saver(c)['running'], '0')
        c.cmd('settings saver 1')
        c.cmd('settings saver-minutes 0')
        time.sleep(2.2)
        self.assertEqual(self.saver(c)['running'], '0')
        with self.assertRaises(RuntimeError):
            c.cmd('settings saver-minutes 1000')
        with self.assertRaises(RuntimeError):
            c.cmd('settings saver maybe')

    def test_a_fullscreen_window_keeps_it_away(self):
        p, c = self.start('mellagui-sv6', '--config', 'none')
        cl = Client('mellagui-sv6')
        self.clients.append(cl)
        win = Window(cl, 200, 100, app_id='film')
        cl.wl.roundtrip()
        c.key(KEY_LEFTMETA, True)
        c.tap(KEY_F)                                             # Super+F: fullscreen
        c.key(KEY_LEFTMETA, False)
        self.wait(lambda: 'fullscreen' in c.windows()['windows'][0]['flags'], 'fullscreen')
        c.cmd('settings saver-minutes 1')
        time.sleep(2.5)
        self.assertEqual(self.saver(c)['running'], '0')

    def test_the_command_can_be_changed(self):
        mark = os.path.join(tmp, 'saver-ran')
        if os.path.exists(mark):
            os.unlink(mark)
        path = os.path.join(tmp, 'sv7.conf')
        with open(path, 'w') as f:
            f.write('[commands]\nsaver = touch %s\n' % mark)
        p, c = self.start('mellagui-sv7', '--config', path)
        c.cmd('saver start')
        self.wait(lambda: os.path.exists(mark), 'the configured command ran')
        self.wait(lambda: self.saver(c)['running'] == '0', 'a program that ends by itself is noticed')

    def test_settings_survive_a_restart(self):
        conf = os.path.join(tmp, 'sv8.settings')
        if os.path.exists(conf):
            os.unlink(conf)
        p, c = self.start('mellagui-sv8', '--config', 'none', '--settings', conf)
        c.cmd('settings saver-minutes 25')
        c.cmd('settings saver 0')
        time.sleep(0.6)                                          # (saved 0.3 s after a change)
        c.cmd('quit')
        p.wait(timeout=5)
        p2, c2 = self.start('mellagui-sv8b', '--config', 'none', '--settings', conf)
        self.assertEqual(self.saver(c2), {'running': '0', 'enabled': '0', 'minutes': '25'})

    def test_the_program_checks_itself(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-matrix')
        if not os.path.exists(exe):
            self.skipTest('mellagui-matrix not built')
        r = subprocess.run([exe, '--selftest'], capture_output=True, text=True, timeout=20)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, 'mellagui-matrix selftest ok'), r.stderr)
        r = subprocess.run([exe, '--bogus'], capture_output=True, text=True, timeout=20)
        self.assertEqual(r.returncode, 2)

    def test_started_by_hand_in_a_window_it_runs_until_escape(self):
        p, c = self.start('mellagui-sv10', '--config', 'none')
        pr = subprocess.Popen([os.path.join(ROOT, 'build', 'mellagui-matrix'), '--windowed', '--speed', '200', '--seed', '5'],
                              env=dict(os.environ, WAYLAND_DISPLAY='mellagui-sv10'))
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        self.wait(lambda: self.matrix_windows(c), 'its window')
        w = self.matrix_windows(c)[0]
        self.assertNotIn('fullscreen', w['flags'])
        time.sleep(0.6)
        a = c.dump(os.path.join(tmp, 'sv10a.ppm'))
        time.sleep(0.4)
        b = c.dump(os.path.join(tmp, 'sv10b.ppm'))
        self.assertNotEqual(a.data, b.data)
        c.abs(w['x'] + 50, w['y'] + 50)                          # a window in a window does not end on pointer movement or letters
        c.abs(w['x'] + 300, w['y'] + 200)
        c.tap(KEY_A)
        time.sleep(0.3)
        self.assertIsNone(pr.poll())
        c.tap(KEY_ESC)
        self.wait(lambda: pr.poll() is not None, 'Escape ends it')

    def test_the_settings_panel_page_switches_it_and_shows_it(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        nodock = os.path.join(tmp, 'sv9-nodock.conf')                    # the dock floats over the lower part of the small screen, where the panel's tabs are
        with open(nodock, 'w') as f:
            f.write('[dock]\nenabled = no\n')
        p, c = self.start('mellagui-sv9', '--config', nodock)
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-sv9'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 11 * 32 + 16)                         # the Saver tab (12th; thirteen tabs share the height)
        time.sleep(0.3)
        c.click(x0 + 168, y0 + 14 + 28 + 14)                             # the check box
        self.wait(lambda: self.saver(c)['enabled'] == '0', 'the box switches it off')
        c.click(x0 + 168, y0 + 14 + 28 + 14)
        self.wait(lambda: self.saver(c)['enabled'] == '1', 'and on again')
        c.click(x0 + 160 + 115, y0 + 14 + 28 + 28 + 8 + 12)              # "Show it now"
        self.wait(lambda: self.saver(c)['running'] == '1', 'the button shows it')
        self.wait(lambda: self.matrix_windows(c), 'its window')
        c.tap(KEY_A)
        self.wait(lambda: self.saver(c)['running'] == '0', 'a key ends it')


class TestDock(Base):
    """The dock: round icons along the bottom of the screen ([dock] and [dock-apps] of mellagui.conf)."""

    BTN_MIDDLE = 274

    def start(self, name, text, *extra):
        self.path = os.path.join(tmp, name + '.conf')
        with open(self.path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path, *extra)
        c.sync()
        return c

    @staticmethod
    def dock(c):
        info, icons = None, []
        for l in c.cmd('dock'):
            m = re.match(r'dock shown=(\d) hover=(-?\d+) n=(\d+)(?: drag=(-?\d+) to=(-?\d+))?', l)
            if m:
                info = dict(shown=int(m[1]), hover=int(m[2]), n=int(m[3]), drag=int(m[4] or -1), to=int(m[5] or -1))
            m = re.match(r'icon (\d+) "(.*)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) running=(\d) launching=(\d)', l)
            if m:
                icons.append(dict(i=int(m[1]), name=m[2], x=int(m[3]), y=int(m[4]), w=int(m[5]), h=int(m[6]), running=int(m[7]),
                                  launching=int(m[8])))
        return info, icons

    def test_the_default_dock_has_the_own_programs(self):
        p, c, stop = self.private_compositor('mellagui-dk1', '--config', 'none')
        info, icons = self.dock(c)
        self.assertEqual(info['shown'], 1)
        names = [i['name'] for i in icons]
        self.assertEqual(names[:6], ['Launcher', 'Terminal', 'Files', 'Editor', 'Calculator', 'Images'])
        self.assertEqual(names[-1], 'Settings')

    def test_entries_geometry_and_options(self):
        c = self.start('mellagui-dk2', '[dock]\nicon-size = 40\ngap = 10\nmargin = 8\n[dock-apps]\nA = [terminal] true\nB = [images] true\n'
                                       'Gone = /no/such/program\nC = true\n')
        info, icons = self.dock(c)
        self.assertEqual([i['name'] for i in icons], ['A', 'B', 'C'])               # the one without a program is left out
        # 800x500, three 40 px icons 10 px apart, 8 px from the bottom and 8 px for the dots below the icons
        self.assertEqual([(i['x'], i['y'], i['w'], i['h']) for i in icons],
                        [(330, 444, 40, 40), (380, 444, 40, 40), (430, 444, 40, 40)])

    def test_the_dock_can_be_switched_off_and_bad_values_are_ignored(self):
        c = self.start('mellagui-dk3', '[dock]\nenabled = no\n')
        info, icons = self.dock(c)
        self.assertEqual((info['shown'], info['n'], icons), (0, 0, []))
        c = self.start('mellagui-dk4', '[dock]\nicon-size = 5\nzoom = 999\ngap = x\nnonsense = 1\nautohide = maybe\n')
        info, icons = self.dock(c)
        self.assertEqual((info['shown'], icons[0]['w']), (1, 48))                    # every value rejected: the built-ins stay

    def test_the_icon_under_the_pointer_grows_and_goes_back(self):
        c = self.start('mellagui-dk5', '[dock]\nicon-size = 40\n[dock-apps]\nA = true\nB = true\nC = true\n')
        c.abs(350, 464)                                                              # the middle of the first icon
        info, icons = self.dock(c)
        self.assertEqual(info['hover'], 0)
        self.assertEqual(icons[0]['w'], 64)                                          # 160 %
        self.assertGreater(icons[1]['w'], 40)                                        # its neighbour grows less
        self.assertLess(icons[1]['w'], icons[0]['w'])
        self.assertEqual(icons[0]['y'] + icons[0]['h'], 484)                         # they grow upwards from the same line
        c.abs(400, 100)
        info, icons = self.dock(c)
        self.assertEqual((info['hover'], [i['w'] for i in icons]), (-1, [40, 40, 40]))

    def test_no_zoom_when_it_is_100_percent(self):
        c = self.start('mellagui-dk6', '[dock]\nzoom = 100\n[dock-apps]\nA = true\n')
        c.abs(400, 460)
        info, icons = self.dock(c)
        self.assertEqual((info['hover'], icons[0]['w']), (0, 48))

    def test_a_click_starts_the_program(self):
        mark = os.path.join(tmp, 'dock-started')
        if os.path.exists(mark):
            os.unlink(mark)
        c = self.start('mellagui-dk7', '[dock]\nicon-size = 40\n[dock-apps]\nTouch = touch %s\n' % mark)
        c.click(400, 464)
        for _ in range(100):
            if os.path.exists(mark):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(mark))

    def test_a_running_program_gets_a_dot_and_a_click_focuses_its_window_or_starts_another(self):
        mark = os.path.join(tmp, 'dock-started2')
        if os.path.exists(mark):
            os.unlink(mark)
        c = self.start('mellagui-dk8', '[dock]\nicon-size = 40\n[dock-apps]\nTouch = touch %s\n' % mark)
        cl = Client('mellagui-dk8')
        self.clients.append(cl)
        win = Window(cl, 200, 100, app_id='touch', title='mine')
        cl.wl.roundtrip()
        other = Window(cl, 200, 100, app_id='other', title='other')        # the newer window has the focus
        cl.wl.roundtrip()
        c.sync()
        self.assertEqual(self.dock(c)[1][0]['running'], 1)
        focused = lambda: [w['title'] for w in c.windows()['windows'] if 'focused' in w['flags']]
        self.assertEqual(focused(), ['other'])
        c.click(400, 464)                                                            # its window is brought forward ...
        time.sleep(0.4)
        self.assertFalse(os.path.exists(mark))                                       # ... nothing is started
        self.assertEqual(focused(), ['mine'])
        c.click(400, 464)                                                            # it is in front already: another instance
        for _ in range(100):
            if os.path.exists(mark):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(mark))
        os.unlink(mark)
        c.click(400, 464, self.BTN_MIDDLE)                                           # the middle button always starts another one
        for _ in range(100):
            if os.path.exists(mark):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(mark))
        cl.wl.close()
        self.clients.remove(cl)
        for _ in range(50):
            if self.dock(c)[1][0]['running'] == 0:
                break
            time.sleep(0.05)
        self.assertEqual(self.dock(c)[1][0]['running'], 0)

    def test_an_icon_zooms_in_and_out_while_its_program_starts_and_stops_when_the_window_is_there(self):
        c = self.start('mellagui-dk15', '[dock]\nicon-size = 40\n[dock-apps]\nSlow = sleep 30\n', )
        base = self.dock(c)[1][0]
        self.assertEqual(base['launching'], 0)
        bottom, centre = base['y'] + base['h'], base['x'] + base['w'] / 2
        c.click(400, 464)                                              # starts `sleep`, which never opens a window
        c.abs(400, 100)                                                # (the pointer leaves: no magnifying to confuse the numbers)
        sizes = []
        for _ in range(50):
            icon = self.dock(c)[1][0]
            sizes.append(icon['w'])
            self.assertEqual(icon['launching'], 1)
            self.assertEqual(icon['w'], icon['h'])
            self.assertLessEqual(abs(icon['y'] + icon['h'] - bottom), 1)             # it stays on its foot ...
            self.assertLessEqual(abs(icon['x'] + icon['w'] / 2 - centre), 1)         # ... and in the middle of its place
            time.sleep(0.03)
        self.assertEqual(min(sizes), base['w'])                        # never smaller than it is ...
        self.assertGreaterEqual(max(sizes), base['w'] + 10)            # ... up to about a third bigger
        self.assertGreater(len(set(sizes)), 5)                         # and smooth: not just two states
        cl = Client('mellagui-dk15')                                   # a window of the program appears: the pulsing ends
        self.clients.append(cl)
        win = Window(cl, 200, 100, app_id='sleep')
        cl.wl.roundtrip()
        self.wait_for(lambda: self.dock(c)[1][0]['launching'] == 0, 'the pulsing ends with the window')
        self.assertEqual(self.dock(c)[1][0]['w'], base['w'])

    def test_the_zooming_gives_up_after_a_while(self):
        p, c, stop = self.private_compositor('mellagui-dk16', '--config', self.write_conf('dk16', '[dock-apps]\nSlow = sleep 30\n'),
                                             env={'MELLAGUI_DOCK_BOUNCE_MS': '700'})
        c.sync()
        icon = self.dock(c)[1][0]
        c.click(icon['x'] + icon['w'] // 2, icon['y'] + icon['h'] // 2)
        self.assertEqual(self.dock(c)[1][0]['launching'], 1)
        self.wait_for(lambda: self.dock(c)[1][0]['launching'] == 0, 'it gives up', timeout=3.0)

    def test_a_second_instance_zooms_until_its_own_window_is_there(self):
        c = self.start('mellagui-dk17', '[dock]\nicon-size = 40\n[dock-apps]\nSlow = sleep 30\n')
        cl = Client('mellagui-dk17')
        self.clients.append(cl)
        win = Window(cl, 200, 100, app_id='sleep')                     # one window of the program exists already
        cl.wl.roundtrip()
        c.sync()
        c.click(400, 464)                                              # its window is in front: this click starts another one
        time.sleep(0.3)
        self.assertEqual(self.dock(c)[1][0]['launching'], 1)
        win2 = Window(cl, 200, 100, app_id='sleep')
        cl.wl.roundtrip()
        self.wait_for(lambda: self.dock(c)[1][0]['launching'] == 0, 'the second window ends the zooming')

    @staticmethod
    def write_conf(name, text):
        path = os.path.join(tmp, name + '.conf')
        with open(path, 'w') as f:
            f.write(text)
        return path

    def wait_for(self, pred, what, timeout=4.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return
            time.sleep(0.04)
        self.fail('timeout: ' + what)

    def test_the_dock_is_above_the_windows_and_takes_the_click(self):
        c = self.start('mellagui-dk9', '[dock]\nicon-size = 40\n[dock-apps]\nA = true\n')
        cl = Client('mellagui-dk9')
        self.clients.append(cl)
        win = Window(cl, 780, 480, app_id='big')
        cl.wl.roundtrip()
        c.sync()
        c.windows()
        c.click(400, 464)                                                            # over the window's bottom part
        c.sync()
        self.assertEqual(c.windows()['windows'][0]['flags'] & {'focused'}, {'focused'})   # the window was not clicked through
        info, icons = self.dock(c)
        self.assertEqual(info['hover'], 0)

    def test_autohide_shows_the_dock_at_the_bottom_edge_only(self):
        c = self.start('mellagui-dk10', '[dock]\nautohide = yes\n[dock-apps]\nA = true\n')
        self.assertEqual(self.dock(c)[0]['shown'], 0)
        c.abs(400, 300)
        self.assertEqual(self.dock(c)[0]['shown'], 0)
        c.abs(400, 499)                                                              # the pointer touches the bottom edge
        self.assertEqual(self.dock(c)[0]['shown'], 1)
        c.abs(400, 300)
        time.sleep(1.0)
        self.assertEqual(self.dock(c)[0]['shown'], 0)

    def test_the_icons_are_painted_round_without_a_background(self):
        c = self.start('mellagui-dk11', '[dock]\nicon-size = 40\nzoom = 100\n[dock-apps]\nCalculator = [calculator] true\n')
        c.abs(10, 10)
        ppm = c.dump(os.path.join(tmp, 'dock11.ppm'))
        top = ppm.px(400, 464 - 16)                                                  # the orange disc, above the glyph
        self.assertTrue(top[0] > 220 and 120 < top[1] < 175 and top[2] < 60, top)
        corner = ppm.px(400 - 19, 444 + 1)                                           # the corner of the icon's square: only wallpaper
        self.assertFalse(corner[0] > 200 and corner[2] < 80, corner)
        left = [ppm.px(x, 464) for x in (330, 350, 370)]                             # the wallpaper left of the icon: no bar behind it
        self.assertTrue(all(abs(l[0] - left[0][0]) < 6 and abs(l[1] - left[0][1]) < 6 for l in left), left)

    def test_a_picture_icon_is_cut_to_a_circle(self):
        import zlib, struct
        png = os.path.join(tmp, 'dock-icon.png')
        raw = b''.join(b'\x00' + bytes([0, 200, 0, 255]) * 16 for _ in range(16))
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        with open(png, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
        c = self.start('mellagui-dk12', '[dock]\nicon-size = 40\nzoom = 100\n[dock-apps]\nPic = [%s] true\n' % png)
        c.abs(10, 10)
        ppm = c.dump(os.path.join(tmp, 'dock12.ppm'))
        mid = ppm.px(400, 464)
        self.assertTrue(abs(mid[0]) < 3 and abs(mid[1] - 200) < 3 and abs(mid[2]) < 3, mid)
        self.assertNotEqual(ppm.px(381, 445), mid)                                   # the corner of the square stays unpainted

    @staticmethod
    def write_png(path, rgba, size=16):
        import zlib, struct
        raw = b''.join(b'\x00' + bytes(rgba) * size for _ in range(size))
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

    def icon_start(self, name, text):
        root = os.path.join(tmp, name + '-icons')
        self.write_png(os.path.join(root, 'icons', 'hicolor', '48x48', 'apps', 'fooapp.png'), (200, 0, 0, 255))
        self.write_png(os.path.join(root, 'icons', 'hicolor', '256x256', 'apps', 'bigapp.png'), (0, 0, 200, 255), 32)
        self.write_png(os.path.join(root, 'icons', 'hicolor', '48x48', 'apps', 'bigapp.png'), (0, 200, 0, 255))   # (the 256 one wins)
        self.write_png(os.path.join(root, 'pixmaps', 'pixapp.png'), (200, 200, 0, 255))
        self.path = os.path.join(tmp, name + '.conf')
        with open(self.path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path, env={'MELLAGUI_ICON_PATH': root})
        c.sync()
        c.abs(10, 10)
        return c

    def test_an_installed_programs_own_icon_is_used(self):
        c = self.icon_start('mellagui-dk18', '[dock]\nicon-size = 40\nzoom = 100\n[dock-apps]\n'
                            'A = [theme:fooapp] true\nB = [bigapp] true\nC = [theme:nothere|pixapp] true\nD = [theme:nothere] true\nE = [nosuch] true\n')
        info, icons = self.dock(c)
        self.assertEqual([i['name'] for i in icons], ['A', 'B', 'C', 'D', 'E'])
        ppm = c.dump(os.path.join(tmp, 'dock18.ppm'))
        mid = lambda i: ppm.px(icons[i]['x'] + icons[i]['w'] // 2, icons[i]['y'] + icons[i]['h'] // 2)
        close = lambda a, b: all(abs(x - y) < 4 for x, y in zip(a, b))
        self.assertTrue(close(mid(0), (200, 0, 0)), mid(0))            # theme:NAME, hicolor 48x48
        self.assertTrue(close(mid(1), (0, 0, 200)), mid(1))            # a bare name that is no built-in icon; the 256 px file is preferred
        self.assertTrue(close(mid(2), (200, 200, 0)), mid(2))          # the first name that has an icon wins; pixmaps work
        for i in (3, 4):                                               # no such icon: the built-in one, no crash
            self.assertFalse(close(mid(i), (200, 0, 0)) or close(mid(i), (0, 0, 200)) or close(mid(i), (200, 200, 0)))
        self.assertTrue(alive())

    def test_the_default_dock_asks_for_the_icons_of_firefox_and_code(self):
        p, c, stop = self.private_compositor('mellagui-dk19', '--config', 'none')
        text = subprocess.run([BIN, '--print-config'], capture_output=True, text=True, timeout=10).stdout
        self.assertIn('Firefox    = [theme:firefox]', text)
        self.assertIn('Code       = [theme:vscode|code]', text)

    # ---- the right-click menu of a dock icon -------------------------------------------------------------------------------

    BTN_RIGHT = 273

    @staticmethod
    def menu(c):
        d = dict(entries=[])
        for l in c.cmd('dockmenu'):
            m = re.match(r'dockmenu open=(\d) item=(-?\d+) sel=(-?\d+)', l)
            if m:
                d.update(open=int(m[1]), item=int(m[2]), sel=int(m[3]))
            m = re.match(r'card x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) title="(.*)"', l)
            if m:
                d.update(card=tuple(int(v) for v in m.groups()[:4]), title=m[5])
            m = re.match(r'entry (\d) "(.*)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) enabled=(\d)', l)
            if m:
                d['entries'].append(dict(label=m[2], box=tuple(int(v) for v in m.groups()[2:6]), enabled=int(m[7])))
        return d

    def right_click(self, c, icon):
        c.abs(icon['x'] + icon['w'] // 2, icon['y'] + icon['h'] // 2)
        c.button(self.BTN_RIGHT, True)
        c.button(self.BTN_RIGHT, False)
        c.sync()

    @staticmethod
    def centre(box):
        return box[0] + box[2] // 2, box[1] + box[3] // 2

    def test_a_right_click_on_an_icon_opens_a_menu_and_esc_or_a_click_outside_closes_it(self):
        mark = os.path.join(tmp, 'dk20.mark')
        if os.path.exists(mark):
            os.unlink(mark)
        c = self.start('mellagui-dk20', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [files] touch %s\nBeta = true\n' % mark)
        icons = self.dock(c)[1]
        self.right_click(c, icons[0])                                   # the Launcher button has no menu
        self.assertEqual(self.menu(c)['open'], 0)
        self.right_click(c, icons[1])
        m = self.menu(c)
        self.assertEqual((m['open'], m['item'], m['title']), (1, 1, 'Alpha'))
        self.assertEqual([e['label'] for e in m['entries']], ['New window', 'Close all windows', 'Change...', 'Remove from dock'])
        self.assertEqual([e['enabled'] for e in m['entries']], [1, 0, 1, 1])        # no window: nothing to close
        card = m['card']
        self.assertLess(card[1] + card[3], icons[1]['y'])                # above the icon
        c.tap(1)                                                         # Esc
        self.assertEqual(self.menu(c)['open'], 0)
        self.right_click(c, icons[1])
        self.assertEqual(self.menu(c)['open'], 1)
        c.click(10, 300)                                                 # a click elsewhere closes it and nothing else happens
        self.assertEqual(self.menu(c)['open'], 0)
        time.sleep(0.3)
        self.assertFalse(os.path.exists(mark))

    def test_the_menu_follows_the_pointer_and_the_keys(self):
        c = self.start('mellagui-dk21', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = true\n')
        self.right_click(c, self.dock(c)[1][1])
        m = self.menu(c)
        c.abs(*self.centre(m['entries'][2]['box']))
        self.assertEqual(self.menu(c)['sel'], 2)
        c.abs(*self.centre(m['entries'][1]['box']))                      # the disabled entry is not lit
        self.assertEqual(self.menu(c)['sel'], -1)
        c.tap(108)                                                       # Down: the first usable entry
        self.assertEqual(self.menu(c)['sel'], 0)
        c.tap(108)                                                       # (entry 1 is skipped)
        self.assertEqual(self.menu(c)['sel'], 2)
        c.tap(103)                                                       # Up
        self.assertEqual(self.menu(c)['sel'], 0)

    def test_new_window_starts_another_instance(self):
        mark = os.path.join(tmp, 'dk22.mark')
        if os.path.exists(mark):
            os.unlink(mark)
        c = self.start('mellagui-dk22', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = touch %s\n' % mark)
        self.right_click(c, self.dock(c)[1][1])
        c.click(*self.centre(self.menu(c)['entries'][0]['box']))
        self.wait_for(lambda: os.path.exists(mark), 'the program was started')
        self.assertEqual(self.menu(c)['open'], 0)

    def test_close_all_windows_asks_every_window_of_the_program_to_close(self):
        c = self.start('mellagui-dk23', '[dock-apps]\nLauncher = [launcher] @launcher\nSlow = sleep 30\n')
        cl = Client('mellagui-dk23')
        self.clients.append(cl)
        w1 = Window(cl, 200, 100, app_id='sleep')
        w2 = Window(cl, 200, 100, app_id='sleep')
        other = Window(cl, 200, 100, app_id='else')
        cl.wl.roundtrip()
        c.sync()
        self.right_click(c, self.dock(c)[1][1])
        m = self.menu(c)
        self.assertEqual(m['entries'][1]['enabled'], 1)
        c.click(*self.centre(m['entries'][1]['box']))
        got = set()
        end = time.time() + 3
        while time.time() < end and len(got) < 2:
            for w in (w1, w2):
                if cl.wl.wait_for(lambda e, w=w: e.obj == w.toplevel and e.name == 'close', 0.2):
                    got.add(w.toplevel)
        self.assertEqual(len(got), 2, 'both windows of the program are asked to close')
        self.assertIsNone(cl.wl.wait_for(lambda e: e.obj == other.toplevel and e.name == 'close', 0.4), 'the other program is left alone')

    def test_change_opens_the_launchers_form_with_that_button(self):
        c = self.start('mellagui-dk24', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [files] true\nBeta = [editor] false\n')
        self.right_click(c, self.dock(c)[1][2])                          # Beta
        c.click(*self.centre(self.menu(c)['entries'][2]['box']))
        lines = c.cmd('launcher')
        self.assertIn('open=1', lines[0])
        self.assertIn('form=1', lines[0])
        self.assertIn('editing=1', lines[0])
        self.assertTrue(any(l.startswith('field 0') and 'text="Beta"' in l for l in lines), lines)
        self.assertTrue(any(l.startswith('field 1') and 'text="false"' in l for l in lines), lines)
        self.assertTrue(any(l.startswith('field 2') and 'text="editor"' in l for l in lines), lines)

    def test_remove_from_dock_moves_the_button_to_the_launcher(self):
        c = self.start('mellagui-dk25', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [files] true\nBeta = [editor] true\n')
        self.right_click(c, self.dock(c)[1][1])
        c.click(*self.centre(self.menu(c)['entries'][3]['box']))
        self.wait_for(lambda: [i['name'] for i in self.dock(c)[1]] == ['Launcher', 'Beta'], 'Alpha leaves the dock')
        text = open(self.path).read()
        self.assertIn('[launcher-apps]', text)
        self.assertIn('Alpha = [files] true', text.split('[launcher-apps]')[1])
        self.assertNotIn('Alpha', text.split('[launcher-apps]')[0])

    def test_the_menu_is_drawn_with_either_renderer(self):
        for rend in ('software', 'gl'):
            path = os.path.join(tmp, 'mellagui-dk26-%s.conf' % rend)
            with open(path, 'w') as f:
                f.write('[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = true\n')
            p, c, stop = self.private_compositor('mellagui-dk26-' + rend, '--config', path, '--renderer', rend)
            c.sync()
            c.abs(10, 10)
            before = c.dump(os.path.join(tmp, 'dk26-%s-a.ppm' % rend))
            self.right_click(c, self.dock(c)[1][1])
            m = self.menu(c)
            c.abs(10, 10)
            after = c.dump(os.path.join(tmp, 'dk26-%s-b.ppm' % rend))
            x, y, w, h = m['card']
            px = after.px(x + 8, y + h - 5)
            self.assertNotEqual(px, before.px(x + 8, y + h - 5), '%s: no menu on the screen' % rend)
            self.assertTrue(all(v < 70 for v in px), (rend, px))        # the dark card
            c.click(10, 300)
            gone = c.dump(os.path.join(tmp, 'dk26-%s-c.ppm' % rend))
            self.assertEqual(gone.px(x + 8, y + h - 5), before.px(x + 8, y + h - 5), '%s: the menu is still there' % rend)

    # ---- the plate under the logos of installed programs --------------------------------------------------------------------

    def logo_png(self, path, size=32):
        """A logo like Firefox's: transparent corners and edges, a red square in the middle."""
        import zlib, struct
        rows = []
        for y in range(size):
            row = b'\x00'
            for x in range(size):
                inside = size // 3 <= x < 2 * size // 3 and size // 3 <= y < 2 * size // 3
                row += bytes([200, 0, 0, 255]) if inside else bytes([0, 0, 0, 0])
            rows.append(row)
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))

    def plate_pixels(self, name, setting):
        png = os.path.join(tmp, name + '.png')
        self.logo_png(png)
        c = self.start(name, '[dock]\nicon-size = 48\nzoom = 100\n%s[dock-apps]\nLogo = [%s] true\n' % (setting, png))
        c.abs(10, 10)
        icon = self.dock(c)[1][0]
        ppm = c.dump(os.path.join(tmp, name + '.ppm'))
        cx, cy = icon['x'] + icon['w'] // 2, icon['y'] + icon['h'] // 2
        return c, ppm.px(cx, cy), ppm.px(cx + int(icon['w'] * 0.42), cy), ppm.px(icon['x'] - 12, cy)      # logo, plate edge, beside the icon

    def test_a_logo_with_transparent_corners_sits_on_a_round_plate(self):
        c, logo, plate, beside = self.plate_pixels('mellagui-dk27', '')
        self.assertTrue(logo[0] > 150 and logo[1] < 60, logo)                       # the red square of the logo
        self.assertTrue(all(v > 215 for v in plate), plate)                          # a light plate behind it (the default)
        self.assertNotEqual(plate, beside)
        c, logo, plate, beside = self.plate_pixels('mellagui-dk28', 'icon-background = dark\n')
        self.assertTrue(all(v < 90 for v in plate), plate)
        c, logo, plate, beside = self.plate_pixels('mellagui-dk29', 'icon-background = none\n')
        self.assertEqual(plate, beside, 'no plate: the wallpaper shows around the logo')
        c, logo, plate, beside = self.plate_pixels('mellagui-dk30', 'icon-background = #3060c0\n')
        self.assertTrue(plate[2] > plate[0] + 60, plate)                             # blue

    def test_a_picture_that_fills_its_square_gets_no_plate(self):
        png = os.path.join(tmp, 'dk31.png')
        self.write_png(png, (0, 200, 0, 255), 32)
        c = self.start('mellagui-dk31', '[dock]\nicon-size = 48\nzoom = 100\n[dock-apps]\nFull = [%s] true\n' % png)
        c.abs(10, 10)
        icon = self.dock(c)[1][0]
        ppm = c.dump(os.path.join(tmp, 'dk31.ppm'))
        px = ppm.px(icon['x'] + icon['w'] // 2 + int(icon['w'] * 0.42), icon['y'] + icon['h'] // 2)
        self.assertTrue(px[1] > 150 and px[0] < 60, px)                              # its own green all the way out, no light plate

    def test_the_plate_is_a_dock_setting(self):
        c = self.start('mellagui-dk32', '[dock-apps]\nA = true\n')
        c.cmd('setting dock-opts')
        self.assertIn('icon-background=light', c.last)
        c.cmd('setraw dock-set icon-background\x1fdark')
        self.wait_for(lambda: 'icon-background = dark' in open(self.path).read(), 'the setting is written')
        c.cmd('setting dock-opts')
        self.assertIn('icon-background=dark', c.last)
        c.cmd('setraw dock-set icon-background\x1f#a0b0c0')
        self.wait_for(lambda: 'icon-background = #a0b0c0' in open(self.path).read(), 'a colour is written')
        c.cmd('setraw dock-set icon-background\x1flight')                          # the built-in value needs no line
        self.wait_for(lambda: 'icon-background' not in open(self.path).read(), 'the built-in value drops its line')
        with self.assertRaises(RuntimeError):
            c.cmd('setraw dock-set icon-background\x1fpurple')

    def test_a_missing_picture_falls_back_to_a_built_in_icon(self):
        c = self.start('mellagui-dk13', '[dock-apps]\nEditor = [/no/such/file.png] true\nQ = [nosuchicon] true\n')
        info, icons = self.dock(c)
        self.assertEqual([i['name'] for i in icons], ['Editor', 'Q'])

    def test_reload_changes_the_dock(self):
        c = self.start('mellagui-dk14', '[dock-apps]\nA = true\n')
        self.assertEqual([i['name'] for i in self.dock(c)[1]], ['A'])
        with open(self.path, 'w') as f:
            f.write('[dock]\nicon-size = 64\n[dock-apps]\nB = true\nC = true\n')
        c.cmd('reload')
        info, icons = self.dock(c)
        self.assertEqual(([i['name'] for i in icons], icons[0]['w']), (['B', 'C'], 64))


class TestDockEdit(Base):
    """The dock-N / dock-put / dock-del / dock-move / dock-set / dock-reset settings rewrite [dock] and [dock-apps] of mellagui.conf only."""

    US = '\x1f'

    def start(self, name, text=None):
        self.path = os.path.join(tmp, name + '.conf')
        if text is None:
            if os.path.exists(self.path):
                os.unlink(self.path)
        else:
            with open(self.path, 'w') as f:
                f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path)
        c.sync()
        return c

    def put(self, c, idx, name, icon, cmd):
        c.cmd('setraw dock-put %d%s%s%s%s%s%s' % (idx, self.US, name, self.US, icon, self.US, cmd))

    def entries(self, c):
        out = []
        for i in range(16):
            c.cmd('setting dock-%d' % i)
            v = c.last[3:]
            if not v:
                break
            out.append(tuple(v.split(self.US)))
        return out

    def test_edits_keep_comments_and_other_sections(self):
        c = self.start('mellagui-de1', '# top\n[theme]\naccent = #ff0000\n\n[dock-apps]\n# mine\nOld = [files] echo old\nOther = echo other\n\n[keys]\nSuper+T = run\n')
        self.assertEqual(self.entries(c), [('Old', 'files', 'echo old'), ('Other', '', 'echo other')])
        self.put(c, -1, 'New', 'web', 'echo new')
        self.assertEqual(open(self.path).read(),
                         '# top\n[theme]\naccent = #ff0000\n\n[dock-apps]\n# mine\nOld = [files] echo old\nOther = echo other\nNew = [web] echo new\n\n[keys]\nSuper+T = run\n')
        c.cmd('setraw dock-move 2%s-1' % self.US)
        c.cmd('setraw dock-del 0')
        self.assertEqual(self.entries(c), [('New', 'web', 'echo new'), ('Other', '', 'echo other')])
        self.put(c, 1, 'Renamed', '', 'echo r || echo s')
        self.assertEqual(self.entries(c)[1], ('Renamed', '', 'echo r || echo s'))
        c.cmd('setraw dock-reset 1')
        self.assertEqual(open(self.path).read(), '# top\n[theme]\naccent = #ff0000\n\n[keys]\nSuper+T = run\n')
        self.assertEqual(self.entries(c)[0][0], 'Launcher')                          # the defaults are back

    def test_options_are_written_as_single_lines(self):
        c = self.start('mellagui-de2', '[dock]\n# look\nzoom = 200\n')
        c.cmd('setraw dock-set icon-size%s64' % self.US)
        c.cmd('setraw dock-set autohide%syes' % self.US)
        self.assertEqual(open(self.path).read(), '[dock]\n# look\nzoom = 200\nicon-size = 64\nautohide = yes\n')
        c.cmd('setting dock-opts')
        self.assertEqual(c.last[3:], 'enabled=1 autohide=1 icon-size=64 zoom=200 gap=10 margin=8 icon-background=light')
        c.cmd('setraw dock-set zoom%s160' % self.US)                                  # the built-in value needs no line
        self.assertNotIn('zoom', open(self.path).read())
        for bad in ('icon-size%s5', 'icon-size%sx', 'nonsense%s1', 'zoom%s300', 'enabled%smaybe'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('setraw dock-set ' + bad % self.US)

    def test_bad_values_are_refused(self):
        c = self.start('mellagui-de3', '[dock-apps]\nA = echo a\n')
        for idx, name, icon, cmd in ((-1, '', '', 'x'), (-1, 'x', '', ''), (-1, 'a=b', '', 'x'), (-1, '#c', '', 'x'), (-1, ' pad', '', 'x'),
                                     (-1, 'ok', '', ' lead'), (-1, 'a', '', 'dup of A'), (5, 'x', '', 'y'), (-2, 'x', '', 'y'),
                                     (-1, 'ok', 'a]b', 'x'), (-1, 'ok', '[x', 'x'), (-1, 'ok', ' x', 'x')):
            with self.assertRaises(RuntimeError, msg=(idx, name, icon, cmd)):
                self.put(c, idx, name, icon, cmd)
        for bad in ('dock-del 3', 'dock-del x', 'dock-move 0%s1' % self.US, 'dock-reset 0'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('setraw ' + bad)
        self.assertEqual(open(self.path).read(), '[dock-apps]\nA = echo a\n')

    def test_the_dock_is_limited_to_sixteen_buttons(self):
        c = self.start('mellagui-de4', '[dock-apps]\n' + ''.join('E%d = echo %d\n' % (i, i) for i in range(16)))
        self.assertEqual(len(self.entries(c)), 16)
        with self.assertRaises(RuntimeError):
            self.put(c, -1, 'Extra', '', 'echo x')

    def test_the_panel_dock_page_removes_a_button_and_toggles_the_dock(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-de5', '[dock-apps]\nA = true\nB = true\nC = true\n')
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-de5'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 10 * 32 + 16)                         # the Dock tab (11th)
        time.sleep(0.3)
        c.click(x0 + 160 + 440 - 78 + 52 + 12, y0 + 14 + 28 + 22 + 8 + 11)   # "x" of the first row
        for _ in range(60):
            if 'A = true' not in open(self.path).read():
                break
            time.sleep(0.05)
        self.assertEqual(open(self.path).read(), '[dock-apps]\nB = true\nC = true\n')
        c.click(x0 + 160 + 96 + 45, y0 + 14 + 28 + 11)                   # the "Look" part
        time.sleep(0.3)
        c.click(x0 + 160 + 10, y0 + 14 + 28 + 22 + 8 + 14)               # "Show the dock"
        for _ in range(60):
            if 'enabled' in open(self.path).read():
                break
            time.sleep(0.05)
        self.assertIn('[dock]\nenabled = no\n', open(self.path).read())
        c.cmd('dock')
        self.assertTrue(c.cmd('dock')[0].startswith('dock shown=0'))

    def test_the_panel_dock_page_takes_an_icon_written_with_brackets(self):
        """The rows show "[theme:firefox] command"; typing the icon the same way, brackets and all, is accepted (they are not part of it)."""
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-de6', '[dock-apps]\nA = true\nB = [files] true\n')
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-de6'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 10 * 32 + 16)                         # the Dock tab (11th)
        time.sleep(0.3)
        c.click(x0 + 214, y0 + 81)                                       # the name button of the first row (A): its values go into the form
        time.sleep(0.3)
        c.click(x0 + 443, y0 + 288)                                      # the icon field
        keys = {ch: k for k, ch in enumerate('qwertyuiop', 16)}
        keys.update({ch: k for k, ch in enumerate('asdfghjkl', 30)})
        keys.update({ch: k for k, ch in enumerate('zxcvbnm', 44)})
        for ch in '[theme:firefox]':
            if ch == '[':
                c.tap(26)
            elif ch == ']':
                c.tap(27)
            elif ch == ':':
                c.key(KEY_LEFTSHIFT, True)
                c.tap(39)
                c.key(KEY_LEFTSHIFT, False)
            else:
                c.tap(keys[ch])
        time.sleep(0.2)
        c.click(x0 + 565, y0 + 318)                                      # Add / Update
        for _ in range(60):
            if '[theme:firefox]' in open(self.path).read():
                break
            time.sleep(0.05)
        self.assertEqual(open(self.path).read(), '[dock-apps]\nA = [theme:firefox] true\nB = [files] true\n')


class TestKeysEdit(Base):
    """The settings panel's Keys page: keys-row-N / keys-set / keys-del / keys-reset / keys-capture rewrite [keys] of mellagui.conf only."""

    US = '\x1f'
    KEY_Z = 44

    def start(self, name, text=None):
        self.path = os.path.join(tmp, name + '.conf')
        if text is None:
            if os.path.exists(self.path):
                os.unlink(self.path)
        else:
            with open(self.path, 'w') as f:
                f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path)
        c.sync()
        return c

    def get(self, c, key):
        c.cmd('setting ' + key)
        return c.last[3:]

    def rows(self, c, page=0):
        c.cmd('settings keys-page %d' % page)
        out = []
        for i in range(10):
            v = self.get(c, 'keys-row-%d' % i)
            if not v:
                break
            out.append(tuple(v.split(self.US, 1)))
        return out

    def test_pages_list_the_table_in_order(self):
        c = self.start('mellagui-ke1')
        self.assertEqual(self.get(c, 'keys-count'), '32')
        first = self.rows(c, 0)
        self.assertEqual(len(first), 10)
        self.assertEqual(first[0], ('Super+Enter', 'terminal'))
        self.assertEqual(len(self.rows(c, 2)), 10)
        self.assertEqual(len(self.rows(c, 3)), 2)
        self.assertEqual(self.rows(c, 99), self.rows(c, 3))                   # a page past the end is the last one
        self.assertIn('close', self.get(c, 'keys-actions').split())
        self.assertNotIn('none', self.get(c, 'keys-actions').split())

    def test_set_del_reset_keep_comments_and_other_sections(self):
        c = self.start('mellagui-ke2', '# top\n[menu]\nA = echo a\n\n[keys]\n# mine\nSuper+T = run\n\n[theme]\naccent = #ff0000\n')
        c.cmd('setraw keys-set alt+f4%sclose' % self.US)
        self.assertEqual(open(self.path).read(),
                         '# top\n[menu]\nA = echo a\n\n[keys]\n# mine\nSuper+T = run\nAlt+F4 = close\n\n[theme]\naccent = #ff0000\n')
        self.assertIn('key Alt+F4 = close', c.cmd('keys'))
        c.cmd('setraw keys-set super+t%sexec echo hi there' % self.US)           # same combination: the line is rewritten in place
        self.assertIn('Super+T = exec echo hi there\n', open(self.path).read())
        self.assertEqual(open(self.path).read().count('Super+T'), 1)
        c.cmd('setraw keys-del Super+Q')                                         # a built-in one is switched off
        self.assertIn('Super+Q = none\n', open(self.path).read())
        self.assertNotIn('key Super+Q = close', c.cmd('keys'))
        c.cmd('setraw keys-set Super+Q%sclose' % self.US)                        # back to the built-in: our line goes away
        self.assertNotIn('Super+Q', open(self.path).read())
        self.assertIn('key Super+Q = close', c.cmd('keys'))
        c.cmd('setraw keys-del Alt+F4')                                          # an own one is dropped
        self.assertNotIn('Alt+F4', open(self.path).read())
        c.cmd('setraw keys-reset 1')
        self.assertEqual(open(self.path).read(), '# top\n[menu]\nA = echo a\n\n[theme]\naccent = #ff0000\n')
        self.assertEqual(self.get(c, 'keys-count'), '32')

    def test_the_file_and_section_are_created(self):
        c = self.start('mellagui-ke3')
        c.cmd('setraw keys-set Super+X%sexec true' % self.US)
        self.assertEqual(open(self.path).read(), '[keys]\nSuper+X = exec true\n')
        self.assertEqual(self.get(c, 'keys-count'), '33')

    def test_refused(self):
        c = self.start('mellagui-ke4', '[keys]\nSuper+T = run\n')
        for bad in ('keys-set Super+Foo%sclose' % self.US, 'keys-set Super+X%snosuch' % self.US, 'keys-set Super+X%sworkspace 9' % self.US,
                    'keys-set Super+X%snone' % self.US, 'keys-set Super+X%sexec' % self.US, 'keys-set Super+X', 'keys-set Super+%sclose' % self.US,
                    'keys-del Super+X', 'keys-del Super+Foo', 'keys-reset 0', 'keys-set Super+X%sclose now' % self.US):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('setraw ' + bad)
        self.assertEqual(open(self.path).read(), '[keys]\nSuper+T = run\n')
        p, c2, stop = self.private_compositor('mellagui-ke5', '--config', 'none')
        with self.assertRaises(RuntimeError):
            c2.cmd('setraw keys-set Super+X%sclose' % self.US)

    def test_capture_records_the_next_combination_and_swallows_it(self):
        c = self.start('mellagui-ke6')
        cl = Client('mellagui-ke6')
        self.clients.append(cl)
        w = Window(cl, 200, 120, title='w')
        cl.wl.roundtrip()
        self.assertEqual(self.get(c, 'keys-capture'), '0')
        c.cmd('settings keys-capture 1')
        self.assertEqual(self.get(c, 'keys-capture'), '1')
        for m in (KEY_LEFTMETA, KEY_LEFTSHIFT):
            c.key(m, True)
        c.tap(KEY_Q)                                                            # would close the window; it is only recorded
        for m in (KEY_LEFTSHIFT, KEY_LEFTMETA):
            c.key(m, False)
        c.sync()
        self.assertEqual(self.get(c, 'keys-capture'), '0')
        self.assertEqual(self.get(c, 'keys-captured'), 'Super+Shift+Q')
        self.assertIsNone(cl.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', 0.4))
        c.key(KEY_LEFTMETA, True)                                               # capture is over: Super+Q closes again
        c.tap(KEY_Q)
        c.key(KEY_LEFTMETA, False)
        self.assertIsNotNone(cl.wl.wait_for(lambda e: e.obj == w.toplevel and e.name == 'close', 2.0))
        c.cmd('settings keys-capture 1')                                        # Escape alone cancels
        c.tap(KEY_ESC)
        self.assertEqual((self.get(c, 'keys-capture'), self.get(c, 'keys-captured')), ('0', ''))

    def test_the_panel_keys_page_removes_and_records(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-ke7')
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-ke7'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 7 * 32 + 16)                          # the Keys tab (8th)
        time.sleep(0.3)
        # the first row is Super+Enter = terminal: its "x" switches the built-in shortcut off
        c.click(x0 + 160 + 440 - 12, y0 + 14 + 28 + 11)
        for _ in range(60):
            if os.path.exists(self.path):
                break
            time.sleep(0.05)
        time.sleep(0.3)
        self.assertIn('Super+Enter = none', open(self.path).read())
        self.assertNotIn('key Super+Enter = terminal', c.cmd('keys'))
        # record a key: the button below the ten rows, then a combination that would otherwise be a shortcut
        py = y0 + 14 + 28 + 10 * 22 + 4 + 22 + 6
        c.click(x0 + 160 + 60, py + 13)                                  # "Record a key..."
        time.sleep(0.2)
        self.assertEqual(self.get(c, 'keys-capture'), '1')
        c.key(KEY_LEFTMETA, True)
        c.tap(self.KEY_Z)
        c.key(KEY_LEFTMETA, False)
        time.sleep(0.3)
        self.assertEqual(self.get(c, 'keys-captured'), 'Super+Z')
        c.click(x0 + 160 + 158 + 40, py + 13)                            # the action field
        for ch in 'close':
            c.tap(KEYCODES[ch])
        c.click(x0 + 160 + 440 - 32, py + 13)                            # Add
        for _ in range(60):
            if 'key Super+Z = close' in c.cmd('keys'):
                break
            time.sleep(0.05)
        self.assertIn('key Super+Z = close', c.cmd('keys'))
        self.assertIn('Super+Z = close', open(self.path).read())

class TestThemeEdit(Base):
    """The settings panel's Colours page: theme-colors / theme-set / theme-del / theme-reset rewrite [theme] of mellagui.conf only."""

    US = '\x1f'

    def start(self, name, text=None):
        self.path = os.path.join(tmp, name + '.conf')
        if text is None:
            if os.path.exists(self.path):
                os.unlink(self.path)
        else:
            with open(self.path, 'w') as f:
                f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path)
        c.sync()
        return c

    def colours(self, c):
        c.cmd('setting theme-colors')
        return dict(t.split('=', 1) for t in c.last[3:].split())

    @staticmethod
    def menu_px(c, name):                                                    # the panel colour, seen in the About dialog's card
        c.click(16, 14)
        c.abs(10, 10)
        c.sync()
        px = c.dump(os.path.join(tmp, name + '.ppm')).px(150, 250)
        c.tap(KEY_ESC)
        c.sync()
        return px

    def test_lists_every_colour_and_marks_the_changed_ones(self):
        c = self.start('mellagui-te1', '[theme]\naccent = #ff0000\n')
        cols = self.colours(c)
        self.assertEqual(len(cols), 30)
        self.assertEqual(cols['accent'], '#ff0000*')
        self.assertEqual(cols['background'], '#14171f')

    def test_set_del_reset_keep_the_rest_of_the_file(self):
        c = self.start('mellagui-te2', '# top\n[keys]\nSuper+T = run\n\n[theme]\n# mine\naccent = #ff0000\n\n[panel]\nheight = 30\n')
        c.cmd('setraw theme-set background%s#102030' % self.US)
        text = open(self.path).read()
        self.assertEqual(text, '# top\n[keys]\nSuper+T = run\n\n[theme]\n# mine\naccent = #ff0000\nbackground = #102030\n\n[panel]\nheight = 30\n')
        self.assertEqual(self.menu_px(c, 'te2a'), (0x10, 0x20, 0x30))                    # the compositor repainted at once
        self.assertEqual(self.colours(c)['background'], '#102030*')
        c.cmd('setraw theme-set accent%s#6fa8ff' % self.US)                              # the built-in value: no line is needed
        self.assertNotIn('accent', open(self.path).read())
        c.cmd('setraw theme-set text%s#abc' % self.US)                                   # the short form is written in full
        self.assertIn('text = #aabbcc\n', open(self.path).read())
        c.cmd('setraw theme-del background')
        self.assertNotIn('background', open(self.path).read())
        self.assertEqual(self.menu_px(c, 'te2b'), (0x14, 0x17, 0x1f))
        c.cmd('setraw theme-reset 1')
        self.assertEqual(open(self.path).read(), '# top\n[keys]\nSuper+T = run\n\n[panel]\nheight = 30\n')

    def test_the_file_and_section_are_created(self):
        c = self.start('mellagui-te3')
        c.cmd('setraw theme-set frame%s#112233' % self.US)
        self.assertEqual(open(self.path).read(), '[theme]\nframe = #112233\n')

    def test_refused(self):
        c = self.start('mellagui-te4', '[theme]\naccent = #ff0000\n')
        for bad in ('theme-set nosuch%s#000000' % self.US, 'theme-set accent%szzz' % self.US, 'theme-set accent%s#12345' % self.US,
                    'theme-set accent', 'theme-del nosuch', 'theme-del accent%s1' % self.US, 'theme-reset 0'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('setraw ' + bad)
        self.assertEqual(open(self.path).read(), '[theme]\naccent = #ff0000\n')
        p, c2, stop = self.private_compositor('mellagui-te5', '--config', 'none')
        with self.assertRaises(RuntimeError):
            c2.cmd('setraw theme-set accent%s#000000' % self.US)

    def test_the_panel_colours_page_changes_a_colour(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-te6')
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-te6'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 8 * 32 + 16)                          # the Colours tab (9th)
        time.sleep(0.3)
        # row 11 of the palette is "background": page 2 (8 per page), third row
        c.click(x0 + 160 + 24, y0 + 14 + 28 + 11)                         # first row: wallpaper-top, to be sure a click selects
        time.sleep(0.2)
        c.click(x0 + 160 + 32 + 30, y0 + 14 + 28 + 11)                    # its name button
        time.sleep(0.2)
        form_y = y0 + 14 + 28 + 8 * 22 + 4 + 22 + 6
        c.click(x0 + 160 + 34 + 50, form_y + 13)                          # the hex field (the caret is where it was clicked)
        c.tap(107)                                                         # End
        for _ in range(8):
            c.tap(14)                                                      # Backspace x8
        for ch in '102030':
            c.tap(KEYCODES[ch])
        c.click(x0 + 160 + 142 + 32, form_y + 13)                         # Apply
        for _ in range(60):
            if 'wallpaper-top' in open(self.path).read() if os.path.exists(self.path) else False:
                break
            time.sleep(0.05)
        self.assertIn('wallpaper-top = #102030', open(self.path).read())
        self.assertEqual(self.colours(c)['wallpaper-top'], '#102030*')

class TestMetricsEdit(Base):
    """The settings panel's Sizes page: metrics / metrics-set / metrics-reset rewrite the size sections of mellagui.conf only."""

    US = '\x1f'

    def start(self, name, text=None):
        self.path = os.path.join(tmp, name + '.conf')
        if text is None:
            if os.path.exists(self.path):
                os.unlink(self.path)
        else:
            with open(self.path, 'w') as f:
                f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path)
        c.sync()
        return c

    def metrics(self, c):
        c.cmd('setting metrics')
        return {k: tuple(int(x) for x in v.split(':')) for k, v in (t.split('=') for t in c.last[3:].split())}

    def test_lists_every_size_with_its_range(self):
        c = self.start('mellagui-me11', '[panel]\nheight = 36\n')
        m = self.metrics(c)
        self.assertEqual(len(m), 26)
        self.assertEqual(m['panel.height'], (36, 20, 64, 32))
        self.assertEqual(m['windows.scrollbar-width'], (14, 4, 40, 14))
        self.assertEqual(m['windows.border-width'], (4, 1, 16, 4))

    def test_set_and_reset_keep_the_rest_of_the_file(self):
        c = self.start('mellagui-me12', '# top\n[theme]\naccent = #ff0000\n\n[panel]\n# bar\nheight = 30\n\n[keys]\nSuper+T = run\n')
        c.cmd('setraw metrics-set panel.height%s40' % self.US)
        self.assertEqual(open(self.path).read(), '# top\n[theme]\naccent = #ff0000\n\n[panel]\n# bar\nheight = 40\n\n[keys]\nSuper+T = run\n')
        c.cmd('setraw metrics-set windows.border-width%s6' % self.US)                      # a new section is added at the end
        self.assertTrue(open(self.path).read().endswith('Super+T = run\n\n[windows]\nborder-width = 6\n'))
        self.assertEqual(self.metrics(c)['windows.border-width'][0], 6)
        c.cmd('setraw metrics-set panel.height%s32' % self.US)                              # the built-in size: the line goes
        self.assertNotIn('height = ', open(self.path).read())
        c.cmd('setraw metrics-reset 1')
        self.assertEqual(open(self.path).read(), '# top\n[theme]\naccent = #ff0000\n\n[panel]\n# bar\n\n[keys]\nSuper+T = run\n'
                         .replace('[panel]\n# bar\n\n', ''))
        self.assertEqual(self.metrics(c)['windows.border-width'][0], 4)

    def test_the_file_is_created(self):
        c = self.start('mellagui-me13')
        c.cmd('setraw metrics-set gadgets.margin%s20' % self.US)
        self.assertEqual(open(self.path).read(), '[gadgets]\nmargin = 20\n')

    def test_refused(self):
        c = self.start('mellagui-me14', '[panel]\nheight = 30\n')
        for bad in ('metrics-set panel.height%s5' % self.US, 'metrics-set panel.height%s65' % self.US, 'metrics-set panel.height%sx' % self.US,
                    'metrics-set panel.nosuch%s30' % self.US, 'metrics-set nosuch.height%s30' % self.US, 'metrics-set panel%s30' % self.US,
                    'metrics-set panel.height', 'metrics-reset 0'):
            with self.assertRaises(RuntimeError, msg=bad):
                c.cmd('setraw ' + bad)
        self.assertEqual(open(self.path).read(), '[panel]\nheight = 30\n')
        p, c2, stop = self.private_compositor('mellagui-me15', '--config', 'none')
        with self.assertRaises(RuntimeError):
            c2.cmd('setraw metrics-set panel.height%s30' % self.US)

    def test_the_panel_sizes_page_applies_a_slider_when_it_is_let_go(self):
        exe = os.path.join(ROOT, 'build', 'mellagui-settings')
        if not os.path.exists(exe):
            self.skipTest('mellagui-settings not built')
        c = self.start('mellagui-me16')
        pr = subprocess.Popen([exe], env=dict(os.environ, WAYLAND_DISPLAY='mellagui-me16'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (pr.poll() is None) and (pr.kill(), pr.wait()))
        win = None
        for _ in range(100):
            ws = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.settings']
            if ws:
                win = ws[0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(win)
        time.sleep(0.4)
        x0, y0 = win['x'], win['y']
        c.click(x0 + 70, y0 + 12 + 9 * 32 + 16)                          # the Sizes tab (10th)
        time.sleep(0.3)
        y = y0 + 14 + 28 + 13                                              # the first slider: windows.titlebar-height (20..64)
        c.abs(x0 + 160 + 200 + 10, y)                                      # drag from near the left end to the right end
        c.button(272, True)
        c.abs(x0 + 160 + 200 + 80, y)
        time.sleep(0.2)
        self.assertFalse(os.path.exists(self.path))                        # nothing is written while the slider is held
        c.abs(x0 + 160 + 200 + 170, y)
        c.button(272, False)
        for _ in range(60):
            if os.path.exists(self.path):
                break
            time.sleep(0.05)
        time.sleep(0.2)
        self.assertIn('titlebar-height = 64', open(self.path).read())
        self.assertEqual(self.metrics(c)['windows.titlebar-height'][0], 64)


class TestScreenshot(Base):
    """Print / Ctrl+Print / Alt+Print: PNG file (own encoder, no zlib in the compositor) and image/png on the clipboard."""
    KEY_PRINT = 99

    @staticmethod
    def decode_png(data):
        """(w, h, [rows of RGB bytes]) of an 8-bit RGB PNG with filter 0 rows; checks the CRCs and the Adler-32."""
        import zlib
        assert data[:8] == b'\x89PNG\r\n\x1a\n'
        off, idat, hdr = 8, b'', None
        while off < len(data):
            n, typ = struct.unpack_from('>I4s', data, off)
            body = data[off + 8:off + 8 + n]
            crc = struct.unpack_from('>I', data, off + 8 + n)[0]
            assert crc == zlib.crc32(typ + body), 'bad CRC in ' + repr(typ)
            if typ == b'IHDR':
                hdr = struct.unpack('>IIBBBBB', body)
            elif typ == b'IDAT':
                idat += body
            off += 12 + n
        w, h, depth, ctype = hdr[:4]
        assert (depth, ctype) == (8, 2)
        raw = zlib.decompress(idat)                  # raises on a wrong Adler-32 or stored-block header
        stride = 1 + w * 3
        assert len(raw) == stride * h
        rows = []
        for y in range(h):
            assert raw[y * stride] == 0
            rows.append(raw[y * stride + 1:(y + 1) * stride])
        return w, h, rows

    def start(self, name, *extra):
        self.shots = os.path.join(tmp, name + '-shots')
        p, c, stop = self.private_compositor(name, '--screenshot-dir', self.shots, *extra)
        return c, name

    def files(self):
        return sorted(os.listdir(self.shots)) if os.path.isdir(self.shots) else []

    def wait_files(self, n):
        for _ in range(100):
            if len(self.files()) >= n:
                break
            time.sleep(0.02)
        return self.files()

    def clipboard_client(self, sock):
        c = Client(sock)
        self.clients.append(c)
        seat = c.wl.bind('wl_seat', 5)
        mgr = c.wl.bind('wl_data_device_manager', 3)
        dev = c.wl.new_id('wl_data_device')
        c.wl.request(mgr, 'get_data_device', dev, seat)
        kb = c.wl.new_id('wl_keyboard')
        c.wl.request(seat, 'get_keyboard', kb)
        Window(c, 200, 150, title='paster')
        c.wl.roundtrip()
        return c, dev

    def paste_png(self, c, dev):
        """The image/png bytes of the current selection, or None when it is not offered."""
        offered = []
        sel = None
        deadline = time.time() + 3
        while time.time() < deadline:
            c.wl.pump(0.05)
            for e in list(c.wl.queue):
                if e.obj != dev:
                    if e.name == 'offer':
                        offered.append(e.args[0].decode() if isinstance(e.args[0], bytes) else e.args[0])
                    continue
                if e.name == 'selection':
                    sel = e
            if sel is not None:
                break
        if sel is None or not sel.args[0]:
            return None
        if 'image/png' not in [m.strip(b'\0').decode() if isinstance(m, bytes) else m for m in offered]:
            return None
        r, w = os.pipe()
        c.wl.request(sel.args[0], 'receive', 'image/png', fds=(w,))
        os.close(w)
        chunks = []
        while True:
            b = os.read(r, 65536)
            if not b:
                break
            chunks.append(b)
        os.close(r)
        return b''.join(chunks)

    def test_print_saves_a_png_equal_to_the_frame(self):
        c, name = self.start('mellagui-shot1')
        c.cmd('abs 780 480')                                       # pointer in a corner: the shot has no cursor anyway
        c.sync()
        time.sleep(0.15)                                           # a fresh compositor draws its first frame on the vblank timer
        c.sync()
        ppm = c.dump(os.path.join(tmp, 'shot1.ppm'))
        c.tap(self.KEY_PRINT)
        c.sync()
        files = self.wait_files(1)
        self.assertEqual(len(files), 1)
        self.assertRegex(files[0], r'^mellagui-\d{8}-\d{6}\.png$')
        path = os.path.join(self.shots, files[0])
        self.assertEqual(oct(os.stat(path).st_mode & 0o777), '0o600')
        with open(path, 'rb') as f:
            w, h, rows = self.decode_png(f.read())
        self.assertEqual((w, h), (800, 500))
        for y in range(h):
            for x in range(0, w, 7):
                if abs(x - 780) < 30 and abs(y - 480) < 30:        # cursor sprite is in the dump only
                    continue
                self.assertEqual(tuple(rows[y][x * 3:x * 3 + 3]), ppm.px(x, y), (x, y))

    def test_print_puts_the_png_on_the_clipboard(self):
        c, name = self.start('mellagui-shot2')
        cl, dev = self.clipboard_client(name)
        c.tap(self.KEY_PRINT)
        got = self.paste_png(cl, dev)
        self.assertIsNotNone(got, 'image/png was not offered')
        files = self.wait_files(1)
        with open(os.path.join(self.shots, files[0]), 'rb') as f:
            self.assertEqual(got, f.read())
        self.assertEqual(self.decode_png(got)[:2], (800, 500))

    def test_ctrl_print_is_clipboard_only(self):
        c, name = self.start('mellagui-shot3')
        cl, dev = self.clipboard_client(name)
        c.key(KEY_LEFTCTRL, True)
        c.tap(self.KEY_PRINT)
        c.key(KEY_LEFTCTRL, False)
        got = self.paste_png(cl, dev)
        self.assertIsNotNone(got)
        self.assertEqual(self.files(), [])

    def test_alt_print_is_the_focused_window_with_its_frame(self):
        c, name = self.start('mellagui-shot4')
        cl = Client(name)
        self.clients.append(cl)
        win = Window(cl, 300, 200, title='target')
        cl.wl.roundtrip()
        c.sync()
        c.key(KEY_LEFTALT, True)
        c.tap(self.KEY_PRINT)
        c.key(KEY_LEFTALT, False)
        files = self.wait_files(1)
        self.assertEqual(len(files), 1)
        with open(os.path.join(self.shots, files[0]), 'rb') as f:
            w, h, rows = self.decode_png(f.read())
        win_info = c.windows()['windows'][0]
        self.assertEqual((w, h), (win_info['w'] + 2 * 4, win_info['h'] + 28 + 4))   # LM_BORDER_WIDTH / LM_TITLEBAR_HEIGHT

    def test_the_keys_are_not_forwarded_to_clients(self):
        c, name = self.start('mellagui-shot5')
        cl = Client(name)
        self.clients.append(cl)
        seat = cl.wl.bind('wl_seat', 5)
        kb = cl.wl.new_id('wl_keyboard')
        cl.wl.request(seat, 'get_keyboard', kb)
        Window(cl, 200, 150, title='k')
        cl.wl.roundtrip()
        cl.wl.queue.clear()
        c.tap(self.KEY_PRINT)
        c.sync()
        cl.wl.roundtrip()
        self.assertEqual([e for e in cl.wl.queue if e.obj == kb and e.name == 'key'], [])

    def test_feedback_flash_and_thumbnail_card_come_and_go(self):
        self.shots = os.path.join(tmp, 'mellagui-shot7-shots')
        p, c, stop = self.private_compositor('mellagui-shot7', '--screenshot-dir', self.shots,
                                             env={'MELLAGUI_TOAST_MS': '700'})
        c.cmd('abs 20 20')
        c.sync()
        time.sleep(0.15)
        c.sync()
        before = c.dump(os.path.join(tmp, 'fx0.ppm'))
        wall = before.px(50, 300)
        card_px = (532, 400)                                       # 800x500: the card is at 528,294 256x190, 16 px from the corner
        wall_card = before.px(*card_px)
        c.tap(self.KEY_PRINT)
        c.sync()
        during = c.dump(os.path.join(tmp, 'fx1.ppm'))              # the white flash is at full strength
        self.assertTrue(all(a > b + 80 for a, b in zip(during.px(50, 300), wall)), (during.px(50, 300), wall))
        time.sleep(0.4)
        c.sync()
        card = c.dump(os.path.join(tmp, 'fx2.ppm'))                # the flash is over, the card stays
        self.assertEqual(card.px(50, 300), wall)
        self.assertTrue(all(abs(a - b) <= 14 for a, b in zip(card.px(*card_px), (0x16, 0x1a, 0x24))), card.px(*card_px))
        self.assertGreater(card.count((0xd8, 0xdd, 0xe8), 536, 456, 776, 472), 20)       # the file name under the thumbnail
        self.wait_files(1)
        c.tap(self.KEY_PRINT)                                      # a second shot while the card is up: the card is not in it
        files = self.wait_files(2)
        self.assertEqual(len(files), 2)
        with open(os.path.join(self.shots, files[-1]), 'rb') as f:
            w, h, rows = self.decode_png(f.read())
        self.assertEqual(tuple(rows[card_px[1]][card_px[0] * 3:card_px[0] * 3 + 3]), wall_card)
        time.sleep(1.2)
        c.sync()
        gone = c.dump(os.path.join(tmp, 'fx3.ppm'))
        self.assertEqual(gone.px(*card_px), wall_card)
        self.assertEqual(gone.px(50, 300), wall)

    def card_click_setup(self, name, *extra, env=None):
        self.shots = os.path.join(tmp, name + '-shots')
        p, c, stop = self.private_compositor(name, '--screenshot-dir', self.shots, *extra, env=dict(env or {}, MELLAGUI_TOAST_MS='6000'))
        c.cmd('abs 20 20')
        c.sync()
        time.sleep(0.15)
        c.sync()
        return c

    CARD_PX = (600, 380)                                       # inside the card of an 800x500 output (528,294 256x190)

    def test_clicking_the_card_opens_the_saved_file_in_the_viewer(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'mellagui-view')):
            self.skipTest('mellagui-view not built')
        c = self.card_click_setup('mellagui-shot8')
        before = c.dump(os.path.join(tmp, 'cc0.ppm')).px(*self.CARD_PX)
        c.tap(self.KEY_PRINT)
        c.sync()
        files = self.wait_files(1)
        time.sleep(0.4)
        c.sync()
        self.assertNotEqual(c.dump(os.path.join(tmp, 'cc1.ppm')).px(*self.CARD_PX), before)     # the card is up
        c.click(*self.CARD_PX)
        end = time.time() + 6
        win = None
        while time.time() < end and not win:
            win = [w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.view']
            time.sleep(0.05)
        self.assertTrue(win, 'the viewer did not open')
        self.assertEqual(win[0]['title'], files[0] + ' - Images')
        c.abs(20, 20)
        c.sync()
        self.assertEqual(c.windows()['windows'][0]['app_id'], 'mellagui.view')   # nothing else opened

    def test_clicking_a_clipboard_only_card_just_closes_it(self):
        c = self.card_click_setup('mellagui-shot9')
        before = c.dump(os.path.join(tmp, 'cd0.ppm')).px(*self.CARD_PX)
        c.key(KEY_LEFTCTRL, True)
        c.tap(self.KEY_PRINT)
        c.key(KEY_LEFTCTRL, False)
        time.sleep(0.4)
        c.sync()
        self.assertNotEqual(c.dump(os.path.join(tmp, 'cd1.ppm')).px(*self.CARD_PX), before)
        c.click(*self.CARD_PX)
        c.abs(20, 20)                                          # the cursor sprite is black
        time.sleep(0.4)
        c.sync()
        self.assertEqual(c.dump(os.path.join(tmp, 'cd2.ppm')).px(*self.CARD_PX), before)       # gone
        self.assertEqual([w for w in c.windows()['windows']], [])

    def test_the_click_does_not_reach_the_window_under_the_card(self):
        c = self.card_click_setup('mellagui-shot10')
        cl = Client('mellagui-shot10')
        self.clients.append(cl)
        seat = cl.wl.bind('wl_seat', 5)
        ptr = cl.wl.new_id('wl_pointer')
        cl.wl.request(seat, 'get_pointer', ptr)
        win = Window(cl, 780, 440, title='big')
        cl.wl.roundtrip()
        ctl_win = c.windows()['windows'][0]
        cx, cy = self.CARD_PX
        self.assertTrue(ctl_win['x'] < cx < ctl_win['x'] + ctl_win['w'] and ctl_win['y'] < cy < ctl_win['y'] + ctl_win['h'])
        c.tap(self.KEY_PRINT)
        time.sleep(0.4)
        c.sync()
        cl.wl.queue.clear()
        c.click(cx, cy)
        c.sync()
        cl.wl.roundtrip()
        self.assertEqual([e for e in cl.wl.queue if e.obj == ptr and e.name == 'button'], [])

    def test_the_card_is_not_shown_over_the_lock_screen_and_not_clickable(self):
        c = self.card_click_setup('mellagui-shot11', '--lock-file', os.path.join(tmp, 'shot11.lock'),
                                  env={'MELLAGUI_LOCK_ITER': '1000'})
        c.cmd('lock set - password hunter22x')
        c.tap(self.KEY_PRINT)
        time.sleep(0.4)
        c.sync()
        c.cmd('lock now')
        time.sleep(0.2)
        c.sync()
        locked = c.dump(os.path.join(tmp, 'ce1.ppm'))
        # the card's thumbnail ring colour (0x3a4256) must not be anywhere in the lock screen
        self.assertEqual(locked.count((0x3a, 0x42, 0x56), 520, 290, 790, 490), 0)
        c.click(*self.CARD_PX)
        time.sleep(0.3)
        self.assertEqual([w for w in c.windows()['windows'] if w['app_id'] == 'mellagui.view'], [])

    def test_the_first_cursor_move_leaves_no_ghost_behind(self):
        p, c, stop = self.private_compositor('mellagui-ghost')
        c.cmd('abs 700 300')
        c.sync()
        time.sleep(0.15)
        c.sync()
        ppm = c.dump(os.path.join(tmp, 'ghost.ppm'))
        self.assertEqual(ppm.count((0, 0, 0), 380, 230, 440, 290), 0, 'the cursor of the first frame was never erased')
        self.assertGreater(ppm.count((0, 0, 0), 695, 295, 725, 325), 10)       # the real one

    def test_without_a_directory_only_the_clipboard(self):
        name = 'mellagui-shot6'
        p, c, stop = self.private_compositor(name)
        cl, dev = self.clipboard_client(name)
        c.tap(self.KEY_PRINT)
        self.assertIsNotNone(self.paste_png(cl, dev))


class TestLauncher(Base):
    """The launcher dialog (Super+A, the dock's Launcher button): every dock button as a big icon, add and remove."""

    KEY_A, KEY_E, KEY_ESC, KEY_ENTER, KEY_TAB, KEY_LEFTMETA, KEY_RIGHT, KEY_DELETE = 30, 18, 1, 28, 15, 125, 106, 111

    def start(self, name, text=None):
        self.path = os.path.join(tmp, name + '.conf')
        self.mark = os.path.join(tmp, name + '.mark')
        if os.path.exists(self.mark):
            os.unlink(self.mark)
        if text is None:
            text = ('[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [terminal] true\nBeta = [files] touch %s\nGamma = [editor] true\n'
                    % self.mark)
        with open(self.path, 'w') as f:
            f.write(text)
        p, c, stop = self.private_compositor(name, '--config', self.path)
        c.sync()
        return c

    @staticmethod
    def info(c):
        d = dict(icons=[], buttons={}, fields={}, kinds={}, add=None)
        for l in c.cmd('launcher'):
            m = re.match(r'launcher open=(\d) edit=(\d) form=(\d) n=(\d+) sel=(-?\d+)', l)
            if m:
                d.update(open=int(m[1]), edit=int(m[2]), form=int(m[3]), n=int(m[4]), sel=int(m[5]))
            m = re.match(r'card x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', l)
            if m:
                d['card'] = tuple(int(v) for v in m.groups())
            m = re.match(r'icon (\d+) "(.*)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)(?: remove=(-?\d+),(-?\d+),(\d+),(\d+))?', l)
            if m:
                d['icons'].append(dict(name=m[2], box=tuple(int(v) for v in m.groups()[2:6]),
                                       remove=tuple(int(v) for v in m.groups()[6:]) if m[7] else None))
            m = re.match(r'add x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', l)
            if m:
                d['add'] = tuple(int(v) for v in m.groups())
            m = re.match(r'button (\d) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) "(.*)"', l)
            if m:
                d['buttons'][int(m[1])] = (tuple(int(v) for v in m.groups()[1:5]), m[6])
            m = re.match(r'field (\d) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) text="(.*)"', l)
            if m:
                d['fields'][int(m[1])] = (tuple(int(v) for v in m.groups()[1:5]), m[6])
            m = re.match(r'kind (-?\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', l)
            if m:
                d['kinds'][int(m[1])] = tuple(int(v) for v in m.groups()[1:])
        return d

    @staticmethod
    def centre(r):
        return r[0] + r[2] // 2, r[1] + r[3] // 2

    def type_text(self, c, text):
        for ch in text:
            c.tap(KEYCODES[ch])
        c.sync()

    def super_a(self, c):
        c.key(self.KEY_LEFTMETA, True)
        c.tap(self.KEY_A)
        c.key(self.KEY_LEFTMETA, False)
        c.sync()

    def conf_apps(self):
        with open(self.path) as f:
            return [l.split('=')[0].strip() for l in f.read().split('[dock-apps]')[1].splitlines() if '=' in l]

    def test_super_a_opens_and_closes_it_and_the_icons_are_the_docks(self):
        c = self.start('mellagui-lc1')
        self.assertEqual(self.info(c)['open'], 0)
        self.super_a(c)
        d = self.info(c)
        self.assertEqual((d['open'], d['edit'], d['form'], d['n']), (1, 0, 0, 3))
        self.assertEqual([i['name'] for i in d['icons']], ['Alpha', 'Beta', 'Gamma'])      # the Launcher button is not listed
        self.assertIsNotNone(d['add'])
        self.assertEqual(d['buttons'][0][1], 'Edit')
        card = d['card']
        for i in d['icons']:                                                               # big round icons inside the card
            self.assertEqual(i['box'][2], 64)
            self.assertTrue(card[0] < i['box'][0] and i['box'][0] + 64 < card[0] + card[2])
            self.assertTrue(card[1] < i['box'][1] and i['box'][1] + 64 < card[1] + card[3])
        self.assertEqual(card[0] + card[2] // 2, 400)                                      # centred on the 800x500 output
        self.super_a(c)                                                                    # Super+A again: closed (Esc, too)
        self.assertEqual(self.info(c)['open'], 0)
        self.super_a(c)
        c.tap(self.KEY_ESC)
        c.sync()
        self.assertEqual(self.info(c)['open'], 0)

    def test_the_docks_launcher_button_opens_it_and_a_click_outside_closes_it(self):
        c = self.start('mellagui-lc2')
        icons = TestDock.dock(c)[1]
        self.assertEqual(icons[0]['name'], 'Launcher')
        c.click(icons[0]['x'] + icons[0]['w'] // 2, icons[0]['y'] + icons[0]['h'] // 2)
        c.sync()
        self.assertEqual(self.info(c)['open'], 1)
        c.click(5, 250)                                                                    # outside the card: closes, nothing below gets it
        c.sync()
        self.assertEqual(self.info(c)['open'], 0)

    def test_a_click_starts_the_program_and_closes_it(self):
        c = self.start('mellagui-lc3')
        self.super_a(c)
        d = self.info(c)
        c.click(*self.centre(d['icons'][1]['box']))                                        # Beta = touch MARK
        c.sync()
        for _ in range(50):
            if os.path.exists(self.mark):
                break
            time.sleep(0.1)
        self.assertTrue(os.path.exists(self.mark))
        self.assertEqual(self.info(c)['open'], 0)

    def test_the_keyboard_walks_the_icons_and_enter_starts_one(self):
        c = self.start('mellagui-lc4')
        self.super_a(c)
        self.assertEqual(self.info(c)['sel'], -1)
        c.tap(self.KEY_RIGHT)
        c.tap(self.KEY_RIGHT)                                                              # first arrow selects the first icon
        c.sync()
        self.assertEqual(self.info(c)['sel'], 1)
        c.tap(self.KEY_ENTER)
        c.sync()
        for _ in range(50):
            if os.path.exists(self.mark):
                break
            time.sleep(0.1)
        self.assertTrue(os.path.exists(self.mark))
        self.assertEqual(self.info(c)['open'], 0)

    def test_edit_mode_shows_an_x_that_removes_the_button_from_the_file(self):
        c = self.start('mellagui-lc5')
        self.super_a(c)
        c.click(*self.centre(self.info(c)['buttons'][0][0]))                               # Edit
        c.sync()
        d = self.info(c)
        self.assertEqual((d['edit'], d['buttons'][0][1]), (1, 'Done'))
        self.assertTrue(all(i['remove'] for i in d['icons']))
        c.click(*self.centre(d['icons'][0]['remove']))                                     # remove Alpha
        c.sync()
        d = self.info(c)
        self.assertEqual([i['name'] for i in d['icons']], ['Beta', 'Gamma'])
        self.assertEqual(d['open'], 1)                                                     # still open, still editing
        self.assertEqual(d['edit'], 1)
        self.assertEqual(self.conf_apps(), ['Launcher', 'Beta', 'Gamma'])
        self.assertEqual([i['name'] for i in TestDock.dock(c)[1]], ['Launcher', 'Beta', 'Gamma'])      # the dock follows
        c.click(*self.centre(d['buttons'][0][0]))                                          # Done
        c.sync()
        self.assertEqual(self.info(c)['edit'], 0)

    def test_the_plus_tile_opens_a_form_that_adds_a_button(self):
        c = self.start('mellagui-lc6')
        self.super_a(c)
        c.click(*self.centre(self.info(c)['add']))
        c.sync()
        d = self.info(c)
        self.assertEqual(d['form'], 1)
        self.type_text(c, 'notes')
        c.tap(self.KEY_ENTER)                                                              # next field
        self.type_text(c, 'true')
        d = self.info(c)
        self.assertEqual((d['fields'][0][1], d['fields'][1][1]), ('notes', 'true'))
        c.click(*self.centre(d['kinds'][3]))                                               # the icon choice "files"
        c.tap(self.KEY_ENTER)
        c.sync()
        d = self.info(c)
        self.assertEqual((d['form'], d['n']), (0, 4))
        self.assertEqual([i['name'] for i in d['icons']][-1], 'notes')
        with open(self.path) as f:
            self.assertIn('notes = [files] true', f.read())
        self.assertEqual(self.info(c)['sel'], 3)

    LAUNCHER_APPS = ('[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [terminal] true\nBeta = [files] true\nGamma = [editor] true\n'
                     '[launcher-apps]\nDelta = [web] true\nEpsilon = [code] true\n')

    def section(self, name):
        with open(self.path) as f:
            text = f.read()
        if '[%s]' % name not in text:
            return []
        body = text.split('[%s]' % name)[1].split('\n[')[0]
        return [l.split('=')[0].strip() for l in body.splitlines() if '=' in l]

    def drag(self, c, frm, to, steps=6, release=True):
        c.abs(*frm)
        c.button(272, True)
        for i in range(1, steps + 1):
            c.abs(frm[0] + (to[0] - frm[0]) * i // steps, frm[1] + (to[1] - frm[1]) * i // steps)
        c.sync()
        if release:
            c.button(272, False)
            c.sync()

    def test_the_launcher_lists_its_own_buttons_after_the_docks(self):
        c = self.start('mellagui-lc10', self.LAUNCHER_APPS)
        self.super_a(c)
        d = self.info(c)
        self.assertEqual([i['name'] for i in d['icons']], ['Alpha', 'Beta', 'Gamma', 'Delta', 'Epsilon'])
        self.assertEqual([i['name'] for i in TestDock.dock(c)[1]], ['Launcher', 'Alpha', 'Beta', 'Gamma'])    # the dock has only its own

    def test_a_button_added_in_the_launcher_stays_out_of_the_dock(self):
        c = self.start('mellagui-lc11', self.LAUNCHER_APPS)
        self.super_a(c)
        c.tap(self.KEY_A)
        self.type_text(c, 'zeta')
        c.tap(self.KEY_ENTER)
        self.type_text(c, 'true')
        c.tap(self.KEY_ENTER)
        c.sync()
        self.assertEqual(self.section('launcher-apps'), ['Delta', 'Epsilon', 'zeta'])
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Beta', 'Gamma'])
        self.assertEqual([i['name'] for i in self.info(c)['icons']][-1], 'zeta')

    def test_edit_mode_removes_a_launcher_only_button_from_its_own_list(self):
        c = self.start('mellagui-lc12', self.LAUNCHER_APPS)
        self.super_a(c)
        c.tap(self.KEY_E)
        c.sync()
        d = self.info(c)
        c.click(*self.centre(d['icons'][3]['remove']))                                     # Delta
        c.sync()
        self.assertEqual(self.section('launcher-apps'), ['Epsilon'])
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Beta', 'Gamma'])

    def test_dragging_a_launcher_icon_onto_the_dock_adds_it_there(self):
        c = self.start('mellagui-lc13', self.LAUNCHER_APPS)
        self.super_a(c)
        d = self.info(c)
        dock = TestDock.dock(c)[1]
        beta = dock[2]
        target = (beta['x'] + 2, beta['y'] + beta['h'] // 2)                               # the left edge of Beta: it joins before Beta
        self.drag(c, self.centre(d['icons'][3]['box']), target, release=False)             # Delta
        mid = self.info(c)
        self.assertEqual(mid['icons'][3]['name'], 'Delta')
        self.assertTrue(any(l.startswith('drop ') for l in c.cmd('launcher')))
        self.assertEqual(mid['open'], 1)
        c.button(272, False)
        c.sync()
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Delta', 'Beta', 'Gamma'])
        self.assertEqual(self.section('launcher-apps'), ['Epsilon'])
        self.assertEqual([i['name'] for i in TestDock.dock(c)[1]], ['Launcher', 'Alpha', 'Delta', 'Beta', 'Gamma'])
        self.assertEqual(self.info(c)['open'], 1)                                          # still open

    def test_dropping_a_launcher_icon_elsewhere_changes_nothing(self):
        c = self.start('mellagui-lc14', self.LAUNCHER_APPS)
        self.super_a(c)
        d = self.info(c)
        self.drag(c, self.centre(d['icons'][3]['box']), (40, 200))
        self.assertEqual(self.section('launcher-apps'), ['Delta', 'Epsilon'])
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Beta', 'Gamma'])
        info = self.info(c)
        self.assertEqual((info['open'], info['drag'] if 'drag' in info else -1), (1, -1))
        self.assertFalse(os.path.exists(self.mark))                                        # and the drag did not click-start anything

    def test_a_dock_icon_can_be_dragged_along_the_dock_to_reorder(self):
        c = self.start('mellagui-lc15', self.LAUNCHER_APPS)
        dock = TestDock.dock(c)[1]
        alpha, gamma = dock[1], dock[3]
        frm = (alpha['x'] + alpha['w'] // 2, alpha['y'] + alpha['h'] // 2)
        self.drag(c, frm, (gamma['x'] + gamma['w'] // 2, frm[1]), release=False)
        info, icons = TestDock.dock(c)
        self.assertEqual((info['drag'], info['to']), (1, 3))
        self.assertEqual(info['n'], 3)                                                     # the icon in the hand is not in the row
        c.button(272, False)
        c.sync()
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Beta', 'Gamma', 'Alpha'])
        self.assertEqual([i['name'] for i in TestDock.dock(c)[1]], ['Launcher', 'Beta', 'Gamma', 'Alpha'])
        self.assertEqual(TestDock.dock(c)[0]['drag'], -1)

    def test_a_dock_icon_dragged_out_of_the_dock_goes_to_the_launcher(self):
        c = self.start('mellagui-lc16', self.LAUNCHER_APPS)
        beta = TestDock.dock(c)[1][2]
        frm = (beta['x'] + beta['w'] // 2, beta['y'] + beta['h'] // 2)
        self.drag(c, frm, (frm[0], frm[1] - 200), release=False)
        info = TestDock.dock(c)[0]
        self.assertEqual((info['drag'], info['to']), (2, -1))
        c.button(272, False)
        c.sync()
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Gamma'])
        self.assertEqual(self.section('launcher-apps'), ['Delta', 'Epsilon', 'Beta'])
        self.assertEqual([i['name'] for i in TestDock.dock(c)[1]], ['Launcher', 'Alpha', 'Gamma'])
        self.super_a(c)
        self.assertEqual([i['name'] for i in self.info(c)['icons']], ['Alpha', 'Gamma', 'Delta', 'Epsilon', 'Beta'])    # nothing was lost

    def test_the_launcher_button_cannot_be_dragged_out_of_the_dock(self):
        c = self.start('mellagui-lc17', self.LAUNCHER_APPS)
        first = TestDock.dock(c)[1][0]
        frm = (first['x'] + first['w'] // 2, first['y'] + first['h'] // 2)
        self.drag(c, frm, (frm[0], frm[1] - 200))
        self.assertEqual(self.section('dock-apps'), ['Launcher', 'Alpha', 'Beta', 'Gamma'])
        self.assertEqual(self.info(c)['open'], 0)                                          # and the drag did not open the launcher

    def test_a_small_movement_is_still_a_click(self):
        c = self.start('mellagui-lc18')
        beta = TestDock.dock(c)[1][2]
        frm = (beta['x'] + beta['w'] // 2, beta['y'] + beta['h'] // 2)
        self.drag(c, frm, (frm[0] + 3, frm[1] - 2), steps=2)
        for _ in range(50):
            if os.path.exists(self.mark):
                break
            time.sleep(0.1)
        self.assertTrue(os.path.exists(self.mark))                                         # Beta = touch MARK ran

    def test_the_form_checks_its_fields_and_esc_goes_back(self):
        c = self.start('mellagui-lc7')
        self.super_a(c)
        c.tap(self.KEY_A)                                                                  # A opens the form
        c.sync()
        self.assertEqual(self.info(c)['form'], 1)
        c.click(*self.centre(self.info(c)['buttons'][0][0]))                               # Add with nothing typed
        c.sync()
        self.assertTrue(any('message "Give it a name' in l for l in c.cmd('launcher')))
        self.type_text(c, 'alpha')                                                         # the name is taken (any case)
        c.tap(self.KEY_ENTER)
        self.type_text(c, 'true')
        c.tap(self.KEY_ENTER)
        c.sync()
        self.assertTrue(any('message "Not added' in l for l in c.cmd('launcher')))
        self.assertEqual(self.info(c)['form'], 1)
        c.tap(self.KEY_ESC)                                                                # Esc: back to the icons, not closed
        c.sync()
        d = self.info(c)
        self.assertEqual((d['open'], d['form'], d['n']), (1, 0, 3))

    def test_without_a_config_file_nothing_can_be_removed(self):
        p, c, stop = self.private_compositor('mellagui-lc8', '--config', 'none')
        c.sync()
        self.super_a(c)
        d = self.info(c)
        self.assertEqual((d['open'], d['n']), (1, 10))                                    # the built-in dock (with the Camera and Record buttons), without its Launcher button
        c.tap(self.KEY_E)
        c.sync()
        c.click(*self.centre(self.info(c)['icons'][0]['remove']))
        c.sync()
        self.assertEqual(self.info(c)['n'], 10)

    def test_the_dialog_is_painted_translucent(self):
        c = self.start('mellagui-lc9')
        before = c.dump(os.path.join(tmp, 'lc9a.ppm'))
        self.super_a(c)
        d = self.info(c)
        after = c.dump(os.path.join(tmp, 'lc9b.ppm'))
        x, y = d['card'][0] + 10, d['card'][1] + 10                                        # a corner area inside the card, no text
        a, b = before.px(x, y), after.px(x, y)
        self.assertNotEqual(a, b)
        self.assertTrue(all(bc > ac for ac, bc in zip(a, b)), (a, b))                      # a light veil: every channel brighter
        self.assertTrue(all(bc < 255 for bc in b), b)                                      # but not opaque white

    # ---- changing a button that is there -------------------------------------------------------------------------------------

    def conf_text(self):
        with open(self.path) as f:
            return f.read()

    def open_change(self, c, name):
        """Launcher open, edit mode on, a click on the icon `name`: the form with that button's values."""
        if not self.info(c).get('open'):
            self.super_a(c)
        d = self.info(c)
        if not d['edit']:
            c.click(*self.centre(d['buttons'][0][0]))                          # Edit
            d = self.info(c)
        self.assertEqual(d['edit'], 1)
        icon = [i for i in d['icons'] if i['name'] == name][0]
        c.click(*self.centre(icon['box']))
        d = self.info(c)
        self.assertEqual(d['form'], 1, 'the click did not open the form')
        return d

    def focus_line(self, c):
        return [l for l in c.cmd('launcher') if l.startswith('focus')][0]

    def test_a_click_on_an_icon_in_edit_mode_changes_that_button_in_place(self):
        c = self.start('mellagui-lc30')
        d = self.open_change(c, 'Beta')
        self.assertEqual(d['fields'][0][1], 'Beta')                           # the form holds what the file has: name and command as written
        self.assertEqual(d['fields'][1][1], 'touch %s' % self.mark)
        self.assertIn('editing=1', c.cmd('launcher')[0])
        self.assertEqual(d['buttons'][0][1], 'Save')
        for _ in range(4):
            c.tap(14)                                                           # Backspace: "Beta" -> ""
        self.type_text(c, 'bravo')
        c.tap(self.KEY_TAB)
        for _ in range(len(self.mark) + 6):
            c.tap(14)
        self.type_text(c, 'true')
        c.click(*self.centre(d['kinds'][4]))                                    # another icon (kind 4)
        c.click(*self.centre(self.info(c)['buttons'][0][0]))                    # Save
        d = self.info(c)
        self.assertEqual(d['form'], 0)
        self.assertEqual([i['name'] for i in d['icons']], ['Alpha', 'bravo', 'Gamma'])      # the same place in the row
        text = self.conf_text()
        self.assertEqual(self.conf_apps(), ['Launcher', 'Alpha', 'bravo', 'Gamma'])
        self.assertIn('bravo = [', text)
        self.assertIn('] true\n', text)
        self.assertNotIn('touch', text)
        self.assertNotIn('Beta', text)
        self.assertEqual(d['edit'], 1)                                          # still in edit mode

    def test_a_launcher_only_button_is_changed_in_its_own_list(self):
        c = self.start('mellagui-lc31', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [terminal] true\n[launcher-apps]\nSolo = [files] true\nTwo = true\n')
        d = self.open_change(c, 'Solo')
        self.assertEqual(d['fields'][0][1], 'Solo')
        for _ in range(4):
            c.tap(14)
        self.type_text(c, 'x')
        c.click(*self.centre(self.info(c)['buttons'][0][0]))
        text = self.conf_text()
        self.assertEqual(text.split('[launcher-apps]')[1].splitlines()[1:3], ['x = [files] true', 'Two = true'])
        self.assertIn('Alpha = [terminal] true', text.split('[launcher-apps]')[0])         # the dock's list is as it was

    def test_changing_keeps_a_picture_or_theme_icon_unless_another_is_chosen(self):
        c = self.start('mellagui-lc32', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [theme:nosuchicon|alsonone] true\nBeta = [files] true\n')
        d = self.open_change(c, 'Alpha')
        self.assertIn('kind -2', self.focus_line(c))
        c.click(*self.centre(d['buttons'][0][0]))                               # Save without touching the icon
        self.assertIn('Alpha = [theme:nosuchicon|alsonone] true', self.conf_text())
        d = self.open_change(c, 'Alpha')
        c.click(*self.centre(d['kinds'][-1]))                                   # "auto"
        c.click(*self.centre(self.info(c)['buttons'][0][0]))
        self.assertIn('Alpha = true', self.conf_text())

    def test_a_name_another_button_has_is_refused_and_cancel_changes_nothing(self):
        c = self.start('mellagui-lc33')
        before = self.conf_text()
        d = self.open_change(c, 'Gamma')
        for _ in range(5):
            c.tap(14)
        self.type_text(c, 'alpha')                                              # (any letter case): Alpha is taken
        c.click(*self.centre(self.info(c)['buttons'][0][0]))
        d = self.info(c)
        self.assertEqual(d['form'], 1)
        self.assertIn('Not saved', self.focus_line(c))
        self.assertEqual(self.conf_text(), before)
        c.click(*self.centre(d['buttons'][1][0]))                               # Cancel
        d = self.info(c)
        self.assertEqual((d['form'], d['edit']), (0, 1))
        self.assertEqual(self.conf_text(), before)
        self.open_change(c, 'Gamma')                                            # its own name in another letter case is fine
        for _ in range(5):
            c.tap(14)
        self.type_text(c, 'gamma')
        c.click(*self.centre(self.info(c)['buttons'][0][0]))
        self.assertIn('gamma = ', self.conf_text())

    def test_enter_on_the_highlighted_icon_in_edit_mode_changes_it(self):
        c = self.start('mellagui-lc34')
        self.super_a(c)
        c.tap(self.KEY_E)                                                       # edit mode
        c.tap(self.KEY_RIGHT)                                                   # first the sel moves onto the first icon, then on to Beta
        c.tap(self.KEY_RIGHT)
        c.tap(self.KEY_ENTER)
        d = self.info(c)
        self.assertEqual(d['form'], 1)
        self.assertIn(d['fields'][0][1], ('Alpha', 'Beta'))
        c.tap(self.KEY_ESC)                                                     # back to the icons
        self.assertEqual(self.info(c)['form'], 0)

    # ---- the icon text field of the form ----------------------------------------------------------------------------------

    def type_icon(self, c, text):
        keys = {ch: k for k, ch in enumerate('qwertyuiop', 16)}
        keys.update({ch: k for k, ch in enumerate('asdfghjkl', 30)})
        keys.update({ch: k for k, ch in enumerate('zxcvbnm', 44)})
        for ch in text:
            if ch == '[':
                c.tap(26)
            elif ch == ']':
                c.tap(27)
            elif ch == ':':
                c.key(KEY_LEFTSHIFT, True)
                c.tap(39)
                c.key(KEY_LEFTSHIFT, False)
            else:
                c.tap(keys[ch])
        c.sync()

    def test_the_form_has_an_icon_text_field_that_takes_theme_names_with_or_without_brackets(self):
        c = self.start('mellagui-lc35')
        self.super_a(c)
        d = self.info(c)
        c.click(*self.centre(d['add']))
        d = self.info(c)
        self.assertEqual(sorted(d['fields']), [0, 1, 2], 'the form has no icon field')
        self.type_text(c, 'web')                                                 # the name
        c.tap(self.KEY_TAB)
        self.type_text(c, 'true')                                                # the command
        c.click(*self.centre(d['fields'][2][0]))                                 # the icon field
        self.type_icon(c, '[theme:fx]')
        self.assertEqual(self.info(c)['fields'][2][1], '[theme:fx]')
        c.click(*self.centre(self.info(c)['buttons'][0][0]))                     # Add
        self.assertIn('web = [theme:fx] true', self.conf_text())                 # (stored without the brackets of the typed text)

    def test_a_built_in_icon_chosen_below_fills_the_field_and_typing_a_name_picks_it(self):
        c = self.start('mellagui-lc36')
        self.super_a(c)
        c.click(*self.centre(self.info(c)['add']))
        d = self.info(c)
        c.click(*self.centre(d['kinds'][3]))                                     # "files"
        self.assertEqual(self.info(c)['fields'][2][1], 'files')
        self.assertIn('kind 3', self.focus_line(c))
        c.click(*self.centre(d['kinds'][-1]))                                    # auto: the field is empty again
        self.assertEqual(self.info(c)['fields'][2][1], '')
        c.click(*self.centre(d['fields'][2][0]))
        self.type_icon(c, 'files')                                               # typed: the same icon is picked
        self.assertIn('kind 3', self.focus_line(c))
        self.type_icon(c, 'x')                                                   # "filesx" is no built-in name: it is the user's own
        self.assertIn('kind -2', self.focus_line(c))

    def test_the_icon_of_an_existing_button_can_be_typed_over(self):
        c = self.start('mellagui-lc37', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [theme:old] true\n')
        d = self.open_change(c, 'Alpha')
        self.assertEqual(d['fields'][2][1], 'theme:old')                         # as the file has it
        c.click(*self.centre(d['fields'][2][0]))
        for _ in range(len('theme:old')):
            c.tap(14)
        self.type_icon(c, 'theme:new')
        c.click(*self.centre(self.info(c)['buttons'][0][0]))
        self.assertIn('Alpha = [theme:new] true', self.conf_text())

    def test_tab_visits_name_command_icon_field_and_the_icon_row(self):
        c = self.start('mellagui-lc38')
        self.super_a(c)
        c.click(*self.centre(self.info(c)['add']))
        seen = []
        for _ in range(5):
            seen.append(int(self.focus_line(c).split()[1]))
            c.tap(self.KEY_TAB)
        self.assertEqual(seen, [0, 1, 2, 3, 0])

    def test_every_text_field_of_the_form_is_redrawn_while_typing_with_either_renderer(self):
        """The GL renderer caches the dialog by a hash of what it shows: the icon text field was left out of it, so typing there did not show."""
        for rend in ('software', 'gl'):
            self.path = os.path.join(tmp, 'mellagui-lc39-%s.conf' % rend)
            with open(self.path, 'w') as f:
                f.write('[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = [terminal] true\n')
            try:
                p, c, stop = self.private_compositor('mellagui-lc39-' + rend, '--config', self.path, '--renderer', rend)
            except Exception:
                continue
            c.sync()
            self.super_a(c)
            c.click(*self.centre(self.info(c)['add']))
            d = self.info(c)
            for i in range(3):
                x, y, w, h = d['fields'][i][0]
                c.click(x + 50, y + 10)
                self.type_text(c, 'a')                                    # (the first letter may change other things the cache is keyed on)
                before = c.dump(os.path.join(tmp, 'lc39-%s-%d-a.ppm' % (rend, i)))
                self.type_text(c, 'bc')
                after = c.dump(os.path.join(tmp, 'lc39-%s-%d-b.ppm' % (rend, i)))
                changed = sum(1 for yy in range(y + 4, y + h - 4) for xx in range(x + 6, x + 60) if before.px(xx, yy) != after.px(xx, yy))
                self.assertGreater(changed, 20, '%s renderer: field %d does not show what is typed' % (rend, i))

    def popup(self, c):
        d = dict(entries=[], open=0, owner=-1)
        for l in c.cmd('dockmenu'):
            m = re.match(r'dockmenu open=(\d) item=(-?\d+) sel=(-?\d+) owner=(-?\d+)', l)
            if m:
                d.update(open=int(m[1]), owner=int(m[4]))
            m = re.match(r'entry (\d) "(.*)" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) enabled=(\d)', l)
            if m:
                d['entries'].append(dict(label=m[2], pos=(int(m[3]) + int(m[5]) // 2, int(m[4]) + int(m[6]) // 2), enabled=int(m[7])))
        return d

    def right_click_at(self, c, pos):
        c.abs(*pos)
        c.button(273, True)
        c.button(273, False)
        c.sync()

    def test_the_forms_fields_paste_and_have_a_right_click_menu(self):
        c = self.start('mellagui-lc40')
        self.super_a(c)
        c.click(*self.centre(self.info(c)['add']))
        d = self.info(c)
        c.cmd('clip add my app')
        c.key(29, True)
        c.tap(47)                                                       # Ctrl+V into the name field (it has the focus)
        c.key(29, False)
        self.assertEqual(self.info(c)['fields'][0][1], 'my app')
        c.key(29, True)
        c.tap(45)                                                       # Ctrl+X: the field is empty, the text is the newest clipboard entry
        c.key(29, False)
        self.assertEqual(self.info(c)['fields'][0][1], '')
        self.right_click_at(c, self.centre(d['fields'][1][0]))          # the command field's menu
        m = self.popup(c)
        self.assertEqual((m['open'], m['owner']), (1, 2))
        self.assertEqual([(e['label'], e['enabled']) for e in m['entries']], [('Cut', 0), ('Copy', 0), ('Paste', 1), ('Clear', 0)])
        c.click(*m['entries'][2]['pos'])                                # Paste: the cut text lands in the command field
        self.assertEqual(self.info(c)['fields'][1][1], 'my app')
        self.assertEqual(self.popup(c)['open'], 0)
        self.right_click_at(c, self.centre(d['fields'][1][0]))
        m = self.popup(c)
        self.assertEqual([e['enabled'] for e in m['entries']], [1, 1, 1, 1])
        c.click(*m['entries'][3]['pos'])                                # Clear
        self.assertEqual(self.info(c)['fields'][1][1], '')
        self.right_click_at(c, self.centre(d['fields'][2][0]))          # the icon field has one, too; the menu is above the dialog
        self.assertEqual(self.popup(c)['open'], 1)
        c.tap(self.KEY_ESC)                                             # Esc closes the menu, not the form
        self.assertEqual((self.popup(c)['open'], self.info(c)['form']), (0, 1))

    def test_typing_in_the_launcher_searches_by_name(self):
        c = self.start('mellagui-lc41', '[dock-apps]\nLauncher = [launcher] @launcher\nTerminal = [terminal] true\nText Editor = [editor] true\nCalculator = [calculator] true\n')
        self.super_a(c)
        self.assertEqual(self.info(c)['n'], 3)
        c.tap(20)                                                       # "t": the search starts
        self.assertIn('searching=1 search="t"', c.cmd('launcher')[0])
        c.tap(18)                                                       # "e": "te" -> Terminal and Text Editor (E is a shortcut only outside a search)
        d = self.info(c)
        self.assertEqual([i['name'] for i in d['icons']], ['Terminal', 'Text Editor'])
        self.assertIsNone(d['add'], 'no "+" tile while searching')
        self.assertEqual(d['edit'], 0)
        c.tap(45)                                                       # "x": "tex" -> Text Editor, and the first icon is highlighted
        d = self.info(c)
        self.assertEqual([i['name'] for i in d['icons']], ['Text Editor'])
        self.assertEqual(d['sel'], 0)
        c.tap(14)                                                       # Backspace: "te"
        self.assertEqual([i['name'] for i in self.info(c)['icons']], ['Terminal', 'Text Editor'])
        c.tap(self.KEY_ESC)                                             # the first Esc clears the text, the search stays
        d = self.info(c)
        self.assertEqual((d['open'], d['n']), (1, 3))
        self.assertIn('searching=1 search=""', c.cmd('launcher')[0])
        c.tap(self.KEY_ESC)                                             # the second leaves the search, the third closes the dialog
        self.assertIn('searching=0', c.cmd('launcher')[0])
        c.tap(self.KEY_ESC)
        self.assertEqual(self.info(c)['open'], 0)

    def test_enter_starts_the_found_program_and_nothing_found_says_so(self):
        mark = os.path.join(tmp, 'lc42.mark')
        if os.path.exists(mark):
            os.unlink(mark)
        c = self.start('mellagui-lc42', '[dock-apps]\nLauncher = [launcher] @launcher\nAlpha = touch %s\nBeta = true\n' % mark)
        self.super_a(c)
        c.tap(48)                                                       # "b": only Beta
        self.assertEqual([i['name'] for i in self.info(c)['icons']], ['Beta'])
        c.tap(44)                                                       # "bz": nothing
        d = self.info(c)
        self.assertEqual((d['n'], d['sel']), (0, -1))
        c.tap(14)
        c.tap(14)                                                       # empty again: everything
        self.assertEqual(self.info(c)['n'], 2)
        c.tap(30)                                                       # "a" is a shortcut (add) outside the search, but here it is text
        self.assertEqual([i['name'] for i in self.info(c)['icons']], ['Alpha', 'Beta'])    # "a" is in both names
        c.tap(38)                                                       # "al": Alpha
        c.tap(self.KEY_ENTER)
        for _ in range(100):
            if os.path.exists(mark):
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(mark), 'the program was started')
        self.assertEqual(self.info(c)['open'], 0)

    def test_the_shortcuts_still_work_outside_a_search_and_slash_starts_one(self):
        c = self.start('mellagui-lc43')
        self.super_a(c)
        c.tap(self.KEY_E)                                               # E: edit mode (not a search)
        self.assertEqual((self.info(c)['edit'], 'searching=0' in c.cmd('launcher')[0]), (1, True))
        c.tap(self.KEY_E)
        c.tap(53)                                                       # "/": a search with no text yet
        self.assertIn('searching=1 search=""', c.cmd('launcher')[0])
        c.tap(self.KEY_E)                                               # now "e" is text: Beta and Gamma ... have an "e"? "Beta" does
        self.assertIn('search="e"', c.cmd('launcher')[0])


if __name__ == '__main__':
    unittest.main(verbosity=2)

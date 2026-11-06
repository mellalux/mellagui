"""End-to-end tests: start the compositor with the headless backend and talk to it
through the raw wire client in wlclient.py.

Environment:
    LUMEN_BIN   compositor binary (default build/lumen; use build/lumen-asan for sanitizers)
"""
import os
import shutil
import signal
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
BIN = os.environ.get('LUMEN_BIN', os.path.join(ROOT, 'build', 'lumen'))

KEY_ESC, KEY_1, KEY_2, KEY_Q, KEY_A, KEY_M, KEY_ENTER, KEY_F, KEY_TAB = 1, 2, 3, 16, 30, 50, 28, 33, 15
KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT = 125, 42, 29, 56
KEY_UP, KEY_LEFT, KEY_RIGHT, KEY_DOWN = 103, 105, 106, 108
KEY_SPACE, KEY_RIGHTALT = 57, 100
KEY_G = 34
BTN_LEFT, BTN_RIGHT = 272, 273

proc = None
tmp = None
ctl = None
LOG = None


def setUpModule():
    global proc, tmp, ctl, LOG
    tmp = tempfile.mkdtemp(prefix='lumen-test-')
    os.chmod(tmp, 0o700)
    os.environ['XDG_RUNTIME_DIR'] = tmp
    os.environ['WAYLAND_DISPLAY'] = 'lumen-test'
    os.environ['XDG_CACHE_HOME'] = tmp        # lumen-run keeps its history here
    LOG = os.path.join(tmp, 'lumen.log')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0:exitcode=77',
               UBSAN_OPTIONS='print_stacktrace=1')
    proc = subprocess.Popen([BIN, '--backend', 'headless', '--size', '1024x600', '--socket', 'lumen-test',
                             '--control', os.path.join(tmp, 'ctl'), '--log-file', LOG, '--log-level', 'debug',
                             '--terminal', 'true'],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        if os.path.exists(os.path.join(tmp, 'ctl')) and os.path.exists(os.path.join(tmp, 'lumen-test')):
            break
        if proc.poll() is not None:
            raise RuntimeError('compositor exited early: %s' % proc.stdout.read().decode())
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
    out = proc.stdout.read().decode(errors='replace')
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
        self.assertEqual((r['w'], r['h']), (1024 - 8, 600 - 28 - 28 - 4))
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
        self.assertEqual((cfg[0], cfg[1]), (512 - 8, 600 - 28 - 28 - 4))
        self.assertNotIn(XDG_STATE_MAXIMIZED, cfg[2])
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertIn('tiled-left', r['flags'])
        self.assertEqual((r['x'], r['y'], r['w'], r['h']), (4, 56, 504, 540))
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

    def test_panel_workspace_click(self):
        ctl.click(124, 14)   # second workspace cell
        ctl.sync()
        self.assertEqual(ctl.windows()['workspace'], 1)
        ctl.click(98, 14)
        ctl.sync()
        self.assertEqual(ctl.windows()['workspace'], 0)

    def test_panel_exit_needs_confirmation(self):
        idle = (0x2d, 0x3a, 0x5a)
        danger = (0xc0, 0x39, 0x2b)
        ctl.click(925, 14)       # first click only arms the button
        ctl.sync()
        self.assertTrue(alive())
        ppm = ctl.dump(os.path.join(tmp, 'exit1.ppm'))
        self.assertEqual(ppm.px(904, 6), danger)
        ctl.abs(500, 300)        # moving away cancels the confirmation
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'exit2.ppm'))
        self.assertEqual(ppm.px(904, 6), idle)
        ctl.click(925, 14)       # armed again, still running
        ctl.abs(500, 300)
        ctl.sync()
        self.assertTrue(alive())

    def test_panel_exit_second_click_quits(self):
        # separate instance: the shared compositor must keep running
        d = tempfile.mkdtemp(prefix='lumen-exit-')
        os.chmod(d, 0o700)
        p2 = subprocess.Popen([BIN, '--backend', 'headless', '--size', '1024x600', '--socket', 'lumen-exit',
                               '--control', os.path.join(d, 'ctl'), '--terminal', 'true'],
                              env=dict(os.environ, XDG_RUNTIME_DIR=d),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if os.path.exists(os.path.join(d, 'ctl')):
                    break
                time.sleep(0.05)
            c2 = Ctl(os.path.join(d, 'ctl'))
            c2.click(925, 14)
            c2.sync()
            self.assertIsNone(p2.poll())
            c2.click(925, 14)
            self.assertEqual(p2.wait(timeout=5), 0)
        finally:
            if p2.poll() is None:
                p2.kill()
            p2.stdout.close()
            shutil.rmtree(d, ignore_errors=True)

    def test_app_menu_opens_and_closes(self):
        ctl.abs(900, 500)
        ctl.sync()
        before = ctl.dump(os.path.join(tmp, 'menu0.ppm'))
        ctl.click(40, 14)                       # "Lumen" launcher button opens the menu
        ctl.abs(300, 300)
        ctl.sync()
        opened = ctl.dump(os.path.join(tmp, 'menu1.ppm'))
        self.assertEqual(opened.px(150, 40), (0x14, 0x17, 0x1f))       # menu background
        self.assertNotEqual(before.px(150, 40), (0x14, 0x17, 0x1f))
        ctl.tap(KEY_ESC)                        # Escape closes it
        ctl.sync()
        closed = ctl.dump(os.path.join(tmp, 'menu2.ppm'))
        self.assertEqual(closed.px(150, 40), before.px(150, 40))
        ctl.click(40, 14)                       # open again, then an outside click closes it
        ctl.click(600, 400)
        ctl.sync()
        closed = ctl.dump(os.path.join(tmp, 'menu3.ppm'))
        self.assertEqual(closed.px(150, 40), before.px(150, 40))
        ctl.click(40, 14)                       # the launcher button toggles
        ctl.click(40, 14)
        ctl.abs(900, 500)
        ctl.sync()
        closed = ctl.dump(os.path.join(tmp, 'menu4.ppm'))
        self.assertEqual(closed.px(150, 40), before.px(150, 40))

    def test_app_menu_starts_calculator(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'lumen-calc')):
            self.skipTest('lumen-calc not built')
        ctl.click(40, 14)
        ctl.click(60, 69)                       # second entry: Calculator
        win = wait_for_window('lumen.calc')
        self.assertIsNotNone(win, 'menu entry did not start the calculator')
        ctl.sync()
        ctl.abs(900, 500)
        ctl.sync()
        ppm = ctl.dump(os.path.join(tmp, 'menu5.ppm'))
        self.assertNotEqual(ppm.px(150, 40), (0x14, 0x17, 0x1f), 'menu stays open after launching')
        ctl.key(KEY_LEFTMETA, True)             # Super+Q closes the calculator again
        ctl.tap(KEY_Q)
        ctl.key(KEY_LEFTMETA, False)
        for _ in range(100):
            if not ctl.windows()['windows']:
                break
            time.sleep(0.05)
        self.assertFalse(ctl.windows()['windows'])

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
        self.assertEqual((cfg[0], cfg[1]), (1024 - 8, 600 - 28 - 28 - 4))
        c.wl.roundtrip()
        r = ctl.windows()['windows'][0]
        self.assertEqual((r['x'], r['y'], r['w'], r['h']), (4, 56, 1016, 540))

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
        c.wl.roundtrip()
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

    def test_history_is_limited_and_multiline_text_is_kept(self):
        for i in range(7):
            self.copy('item %d' % i)
            self.wait_clips(min(i + 1, 5))
        got = self.wait_clips(5)
        self.assertEqual(got[0], 'item 6')
        self.assertEqual(got[-1], 'item 2')
        self.copy('two\nlines')
        for _ in range(100):
            if self.clips()[:1] == ['two\\nlines']:
                break
            time.sleep(0.02)
        self.assertEqual(self.clips()[0], 'two\\nlines')

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
        self.assertTrue(ctl.last.endswith('gadget=0'))
        ctl.sync()
        hidden = ctl.dump(os.path.join(tmp, 'gadget0.ppm')).px(*probe)
        self.assertNotEqual(hidden, shown)
        self.assertIsNone(self.rect())

    def test_gadget_shows_text(self):
        self.copy('hello gadget')
        self.wait_clips(1)
        ctl.sync()
        full = ctl.dump(os.path.join(tmp, 'gfull.ppm'))
        # the first entry slot (below the 32 px title bar) now contains bright text pixels
        x, y, w, h = self.rect()
        box = (x, y + 32, x + w, y + 32 + 44)
        self.assertGreater(full.count((0xe6, 0xe9, 0xef), *box), 20)

    def test_default_place_is_top_right_below_the_panel(self):
        x, y, w, h = self.rect()
        self.assertEqual((x + w, y), (1024 - 12, 28 + 12))
        self.assertEqual((w, h), (300, 262))

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
        self.assertEqual(self.rect(), (0, 28, w, h))
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
        self.assertEqual(self.rect(), (x, y, 200, 86))           # minimum: title + one entry
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
        self.assertGreater(full.count(bright, x, y + 32, x + w, y + nh), 20)      # the newest entry is shown
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
    LAYOUT_BTN = (738, 14)          # panel indicator on the 1024 px wide test output
    STICKY_BTN = (802, 14)

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
        sticky_before = [before.px(x, 14) for x in range(765, 840, 3)]
        sticky_after = [after.px(x, 14) for x in range(765, 840, 3)]
        self.assertNotEqual(sticky_before, sticky_after, 'sticky button looks the same when enabled')
        layout_before = [before.px(x, 14) for x in range(722, 754, 2)]
        layout_after = [after.px(x, 14) for x in range(722, 754, 2)]
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
        e = dict(os.environ, **env)
        p = subprocess.Popen([path], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        return p

    def test_calc_engine_selftest(self):
        path = os.path.join(ROOT, 'build', 'lumen-calc')
        if not os.path.exists(path):
            self.skipTest('lumen-calc not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_calc_clicks_and_keys(self):
        p = self.spawn('lumen-calc')
        win = wait_for_window('lumen.calc')
        self.assertIsNotNone(win, 'calculator window did not appear')
        self.assertEqual((win['w'], win['h']), (264, 360))
        time.sleep(0.3)
        n = [0]

        def centre(row, col):       # layout: 8px padding, 56x48 keys, 8px gaps, 60px display
            return win['x'] + 8 + col * 64 + 28, win['y'] + 76 + row * 56 + 24

        def click(row, col):
            ctl.click(*centre(row, col))
            time.sleep(0.05)

        def display():
            n[0] += 1
            ctl.abs(900, 500)       # keep the hover highlight out of the picture
            time.sleep(0.1)
            ppm = ctl.dump(os.path.join(tmp, 'calc%d.ppm' % n[0]))
            return [ppm.px(win['x'] + x, win['y'] + y) for y in range(8, 68, 2) for x in range(8, 256, 2)]

        zero = display()
        # grid: row0 C < % / | row1 7 8 9 x | row2 4 5 6 - | row3 1 2 3 + | row4 +/- 0 . =
        click(3, 0); click(3, 1); click(3, 3); click(3, 2); click(4, 3)    # 1 2 + 3 =
        mouse_15 = display()
        self.assertNotEqual(mouse_15, zero)
        click(0, 0)                                                       # C
        self.assertEqual(display(), zero)
        click(3, 0); click(2, 1)                                          # 1 5
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

    def test_web_selftest(self):
        path = os.path.join(ROOT, 'build', 'lumen-web')
        if not os.path.exists(path):
            self.skipTest('lumen-web not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_web_links_and_history(self):
        path = os.path.join(ROOT, 'build', 'lumen-web')
        if not os.path.exists(path):
            self.skipTest('lumen-web not built')
        site = os.path.join(tmp, 'site')
        os.makedirs(site, exist_ok=True)
        with open(os.path.join(site, 'a.html'), 'w') as f:
            f.write('<html><head><title>A page</title></head><body><p><a href="b.html">go</a></p></body></html>')
        with open(os.path.join(site, 'b.html'), 'w') as f:
            f.write('<html><head><title>B page</title></head><body><h1>Second</h1></body></html>')
        p = subprocess.Popen([path, os.path.join(site, 'a.html')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('lumen.web')
        self.assertIsNotNone(win, 'browser window did not appear')
        time.sleep(0.3)

        def title():
            return wait_for_window('lumen.web')['title']

        self.assertEqual(title(), 'A page - Lumen Web')
        # layout: 34px toolbar, 16px padding, first line below a 12px paragraph gap; "go" is 16px wide
        ctl.click(win['x'] + 24, win['y'] + 34 + 32)
        for _ in range(40):
            if title() == 'B page - Lumen Web':
                break
            time.sleep(0.05)
        self.assertEqual(title(), 'B page - Lumen Web')
        ctl.click(win['x'] + 19, win['y'] + 17)         # back button
        for _ in range(40):
            if title() == 'A page - Lumen Web':
                break
            time.sleep(0.05)
        self.assertEqual(title(), 'A page - Lumen Web')
        ctl.abs(900, 500)

    # ---- lumen-web: JavaScript, DOM, https -------------------------------------------------------------------------

    def web_dump(self, target, wait_ms=100, env=None):
        path = os.path.join(ROOT, 'build', 'lumen-web')
        if not os.path.exists(path):
            self.skipTest('lumen-web not built')
        e = dict(os.environ, **(env or {}))
        r = subprocess.run([path, '--dump', target, str(wait_ms)], capture_output=True, text=True, timeout=30, env=e)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout, r.stderr

    def test_js_engine_checks(self):
        path = os.path.join(ROOT, 'build', 'js-test')
        if not os.path.exists(path):
            self.skipTest('js-test not built')
        scripts = sorted(f for f in os.listdir(os.path.join(ROOT, 'tests', 'js')) if f.endswith('.js'))
        self.assertTrue(scripts)
        for f in scripts:
            r = subprocess.run([path, '--check', os.path.join(ROOT, 'tests', 'js', f)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, f + ': ' + r.stdout + r.stderr)
            self.assertIn(' 0 failures', r.stdout)

    def test_js_dom_page(self):
        out, err = self.web_dump(os.path.join(ROOT, 'tests', 'web', 'dom.html'))
        results = [l for l in out.splitlines() if l.startswith('RESULT')]
        self.assertEqual(len(results), 1, out)
        self.assertIn(' 0 failures', results[0], out)
        self.assertFalse([l for l in out.splitlines() if l.startswith('FAIL')], out)

    def write_page(self, name, body):
        site = os.path.join(tmp, 'site')
        os.makedirs(site, exist_ok=True)
        path = os.path.join(site, name)
        with open(path, 'w') as f:
            f.write(body)
        return path

    def test_js_changes_the_rendered_page(self):
        page = self.write_page('js1.html', '''<html><head><title>T</title></head><body><h1 id="h">Before</h1><ul id="l"></ul>
<script>
document.getElementById('h').textContent = 'After';
['a', 'b'].forEach(function (x) { var li = document.createElement('li'); li.textContent = x.toUpperCase(); document.getElementById('l').appendChild(li); });
document.title = 'New title';
document.write('<p>written</p>');
setTimeout(function () { document.body.appendChild(document.createTextNode('later')); }, 500);
</script><noscript>no script here</noscript></body></html>''')
        out, err = self.web_dump(page, 1000)
        self.assertIn('title: New title', out)
        self.assertIn('After', out)
        self.assertNotIn('Before', out)
        self.assertIn('* A', out)
        self.assertIn('* B', out)
        self.assertIn('written', out)
        self.assertIn('later', out)
        self.assertNotIn('no script here', out)

    def test_js_errors_are_reported_and_do_not_stop_the_page(self):
        page = self.write_page('js2.html', '''<html><body><p id="p">start</p>
<script>undefined_function();</script>
<script>document.getElementById('p').textContent = 'second script ran'; console.log('hello', 1 + 1);</script>
<script>for (;;) {}</script>
<script>document.getElementById('p').textContent += ' and the third';</script>
</body></html>''')
        out, err = self.web_dump(page, 0)
        self.assertIn('second script ran and the third', out)
        self.assertIn('[js error]', err)
        self.assertIn('undefined_function is not defined', err)
        self.assertIn('[js log] hello 2', err)
        self.assertIn('ran too long', err)                    # the runaway loop is stopped by the instruction budget

    def test_js_external_script_and_click_links(self):
        self.write_page('lib.js', "var answer = 6 * 7; function hello(n) { return 'hello ' + n; }")
        page = self.write_page('js3.html', '''<html><body><script src="lib.js"></script><p id="p"></p><a id="a" href="x.html">go</a><div id="d">box</div>
<script>
document.getElementById('p').textContent = hello(answer);
document.getElementById('d').addEventListener('click', function () { this.textContent = 'clicked'; });
document.getElementById('a').addEventListener('click', function (e) { e.preventDefault(); });
location.href = 'other.html';
</script></body></html>''')
        out, err = self.web_dump(page, 0)
        self.assertIn('hello 42', out)
        self.assertIn('navigate: file://', out)
        self.assertIn('other.html', out)
        links = [l for l in out.splitlines() if l.startswith('link: ')]
        self.assertEqual(len([l for l in links if l.startswith('link: js:')]), 2, links)   # both clickable elements are js: links

    def test_js_click_and_timer_in_the_browser_window(self):
        path = os.path.join(ROOT, 'build', 'lumen-web')
        if not os.path.exists(path):
            self.skipTest('lumen-web not built')
        page = self.write_page('js4.html', '''<html><head><title>start</title></head><body><button id="b">Press</button>
<script>
document.getElementById('b').addEventListener('click', function () { document.title = 'clicked'; setTimeout(function () { document.title = 'timer fired'; }, 300); });
</script></body></html>''')
        p = subprocess.Popen([path, page], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None) and (p.kill(), p.wait()))
        win = wait_for_window('lumen.web')
        self.assertIsNotNone(win, 'browser window did not appear')

        def title():
            return wait_for_window('lumen.web')['title']

        def wait_title(want):
            for _ in range(60):
                if title() == want:
                    return True
                time.sleep(0.05)
            return False

        self.assertTrue(wait_title('start - Lumen Web'), title())
        ctl.click(win['x'] + 24, win['y'] + 34 + 16 + 8)        # the button (rendered as a link) on the first line
        self.assertTrue(wait_title('clicked - Lumen Web'), title())
        self.assertTrue(wait_title('timer fired - Lumen Web'), title())      # the timer runs in the main loop without input
        ctl.abs(900, 500)

    def make_cert(self):
        if not shutil.which('openssl'):
            self.skipTest('openssl command not available')
        d = os.path.join(tmp, 'tls')
        os.makedirs(d, exist_ok=True)
        key, crt = os.path.join(d, 'key.pem'), os.path.join(d, 'cert.pem')
        if not os.path.exists(crt):
            r = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', crt, '-days', '2',
                                '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], capture_output=True)
            if r.returncode != 0:
                self.skipTest('cannot create a test certificate')
        return key, crt

    def start_https_server(self, key, crt):
        site = os.path.join(tmp, 'site')
        os.makedirs(site, exist_ok=True)
        with open(os.path.join(site, 'secure.html'), 'w') as f:
            f.write('<html><head><title>Secure page</title></head><body><h1>Hello TLS</h1><p id="p">plain</p>'
                    '<script>document.getElementById("p").textContent = "script over https";</script></body></html>')
        code = ('import http.server, ssl, sys, os\n'
                'os.chdir(sys.argv[1])\n'
                'h = http.server.ThreadingHTTPServer(("0.0.0.0", 0), http.server.SimpleHTTPRequestHandler)\n'
                'c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(sys.argv[2], sys.argv[3])\n'
                'h.socket = c.wrap_socket(h.socket, server_side=True)\n'
                'print(h.server_address[1], flush=True)\n'
                'h.serve_forever()\n')
        srv = subprocess.Popen([sys.executable, '-I', '-c', code, site, crt, key], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(lambda: (srv.kill(), srv.wait(), srv.stdout.close()))
        port = int(srv.stdout.readline())
        return port

    def test_https_with_trusted_certificate(self):
        key, crt = self.make_cert()
        port = self.start_https_server(key, crt)
        out, err = self.web_dump('https://localhost:%d/secure.html' % port, 0, env={'SSL_CERT_FILE': crt})
        self.assertIn('title: Secure page', out)
        self.assertIn('Hello TLS', out)
        self.assertIn('script over https', out)               # scripts run on https pages too

    def test_https_rejects_untrusted_and_mismatched_certificates(self):
        key, crt = self.make_cert()
        port = self.start_https_server(key, crt)
        out, err = self.web_dump('https://localhost:%d/secure.html' % port, 0, env={'SSL_CERT_FILE': os.devnull})
        self.assertIn('Cannot load page', out)
        self.assertIn('certificate not trusted', out)
        self.assertNotIn('Hello TLS', out)
        out, err = self.web_dump('https://127.0.0.2:%d/secure.html' % port, 0, env={'SSL_CERT_FILE': crt})
        self.assertIn('Cannot load page', out)                # trusted certificate, but it is not valid for this name
        self.assertNotIn('Hello TLS', out)
        out, err = self.web_dump('http://localhost:%d/secure.html' % port, 0)
        self.assertIn('Cannot load page', out)                # plain http against a TLS port is an error, not garbage

    def test_app_menu_starts_browser(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'lumen-web')):
            self.skipTest('lumen-web not built')
        ctl.click(40, 14)
        ctl.click(60, 95)                       # third entry: Browser
        win = wait_for_window('lumen.web')
        self.assertIsNotNone(win, 'menu entry did not start the browser')
        ctl.abs(900, 500)
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(KEY_Q)
        ctl.key(KEY_LEFTMETA, False)
        for _ in range(100):
            if not ctl.windows()['windows']:
                break
            time.sleep(0.05)
        self.assertFalse(ctl.windows()['windows'])

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
        if not os.path.exists(os.path.join(ROOT, 'build', 'lumen-run')):
            self.skipTest('lumen-run not built')
        open_it()
        win = wait_for_window('lumen.run')
        self.assertIsNotNone(win, 'run dialog did not appear')
        time.sleep(0.3)
        self.type_text('touch ' + flag)
        ctl.tap(KEY_ENTER)
        for _ in range(100):
            if os.path.exists(path) and not [w for w in ctl.windows()['windows'] if w['app_id'] == 'lumen.run']:
                break
            time.sleep(0.05)
        self.assertTrue(os.path.exists(path), 'command was not run')
        self.assertFalse([w for w in ctl.windows()['windows'] if w['app_id'] == 'lumen.run'],
                         'dialog stays open after running')

    def test_run_dialog_selftest(self):
        path = os.path.join(ROOT, 'build', 'lumen-run')
        if not os.path.exists(path):
            self.skipTest('lumen-run not built')
        r = subprocess.run([path, '--selftest'], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_run_dialog_from_panel_button(self):
        self.run_dialog_runs(lambda: ctl.click(870, 14))

    def test_run_dialog_from_super_r(self):
        def open_it():
            ctl.key(KEY_LEFTMETA, True)
            ctl.tap(19)             # KEY_R
            ctl.key(KEY_LEFTMETA, False)
        self.run_dialog_runs(open_it)

    def test_run_dialog_escape_closes_without_running(self):
        if not os.path.exists(os.path.join(ROOT, 'build', 'lumen-run')):
            self.skipTest('lumen-run not built')
        ctl.click(870, 14)
        self.assertIsNotNone(wait_for_window('lumen.run'))
        time.sleep(0.3)
        ctl.tap(KEY_ESC)
        for _ in range(100):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'lumen.run']:
                break
            time.sleep(0.05)
        self.assertFalse([w for w in ctl.windows()['windows'] if w['app_id'] == 'lumen.run'])

    def test_demo_renders_and_quits_on_escape(self):
        p = self.spawn('lumen-demo')
        win = wait_for_window('lumen.demo')
        self.assertIsNotNone(win, 'demo window did not appear')
        time.sleep(0.3)
        ppm = ctl.dump(os.path.join(tmp, 'demo.ppm'))
        colours = {ppm.px(win['x'] + x, win['y'] + 150) for x in range(0, 400, 20)}
        self.assertGreater(len(colours), 5, 'gradient not rendered')
        ctl.tap(KEY_ESC)
        self.assertEqual(p.wait(timeout=5), 0)

    def test_terminal_runs_shell_and_gets_keys(self):
        out = os.path.join(tmp, 'term-out.txt')
        script = os.path.join(tmp, 'shell.sh')
        with open(script, 'w') as f:
            f.write('#!/bin/sh\nread line\necho "got:$line" > "%s"\nread line2\n' % out)
        os.chmod(script, 0o755)
        p = self.spawn('lumen-term', SHELL=script)
        win = wait_for_window('lumen.term')
        self.assertIsNotNone(win, 'terminal window did not appear')
        time.sleep(0.4)
        type_text('lumen')
        for _ in range(50):
            if os.path.exists(out) and open(out).read().strip():
                break
            time.sleep(0.05)
        self.assertEqual(open(out).read().strip(), 'got:lumen')
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
        p = self.spawn('lumen-term', SHELL=script)
        self.assertEqual(p.wait(timeout=5), 0)


DEFAULT_SETTINGS = {'layouts': 'us,ee,de,fi', 'layout': 'us', 'sticky': '0', 'repeat-delay': '400',
                    'repeat-rate': '25', 'pointer-speed': '100', 'natural-scroll': '0', 'sound': '1', 'gadget': '1'}


def settings():
    """Current settings from the control socket as a dict (the `file` entry is "-" when nothing is saved)."""
    ctl.cmd('settings')
    return dict(kv.split('=', 1) for kv in ctl.last.split()[1:])


def settings_restore():
    for k in ('layouts', 'layout', 'sticky', 'repeat-delay', 'repeat-rate', 'pointer-speed', 'natural-scroll',
              'sound', 'gadget'):
        ctl.cmd('settings %s %s' % (k, DEFAULT_SETTINGS[k]))


class TestSettings(Base):
    """lumen_settings_v1, the settings file and the lumen-settings panel."""

    def setUp(self):
        super().setUp()
        self.addCleanup(settings_restore)

    def bound(self):
        """A raw client bound to lumen_settings_v1 with the first value burst already read."""
        c = self.client()
        obj = c.wl.bind('lumen_settings_v1')
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
        obj = c.wl.bind('lumen_settings_v1')
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

    def private_compositor(self, name, *args):
        sock = os.path.join(tmp, name + '.ctl')
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=0:exitcode=77')
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

    def test_settings_survive_a_restart(self):
        path = os.path.join(tmp, 'persist', 'deeper', 'settings.conf')       # directories are created
        p, c, stop = self.private_compositor('lumen-set1', '--settings', path)
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
        p2, c2, stop2 = self.private_compositor('lumen-set2', '--settings', path)
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
        p, c, stop = self.private_compositor('lumen-set3', '--settings', path, '--layouts', 'fi,us')
        c.cmd('settings')
        s = dict(kv.split('=', 1) for kv in c.last.split()[1:])
        self.assertEqual(s['layouts'], 'fi,us')              # --layouts wins
        self.assertEqual(s['sound'], '0')                    # from the file
        self.assertEqual(s['repeat-rate'], '25')             # bad value ignored

    def test_no_file_without_settings_option_on_headless(self):
        self.assertEqual(settings()['file'], '-')

    # ---- the client ---------------------------------------------------------------------------------------------------

    def exe(self, name='lumen-settings'):
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
        win = wait_for_window('lumen.settings')
        self.assertIsNotNone(win, 'settings window did not appear')
        self.assertEqual((win['w'], win['h']), (620, 440))
        time.sleep(0.3)
        return p, win

    def test_panel_opens_from_menu_and_super_i(self):
        self.exe()
        n = len(ctl.windows()['windows'])
        ctl.key(KEY_LEFTMETA, True)
        ctl.tap(23)                                             # KEY_I
        ctl.key(KEY_LEFTMETA, False)
        # headless tests start lumen with --terminal true, but the settings command is the sibling binary
        win = wait_for_window('lumen.settings')
        self.assertIsNotNone(win, 'Super+I did not open the settings panel')
        ctl.abs(win['x'] + 100, win['y'] + 100)
        ctl.tap(KEY_ESC)                                        # Esc closes it
        for _ in range(100):
            if not [w for w in ctl.windows()['windows'] if w['app_id'] == 'lumen.settings']:
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
        ctl.click(x0 + 70, y0 + 12 + 36 + 16)                   # "Pointer" tab
        time.sleep(0.15)
        ctl.click(x0 + 168, y0 + 42 + 28 + 14)                  # natural scrolling box (second row, below the speed slider)
        time.sleep(0.15)
        self.assertEqual(settings()['natural-scroll'], '1')
        ctl.click(x0 + 70, y0 + 12 + 72 + 16)                   # "Desktop" tab
        time.sleep(0.15)
        ctl.click(x0 + 168, y0 + 42 + 28 + 14)                  # sound box (second row, below the gadget box)
        time.sleep(0.15)
        self.assertEqual(settings()['sound'], '0')
        # keyboard only: Tab x3 reaches the first widget of the page (the gadget checkbox), Space toggles it
        ctl.abs(x0 + 400, y0 + 300)
        for _ in range(4):
            ctl.tap(KEY_TAB)
        ctl.tap(KEY_SPACE)
        time.sleep(0.15)
        self.assertEqual(settings()['gadget'], '0')

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
        ctl.click(x0 + 200, y0 + 366)                           # the "Try it" field
        time.sleep(0.15)

        def field():
            ctl.abs(900, 500)
            time.sleep(0.15)
            ppm = ctl.dump(os.path.join(tmp, 'try.ppm'))
            ctl.abs(x0 + 200, y0 + 366)
            return [ppm.px(x0 + x, y0 + y) for y in range(356, 376) for x in range(166, 190)]
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


if __name__ == '__main__':
    unittest.main(verbosity=2)

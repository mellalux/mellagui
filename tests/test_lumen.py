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


if __name__ == '__main__':
    unittest.main(verbosity=2)

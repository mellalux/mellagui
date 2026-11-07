# MellaGUI architecture

> **Implementation status.** All modules described here exist. Differences from the original design:
> logind `TakeDevice` is implemented by `src/logind.c` (with a fallback to opening the device files, which needs the
> `video` and `input` groups); `client/wlc.[ch]` is a small client library shared by the
> clients; `src/font.c` is generated from a Terminus PSF font by `tools/gen_font.py`;
> `backend_drm.c` has not been exercised on real hardware.


MellaGUI is a from-scratch Wayland compositor and built-in shell in C11 that runs
directly on a Linux VT (DRM/KMS + evdev). No X11, no Xwayland, no libwayland,
wlroots, libinput, xkbcommon, udev, gbm or EGL: the Wayland wire protocol, the
protocol objects, the keymap and the software renderer are all implemented in
this tree. Only libdrm (headers), libc, `linux/input.h`, `linux/kd.h` and
`linux/vt.h` are used.

## Process model

One process, **one thread**, one epoll loop (`loop.c`). There is no locking
anywhere. Every callback runs to completion on the loop; callbacks may add and
remove sources/timers/idles (removal is deferred until the dispatch round ends).

```
                 +-----------------------------------------------------+
 /dev/input/* -> | input_evdev --> seat --> wm ---------> xdg ---> client
                 |                  |  ^      |  ^            ^
 client socket ->| server/wire --> surface ----+  |            |
                 |   (requests)     |  commit/damage            |
                 |                  v                           |
                 |                 wm --> render --> backend --> DRM page flip
                 |                   ^______ frame done ________/
                 +-----------------------------------------------------+
```

### Data flow

* **Input -> client**: `input_evdev` (kernel evdev events, key repeat, pointer
  acceleration, modifier tracking) -> `lm_seat_handle_input()` -> for keys
  `lm_wm_key()` (shortcuts) else `wl_keyboard.key`; for pointer events
  `lm_wm_pick()` finds the thing under the cursor, `lm_wm_pointer_*()` handles
  hover/click on decorations and the panel, otherwise the seat sends
  `wl_pointer.*` to the focused client surface.
* **Client -> screen**: `wl_surface.commit` -> `surface.c` applies pending
  state, copies the shm buffer into the surface's private texture
  (`lm_buffer_copy`), releases the buffer, computes damage -> root surface role
  `damage` hook -> `lm_wm_surface_damage()` (maps to global coordinates) ->
  `lm_render_damage()` -> renderer draws when an output is ready ->
  `lm_output_present()` -> backend page flip -> `output_ready` ->
  `lm_wm_send_frame_done()` fires `wl_surface.frame` callbacks.
* **wm -> client**: the wm calls `window->ops->configure()` / `close()`
  (implemented by `xdg.c`), which emits the xdg events; the client answers with
  `ack_configure` + a commit; `xdg.c` calls `lm_wm_window_set_geometry()`.

### Start-up order (main.c) and teardown (reverse)

1. parse argv / env into `struct lm_config`; `lm_log_*`
2. `lm_loop_create`, `lm_server_create`, install SIGINT/SIGTERM -> `lm_server_quit`
3. backend (`lm_backend_drm_create` or `lm_backend_headless_create`) ->
   `srv->backend`, listener installed (see below), **not yet initialised**
4. `lm_render_create`, `lm_compositor_create`, `lm_shm_create`,
   `lm_output_mgr_create`, `lm_seat_create`, `lm_xdg_create`, `lm_wm_create`,
   `lm_input_create` (+ `lm_input_set_handler(lm_seat_handle_input_cb, seat)`)
5. `lm_backend_init()` -> emits `output_added` for every output
6. `lm_server_listen()`; export `WAYLAND_DISPLAY`; `--exec` command via
   `lm_server_spawn`; `lm_loop_run`
7. teardown: input, wm, xdg, seat, outputs, shm, compositor, render, backend
   (restores the VT), `lm_server_destroy`, loop

The backend listener installed in main.c fans events out:

| listener callback | calls |
|---|---|
| `output_added` | `lm_output_mgr_add`, `lm_render_output_added`, `lm_wm_output_added` |
| `output_changed` | `lm_output_mgr_changed`, `lm_render_output_changed`, `lm_wm_output_changed` |
| `output_removed` | `lm_wm_output_removed`, `lm_render_output_removed`, `lm_output_mgr_remove` |
| `output_ready` | `lm_render_output_ready` |
| `session_active` | `lm_input_set_active`, on true also `lm_render_damage_all` |

Any `srv->xxx` module pointer can be NULL during start-up/teardown; module code
must tolerate a NULL peer.

## Modules, files and ownership map

Every file has exactly one owning agent. Nobody edits a file they do not own;
header/API change requests go to the architect (`header_requests`). Headers in
`src/*.h` were written by the architect and are frozen in the implement phase.

| file(s) | owner | role |
|---|---|---|
| `src/log.[ch]`, `src/list.h`, `src/util.h` | log: **core**; list/util: architect (header-only, complete) | logging; intrusive lists + signals; rect/region helpers |
| `src/loop.[ch]` | **core** | epoll loop, fd sources, timerfd timers, idle callbacks, signalfd |
| `src/wire.[ch]` | **core** | wire protocol framing, marshalling, SCM_RIGHTS (server and client use) |
| `src/protocol.[ch]` | **core** | interface descriptors and opcode constants |
| `src/server.[ch]` | **core** | display socket, clients, object table, globals, dispatch, errors |
| `src/surface.[ch]` | **surface** | wl_compositor/surface/region/subcompositor/subsurface, state, roles |
| `src/shm.[ch]` | **surface** | wl_shm, pools (SIGBUS-safe), wl_buffer, texture copy |
| `src/xdg.[ch]` | **shell** | xdg_wm_base/surface/toplevel/popup/positioner, decoration manager |
| `src/wm.[ch]` | **shell** | window manager, panel, shortcuts, workspaces, scene for the renderer |
| `src/seat.[ch]`, `src/keymap.[ch]` | **input** | wl_seat, data device, cursor, pointer grabs, static US keymap |
| (gadget) | **desktop gadget** | `seat.c` keeps the clipboard history (it reads each new text selection through a pipe it hands to the source); `wm.c` places the card (`LM_SCENE_GADGET`, right after the wallpapers; docked top right until dragged), picks its title bar (`LM_PICK_GADGET_TITLE`: move grab, double click resets) and edges (`LM_PICK_GADGET_BORDER`: resize grab, 200x86..640x560), `render.c` paints it (rounded corners, stacked-alpha shadow, wraps the entries). Super+G toggles; a moved or resized card is remembered in `~/.config/mellagui/gadgets.conf` (`--gadgets-file`, one `name x y w h` line each) |
| `src/sound.h`, `src/sound.c` | **sound** | sticky-key and screenshot beeps: synthesised S16 buffer piped to a detached `paplay` child (default device, via the sound server, which wakes a sleeping HDMI sink itself) or `aplay -D` (a chosen ALSA device); no audio library |
| `src/screenshot.[ch]`, `src/png.[ch]` | **wm** | Print / Alt+Print / Ctrl+Print (`wm.c` `take_screenshot`): `lm_render_capture` repaints the scene into a private buffer (no cursor), `png.c` writes an uncompressed 8-bit RGB PNG (stored deflate blocks, CRC-32/Adler-32 in-tree, no zlib), `screenshot.c` saves it (0600, `mellagui-YYYYMMDD-HHMMSS.png` in `--screenshot-dir`) and makes the compositor the clipboard owner with `lm_seat_set_selection_data(.., "image/png", ..)`; the MBs are written to the reader by a short-lived forked child so a slow client cannot block the loop; the history gadget stays text-only; `lm_render_shot_feedback` (render.c, `shot_fx`) shows a 150 ms white flash and a thumbnail card for `MELLAGUI_TOAST_MS` (8 s), painted above the scene and below the cursor (never over the lock screen), skipped by `lm_render_capture`; a click on the card (`LM_PICK_SHOT_CARD`, `wm.c` `open_shot`) closes it and opens the saved file with `config.view_cmd` (mellagui-view) |
| `src/ddc.[ch]` | **sound** | DDC/CI (VESA MCCS) packets for `client/mellagui-monitor.c`: get/set VCP feature requests, reply parser with checksum (unit-tested), blocking I2C I/O on `/dev/i2c-N` (slave 0x37) |
| `src/sysinfo.[ch]` | **sound** | CPU / memory / temperature and the device (maker, model, CPU, OS, kernel, host, uptime: DMI, `/proc/cpuinfo`, `/etc/os-release`) for the system monitor gadget: parsers of `/proc/stat`, `/proc/meminfo`, the hwmon / thermal sensor search (env `MELLAGUI_SYS_ROOT` prefixes the paths for tests) and a 1 s sampler with a 60-sample history; unit-tested |
| `src/mixer.[ch]` | **sound** | the volume mixer behind the second desktop gadget: `pactl` (default sink, outputs, one slider per application) / `wpctl` (master) / `amixer Master` as backends, polled while shown, commands queued and coalesced, all through `lm_proc_run()` children so the loop never blocks; the text parsers and the card layout / hit-test (`lm_mixer_layout`, `lm_mixer_hit`, shared by `render.c` and `wm.c`) are pure and unit-tested |
| `src/audio.[ch]` | **sound** | sound outputs for the settings panel: ALSA playback devices from `/proc/asound` (HDMI marked), PulseAudio/PipeWire sinks and the default sink via `pactl` (else `wpctl status`; `lm_audio_sink_status()` says why the list is empty), 1 s timeout; PipeWire card profiles via `pw-cli` (`lm_audio_list_profiles` / `lm_audio_set_profile`: one entry per output type of each card, e.g. analog / HDMI / S/PDIF, the pure `output:X` profile preferred; this is how HDMI audio is chosen; `settings.c` re-scans the sinks 0.8 s and 2.3 s after a switch); the chosen ALSA device is where `sound.c` plays (`aplay -D`) |
| `src/lock.[ch]`, `src/sha256.[ch]` | **settings** | screen lock inside the compositor: a password (8..128 bytes) or PIN (4..12 digits) kept only as a salted PBKDF2-HMAC-SHA256 hash (`sha256.c`, in-tree, unit-tested with the RFC vectors) in `lock.conf` (mode 0600, `--lock-file`). While locked `seat.c` (`lm_seat_set_locked`) drops keyboard / pointer focus, cancels grabs, hides the cursor and sends the keys to `lm_lock_key()` only (no wm shortcut, Ctrl+Alt+Fn and Super+Space excepted); `wm.c`'s scene is then just one `LM_SCENE_LOCK` per output (`render.c` `paint_lock`). Three wrong tries start a doubling delay (5 s .. 5 min) that also covers the settings key `lock-set`; `lm_lock_request_quit` (Super+Shift+E / the Exit button ask for the secret before quitting), `vt=` (console switching on the lock screen), idle timeout, monitor standby after `blank` minutes locked (`set_power` backend op = DRM DPMS; the first input only wakes and is swallowed, `lm_lock_activity` returns true), lock at start, Super+L and the panel's *Lock* button (right of the clock) lock. Env for tests: `MELLAGUI_LOCK_ITER`, `_DELAY_MS`, `_UNIT_MS`; control socket `lock`, `lock now`, `lock set OLD KIND NEW [MIN [START [BLANK [VT]]]]`, `lock quit`, `power`, `vt` |
| `src/settings.[ch]` | **settings** | `mellagui_settings_v1` global (`set(key, value)` -> a burst of `value` events + `done` to every bound client), validation and application of each key (layouts: `lm_seat_set_layouts` + `lm_input_set_layouts`; repeat: `lm_seat_send_repeat_info`; sticky/gadget through input/wm), the `key=value` settings file (loaded in main.c before argv, saved debounced + atomically) |
| `src/input.h`, `src/input_evdev.c` | **input** | evdev reading, repeat, acceleration, hotplug, injection, layout + sticky keys state |
| `src/output.[ch]` | **backend** | wl_output globals, output layout queries |
| `src/gl.[ch]`, `src/render_gl.inc` | **render** | hybrid GL renderer (`--renderer`, env `MELLAGUI_RENDERER`). `gl.c`: EGL / GLES2 / GBM through `dlopen` with our own declarations (surfaceless Mesa platform for headless, GBM platform + window surfaces for scanout, `GL_EXT_texture_format_BGRA8888` so premultiplied ARGB uploads as it is, y handled by a sign in the vertex shader). `render_gl.inc` (included by `render.c`, which owns the painters it reuses): per frame the whole output is redrawn on the GPU - background texture (wallpaper + gadgets, CPU-painted only where damaged and not covered by an opaque surface), window frames as solid rectangles + a title bar texture (hash-cached), surface trees as quads from `lm_surface.gl_tex` (`surface.c` keeps `gl_full` / `gl_damage`), panel / menu / lock as scratch-painted textures when damage touches them, the screenshot card painted over black and white to recover its alpha, flash and cursor. Headless reads the offscreen target back into the memory framebuffer on `dump` (`lm_render_readback`). The DRM backend does not use it yet; dmabuf is still copied by the CPU. Tests: `build/gl-test` (`TestGL`, incl. the GBM window path on a render node), `TestGLParity` |
| `src/logind.[ch]` | **backend** | device access through systemd-logind without libsystemd / libdbus: a small D-Bus client (EXTERNAL auth, `NEGOTIATE_UNIX_FD`, `SCM_RIGHTS`, little-endian only; `DBUS_SYSTEM_BUS_ADDRESS` or `/run/dbus/system_bus_socket`), `Manager.GetSession($XDG_SESSION_ID)` / `GetSessionByPID`, `Session.TakeControl`, `Properties.Get(Seat)` (the seat object path: `seat/self` does not exist), `TakeDevice` / `ReleaseDevice`, `PauseDevice` (the watched DRM device: the backend stops drawing and calls `lm_logind_pause_done`; others are acked by `logind.c`), `ResumeDevice`, `Seat.SwitchTo`. Calls are synchronous (3 s limit), signals are queued and dispatched from the loop (idle) so callbacks may call back in. `lm_dev_open` / `lm_dev_close` take a possibly NULL `lm_logind` (then plain `open` / `close`). With logind the DRM backend skips `vt_setup`, `drmSetMaster` / `drmDropMaster` and the tty (logind sets the console to graphics mode and restores it); the input backend closes all devices on pause and takes them again on resume (evdev nodes are revoked while paused). `--logind off|auto|on`. Tests: `tests/fake_logind.py` + `build/logind-test` (`TestLogind`) |
| `src/backend.h`, `src/backend_drm.c`, `src/backend_headless.c` | **backend** | DRM/KMS + VT; headless + control socket |
| `src/dmabuf.[ch]` | **protocol** | `zwp_linux_dmabuf_v1` v4: default/surface feedback (the display GPU's render node as main device, a memfd format table), params -> `lm_buffer_create_dmabuf()` (shm.c); only linear ARGB8888/XRGB8888, one plane, no flags; the fd must be a real dmabuf (the sync ioctl is the test), bounds checked against its size |
| `src/cvt.[ch]`, `src/edid.[ch]` | **backend** | video-mode helpers without DRM types (unit-tested in `keymap-test`): CVT reduced-blanking timings (`lm_cvt_rb_fit`: the highest usual refresh rate that fits a pixel clock limit; `drm_list_modes` offers the panel's native size that way when the kernel pruned it); the EDID's native timing and the rule for when the offered mode has the wrong shape (a 21:9 panel with only 16:9 modes gets a generated mode of its own shape instead of being stretched by the monitor) |
| `src/render.[ch]`, `src/font.[ch]` | **render** | software compositor, cursor, decorations, 8x16 font |
| `src/main.c` | **core** | arguments, wiring, signals, clean shutdown |
| `client/mellagui-term.c` (later) | **clients** | terminal emulator |
| `client/mellagui-settings.c` | **clients** | settings panel (Keyboard, Pointer, Desktop, Display, Sound, Security): tabs, checkboxes, radio lists, sliders, layout list and a typing test field drawn immediate-mode from a per-frame widget list; also `--get` / `--set` for scripts; it only shows what the compositor reports back |
| `client/wlwin.[ch]` | **clients** | one software-drawn xdg-toplevel on top of `wlc`: registry binding, a single shm buffer, keymap/modifiers, fill/text helpers; used by calc, run and settings (not by the terminal) |
| `client/mellagui-view.c`, `client/img.[ch]` | **clients** | image viewer on `wlwin` (menu entry *Images*; no argument = the newest image of `~/Pictures`; FILE = its directory starting there): `img.c` decodes PNG (own inflate after zlib's puff: stored / fixed / dynamic blocks; colour types 0,2,3,4,6, depths 1..16, tRNS for palettes, all five row filters; no interlacing, refused with a message; at most 64 Mpixel) and P6 PPM, transparency over a grey checkerboard; the view box-filters down (cached per zoom), nearest-neighbour up; Left/Right/Home/End browse, +/-/wheel zoom around the pointer, drag pans, F/0, R, Esc. `--decode FILE OUT.ppm` and `--selftest` for the tests (`TestImages` feeds it zlib output of every type, `TestViewer` drives the window) |
| `client/mellagui-calc.c` | **clients** | calculator (engine + UI in one file; `--selftest` runs the engine checks) |
| `tests/wlclient.py`, `tests/test_*.py`, `tests/run.sh` | **tests** | end-to-end tests against `mellagui --backend headless` |
| `Makefile`, `docs/`, `README.md` | architect | build, docs |

(The role names are suggestions for grouping; the invariant is one owner per file.)

## Cross-module seams (what to read in the headers)

* **Objects.** `server.h`: `struct lm_resource` (id, iface, version,
  `handlers[]` indexed by request opcode, `data`, `destroy` hook,
  `destroy_signal`). `lm_resource_create()` for client ids,
  `lm_resource_create_server()` for server ids, `lm_resource_post()` to send
  events by opcode with varargs following the signature, `lm_resource_destroy()`
  from destructor handlers. Interface tables and opcodes: `protocol.h`, which also
  has the **resource data contract** (what `res->data` points to per interface).
* **Surfaces and roles.** `surface.h`: pending/current state split, role
  (`commit` + `damage` hooks, active iff `role_data != NULL`), subsurface tree
  visit/pick/extents, frame callbacks, output enter/leave.
* **Windows.** `wm.h`: `struct lm_window` created by xdg, `struct lm_window_ops`
  (`configure`, `close`) implemented by xdg, window geometry convention, popups,
  `lm_wm_scene_for_each()` for the renderer.
* **Focus and grabs.** `seat.h`: `lm_seat_set_keyboard_focus`,
  `lm_seat_pointer_grab(struct lm_pointer_grab_ops)`, primitive send functions,
  serial validation, cursor shape override.
* **Backend.** `backend.h`: `struct lm_output`, `struct lm_framebuffer` (with
  buffer age), listener callbacks, vtable, headless control protocol.
* **Input.** `input.h`: `struct lm_input_event` (key with `mods_*` and
  `repeat`, relative/absolute motion, button, axis, touch) and `lm_input_inject`.

## Advertised globals

| interface | version | created by |
|---|---|---|
| `wl_compositor` | 4 | surface.c |
| `wl_subcompositor` | 1 | surface.c |
| `wl_shm` | 1 (ARGB8888, XRGB8888) | shm.c |
| `wl_output` | 3 (one per output) | output.c |
| `wl_seat` | 5 | seat.c |
| `wl_data_device_manager` | 3 | seat.c |
| `xdg_wm_base` | 3 | xdg.c |
| `zxdg_decoration_manager_v1` | 1 (always `server_side`) | xdg.c |
| `mellagui_settings_v1` | 1 (MellaGUI private) | settings.c |

## Design decisions worth knowing

* **Surfaces copy, they do not keep, client memory.** On commit the damaged
  part of the wl_buffer is converted to a private premultiplied ARGB8888
  `struct lm_texture` and the buffer is released immediately. The renderer
  never touches client shm; the only SIGBUS exposure (client truncating its
  pool) is inside `lm_buffer_copy`.
* **Rendering is CPU compositing** into backend framebuffers; `lm_framebuffer.age` drives partial redraw. No GL.
  The DRM backend composes into a *shadow frame* in ordinary memory (age 1) and `present` copies only the changed
  rectangles into the back dumb buffer (each dumb buffer keeps a list of what it still lacks): dumb buffers are
  write-combining, reading them back for alpha blending runs at ~54 MB/s (writes 6.9 GB/s).
* **DRM hotplug never force-probes on a timer.** `drmModeGetConnector` re-reads every EDID over I2C (230 ms for 7
  connectors on the dev machine) and stalls the single-threaded loop; the 2 s poll uses `drmModeGetConnectorCurrent`
  and a forced probe only follows a kernel `drm` `HOTPLUG=1` uevent (netlink group 1) and at start-up.
* **Key repeat.** The compositor advertises `wl_keyboard.repeat_info`; clients
  repeat themselves. `input_evdev` also generates repeats (flagged
  `repeat = true`) for compositor shortcuts only; the seat does not forward them.
* **Modifiers** are tracked once, in the input module, and carried in each key
  event (`LM_MOD_*` = XKB real-modifier bit layout matching the generated keymap): depressed,
  latched (sticky keys), locked (Caps/Num and sticky locks) and the active layout `group`.
  State changes without a key (layout switch, latch consumed, sticky toggled) are `LM_INPUT_MODS`
  events; the seat turns both into `wl_keyboard.modifiers`.
* **Layouts / sticky keys.** `input_evdev.c` owns them (`lm_input_*layout*`, `lm_input_*sticky*`): one
  XKB group per configured layout (`--layouts`), Super+Space and the panel indicator switch,
  right Alt is AltGr (Mod5 / LevelThree) unless the only layout is `us`. Sticky keys: a clean tap
  latches a modifier until the next key press, a second tap locks, a third unlocks, Shift x5 toggles.
  The wm sees depressed|latched|locked modifiers in `lm_wm_key`, so latched Super works for shortcuts.
* **Keymap.** `keymap.c` holds the layout tables (typewriter block as UTF-8 strings per level, `us` plus
  overrides) and generates the XKB text (`keymap_xkb.h` has the layout independent part, dumped from
  the stock evdev keymap) delivered over a memfd (`xkb_v1`); it also offers the client side
  evdev -> keysym -> UTF-8 helpers (`lm_keymap_client_load` parses the group names from the keymap,
  `lm_keymap_client_group` follows `wl_keyboard.modifiers`) that clients link instead of xkbcommon.
  The XKB text could not be compiled with libxkbcommon on the dev machine (not installed): the
  layout part follows the syntax of the stock keymaps, but test it with a real xkbcommon client.
* **Settings.** One source of truth per value: layouts/active layout/sticky live in `input_evdev.c`, the gadget in `wm.c`, the
  rest in `server->config`. `settings.c` reads them back from there, so a change made with Super+Space, five Shifts or Super+G is
  announced to the panel the same way (`lm_settings_changed`). Changing the layout *set* builds a new keymap memfd and re-sends
  `wl_keyboard.keymap` to every keyboard object (the seat forgets its last group so the modifiers event that follows is not
  deduplicated: a new keymap resets the clients' xkb state).
* **Decorations** are server-side only. The wm computes all frame/panel
  rectangles; the renderer only paints them.
* **Pointer grabs** (`lm_pointer_grab`) are used for interactive move/resize and
  xdg_popup grabs. Starting a grab clears pointer focus.
* **Headless backend** has a line-based control socket (`dump`, `sync`, `key`,
  `motion`, `abs`, `button`, `axis`, `windows`, `kbd`, `layout`, `sticky`, `settings [key value]`, `quit`) documented in
  `backend.h`; the Python tests use it for synthetic input and screenshots.

## Error handling policy

* Everything that comes from a client is hostile: lengths, offsets, strides,
  fds, ids, coordinates. Use `__builtin_*_overflow` (or the clamping helpers in
  `util.h`) for every size computation; no VLAs of untrusted size; bound all
  loops and queue sizes.
* A protocol violation is answered with `wl_display.error`
  (`lm_resource_post_error` / `lm_client_post_error`) and the client is
  disconnected after the current dispatch round. The handler returns right after
  posting the error.
* Never crash or hang on malformed input; never abort() because of a client.
* On disconnect every resource of the client is destroyed (reverse creation
  order); no leaks (`make asan` must stay clean).
* Backpressure: if a client's output queue exceeds its limit it is dropped.
* Environment problems (no permission for /dev/input, /dev/dri) produce one
  clear diagnostic with the remedy (group `input`, logind seat session,
  running from a real VT) and, where possible, the compositor keeps running.
* The DRM backend always restores the VT (KD_TEXT, VT_AUTO, previous CRTC, drop
  master) on exit, on SIGINT/SIGTERM/SIGHUP and on fatal signals (async-signal-safe
  minimal restore path).

## Conventions

C11 (`-std=gnu11`), 4-space indent, snake_case, public symbols prefixed `lm_`,
header guards `MELLAGUI_<NAME>_H`, comments only where non-obvious, English
identifiers and comments. Build: `make`, `make asan`, `make test`.
Binaries: `build/mellagui`, `build/mellagui-term`, `build/mellagui-calc`, `build/mellagui-view`.

## Testing

`tests/run.sh` starts `build/mellagui --backend headless --size 1024x768 --socket
<unique> --control <path>` with a private `XDG_RUNTIME_DIR`, then runs
`tests/test_*.py`. `tests/wlclient.py` is a raw-socket Wayland client (struct
packing, `socket.sendmsg` with SCM_RIGHTS, `os.memfd_create` + `mmap` for shm
buffers). Tests assert on protocol events, on `windows` output and on PPM dumps.

## Changing the resolution while running

`mellagui_settings_v1` key `resolution` -> `settings.c` stores `drm_mode_w/h/hz` in the config and calls `ops->apply_mode(out)` of every
output (`lm_output_apply_mode`). `backend_drm.c` re-picks the mode with the same `pick_mode()` it uses at start (listed mode, generated
CVT mode, preferred mode, or the native-shape correction), creates two new dumb buffers of the new size, `drmModeSetCrtc`s with them
(on failure the old buffers and mode are put back) and then reports `output_changed`, which makes `output.c` re-announce `wl_output`,
`render.c` redraw everything and `wm.c` re-apply the maximized/fullscreen geometry. The change stays *pending*: `settings.c` arms a timer
(15 s, `MELLAGUI_REVERT_MS`) that restores the previous mode unless `resolution-confirm=1` arrives; only a kept value is saved. The headless
backend implements the same two ops (a reallocated framebuffer) so all of this is tested end to end.

## GPU clients (dmabuf) and the painting fast paths

A GPU-rendering client (Mesa EGL, Firefox WebRender) allocates a linear buffer object on the render node named by the dmabuf
feedback and attaches it as a `wl_buffer`. On commit `shm.c` copies it into the surface's texture like any shm buffer; for a dmabuf
the source is the mmap of its fd, read after `DMA_BUF_IOCTL_SYNC` (which also waits for the GPU) with SSE4.1 streaming loads
(`stream_copy_fix_sse41` does the alpha fix in the same pass, so the pixels are not read back from the texture), under
a SIGBUS guard (`sigsetjmp`; any other SIGBUS goes to the previous handler). `lm_texture.solid` records that every pixel had alpha
0xff (sticky until the next full copy), so windows whose buffer format has alpha but which are in fact opaque blit with memcpy.
`render.c` `paint_rect()` first looks for the biggest opaque rectangle of any surface in the repainted area (the whole surface when
solid, else a rectangle of the client's opaque region) and paints only the items from that surface upwards inside it; the strips
around it (transparent corners) get the full stack. 4-pixel SSE2 groups handle fill, blit and `fix_pixels`.

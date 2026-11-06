# Lumen architecture

> **Implementation status.** All modules described here exist. Differences from the original design:
> logind `TakeDevice` is *not* implemented (`seatdev.c` opens devices directly, so the user needs the
> `video` and `input` groups); `client/wlc.[ch]` is a small client library shared by `lumen-demo` and
> `lumen-term`; `src/font.c` is generated from a Terminus PSF font by `tools/gen_font.py`;
> `backend_drm.c` has not been exercised on real hardware.


Lumen is a from-scratch Wayland compositor and built-in shell in C11 that runs
directly on a Linux VT (DRM/KMS + evdev). No X11, no Xwayland, no libwayland,
wlroots, libinput, xkbcommon, udev, gbm or EGL: the Wayland wire protocol, the
protocol objects, the keymap and the software renderer are all implemented in
this tree. Only libdrm (headers), libc, `linux/input.h`, `linux/kd.h` and
`linux/vt.h` are used; `libsystemd.so.0` is optionally `dlopen()`ed for
logind device access.

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
3. `lm_seatdev_create` -> `srv->seatdev`
4. backend (`lm_backend_drm_create` or `lm_backend_headless_create`) ->
   `srv->backend`, listener installed (see below), **not yet initialised**
5. `lm_render_create`, `lm_compositor_create`, `lm_shm_create`,
   `lm_output_mgr_create`, `lm_seat_create`, `lm_xdg_create`, `lm_wm_create`,
   `lm_input_create` (+ `lm_input_set_handler(lm_seat_handle_input_cb, seat)`)
6. `lm_backend_init()` -> emits `output_added` for every output
7. `lm_server_listen()`; export `WAYLAND_DISPLAY`; `--exec` command via
   `lm_server_spawn`; `lm_loop_run`
8. teardown: input, wm, xdg, seat, outputs, shm, compositor, render, backend
   (restores the VT), seatdev, `lm_server_destroy`, loop

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
| (gadget) | **desktop gadget** | `seat.c` keeps the clipboard history (it reads each new text selection through a pipe it hands to the source); `wm.c` places the card (`LM_SCENE_GADGET`, right after the wallpapers; docked top right until dragged), picks its title bar (`LM_PICK_GADGET_TITLE`: move grab, double click resets) and edges (`LM_PICK_GADGET_BORDER`: resize grab, 200x86..640x560), `render.c` paints it (rounded corners, stacked-alpha shadow, wraps the entries). Super+G toggles |
| `src/sound.h`, `src/sound.c` | **sound** | sticky-key beeps: synthesised S16 buffer piped to a detached `aplay` child (no audio library) |
| `src/settings.[ch]` | **settings** | `lumen_settings_v1` global (`set(key, value)` -> a burst of `value` events + `done` to every bound client), validation and application of each key (layouts: `lm_seat_set_layouts` + `lm_input_set_layouts`; repeat: `lm_seat_send_repeat_info`; sticky/gadget through input/wm), the `key=value` settings file (loaded in main.c before argv, saved debounced + atomically) |
| `src/input.h`, `src/input_evdev.c` | **input** | evdev reading, repeat, acceleration, hotplug, injection, layout + sticky keys state |
| `src/seatdev.[ch]` | **input** | device open (direct / logind TakeDevice via dlopen) |
| `src/output.[ch]` | **backend** | wl_output globals, output layout queries |
| `src/backend.h`, `src/backend_drm.c`, `src/backend_headless.c` | **backend** | DRM/KMS + VT; headless + control socket |
| `src/render.[ch]`, `src/font.[ch]` | **render** | software compositor, cursor, decorations, 8x16 font |
| `src/main.c` | **core** | arguments, wiring, signals, clean shutdown |
| `client/lumen-demo.c` | **clients** | demo client using `wire.c` + `protocol.c` in client mode |
| `client/lumen-term.c` (later) | **clients** | terminal emulator |
| `client/lumen-settings.c` | **clients** | settings panel: tabs, checkboxes, sliders, layout list and a typing test field drawn immediate-mode from a per-frame widget list; also `--get` / `--set` for scripts; it only shows what the compositor reports back |
| `client/lumen-calc.c` | **clients** | calculator (engine + UI in one file; `--selftest` runs the engine checks) |
| `tests/wlclient.py`, `tests/test_*.py`, `tests/run.sh` | **tests** | end-to-end tests against `lumen --backend headless` |
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
| `lumen_settings_v1` | 1 (Lumen private) | settings.c |

## Design decisions worth knowing

* **Surfaces copy, they do not keep, client memory.** On commit the damaged
  part of the wl_buffer is converted to a private premultiplied ARGB8888
  `struct lm_texture` and the buffer is released immediately. The renderer
  never touches client shm; the only SIGBUS exposure (client truncating its
  pool) is inside `lm_buffer_copy`.
* **Rendering is CPU compositing** into backend framebuffers (DRM dumb buffers,
  double buffered; `lm_framebuffer.age` drives partial redraw). No GL.
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
header guards `LUMEN_<NAME>_H`, comments only where non-obvious, English
identifiers and comments. Build: `make`, `make asan`, `make test`.
Binaries: `build/lumen`, `build/lumen-demo`, `build/lumen-term`, `build/lumen-calc`.

## Testing

`tests/run.sh` starts `build/lumen --backend headless --size 1024x768 --socket
<unique> --control <path>` with a private `XDG_RUNTIME_DIR`, then runs
`tests/test_*.py`. `tests/wlclient.py` is a raw-socket Wayland client (struct
packing, `socket.sendmsg` with SCM_RIGHTS, `os.memfd_create` + `mmap` for shm
buffers). Tests assert on protocol events, on `windows` output and on PPM dumps.

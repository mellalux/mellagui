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
| `wm.c` / `render.c` (session dialog) | **wm** | the panel's round power button (right end; the Lock and Exit buttons are gone) opens a modal dialog on the output whose panel was clicked (`power_set`, remembered by output name so an unplugged output ends it): `LM_SCENE_POWER` (above windows, popups and notifications, below the Alt+Tab list and the lock screen) = the output dimmed + a rounded card with two round buttons. While open `lm_wm_pick` returns `LM_PICK_POWER_BUTTON` / `_CARD` / `_BACKDROP` for every point (a press on the backdrop closes it and, as it is consumed, its release is not forwarded either) and `lm_wm_key` swallows every key except Ctrl+Alt+Fn: Esc closes, Left / Right / Tab move the highlight, Enter activates it, L = Lock, E = Exit. Lock = `lm_lock_now` (the settings open while no secret is set), Exit = `request_quit` (with a secret set it locks first and quits after it was typed). `paint_shape` draws discs and the power / padlock icons from signed-distance functions with a 1 px anti-aliased edge. The GL renderer gets the dialog through `gl_scratch_alpha` (painted over black and over white, alpha from the difference) cached by the highlight |
| `dock.c` / `dockicon.c` / `wm.c` / `render.c` (dock) | **wm** (config in `dock.c`) | the round icon buttons without a background along the bottom of the primary output, a Dock-like launcher. `[dock]` (enabled, autohide, icon-size, zoom, gap, margin) and `[dock-apps]` (`Name = [icon] command`, same command rules as `[menu]`, at most `LM_DOCK_MAX` = 16) are parsed by `dock.c` (`lm_dock_conf`, global `lm_dock`, reset/resolve on every reload like the menu; `lm_launch_exists` drops entries whose program is missing). An icon is a built-in name (`dockicon.c`: a coloured disc with a white glyph drawn from signed distances, with its own soft shadow) or a PNG / JPEG / PPM (`lm_img_load_alpha`, premultiplied and area-averaged to the zoomed size at load, cut to a circle when its corners are opaque); without one it is guessed from the entry's name. `LM_SCENE_DOCK` sits above the panel and below the menu (the dock floats over windows and takes no screen area). Geometry (`dock_geometry` / `dock_info` in wm.c): the row is centred on the primary output; while the pointer is in it the icons near it grow with a cosine bell up to `zoom` percent and the others are pushed aside, but `dock_pick` measures against the *unmagnified* places so nothing moves under the pointer (`LM_PICK_DOCK_ITEM` / `LM_PICK_DOCK`; the taller magnified strip counts only once the pointer is inside). A program started by a click makes its icon hop (a parabola, up to half the icon size, 640 ms a hop, `dock_anim_tick` repaints every 33 ms) until the number of windows matching it grows or 12 s pass (control `dock`: `launching=1`). A dot under an icon = a mapped window whose app id (dots as dashes, lower case) equals the lower-cased program name of the command (`program_key`); a click brings the most recently used such window forward, but when that window already has the focus (or there is none) it starts the command again; a middle click always starts it. Autohide: the dock is only in the scene while `dock_shown`, which the pointer touching the bottom 2 rows turns on and a 600 ms timer turns off after it left. A fullscreen window hides it. GL: `gl_scratch_alpha` like the power dialog, cached by a hash of the boxes / hover / running dots. Settings keys `dock-0..15` (`name<US>icon<US>command`), `dock-opts`, `dock-put/del/move/set/reset` (`dock_op`, `lm_confedit_*`); control socket `dock` lists the icons the way they are laid out now |
| `about.c` / `wm.c` / `render.c` (About dialog, logo button) | **wm** | the round logo button top left of the panel replaced the MellaGUI menu (which is gone, with `[menu]`, `--app`, the settings' Menu page and `config.apps`): `panel_info.launcher` is a circle, `LM_PICK_PANEL_LAUNCHER` tests the circle. A click opens the modal About dialog (`about_set` / `about_info` in wm.c, remembered by output name like the session dialog; `LM_SCENE_ABOUT` above windows, popups and notifications, `LM_PICK_ABOUT_BUTTON` / `_CARD` / `_BACKDROP`; Esc, Enter, Space, Close or a click outside close it, the two modal dialogs close each other). The card: the 96 px logo (`src/logo_data.h`, generated by `tools/gen_logo.py` from `tools/mella-logo.jpg`), the name and `lm_about_collect` rows (read from /proc, uname and the server's state when it opens). `paint_logo` samples the logo bilinearly into any square (the 32 px picture up to 48 px, else the 96 px one) |
| `saver.c`, `client/mellagui-matrix.c` | **wm** / clients | the screensaver: a 1 s timer (`tick_cb`) starts `config.saver_cmd` as a normal client after `saver_min` idle minutes (idle = time since the last input that `lm_seat_handle_input` saw), unless the screen is locked or a window is fullscreen. The first input while it runs kills its process group and is dropped (`lm_saver_activity` returns true, like the lock's standby wake-up); locking ends it too. The Matrix program is a `wlwin` client that asks for fullscreen before its first commit, hides the pointer (`set_cursor` with no surface) and ends itself on a key, a button, a wheel turn or a pointer travel of 14 px |
| `src/lock.[ch]`, `src/sha256.[ch]` | **settings** | screen lock inside the compositor: a password (8..128 bytes) or PIN (4..12 digits) kept only as a salted PBKDF2-HMAC-SHA256 hash (`sha256.c`, in-tree, unit-tested with the RFC vectors) in `lock.conf` (mode 0600, `--lock-file`). While locked `seat.c` (`lm_seat_set_locked`) drops keyboard / pointer focus, cancels grabs, hides the cursor and sends the keys to `lm_lock_key()` only (no wm shortcut, Ctrl+Alt+Fn and Super+Space excepted); `wm.c`'s scene is then just one `LM_SCENE_LOCK` per output (`render.c` `paint_lock`). Three wrong tries start a doubling delay (5 s .. 5 min) that also covers the settings key `lock-set`; `lm_lock_request_quit` (Super+Shift+E / the Exit button ask for the secret before quitting), `vt=` (console switching on the lock screen), idle timeout, monitor standby after `blank` minutes locked (`set_power` backend op = DRM DPMS; the first input only wakes and is swallowed, `lm_lock_activity` returns true), lock at start, Super+L and the panel's *Lock* button (right of the clock) lock. Env for tests: `MELLAGUI_LOCK_ITER`, `_DELAY_MS`, `_UNIT_MS`; control socket `lock`, `lock now`, `lock set OLD KIND NEW [MIN [START [BLANK [VT]]]]`, `lock quit`, `power`, `vt` |
| `src/capture.[ch]`, `src/jpegenc.[ch]`, `src/mp4.[ch]`, `client/mellagui-record.c` | **recorder** | screen recorder as a separate program. `capture.c` is the private `mellagui_capture_v1` global (`list` -> `output` events, `grab(index, wl_buffer)` -> the output is painted by `lm_render_capture` (scene without cursor, GL readback with the GL renderer) and written into the client's wl_shm buffer with `lm_buffer_write` (shm.c, `pwrite` on the pool fd: a client that truncated its pool gets an error, never a SIGBUS) -> `ready` / `failed(reason)`: `no-output` `buffer` `size` `locked` `capture`); served only to MellaGUI's own programs (`lm_settings_client_allowed`, the rule of `--settings-access`), nothing while the screen lock is on. `mellagui-record` keeps a ring of 4 shared buffers, asks for one picture at a time every 1/fps s, drops a picture identical to the one before (hash; the earlier one just lasts longer, so a still screen costs nothing), and hands the others to an encoder thread: `jpegenc.c` (baseline JPEG, 4:4:4, Annex K tables, even/odd DCT, flat blocks shortcut) and `mp4.c` (ftyp + 64-bit mdat appended as pictures arrive, moov with `stts` (variable frame times, ms) / `stsz` / one chunk written by `lm_mp4_close`; sample entry `mp4v` + esds object type 0x6C = Motion JPEG, plays in ffmpeg / VLC / mpv, not in browsers; a file that was never closed has no index; 0600, never overwrites). When the encoder is behind, pictures are skipped, not queued. No cursor, no sound. Tests `TestRecord` |
| `src/settings.[ch]` | **settings** | `mellagui_settings_v1` global (`set(key, value)` -> a burst of `value` events + `done` to every bound client), validation and application of each key (layouts: `lm_seat_set_layouts` + `lm_input_set_layouts`; repeat: `lm_seat_send_repeat_info`; sticky/gadget through input/wm), the `key=value` settings file (loaded in main.c before argv, saved debounced + atomically) |
| `src/input.h`, `src/input_evdev.c` | **input** | evdev reading, repeat, acceleration, hotplug, injection, layout + sticky keys state |
| `src/output.[ch]` | **backend** | wl_output globals, output layout queries |
| `src/output.c` (layout) | **output** | `layout_pack` places the outputs in a row, top aligned, sorted by their rank in the setting `output-order` (comma separated connector names, leftmost first; unnamed ones follow in connection order); `lm_output_mgr_relayout` repacks and announces the moved ones (`wl_output.geometry`), `lm_wm_relayout_outputs` records where every window is on its output beforehand and moves it along afterwards (maximized ones follow the new geometry); `lm_output_primary` = the leftmost output, where the gadgets and notifications are. The headless backend has `--output WxH` (repeatable, up to 3 more outputs), control `outputs` and `dump PATH N` for tests. Settings: `output-order` (saved), `outputs` (read only list); the Display page has `<` / `>` per monitor |
| `src/wm.c` (notifications) | **wm** | `lm_wm_notify` (called by the write-only settings key `notify` = "title<TAB>body<TAB>ms", i.e. by `mellagui-notify` and the control socket `notify`): up to `LM_NOTIFY_MAX` cards, newest first, stacked down from the top right of the first output (`note_rects`), body wrapped to 36 columns x 4 lines (`note_wrap`), a timer for expiries; each card is a `LM_SCENE_NOTIFY` item (software `paint_notify`, GL: a hash-cached texture per card) above the windows and panel, `LM_PICK_NOTIFY` makes a click dismiss it and swallows the click; not shown while locked |
| `src/render.c` (wallpaper), `src/img.[ch]` | **render** | settings `wallpaper` / `wallpaper-mode`: `lm_render_set_wallpaper` decodes the file with `img.c` (shared with the viewer; PNG / JPEG (`jpeg.c`) / PPM, at most 64 Mpixel) and bumps `wall.gen`; each output builds its own scaled copy on demand (`wall_build`: fill = cover + centre crop, fit = letterbox, stretch, center; box average when shrinking, bilinear when growing) and `paint_wallpaper` copies the rows over the gradient (so it also feeds the GL background texture); a bad file gives status `error: ...` (`wallpaper-status`) and the gradient |
| `src/wm.c` (switcher) | **wm** | Alt+Tab: `lm_wm_key` opens `sw_*` state on Alt+Tab (the windows sorted by `focus_seq`, which `set_focus` bumps), steps on Tab / Shift+Tab, `sw_commit` on the release of Alt (the release is passed on), Esc closes; the list is a scene item `LM_SCENE_SWITCHER` above everything but the lock screen, painted by `paint_switcher` (software) or a damage-driven chrome texture (GL); closed when a window goes away or the screen locks |
| `src/gl.[ch]`, `src/render_gl.inc` | **render** | hybrid GL renderer (`--renderer`, env `MELLAGUI_RENDERER`). `gl.c`: EGL / GLES2 / GBM through `dlopen` with our own declarations (surfaceless Mesa platform for headless, GBM platform + window surfaces for scanout, `GL_EXT_texture_format_BGRA8888` so premultiplied ARGB uploads as it is, y handled by a sign in the vertex shader). `render_gl.inc` (included by `render.c`, which owns the painters it reuses): per frame the whole output is redrawn on the GPU - background texture (wallpaper + gadgets, CPU-painted only where damaged and not covered by an opaque surface), window frames as solid rectangles + a title bar texture (hash-cached), surface trees as quads from `lm_surface.gl_tex` (`surface.c` keeps `gl_full` / `gl_damage`), panel / menu / lock as scratch-painted textures when damage touches them, the screenshot card painted over black and white to recover its alpha, flash and cursor. Headless reads the offscreen target back into the memory framebuffer on `dump` (`lm_render_readback`). DRM (`--renderer gl`): `drm_init` makes the context on a GBM device on the card fd (`lm_backend.gl`, picked up by the renderer in `output_added`); every output gets a `gl_scan` = a GBM surface + EGL window, a black first frame that carries the modeset, and `drm_gl_present` = `eglSwapBuffers` + `lm_gl_window_swap` (lock the front bo) + `drmModeAddFB2` (cached per GEM handle) + page flip; the page flip event releases the buffer it replaced. Mode changes build a new window and keep the old one until the new mode is accepted; session resume drops a flip that never completed. Falls back to dumb buffers when the context cannot be made (unless `--renderer gl`). Confirmed on a monitor by the user. Zero copy: a dmabuf commit (`surface.c` `attach_buffer`, normal transform only) calls `lm_render_dmabuf_import` = `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` bound to the surface's texture (`lm_gl_tex_bind_image`); `tex.pixels` stays NULL (`tex` keeps size + opacity, XRGB is solid), the wl_buffer is ref'd in `gl_held` and released when the next buffer replaces it, the surface is unmapped or destroyed; a failed import or a shm buffer goes back to the copy path (`drop_zero_copy`). Screenshots of a GL renderer are rendered offscreen by the GPU (`gl_capture`), since there are no CPU pixels. Invalid premultiplied data is not repaired any more (the copy step did). Layouts: `dmabuf.c` builds its format table (`fill_formats`) after the backend is up, from `eglQueryDmaBufModifiersEXT` through `lm_gl_dmabuf_modifiers` - ARGB8888 / XRGB8888 in linear plus every single-plane layout the GPU samples through a plain GL_TEXTURE_2D (not `external_only`, and only layouts on the `lm_gl_dmabuf_modifier_ok` list: linear and Intel X / Y / Yf tiled; compressed layouts have a second plane that `zwp_linux_buffer_params_v1.add` would have to take). `params_make_buffer` refuses a (format, modifier) that was not offered; the modifier travels in `lm_dmabuf_fd` to `eglCreateImage` (`EGL_DMA_BUF_PLANE0_MODIFIER_*`). A tiled buffer cannot be copied by the CPU: if its import fails `attach_buffer` returns -3 and the content stays as it was. Software rendering offers linear only. Tests: `build/gl-test` (`TestGL`, incl. the GBM window path on a render node), `TestGLParity` |
| `src/logind.[ch]` | **backend** | device access through systemd-logind without libsystemd / libdbus: a small D-Bus client (EXTERNAL auth, `NEGOTIATE_UNIX_FD`, `SCM_RIGHTS`, little-endian only; `DBUS_SYSTEM_BUS_ADDRESS` or `/run/dbus/system_bus_socket`), `Manager.GetSession($XDG_SESSION_ID)` / `GetSessionByPID`, `Session.TakeControl`, `Properties.Get(Seat)` (the seat object path: `seat/self` does not exist), `TakeDevice` / `ReleaseDevice`, `PauseDevice` (the watched DRM device: the backend stops drawing and calls `lm_logind_pause_done`; others are acked by `logind.c`), `ResumeDevice`, `Seat.SwitchTo`. Calls are synchronous (3 s limit), signals are queued and dispatched from the loop (idle) so callbacks may call back in. `lm_dev_open` / `lm_dev_close` take a possibly NULL `lm_logind` (then plain `open` / `close`). With logind the DRM backend skips `vt_setup`, `drmSetMaster` / `drmDropMaster` and the tty (logind sets the console to graphics mode and restores it); the input backend closes all devices on pause and takes them again on resume (evdev nodes are revoked while paused). `--logind off|auto|on`. Tests: `tests/fake_logind.py` + `build/logind-test` (`TestLogind`) |
| `src/backend.h`, `src/backend_drm.c`, `src/backend_headless.c` | **backend** | DRM/KMS + VT; headless + control socket |
| `src/dmabuf.[ch]` | **protocol** | `zwp_linux_dmabuf_v1` v4: default/surface feedback (the display GPU's render node as main device, a memfd format table), params -> `lm_buffer_create_dmabuf()` (shm.c); only linear ARGB8888/XRGB8888, one plane, no flags; the fd must be a real dmabuf (the sync ioctl is the test), bounds checked against its size |
| `src/cvt.[ch]`, `src/edid.[ch]` | **backend** | video-mode helpers without DRM types (unit-tested in `keymap-test`): CVT reduced-blanking timings (`lm_cvt_rb_fit`: the highest usual refresh rate that fits a pixel clock limit; `drm_list_modes` offers the panel's native size that way when the kernel pruned it); the EDID's native timing and the rule for when the offered mode has the wrong shape (a 21:9 panel with only 16:9 modes gets a generated mode of its own shape instead of being stretched by the monitor) |
| `src/render.[ch]`, `src/font.[ch]` | **render** | software compositor, cursor, decorations, 8x16 font |
| `src/main.c` | **core** | arguments, wiring, signals, clean shutdown |
| `client/mellagui-term.c` (later) | **clients** | terminal emulator |
| `client/mellagui-settings.c` | **clients** | settings panel (Keyboard, Pointer, Desktop, Display, Sound, Security): tabs, checkboxes, radio lists, sliders, layout list and a typing test field drawn immediate-mode from a per-frame widget list; also `--get` / `--set` for scripts; it only shows what the compositor reports back |
| `client/wlwin.[ch]` | **clients** | one software-drawn xdg-toplevel on top of `wlc`: registry binding, a single shm buffer, keymap/modifiers, fill/text helpers; used by calc, run and settings (not by the terminal) |
| `client/mellagui-files.c` | **clients** | two-pane file manager on `wlwin` (dock button *Files*; `--choose wallpaper` is the one-pane picture chooser, `dual` false): `struct pane` = a folder listing with its cursor (`list_dir` reads a folder into a sorted array: folders first, then names without regard to case; links followed with `stat`, a dangling link is shown dim and not opened; hidden files only with Ctrl+H; a failed listing keeps the old one; row 0 is `..` except in `/`; `reread_pane` keeps cursor, scroll position and marks), two of them in `struct app` with `act` the active one; paths are normalized textually (`normalize`, `join_path`); opening goes by `classify` (png / jpg / ppm by extension, else the first 4 KB: a NUL byte = binary) and starts the sibling program (`mellagui-view`, `mellagui-edit`, `mellagui-term` with the folder as its working directory) through `launch` (fork + `setsid` + `execvp`, a close-on-exec pipe reports a failed exec; SIGCHLD is ignored so nothing is left as a zombie). **Preview** (`struct preview`, `preview_load`): the pane that is not active turns into a view of the file under the cursor of the active one: text (first 128 KB, tabs expanded), an 8-bytes-a-line hex dump for files with a NUL byte, a picture (`src/img.c`, shared with the viewer; decoded only once the cursor has rested 150 ms, scaled by area averaging by `preview_scale`, never enlarged past 8x) or the listing of a folder; `preview_update` runs while painting. **File operations** are the only place where the program changes files: `op_targets` (the marked entries, else the one under the cursor, as names, never `..`), `do_copy_move` (never replaces an existing path, refuses a folder into itself, one source may take a new name, several need a folder), `copy_node` (recursive, links copied as links, modes and times kept), `move_node` (`rename`, else copy + `remove_tree` across file systems), `remove_tree` (links are not followed, the root is refused), `do_mkdir`, `do_rename`; they ask through the prompt line (`P_COPY` ... `P_DELETE_HARD`, delete needs Enter; Tab completes with `lm_complete_path` from `client/pathcomp.h`, shared with the editor) and then run as a `struct job`: `job_plan` work in `do_copy_move` / `do_delete` makes the item list, `job_execute` does it in a pthread (`job_start`; `sync_jobs` runs it inline for the selftest) while the main loop polls `finished` every 80 ms and draws the progress (atomic `done` / `total` units: bytes for copy, move and trash, files and folders for a real delete) in the status line; Esc sets `cancel`, which `copy_file` / `copy_node` / `remove_tree` check (a half-written copy is removed), the thread is joined by `job_finish` or at exit. A normal delete is `trash_item`: a move into `$XDG_DATA_HOME/Trash/files` (a taken name gets `.2`, `.3` ...) plus `info/NAME.trashinfo` (`Path`, `DeletionDate`); a folder that is inside the trash deletes for good. `go_trash` (Trash button, Ctrl+B) shows `Trash/files` in the active pane; `do_restore` (Restore button, Ctrl+Z, only inside it) reads each `.trashinfo` (`trash_origin`), skips what has no record or whose old place is taken, and runs `restore_item` (`mkdir_p` of the parent, `move_node`, then the info file is removed) as a `J_RESTORE` job; `cmd_empty_trash` / `do_empty_trash` (Empty button, Ctrl+E, question `P_EMPTY`) make one `J_DELETE` job of every entry of `files/` and `info/` (`emptying`, the folders stay). The round buttons (`struct rbtn`, `draw_rbtn` with `lm_round_fill` from `client/uikit.h`) are the toolbar and the function key bar. `--selftest` covers sorting / paths / classification, copy / move / rename / mkdir / delete and the preview; `TestClients.test_files_*` drive the window by keys and look at the titles of the windows it opens and at the files they change |
| `client/mellagui-edit.c` | **clients** | text editor on `wlwin` (dock button *Editor*): the document is an array of lines (UTF-8, tabs shown at 4 columns, \n only; CRLF and a missing final newline are remembered and written back), the cursor and the selection anchor are (line, byte offset), undo / redo are whole-document snapshots (64 deep; typing joins one step), find is a plain substring search that wraps, saving writes `FILE.PID.tmp` and renames it (mode kept). `wlwin_ops.close` lets it refuse the compositor's close request once while there are unsaved changes. The text zoom (`set_zoom_pct`, 50..500 %, same levels and area-averaged glyphs as the terminal: `client/uikit.h`) asks the compositor for a window as big as the same columns and rows need (`wlwin_request_size`; not while maximized / fullscreen, `wlwin.fixed_size`), and is saved in `editor.conf` 0.4 s after the last change. A rounded button bar (`TOP_H`, `btn_rect`, `fill_round`) and the F1 list (`paint_help`) make the keys discoverable; the Open / Save as prompts resolve names against the file's folder (`base_dir`, `resolve_path`), Tab completes (`complete_path`, selftest covers it) and a suggested name is replaced by typing (`pfresh`); Save as onto another existing file needs a second Enter (`ow_armed`). Clipboard like the terminal's (`client/wlclip.[ch]`, shared with the calculator: own `wl_data_source`, pastes from our own copy when we own the selection). `--selftest` covers the model; `TestClients.test_edit_*` drive the window with keys |
| `client/mellagui-view.c`, `src/img.[ch]` | **clients** | image viewer on `wlwin` (dock button *Images*; no argument = the newest image of `~/Pictures`; FILE = its directory starting there): `img.c` decodes PNG (own inflate after zlib's puff: stored / fixed / dynamic blocks; colour types 0,2,3,4,6, depths 1..16, tRNS for palettes, all five row filters; no interlacing, refused with a message; at most 64 Mpixel) and P6 PPM, transparency over a grey checkerboard; the view box-filters down (cached per zoom), nearest-neighbour up; Left/Right/Home/End browse, +/-/wheel zoom around the pointer, drag pans, F/0, R, Esc. `--decode FILE OUT.ppm` and `--selftest` for the tests (`TestImages` feeds it zlib output of every type, `TestViewer` drives the window) |
| `client/mellagui-calc.c` | **clients** | calculator (engine + UI in one file; `--selftest` runs the engine checks): `struct calc` is the pocket-calculator engine (left-to-right, `expr` is the sum so far for the line above the result, `done` after `=`, memory `mem` / `has_mem`, `calc_unary` sqrt / square / 1/x, `calc_paste` takes the first number of a text); the UI is a table of `struct keydef` (`KEYS`, kinds give the colours, the `=` key spans two rows) drawn with `lm_round_fill`; Ctrl+C / Ctrl+V go through `client/wlclip.[ch]`, the clipboard code the editor shares |
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
| `mellagui_capture_v1` | 1 (MellaGUI private) | capture.c |

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

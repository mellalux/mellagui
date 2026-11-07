# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

    make                 # build/mellagui (compositor), build/mellagui-term, build/mellagui-calc, build/mellagui-run, build/mellagui-settings, build/mellagui-view
    make run             # build + start mellagui on a real VT (EXEC=, TIMEOUT=, HEADLESS=1)
    make asan            # build/mellagui-asan (ASan + UBSan)
    make fuzz-run        # coverage-guided fuzzer of the whole compositor, FUZZ_TIME=60 s (tests/fuzz_server.c, see docs/FUZZING.md)
    build/keymap-test    # layout tables / keymap generator unit test (also run by tests/run.sh)
    tests/run.sh         # all end-to-end tests (needs a built build/mellagui; plus mellagui-term etc. for TestClients)
    MELLAGUI_BIN=build/mellagui-asan tests/run.sh      # same under the sanitizers (exit code 77 / "Sanitizer" in output = failure)
    python3 -I tests/test_mellagui.py TestWindows.test_move_with_titlebar     # a single test
    build/mellagui --backend headless --size 1024x600 --socket t0 --control /tmp/ctl      # manual headless run
    python3 tools/ppm2png.py frame.ppm frame.png                  # to look at a `dump` from the control socket

Only gcc, make, pkg-config and the libdrm headers exist on the dev machine (no libwayland, xkbcommon, libinput,
wlroots, meson ...); nothing may depend on them. No X11 anywhere. Tests need `XDG_RUNTIME_DIR` (the harness sets one).

## Architecture

Single-process, single-threaded epoll loop (`src/loop.c`); a from-scratch Wayland server (no libwayland).
`docs/ARCHITECTURE.md` and the header comments in `src/*.h` are the module contracts; read the header before a `.c`.

Data flow:
* **input**: `input_evdev.c` (or `lm_input_inject` from the headless control socket) → `seat.c`
  (`lm_seat_handle_input`) → `wm.c` first (`lm_wm_key` shortcuts, `lm_wm_pick`/`pointer_button` for frames and panel) →
  only if the wm declines, `wl_keyboard`/`wl_pointer` events to the focused client.
* **output**: client `wl_surface.commit` → `surface.c` copies the shm buffer into the surface's own texture and calls the
  role's `damage` hook → `xdg.c` forwards to `lm_wm_surface_damage` → `render.c` accumulates per-output damage →
  `backend_*.c` `get_framebuffer`/`present` → `output_ready` (page-flip event or 60 Hz timer) → frame callbacks.
* **windows**: `xdg.c` is only a protocol adapter; all policy (placement, focus, maximize, move/resize grabs, workspaces,
  decorations geometry, panel layout, the "MellaGUI" app menu (`config.apps`, `--app`), shortcuts) lives in `wm.c`, which also exposes the scene (`lm_wm_scene_for_each`)
  that `render.c` paints bottom-to-top. The wm talks back to clients only through `struct lm_window_ops`.
* **request dispatch** (`server.c`): object table per client, handlers get already-validated/resolved args; protocol
  violations → `wl_display.error` + disconnect. Interface signature tables are hand-written in `protocol.c`
  (opcodes are named in `protocol.h`); `wire.c` is shared with the clients.
* **screen lock**: `lock.c` (+ `sha256.c`) lives in the compositor so no client can unlock it. While locked `seat.c` routes keys to `lm_lock_key()` only (no wm shortcuts, no client events, no focus), `wm.c`'s scene is just `LM_SCENE_LOCK`. The secret is only a salted PBKDF2 hash in `lock.conf` (0600); changing it goes through the settings key `lock-set` (needs the current secret, shares the wrong-attempt delay). Tests shorten the hash/delay/minute with `MELLAGUI_LOCK_ITER` / `_DELAY_MS` / `_UNIT_MS`; control socket `lock ...`. Server hardening notes and the systemd unit: `docs/SERVER.md`, `contrib/mellagui.service`.
* **renderers**: `render.c` is the software compositor (headless default, and the fallback); `render_gl.inc` + `gl.c` the hybrid GL one (default `auto` on drm; `--renderer gl` / `MELLAGUI_RENDERER=gl`; headless offscreen + readback, on drm GBM windows scanned out by `backend_drm.c` `gl_scan_*` / `drm_gl_present`; dmabuf clients are sampled in place in linear and GPU-supported tiled layouts (`dmabuf.c` `fill_formats`, offered only with GL; surfaces then have no `tex.pixels`, only `gl_dmabuf`/`gl_img`/`gl_held`); run the whole suite with `MELLAGUI_RENDERER=gl` to check parity). GL libs are dlopen()ed, there are no EGL/GLES/gbm headers here.
* **output layout**: settings `output-order` -> `lm_wm_relayout_outputs` (wm.c) -> `layout_pack` (output.c); `--output WxH` adds headless outputs for tests (`TestOutputLayout`).
* **notifications**: `lm_wm_notify` (wm.c) <- settings key `notify` <- `client/mellagui-notify.c` / control `notify MS TITLE BODY`; cards are `LM_SCENE_NOTIFY` items; tests `TestNotify`.
* **wallpaper**: settings `wallpaper` / `wallpaper-mode` -> `lm_render_set_wallpaper` (render.c, decodes with `src/img.c`, which the viewer shares) -> per-output scaled copy in `paint_wallpaper`; the panel's Desktop page has the field (`W_FIELD`), radios and buttons. Tests `TestWallpaper`.
* **screenshots**: Print / Alt+Print / Ctrl+Print in `wm.c` → `screenshot.c` (`lm_render_capture` repaints into a private buffer, `png.c` writes stored-deflate PNGs, file in `--screenshot-dir` and an `image/png` selection owned by the compositor via `lm_seat_set_selection_data`). Tests: `TestScreenshot`.
* **backends**: `backend_drm.c` (legacy KMS, two dumb buffers + a cached shadow frame, uevent-driven hotplug, VT_PROCESS switching, restores console on every exit path —
  never tested on hardware) and `backend_headless.c` (memory framebuffer + line-based control socket used by all tests).
* **clients**: `mellagui-run.c` is the Run dialog (Super+R / panel *Run* button, `--run CMD`; `--selftest`); `client/wlc.[ch]` is a tiny client library on top of `wire.c`/`protocol.c`, `client/wlwin.[ch]` a one-window software-drawn xdg-toplevel layer on top of it (registry, shm buffer, keymap/modifiers, fill/text helpers) shared by calc, run and settings; `mellagui-edit.c` is the text editor (`--selftest`; `wlwin_ops.close` can veto the compositor's close request; tests `TestClients.test_edit_*`); `mellagui-view.c` is the image viewer (decoders in `src/img.c`: PNG with its own inflate + P6; Images menu entry; `--decode`/`--selftest`; tests `TestImages`/`TestViewer`); `mellagui-calc.c` is a calculator (engine + UI, `--selftest`); `mellagui-settings.c` is the settings panel (talks to the private `mellagui_settings_v1` global of `src/settings.c`; values persist in `~/.config/mellagui/settings.conf`, headless only with `--settings FILE`; `--get`/`--set`/`--selftest`); `mellagui-monitor.c` is the monitor's own settings panel (brightness, contrast, volume/mute, input, colour preset, standby over DDC/CI via `src/ddc.c` on `/dev/i2c-N`, the bus found through `/sys/class/drm/*/ddc`; a laptop panel's `/sys/class/backlight`; `--list`/`--set NAME HEXCODE VALUE`; env `MELLAGUI_MONITOR_FAKE=FILE` replaces the hardware in tests; opened from the Display page of the settings); `mellagui-term.c` is a VT emulator (button bar Paste / A- / zoom drop-down / A+ / Reset; the font zooms 50..500 % in 14 steps (`ZOOM_LEVELS`) with Ctrl +/-/0, Ctrl+wheel, the buttons or the list; whole multiples of 100 % are crisp, other sizes area-averaged (`build_zoom_tables`); the terminal keeps its own `terminal.conf` (`zoom=`, `$XDG_CONFIG_HOME/mellagui/`, read at start, saved 0.4 s after a change and on exit; the tests point `XDG_CONFIG_HOME` at a temp dir); tests `TestClients.test_terminal_*`)
  (pty, parser, scrollback, selection/clipboard); `src/font.c` + `src/font_data.h` (generated by `tools/gen_font.py`
  from `/usr/share/consolefonts/Uni2-Terminus16.psf.gz`) and `src/keymap.c` (layout tables → generated XKB keymap + client keysym lookup; `--layouts`, Super+Space, sticky keys live in `input_evdev.c`) are shared with clients.

Gotchas:
* wm coordinates: `lm_window.{x,y,width,height}` is the *window geometry* (client area); the frame is drawn around it
  (see `wm.h`). Workspaces are 0-based in the control socket output.
* Sub-surface position and `set_window_geometry` take effect on the *parent's* commit (protocol-correct; tests rely on it).
* The headless backend's `windows`/`dump`/`sync` commands are the test oracle; add a test in `tests/test_mellagui.py`
  (helpers: `Window`, `Ctl`, `Ppm` in `tests/wlclient.py`) when changing behaviour.
* The XKB keymap text is generated (`lm_keymap_build`) and cannot be compiled here (no libxkbcommon): real xkbcommon clients (VS Code) are the only way to verify it.
* Devices: with a usable logind session (`--logind auto`, default) `src/logind.c` (a from-scratch D-Bus client, `docs/ARCHITECTURE.md`) hands out the DRM and evdev fds and does VT switching (`PauseDevice` → ack, `ResumeDevice`, `Seat.SwitchTo`); otherwise plain `open(2)` + VT_PROCESS, which needs groups `video`+`input`. Backends call `lm_dev_open` / `lm_dev_close` so both paths share the code. Tested only against `tests/fake_logind.py` (`TestLogind`, `build/logind-test`) and by hand up to `TakeControl` on the real logind.

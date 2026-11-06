# Lumen

A graphical environment for Linux that runs **without X11**: a from-scratch
Wayland compositor with a built-in shell (panel, four workspaces, window
frames, wallpaper), written in C11 with no dependencies besides libc and the
libdrm headers. It runs directly on a Linux virtual terminal using DRM/KMS and
evdev and composites in software (wl_shm clients only, no GL).

Included programs:

| binary            | what it is                                                       |
|-------------------|------------------------------------------------------------------|
| `build/lumen`     | the compositor / shell                                            |
| `build/lumen-term`| terminal emulator (xterm-compatible subset, 256/true colour, scrollback, mouse selection + clipboard) |
| `build/lumen-run` | "Run command" dialog (Super+R or the panel's *Run* button; Up/Down history) |
| `build/lumen-calc`| calculator (mouse or keyboard; `--selftest` checks the arithmetic engine) |
| `build/lumen-demo`| small animated demo client                                        |

## Status (read this)

* Developed and **tested headless** (60+ end-to-end tests with a from-scratch Python wire client, ASan/UBSan clean).
  Rendering, the window manager, xdg-shell, input routing, clipboard, terminal and the demo are exercised there.
* The **DRM/KMS + VT backend (`src/backend_drm.c`) has not been run on real hardware** by its author:
  it was written carefully against the kernel APIs and compiles warning-free, but expect to debug it on first use.
  Use `--timeout` for the first run (see below).
* **logind device access is not implemented.** An ordinary user needs to be in the groups `video` and `input`.
* Not tested against third-party Wayland clients (GTK, Qt, foot ...): none were available where it was built.
  Only `wl_shm` buffers are supported, so GPU-accelerated clients must use their software renderers
  (`GSK_RENDERER=cairo`, `QT_QUICK_BACKEND=software`, `WLR_RENDERER=pixman`-like settings).
* Keyboard layout is US only. No XWayland, layer-shell, viewporter, xdg-output, IME or screen locker.

## Build

Requires gcc, make, pkg-config and the libdrm headers (`libdrm-dev`).

    make            # build/lumen, build/lumen-demo, build/lumen-term, build/lumen-calc
    make asan       # build/lumen-asan with AddressSanitizer + UBSan
    make run        # build and start Lumen (real VT; see below). EXEC=<prog>, TIMEOUT=<s>, HEADLESS=1 are optional
    make test       # end-to-end tests (headless backend, Python 3)
    LUMEN_BIN=build/lumen-asan tests/run.sh   # the same tests under the sanitizers

## Run on a real console (first time)

1. One-time: give your user access to the GPU and input devices, then log out and in again:

       sudo usermod -aG video,input $USER

2. Switch to a free text console with **Ctrl+Alt+F3** and log in there (not over ssh, not from a terminal emulator;
   no X/Wayland session may be running on that VT).
3. Start it with a safety timeout so a mistake cannot trap you:

       cd /path/to/GUI
       build/lumen --timeout 60 --terminal build/lumen-term --exec build/lumen-term --log-file ~/lumen.log

   Without `--timeout` it runs until you quit (Super+Shift+E) or kill it.
   If no keyboard can be opened (missing `input` group) Lumen refuses to start instead of leaving a dead screen.

Leave with **Super+Shift+E**, or switch consoles with **Ctrl+Alt+F1..F12** (Lumen hands the display back
and takes it again when you return).

### If the screen is stuck

Lumen restores the text console on every normal exit and on SIGSEGV/SIGABRT. After a `kill -9` the console can stay
in graphics mode with the keyboard off. From another machine (ssh) or another VT that still works:

    pkill lumen                # normal exit path restores everything
    sudo kbd_mode -u -C /dev/tty3 ; sudo chvt 1     # last resort (adjust tty number)

## Keys and mouse

| key                         | action                                  |
|-----------------------------|-----------------------------------------|
| Super+Enter                 | open terminal (`--terminal` command)    |
| Super+R                     | run dialog (`--run` command)            |
| Super+Q                     | close focused window                    |
| Super+1..4 / +Shift         | switch workspace / move window there    |
| Super+Tab (+Shift)          | cycle windows (minimized ones included) |
| Super+M / Super+F           | maximize / fullscreen toggle            |
| Super+Left / Super+Right    | tile window to the left / right half    |
| Super+Up / Super+Down       | maximize / restore (un-maximize or un-tile) |
| Super+Shift+E               | quit Lumen                              |
| Ctrl+Alt+F1..F12            | switch virtual console                  |

Panel: *Lumen* (app menu: Terminal, Calculator and, if found (`~/VSCode/code` or the snap's binary), Code; Esc or an outside click closes it;
add or replace entries with `--app "Name=command"`, repeatable), workspace cells, taskbar, then on the right *Run* (run dialog) and *Exit*
(first click turns it red and shows "Sure?", the second click quits; moving the pointer away cancels).

Mouse: drag the title bar to move, drag the border/corners to resize, double-click the title bar to maximize,
buttons for minimize/maximize/close, click the panel's workspace cells or the *Lumen* launcher.
The panel's taskbar lists the windows of the current workspace: click a button to focus it (or restore it if minimized),
click the focused one to minimize it, middle-click to close. Dragging a tiled window restores its previous size.

In `lumen-run`: type a shell command, Enter starts it detached (no window of its own is left behind), Esc cancels,
Up/Down browse the history (`~/.cache/lumen-run-history`), Ctrl+U clears the line.

In `lumen-calc`: digits, `.`, `+ - * /`, `%`, Enter or `=` to evaluate, Backspace to erase, Esc or `c` to clear
(operators are applied left to right).

In `lumen-term`: select with the mouse (auto-copies), middle-click or Ctrl+Shift+V / Shift+Insert paste,
Ctrl+Shift+C copy, Shift+PageUp/PageDown scrollback, mouse wheel scrolls.

## Options

    build/lumen --help

Notable: `--backend drm|headless`, `--exec CMD`, `--terminal CMD`, `--run CMD`, `--app NAME=CMD`, `--socket NAME`, `--drm-device /dev/dri/cardN`,
`--log-level debug`, `--log-file PATH`, `--no-grab`, `--timeout SECONDS`.
Clients find the compositor through `WAYLAND_DISPLAY` (set automatically for `--exec` and the launcher).

## Headless mode and tests

    build/lumen --backend headless --size 1280x720 --control /tmp/lumen.ctl --socket demo

The control socket takes one line per command (`dump <file.ppm>`, `windows`, `key <code> <0|1>`, `abs <x> <y>`,
`button <code> <0|1>`, `axis`, `sync`, `quit`; see `src/backend.h`). `tools/ppm2png.py` converts dumps to PNG.
`tests/wlclient.py` is a dependency-free Wayland client library in Python.

## Troubleshooting

* `no permission to open /dev/dri/card*` / `/dev/input/event*` – join the groups `video` / `input` and log in again.
* `cannot become DRM master` – another display server owns the GPU on this VT (or you are on the wrong VT).
* `/dev/tty is not a Linux virtual console` – start Lumen from a text console (Ctrl+Alt+F3), not from ssh or a terminal emulator.
* Black screen – read the log file (`--log-file`), try `--drm-device /dev/dri/card0`.
* Graphics-accelerated apps show nothing – they need a software renderer; see Status.

Architecture notes for developers: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [CLAUDE.md](CLAUDE.md).

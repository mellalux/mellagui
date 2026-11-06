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
| `build/lumen-settings` | settings panel (Super+I or *Lumen* → Settings): keyboard layouts, sticky keys, key repeat, pointer speed, natural scrolling, gadget, sound, resolution, sound outputs; `--get` / `--set KEY=VALUE` for scripts |
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
| Super+Space (+Shift)        | next / previous keyboard layout         |
| Super+Shift+E               | quit Lumen                              |
| Ctrl+Alt+F1..F12            | switch virtual console                  |

Panel: *Lumen* (app menu: Terminal, Calculator, if found (`~/VSCode/code` or the snap's binary) Code, if found (`/snap/bin/firefox` or `/usr/bin/firefox`; started with native Wayland and software rendering) Firefox, and Settings; Esc or an outside click closes it;
add or replace entries with `--app "Name=command"`, repeatable), workspace cells, taskbar, then on the right the layout indicator, *Sticky*, *Run* (run dialog) and *Exit*
(first click turns it red and shows "Sure?", the second click quits; moving the pointer away cancels).

Keyboard layouts and sticky keys: `--layouts us,ee,de,fi` (the default; up to four, the first is active at start; also `us,ee`
etc.) selects what you can switch between with Super+Space, or with the panel indicator (left click next, right click previous).
Layouts: `us`, `ee` (Estonian: ü õ ö ä, AltGr+f = š, AltGr+z = ž), `de`, `fi`; no dead keys. With anything but a lone `us`
the right Alt key is AltGr (third/fourth level, `AltGr+e` = €). `--sticky` (or the panel's *Sticky* button, or pressing Shift five
times in a row) turns on sticky keys: tapping Shift/Ctrl/Alt/AltGr/Super applies it to the next key press only, tapping it twice
locks it, a third tap unlocks. The button shows the latched (`Sh`) or locked (`SH`) modifiers. Each change beeps (latch, lock, unlock, sticky on/off; the tones are
synthesised and piped to `aplay`, so it needs alsa-utils and a sound card; `--no-sound` silences it).

Settings panel: Super+I or *Lumen* → *Settings* opens `lumen-settings` (Tab / Shift+Tab, Enter or Space, Left/Right on sliders, Esc).
*Keyboard* enables and orders the layouts (the checkbox, the `^` button; *active* picks the current one - the clients get a new keymap at
once, nothing has to be restarted), sticky keys, key repeat delay and rate, and has a field for trying the layout. *Pointer*: speed and
natural scrolling. *Desktop*: the clipboard gadget and the sound feedback. *Display*: the resolution (automatic, the monitor's preferred
mode, or one of its modes - the list includes a mode in the panel's own shape when the GPU does not offer the native one); it is applied at
once and goes back after 15 s unless you press *Keep* (a wrong mode can leave the screen black, and a black screen cannot confirm anything);
only a kept resolution is saved. *Sound*: the ALSA device for Lumen's own beeps (HDMI/DisplayPort outputs are listed and marked) and, when a
PulseAudio/PipeWire server runs, the default output for applications such as Firefox (`pactl`, else PipeWire's `wpctl`). When nothing is
listed the page says why: neither tool installed (`sudo apt install pulseaudio-utils`), or no sound server answering (start it with
`systemctl --user enable --now pipewire pipewire-pulse wireplumber`; it can take a few seconds until the devices appear). Changes apply immediately and are saved (about 0.3 s later) to
`$XDG_CONFIG_HOME/lumen/settings.conf` (`~/.config/lumen/settings.conf`; `--settings FILE` chooses another file, `--settings none` saves
nothing; headless runs save nothing unless `--settings` is given). The file is plain `key=value` and is read at start; options such as
`--layouts` or `--sticky` on the command line win over it. From a script: `lumen-settings --set layouts=ee,us repeat-rate=30`,
`lumen-settings --get`. Anyone on the user's Wayland socket can change the settings (the `lumen_settings_v1` protocol has no authentication).

Desktop gadget: a rounded, shadowed card on the first output (above the wallpaper, below all windows), docked at the top right until
you move it, with the clipboard viewer: the last five distinct texts that applications copied, newest first (up to 2 KiB read per
copy; images and files are ignored). The compositor reads them itself, so it also sees what was copied while no window had focus.
Drag the title bar to move it (it stays on the output, below the panel); drag an edge or corner (the dotted grip bottom right) to
resize it, 200x86 up to 640x560: a narrower card wraps the texts, a lower one shows fewer entries, a taller one gives each entry more
lines. Double-click the title bar to get the default size and place back (position and size are not saved). Super+G shows/hides it,
`--no-gadget` starts it hidden.

Volume mixer gadget: a second card of the same kind (Super+V, `--mixer`, or *Desktop* → *Volume mixer gadget* in the settings panel; hidden
by default). It docks below the clipboard card (beside it on a small screen) and is moved/resized the same way. Inside: the master
volume slider with a mute button, and - with a PulseAudio/PipeWire pulse server and `pactl` (package `pulseaudio-utils`) - the output
(click it to switch to the next one, e.g. HDMI) and one slider with a mute button per playing application. Without `pactl` it uses
PipeWire's `wpctl` (master volume and mute of the default output), and without that ALSA's `Master` control through `amixer`. The state is
read every 3 s while the card is visible and after each change; the programs run as short child processes started by the event loop, so a
slow one never stalls the compositor, and slider drags are coalesced (one command at a time).

System monitor gadget: a third card (Super+S, `--sysinfo`, or *Desktop* → *System monitor gadget*; hidden by default) with the CPU load (a
bar, a one-minute graph and one small bar per logical CPU), memory and swap use, and the CPU temperature (the package sensor of
`coretemp`/`k10temp`, else a CPU thermal zone; bar green / amber from 70 / red from 85 degrees) - plain reads of `/proc/stat`,
`/proc/meminfo`, `/sys/class/hwmon` and `/sys/class/thermal` once a second while it is visible. Rows that do not fit the card's height
are left out (CPU, memory and temperature always stay). The three cards dock under each other at the top right, in a column to the left
when the screen is too low, and cascade when even that does not fit.

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

Notable: `--backend drm|headless`, `--exec CMD`, `--terminal CMD`, `--run CMD`, `--app NAME=CMD`, `--socket NAME`, `--drm-device /dev/dri/cardN`, `--mode WxH[@Hz]`,
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
* Picture stretched (circles look like ovals) – the monitor's panel has another shape than the video mode (e.g. a 21:9 panel driven
  with 16:9 2048x1152: the GPU's HDMI pixel clock limit cut the panel's own mode away). Lumen corrects this by itself: when the
  preferred mode's shape differs from the monitor's native timing (the first detailed timing in its EDID) by more than 8 %, and the
  physical size in the EDID does not contradict that (broken EDIDs of projectors and TVs are left alone), it generates a mode of the
  same width and the native shape (CVT reduced blanking, e.g. 2048x864) and logs it; if the kernel refuses it, the preferred mode is
  used. `--mode WxH[@Hz]` forces a size, `--mode preferred` turns the correction off; the Display page of the settings panel changes it
  while running.
* Graphics-accelerated apps show nothing – they need a software renderer; see Status.

Architecture notes for developers: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [CLAUDE.md](CLAUDE.md).

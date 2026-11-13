# Desktop gadgets

A gadget is a card on the desktop (clipboard history, volume mixer, system monitor, smart home). It is **a program of its own**, not part of
the compositor: an ordinary Wayland client whose window the compositor treats in a special way. Four are part of MellaGUI
(`mellagui-gadget clipboard|mixer|system|smart`); anyone can add more.

## What the compositor does with a gadget window

A toplevel whose **app id starts with `mellagui.gadget.`** is a gadget:

* no frame, no title bar, always at the bottom of the stack (above the wallpaper, below every window), on every workspace;
* never raised or focused by a click, not in the taskbar, Alt+Tab, the overview or the focus cycling;
* **docked** at the top right under the panel until the user moves or resizes it: the cards stack downwards, then to the left, in the order
  clipboard, mixer, system, smart, then the cards from the folder (by name), then cards nobody listed;
* moved / resized **by the program**: it sends `xdg_toplevel.move` / `resize` on a press (the compositor only honours the request while that button is
  still held), the compositor snaps the card to the screen's edges and to other cards. Send `set_min_size` / `set_max_size` (limits are applied;
  without a maximum 640 x 1600). A card that was moved is remembered in `windows.conf` (the next start puts it back).

The window is the card: paint the whole buffer yourself. Use a **premultiplied ARGB8888** buffer to get round corners over the desktop (the
built-in cards use a 12 px radius and the palette of `[theme]`).

## Glued cards and zoom

* **Glue**: a card pulled against the bottom edge of another one snaps flush to it; from then on it hangs on that card, and dragging the upper one carries
  it (and whatever hangs under it) along. Dragging a lower card moves only that card and what is under it; pulling it away unglues it.
* **Zoom**: the small `- 100 % +` pill at a card's bottom left corner (it shows while the pointer is on the card) makes its content (and so the card)
  bigger or smaller (50 .. 300 %); a click on the percentage, or a double click on the title, puts card and content back. The right button's menu does
  the same, and so do Ctrl + / - / 0 while the clipboard card has the keyboard. `mellagui-gadget` programs (the four and `text`) do it themselves; a program of
  your own can use `wlwin_set_zoom` + `wlwin_request_size` from `client/wlwin.h`.

## Adding a gadget: a file in the gadgets folder

`~/.config/mellagui/gadgets/NAME.conf` (`$XDG_CONFIG_HOME/mellagui/gadgets`, `$MELLAGUI_GADGET_DIR` replaces the folder). The file name is the
gadget's name: 1..24 characters `a-z 0-9 - _`. **A file named like one of the four (`clipboard.conf`, `mixer.conf`, `system.conf`, `smart.conf`) replaces that
gadget's program and size** (`command`, `size`; whether it is on stays the setting's and the shortcut's business), so any of them can be swapped for your own. At most 8 files; the folder is read when the compositor starts.

    command   = mellagui-gadget text uptime --title Uptime --every 30 -- uptime
    autostart = yes          # default yes; no = started only when it is switched on
    size      = 240x120      # the size the card starts with (when it was not moved) and a double click on its title goes back to

* `command` is expanded like a dock button's (`@terminal`, `~/`, `a || b`, bare `mellagui-*` found next to the compositor).
* The program must open a toplevel with the app id **`mellagui.gadget.NAME`** (NAME = the file name without `.conf`). It is ended with SIGTERM
  (to its process group) when the gadget is switched off or the compositor ends; a program that exits on its own switches its gadget off.
* Switch it on / off: key binding `Super+F9 = gadget-toggle NAME` in `[keys]`, the control socket (`gadget NAME 0|1`) on the headless backend.
  A gadget that was started by hand (not from the folder) is docked after the others too, but nothing can switch it.

## Making a card out of a shell script: `mellagui-gadget text`

    mellagui-gadget text NAME [--title T] [--every SECONDS] [--size WxH] [--] PROGRAM [ARGS...]

Runs PROGRAM (no shell; use `sh -c '...'` for pipes) now and every SECONDS (default 5), shows what it printed (the first 16 KiB, as many lines as the card
holds) under the title; a double click on the title puts the card back. The card needs the compositor's settings protocol only for that double click.

    # ~/.config/mellagui/gadgets/disk.conf
    command = mellagui-gadget text disk --title Disk --every 60 -- sh -c "df -h / /home | tail -n +1"

## Writing one in C

`client/mellagui-gadget.c` is the model: `wlwin_argb()`, `wlwin_open(..., "mellagui.gadget.NAME", ...)`, `wlwin_move` / `wlwin_resize` for the
edges and the title bar (6 px at the left / right / bottom, 3 px at the top resize), `wlwin_round_fill` for the card. Any Wayland client library
works: only `xdg_shell` and `wl_shm` are needed.

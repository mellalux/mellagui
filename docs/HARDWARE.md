# Trying MellaGUI on real hardware

The tests run the compositor on the headless backend. Everything that touches a real screen (`backend_drm.c`, the GL renderer on GBM, the night
light's gamma table, hotplug, VT switching, the monitor's power state) is only seen by someone sitting in front of it. This is the list to go
through on a VT. Each step says what to do, what should happen and what to send back when it does not.

## Before you start

    make                                   # builds build/mellagui and build/hwprobe
    build/mellagui --log-level debug --log-file ~/mellagui.log      # on a text console (VT), not inside another desktop
    # leave it with Ctrl+Alt+F2 (another VT), or the session dialog's Exit

`build/hwprobe` is the second pair of eyes: it **reads** the card's state without becoming DRM master, so run it from another VT (Ctrl+Alt+F2) or
over ssh while MellaGUI owns the screen:

    build/hwprobe                          # connectors (connected, modes, DPMS), the CRTCs in use, the gamma table's size and end points

Example from the dev machine (Intel HD 4000, one ultrawide on HDMI):

    card /dev/dri/card1 driver=i915 1.6.0
    connector HDMI-A-3 connected 930x390 mm, 26 modes dpms=on
    crtc 45 2560x1080 50Hz fb=107 gamma_size=256 last(r,g,b)=65535,65535,65535 mid(r,g,b)=32896,32896,32896 -> neutral

When something fails, send: the last 60 lines of `~/mellagui.log`, the output of `build/hwprobe`, `git log -1 --oneline`, and the card
(`lspci -nn | grep -i vga`).

## The list

Mark each line `ok`, `wrong` (say how) or `n/a` (no such hardware).

| # | What to do | Should happen | If not, look at |
|---|---|---|---|
| 1 | Start on a VT | The desktop appears at the monitor's preferred mode, the pointer moves, keys work. The log says which renderer it chose (`gl` or `sw`) and how it got the devices (logind or the `video` / `input` groups). | the first 30 log lines |
| 2 | `--renderer gl`, then `--renderer software` (or `MELLAGUI_RENDERER=`) | Both draw the same picture. With gl: no tearing while a window is dragged, the wobble is smooth. `grep stall: ~/mellagui.log` shows no (or few) lines: a line means the loop was blocked longer than 16 ms. | `stall:` lines (they carry an `addr2line` hint) |
| 3 | Super+Shift+N (night light on), `build/hwprobe` from another VT | `last(r,g,b)` is no longer equal: blue lower than green lower than red, `TINTED`. The picture turns warm over 1.5 s. Off again: `neutral`. | `drm: no gamma table on ...` in the log (the driver has none) |
| 4 | Night light on, press Print (screenshot) and record 5 s with `mellagui-record --start` | The files are **not** tinted (the tint is in the card, not in the picture). | |
| 5 | Night light on, leave MellaGUI (session dialog > Exit) | The console is neutral again, `hwprobe` says `neutral`. | `out_gamma` / modeset restore in `backend_drm.c` |
| 6 | Settings > Night: the temperature slider and the schedule | The tint follows the slider; `auto` switches at the times (try `From` = a minute from now). | `night` lines of the log |
| 7 | Unplug the monitor cable, wait 3 s, plug it in again (a KVM switch is the same) | The output goes away and comes back, windows are refitted, maximized ones fill the screen, the pointer is on the screen, nothing crashes. With two monitors: the other one keeps working. | `drm: hotplug` / `uevent` lines |
| 8 | Ctrl+Alt+F2, log in, Ctrl+Alt+F1 (the VT number MellaGUI runs on) | The picture is back at once, the keyboard and the pointer work, no key is stuck, no console text is left on the screen. | `PauseDevice` / `ResumeDevice` or `VT_` lines |
| 9 | Lock (Super+L or the session dialog), wait for `Screen off after` (1 min), `hwprobe` | `dpms=off` while the screen is dark; the first key or mouse move wakes it (`dpms=on`) and is not passed to a window. | `dpms` in `hwprobe` |
| 10 | Screensaver (Settings > Security > Screensaver, 1 min) | The Matrix room runs smoothly; `build/mellagui-matrix --bench 1920x1080` says the time of a frame. | |
| 11 | Suspend from the session dialog and wake the computer | The desktop is back, the screen is not black, sound and the clock work, the monitor's tint is as before. | log after the wake-up |
| 12 | Settings > Display > Scale: 150 %, then 200 % | Everything is sharp, programs redraw, the pointer is the right size; back to 100 % is clean. | `scale` lines |
| 13 | Settings > Display > Resolution: another mode, confirm / let it time out | The mode changes, the revert after 15 s without confirmation works. | `resolution` lines |
| 14 | Open a video (`mellagui-view FILE.mp4`) and a game / browser (any dmabuf client) | Smooth; with a dmabuf client the log has no `dmabuf` errors. | `dmabuf` lines |
| 15 | Start chime (log in again) and click a dock icon | A sound plays; if not, `pactl info` and the log's `sound` lines. | |
| 16 | `mellagui-monitor` (Settings > Display > Monitor) | Brightness / contrast change over DDC/CI if the monitor and `/dev/i2c-N` allow it, else it says why. | `ddc` lines |
| 17 | Two monitors, Settings > Display > Layout | The order can be changed, the gadgets stay at the top right of the leftmost one, windows can be dragged across. | |
| 18 | Mouse / touchpad / keyboard: layouts (Super+Space), key repeat, sticky keys, the on-screen keyboard (Super+K) | They work as in the settings. | `input` lines |
| 19 | Drag a window by its title bar to the left / right edge, a corner, the top edge (and Super+Ctrl+Left / Right, with Shift for the lower ones) | A translucent accent rectangle shows the half / quarter / whole screen while the pointer is there, and only then; let go and the window goes there (the top edge maximizes). With two monitors, crossing from one to the other does not show it. With gl the preview is not black or flickering. | `snap` lines (the control socket's `snap` is headless only) |
| 20 | Lock (Super+L), wait for `Screen off after`; then with the screensaver on, let it start and its `Monitor sleeps after` pass | The picture fades out over about 2 s (not a jump to black) before `dpms=off`; the first touch fades it back in over a fraction of a second and is not passed on. With the saver running `top` shows `mellagui-matrix` gone while the monitor sleeps. | `screenoff` lines, `hwprobe` |
| 21 | Settings > Input > Keyboard: show the flag of the layout in use; the lock screen with the wallpaper behind it (Settings > Security) | The panel shows a flag (and the text you chose); the lock screen shows the blurred, dimmed wallpaper behind the clock and the pill-shaped field. | |
| 22 | Record 10 s of scrolling a long page with `mellagui-record --start --codec h264`, play it with `mellagui-view` and with ffmpeg | The file is small (a scrolled page costs a few tens of kB a picture) and both players show the same, sharp picture without blocks at the macroblock edges. | `mellagui-record --selftest` |
| 23 | Smart home: a rule `on sunrise+5 ...` in `smart-schedule.conf`, `mellagui-smart --sun` | The times are those of your place (compare with a weather site, within a couple of minutes); the rule fires in its minute while the computer is on. | `mellagui-smart --background` output |

## The login screen (docs/GREETER.md)

Do this on a spare VT or a machine you can reach over ssh: a mistake in the PAM files can lock a login out. Keep a root shell open (or the old display manager enabled) until it works.

| # | What to do | Should happen |
|---|---|---|
| G1 | `sudo systemctl start mellagui-greetd` | VT 1 shows the login screen (clock, users, password field). `journalctl -u mellagui-greetd` says `the login screen runs`. If the screen stays black: `sudo -u mellagui-greeter env XDG_RUNTIME_DIR=/tmp mellagui --kiosk --config none --exec mellagui-greeter` from another VT shows the error. |
| G2 | A wrong password, then the right one | `Login incorrect`, the field is empty again; then the desktop starts as that user (`loginctl list-sessions`: one `greeter` session ended, one `user` session, TYPE wayland, on tty1). |
| G3 | Log out of the desktop | The login screen comes back; the last user is preselected. |
| G4 | Sleep / Restart / Shut down (two clicks each) | They happen. |
| G5 | Ctrl+Alt+F2, log in on the text console, Ctrl+Alt+F1 | The login screen is still there and works. |
| G6 | A user whose account is locked / expired | A message from PAM (`Authentication failure`, `Account expired`) and no session. |

## What is already known

* The dev machine (Intel HD 4000, i915, one monitor on HDMI) runs MellaGUI every day with the GL renderer (libEGL / libgbm are loaded).
  What nobody has written down yet is rows 3 to 13 and 19 to 23 above, which is what this list is for.
* The night light's gamma table is scaled per CRTC (`out_gamma`); `hwprobe` is the quickest check that it reached the hardware.
* Everything on the list can be compared with the headless pictures: `tests/run.sh` must stay green, it does not cover any of this.

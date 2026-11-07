# MellaGUI on a server

What protects a machine that has a MellaGUI screen, and what does not. Nothing here was tested on real hardware yet.

## The threat

Someone stands at the keyboard of an unattended machine. The screen lock stops them from using the session; the rest of this page
closes the ways *around* the lock.

## The ways around the lock, and what closes them

| way around | what closes it |
|---|---|
| quit MellaGUI (Super+Shift+E, the panel's *Exit* button) and use the shell it was started from | with a lock set these ask for the secret first (the screen locks, the secret quits). Better: run MellaGUI as a service so there is **no shell behind it** (below) |
| MellaGUI crashes or is killed and leaves the console | the service restarts it (`Restart=always`); it comes back locked (*Lock when MellaGUI starts*, on by default) |
| a reboot starts the session open | same: the service starts MellaGUI, and `lock.conf` makes it start locked |
| Ctrl+Alt+F1..F12 to another console | another console has its own login, so this is only a way around if one is logged in. *Settings → Security → "Console switch works while locked"* turns it off (`vt=0` in `lock.conf`). Keep an ssh way in: with it off, a forgotten secret can only be fixed over ssh |
| guessing | PBKDF2-HMAC-SHA256, 200000 rounds; three wrong tries start a delay that doubles from 5 s to 5 min (also for changing the lock in the settings) |
| another user's program connecting to the compositor | the Wayland socket is created 0600 and the compositor refuses any client whose uid is not its own (or root), on top of the 0700 runtime directory |
| a browser or editor in the session changing the settings | on the drm backend only MellaGUI's own programs (`mellagui-*` next to the compositor binary) get the settings; others bind an inert object (`--settings-access own`, the default there). This keeps confused programs out; it is not a wall against code that already runs as the same user |
| a program spamming wrong secrets through the settings to keep the lock screen waiting | changing the lock has its own attempt counter, and is refused altogether while the screen is locked |
| a monitor that stays lit | *Screen off after* (default 3 minutes locked) puts the monitors in standby; the first key only wakes them |

## Running as a service

`contrib/mellagui.service` runs MellaGUI as the unprivileged user `mellagui` on tty1 and takes tty1 away from getty. The header of the
file has the install commands. In short:

1. create the user (groups `video`, `input`, ... - the service has no logind session, so MellaGUI logs `logind not used: ...` and opens the device files; `--logind off` silences that), install the binaries to `/usr/local/bin`, install the unit,
2. `systemctl mask getty@tty1.service` and `systemctl enable --now mellagui.service`,
3. **set the lock immediately** (Settings → Security). Until `lock.conf` exists the machine is open to anyone at the keyboard.

Administer the machine over ssh. The terminal inside MellaGUI runs as `mellagui`, not as you.

The unit has a commented block of stricter options (`NoNewPrivileges`, `ProtectSystem=strict`, `DevicePolicy=closed`). They apply to
every program started from MellaGUI too, so they suit a GUI that is only a display.

## What the lock does not do

* It does not stop someone who can log in as the same user elsewhere (ssh, another console): such a program can read `lock.conf` and
  guess a short PIN offline. Use a password of 12+ characters on a machine that matters.
* It does not stop SIGTERM or the control socket's `quit`: whoever can send signals as the user already owns the session.
* A forgotten secret cannot be reset from inside the session. Delete `lock.conf` (`/var/lib/mellagui/config/mellagui/lock.conf` with the unit
  above) over ssh and restart the service.
* The compositor sees every key and the whole screen, so keep it small and updated; the wire-protocol parser is the part clients talk to.

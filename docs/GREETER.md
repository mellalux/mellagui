# The login screen (greeter)

Three parts, all in this repository:

| Program | Runs as | What it does |
|---|---|---|
| `mellagui-greetd` (`client/mellagui-greetd.c`) | root, a systemd service on the first text console | starts the login screen, checks the password through PAM, starts the session |
| `mellagui --kiosk --exec mellagui-greeter` | the unprivileged user `mellagui-greeter` | the compositor of the login screen: nothing but the greeter's window, no shortcuts (Ctrl+Alt+Fn still switches VT), no dock, no helpers, no screensaver; it ends when the greeter ends |
| `mellagui-greeter` (`client/mellagui-greeter.c`) | `mellagui-greeter` | the screen: clock, the users, a password field, the session, Sleep / Restart / Shut down |

    greetd (root) -> worker: PAM session of mellagui-greeter -> mellagui --kiosk -> mellagui-greeter
        ^                                                                              |
        |  unix socket $GREETD_SOCK (a line protocol, see the top of mellagui-greetd.c) |
        +------------------------------------------------------------------------------+
    login USER -> worker: PAM conversation with the greeter; then the greeter is stopped and the session
                  (default: `mellagui`) starts as the user, in a PAM session of its own (pam_systemd: a logind session on the same VT)

The daemon never holds a password; each login is a worker process that passes PAM's questions to the greeter and the answers back. The password
is typed into the greeter, sent in one `answer` line and wiped. root and users with `nologin` / `false` shells never log in; the socket belongs to
the greeter user (mode 0600, `SO_PEERCRED` is checked). A greeter that dies is started again (a crash loop waits 5 s).

## Setting it up (needs root)

    make
    sudo install -m755 build/mellagui build/mellagui-* /usr/local/bin/          # the programs (the greeter is found next to them)
    sudo cp contrib/mellagui-greeter.sysusers /etc/sysusers.d/mellagui-greeter.conf && sudo systemd-sysusers
    sudo cp contrib/pam.d/mellagui contrib/pam.d/mellagui-greeter /etc/pam.d/
    sudo cp contrib/mellagui-greetd.service /etc/systemd/system/
    sudo systemctl disable getty@tty1 gdm lightdm sddm 2>/dev/null; sudo systemctl enable --now mellagui-greetd

Other sessions: a `.desktop` file in `/usr/share/wayland-sessions` (Name, Exec; `NoDisplay` / `Hidden` are left out) is offered in the session
chooser. The last user and session are remembered in `/var/cache/mellagui-greetd/last`.
Options: `mellagui-greetd --greeter-cmd CMD|none --session-cmd CMD --greeter-user USER --socket PATH --state-dir DIR --pam-service NAME --greeter-pam NAME --vt N --sessions-dir DIR --once`.

## Looking at it without root

    build/mellagui-greeter --demo --windowed                   # inside a running MellaGUI: made-up users, the password is "demo"
    printf 'alice secret Alice Example\nbob * Bob\n' > /tmp/users          # user, password (* = none), display name
    MELLAGUI_GREETD_FAKE=/tmp/users build/mellagui-greetd --greeter-cmd none --socket /tmp/g.sock --session-cmd 'xmessage hi' --once &
    GREETD_SOCK=/tmp/g.sock build/mellagui-greeter --windowed

In the fake mode there is no PAM and no `setuid`: the session runs as you. The tests (`TestGreeter`) use it with a headless compositor.

## Keys and mouse

Enter logs in; Tab / Shift+Tab move between the users, the user name (for "Other..."), the password and the session; Left / Right choose a user
or a session; Esc clears; typing goes to the password. A click does the same; a power button needs a second click ("Sure?").
`$MELLAGUI_GREETER_DUMP=FILE` writes the state and the places of everything after every paint (tests).

## What is not tried

Everything that needs root, PAM and logind: the PAM conversation (`libpam.so.0` is loaded with `dlopen`, the structures are declared by hand: the
dev machine has no PAM headers), `pam_systemd` giving the greeter and the user a logind session, the compositor taking the GPU through that
session, and the hand-over of the VT from the login screen to the session. The daemon, the protocol, the screen and the kiosk compositor are
tested (`TestGreeter`). The first real run is the checklist in `docs/HARDWARE.md`, "Login screen".

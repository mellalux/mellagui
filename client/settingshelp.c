#include "settingshelp.h"

#include <string.h>

struct entry {
    const char *key;                    /* the setting's key, or "@Heading text" */
    const char *arg;                    /* NULL = for any arg of that key */
    const char *title;
    const char *text;
};

static const struct entry table[] = {
    /* ---- Keyboard */
    { "@Keyboard layouts", NULL, "Keyboard layouts",
      "Tick the layouts you want to type in. The ticked ones come first, in the order that Super+Space (or a click on the layout badge in the panel) "
      "switches through them.\n"
      "'active' marks the layout in use right now, ^ moves a layout up in the order. At least one layout stays ticked and at most four can be." },
    { "sticky", NULL, "Sticky keys",
      "Tap Shift, Ctrl, Alt, AltGr or Super once and it stays pressed for the next key, so a shortcut can be typed one key at a time - handy with one "
      "hand or a keyboard that is hard to press.\n"
      "Pressing Shift five times in a row switches this on and off. The panel shows 'Sticky' while a modifier is waiting." },
    { "repeat-delay", NULL, "Repeat delay",
      "How long a key has to be held down before it starts to repeat, in milliseconds. A smaller number repeats sooner; the change applies to programs "
      "at once." },
    { "repeat-rate", NULL, "Repeat rate",
      "How many characters a second a key that is held down repeats once it has started." },
    { "osk-avoid", NULL, "Windows and the on-screen keyboard",
      "While the on-screen keyboard (Super+K, or the Kbd button in the panel) is open, windows are kept out from under it: maximized windows get "
      "shorter and floating windows it would cover are moved up.\n"
      "When you close the keyboard they go back, unless you moved or resized them meanwhile." },

    /* ---- Pointer */
    { "@Mouse and touchpad", NULL, "Mouse and touchpad", "How the pointer moves and how scrolling feels. Both apply at once." },
    { "pointer-speed", NULL, "Pointer speed",
      "How far the pointer travels for a movement of the mouse or touchpad. 100 % is the device's own speed; lower is slower and more precise." },
    { "natural-scroll", NULL, "Natural scrolling",
      "Scrolls the way a phone does: the content follows your fingers. With it on, two fingers moving down on a touchpad (or the wheel turned toward "
      "you) pull the page down; with it off the page moves the other way, like the classic mouse wheel." },
    { "@Double click", NULL, "Double click", "The longest time between two clicks that still counts as a double click." },
    { "dblclick", NULL, "Double-click time",
      "The longest time between two clicks that still counts as a double click: title bars (maximize), the file manager, the viewer, the editor and "
      "the gadgets all use it.\n"
      "Try it in the box below: two clicks within the time turn it green. Programs that are open already keep their time until they are restarted. "
      "It is saved in the configuration file ([windows] double-click-ms)." },

    /* ---- Desktop */
    { "@Desktop", NULL, "Desktop", "The cards that can sit on the desktop, and the beeps. Each has a shortcut that shows and hides it." },
    { "gadget", NULL, "Clipboard gadget",
      "A card with the history of what you copied (the last 50 texts). Click an entry to use it again; the x deletes one, the bin empties the history. "
      "Super+G shows or hides it, Super+C picks from it with the keyboard." },
    { "sound", NULL, "Sound feedback",
      "MellaGUI's own beeps, for instance when a sticky key is latched. They are played through ALSA on the output you choose on the Sound page." },
    { "mixer", NULL, "Volume mixer gadget",
      "A card with the master volume and mute, the list of outputs and one slider for every program that is playing. Super+V shows or hides it." },
    { "sysinfo", NULL, "System monitor gadget",
      "A card with the CPU load (and its history), memory, swap, temperature and the name of the machine and system. Super+S shows or hides it." },
    { "@Wallpaper", NULL, "Wallpaper", "The picture behind the windows." },
    { "wallpaper", NULL, "Wallpaper file",
      "The path of a PNG, JPEG or PPM picture (~/ is your home folder). Enter or Apply shows it at once; 'Browse...' lets you pick one in the file "
      "manager; 'No picture' goes back to the plain colours." },
    { "wallpaper-mode", NULL, "How the picture fits",
      "Fill covers the whole screen and cuts what does not fit; Fit shows the whole picture with bars where it is smaller; Stretch squeezes it to the "
      "screen's shape; Center shows it at its own size in the middle." },

    /* ---- Display */
    { "@Monitors, left to right", NULL, "Monitor order",
      "With several monitors the pointer moves from one to the next in this order. < and > move a monitor one place left or right." },
    { "resolution", NULL, "Resolution",
      "Automatic lets the panel choose its own shape; Preferred is the mode the monitor itself recommends; the others are modes it offers. The screen "
      "changes at once and is remembered. The monitor's own settings (brightness, input ...) are under 'Monitor settings...'." },

    /* ---- Sound */
    { "@Output device (HDMI, analog ...)", NULL, "Output device",
      "A sound card can have several outputs (analog, HDMI, S/PDIF). Choose the one the speakers or the monitor are plugged into; this is how sound over "
      "HDMI is switched on." },
    { "audio-profile", NULL, "Card output", "Which output of the sound card is used (analog, HDMI ...). The change applies at once." },
    { "@Applications (PulseAudio / PipeWire)", NULL, "Playback device", "The sound server's own list of outputs: all programs play to the one you choose." },
    { "sink", NULL, "Playback device", "Where the sound of the programs goes. The volume mixer and the keys act on this output." },
    { "@MellaGUI's own beeps (ALSA)", NULL, "Beeps", "The device that plays MellaGUI's own beeps (sticky keys and the like)." },
    { "audio-out", NULL, "Beep device", "The ALSA device for MellaGUI's own beeps. 'System default' follows the sound server." },

    /* ---- Screen lock */
    { "@Screen lock", NULL, "Screen lock",
      "A password or PIN that locks the screen: Super+L, the power menu, or the idle time below. The secret is kept only as a salted hash. "
      "To change anything on this page you must first give the current secret." },
    { "sec-kind", NULL, "Password or PIN", "A password can have any characters; a PIN is digits only (handy on the on-screen keyboard)." },
    { "pw", "0", "Current secret",
      "The password or PIN that is set now. It is needed for every change on this page, so that nobody can change it from an unlocked screen. "
      "Leave it empty while no secret is set." },
    { "pw", "1", "New secret", "The new password or PIN. Leave it empty to keep the present one and change only the other settings." },
    { "pw", "2", "Repeat", "Type the new secret once more, to be sure there is no typing mistake." },
    { "sec-minutes", NULL, "Lock after idle",
      "Lock the screen after this many minutes without keyboard or mouse use. 0 = never. A video that is playing keeps the screen awake." },
    { "sec-blank", NULL, "Screen off after",
      "Once the screen has been locked this long the monitors go to standby. Any key or mouse movement wakes them. 0 = never." },
    { "sec-start", NULL, "Lock at start", "Lock the screen as soon as MellaGUI starts - for a computer that logs in by itself." },
    { "sec-vt", NULL, "Console switch while locked",
      "Lets Ctrl+Alt+F1 ... F12 switch to a text console even when the screen is locked. Leave it off if a console may be logged in: that would "
      "get around the lock." },

    /* ---- Config file */
    { "@Configuration file", NULL, "Configuration file",
      "mellagui.conf holds what the panel does not edit one value at a time: the dock's buttons with their commands, the shortcuts, the colours and the "
      "sizes (the other pages edit parts of it for you). Comments in it are kept.\n"
      "'Edit the file...' opens it in the editor, 'Reload' reads it again at once (Super+Shift+R does the same)." },

    /* ---- Dock */
    { "@Dock", NULL, "Dock",
      "The row of round icons along the bottom edge. The list shows the buttons: ^ and v move one, x removes it, a click on its name edits it. "
      "To add one fill in the name, an icon (a built-in name, theme:NAME for an installed program's icon, or a picture file) and the command, then "
      "press Add (or Update while editing).\n"
      "Commands: @terminal, @run ... are MellaGUI's own; 'a || b' tries a and then b; a program that does not exist is left out." },
    { "dock-enabled", NULL, "Show the dock", "Switches the whole dock off or on. The Launcher (Super+A) still opens all your programs." },
    { "dock-autohide", NULL, "Hide the dock",
      "The dock slides away and comes back when the pointer touches the bottom edge of the screen." },
    { "dock-bg", NULL, "Plate under logos",
      "Logos of installed programs (Firefox, Code ...) have transparent corners; they are put on a round plate like the built-in icons. Light, Dark "
      "or None (no plate). Pictures that fill their whole square never get one." },
    { "dk-icon-size", NULL, "Icon size", "The size of the dock's icons when the pointer is away from them." },
    { "dk-zoom", NULL, "Zoom under the pointer", "How much larger the icon under the pointer becomes (100 % = no zoom); its neighbours grow a little, too." },
    { "dk-gap", NULL, "Space between icons", "The empty space between two icons." },
    { "dk-margin", NULL, "Distance from the edge", "The space between the dock and the bottom edge of the screen." },

    /* ---- Screensaver */
    { "@Screensaver", NULL, "Screensaver", "What the screen does when nobody uses the computer." },
    { "saver", NULL, "Start the screensaver",
      "After the idle time below a full-screen program (falling green code by default) starts. A key, a mouse button or a movement of the pointer ends "
      "it. It does not start over a full-screen window (a film) or the lock screen." },
    { "saver-minutes", NULL, "Idle time", "How many minutes of no keyboard or mouse use pass before the screensaver starts." },

    /* ---- Clock and calendar */
    { "@Clock and date", NULL, "Clock and date", "How the clock in the panel and on the lock screen is written." },
    { "clock-hours", NULL, "24 or 12 hours", "24 hour (13:45) or 12 hour with AM / PM (1:45 PM)." },
    { "clock-seconds", NULL, "Show seconds", "Adds the seconds to the clock; the clock then changes every second." },
    { "clock-date", NULL, "Date in the panel", "Writes the date next to the time in the panel." },
    { "clock-weekday", NULL, "Weekday", "Puts a short weekday name (Mon, Tue ...) before the date." },
    { "date-format", NULL, "Date format", "How the date is written: day.month.year, year-month-day, month/day/year or with the month's name (10 Nov 2026)." },
    { "@Calendar", NULL, "Calendar", "The calendar that opens from the clock." },
    { "cal-enabled", NULL, "Calendar on click", "A click on the clock in the panel shows a month calendar under it; another click, Esc or a click elsewhere hides it." },
    { "cal-week-start", NULL, "First day of the week", "Whether the calendar's weeks start on Monday or on Sunday." },
    { "cal-weeks", NULL, "Week numbers", "Shows the ISO week number at the left of every row of the calendar." },

    /* ---- Keys, Colours, Sizes (the controls are lists and forms: help for the whole page) */
    { "@Keyboard shortcuts", NULL, "Keyboard shortcuts",
      "The compositor's shortcuts, ten to a page (< and > turn the page). Click a key name to edit it, x removes it (a built-in one then reads 'none'). "
      "'Record a key...' listens for the next combination (Esc cancels); choose the action with < and > or type it ('exec COMMAND' starts a program) and "
      "press Add (or Update). 'Restore defaults' brings everything back.\n"
      "Super+/ shows the whole list. A few keys cannot be changed: Alt+Tab, Ctrl+Alt+F1..F12 and Print." },
    { "@Colours", NULL, "Colours",
      "The palette of the shell and of MellaGUI's own programs. Pick a colour in the list, then change it with the three sliders or type a #rrggbb "
      "value; Apply sends it, 'Built-in' puts that colour back, 'Restore defaults' all of them. Programs that are already open keep their colours "
      "until they are restarted." },
    { "col-r", NULL, "Red", "The red part of the colour chosen in the list above, from 0 to 255. The colour changes in the preview at once; Apply makes it the shell's colour." },
    { "col-g", NULL, "Green", "The green part of the colour chosen in the list above, from 0 to 255. The colour changes in the preview at once; Apply makes it the shell's colour." },
    { "col-b", NULL, "Blue", "The blue part of the colour chosen in the list above, from 0 to 255. The colour changes in the preview at once; Apply makes it the shell's colour." },
    { "@Sizes in pixels", NULL, "Sizes in pixels",
      "The sizes of the shell: title bars, borders, the panel, the cards, notifications. Each slider shows its name; a * marks a size that is not the "
      "built-in one. A change is sent when you let go of the slider and the windows follow at once. 'Restore defaults' brings the built-in sizes back." },
};

static const struct entry *find(const char *key, const char *arg)
{
    const struct entry *any = NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].key, key))
            continue;
        if (table[i].arg && arg && !strcmp(table[i].arg, arg))
            return &table[i];
        if (!table[i].arg)
            any = &table[i];
    }
    return any;
}

bool settings_help(const char *key, const char *arg, const char **title, const char **text)
{
    const struct entry *e = find(key, arg);
    if (!e)
        return false;
    *title = e->title;
    *text = e->text;
    return true;
}

bool settings_help_known(const char *key, const char *arg)
{
    return find(key, arg) != NULL;
}

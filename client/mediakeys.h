#ifndef MELLAGUI_MEDIAKEYS_H
#define MELLAGUI_MEDIAKEYS_H

/*
 * The evdev codes of a multimedia keyboard's player keys, for the programs that play sound themselves (the viewer, the file
 * manager's preview): the compositor hands these keys on to such a window instead of driving an MPRIS player (`media` action in
 * wm.c), and the program reads the code of the key event (args[2]), not a keysym - lm_keymap_keysym knows none of them.
 */
#define MK_PLAYCD    200u
#define MK_NEXTSONG  163u
#define MK_PLAYPAUSE 164u
#define MK_PREVSONG  165u
#define MK_STOPCD    166u
#define MK_STOP      128u

#endif

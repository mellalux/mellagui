#ifndef LUMEN_KEYMAP_H
#define LUMEN_KEYMAP_H

/*
 * Static US keyboard layout, shared by the compositor (wl_keyboard.keymap
 * and modifier handling) and by clients (lumen-term / lumen-demo translate
 * evdev key codes to characters without libxkbcommon).
 * Owner: seat.c's owner (keymap.c). No dependencies on server code.
 *
 * Keycodes on the wire are evdev codes (KEY_A = 30); XKB keycodes in the
 * keymap text are evdev + 8. The embedded keymap text is a complete
 * xkb_keymap (xkb_keycodes/types/compat/symbols) written so that real
 * libxkbcommon-based clients (GTK, Qt, SDL ...) can compile it; use
 * /usr/share/X11/xkb/{keycodes/evdev,symbols/us,symbols/pc, types and compat files}
 * as the reference (inline the pieces you need, no `include` statements that
 * depend on files existing on the machine).
 */

#include <stddef.h>
#include <stdint.h>

/* Modifier bits = XKB "real modifier" indices as serialised in
 * wl_keyboard.modifiers (mods_depressed / mods_locked). The keymap must map
 * them exactly like this (the usual pc/evdev mapping). */
enum {
    LM_MOD_SHIFT = 1u << 0,
    LM_MOD_CAPS  = 1u << 1,   /* Lock  */
    LM_MOD_CTRL  = 1u << 2,   /* Control */
    LM_MOD_ALT   = 1u << 3,   /* Mod1 */
    LM_MOD_NUM   = 1u << 4,   /* Mod2 (NumLock) */
    LM_MOD_SUPER = 1u << 6,   /* Mod4 */
};

/* Keymap text, NUL-terminated. wl_keyboard.keymap size = strlen() + 1. */
const char *lm_keymap_us_text(void);

/*
 * Level-resolved keysym for an evdev code under the given LM_MOD_* mask
 * (uses Shift, Caps Lock for letters, NumLock for keypad). 0 = no symbol.
 */
uint32_t lm_keymap_us_keysym(uint32_t evdev_code, uint32_t mods);

/*
 * UTF-8 text produced by a keysym: Latin-1 and Unicode keysyms
 * (0x20..0x7e, 0xa0..0x10ffff, 0x01000000|cp) plus keypad digits/operators.
 * Writes up to 7 bytes + NUL into out, returns the byte length, 0 if the
 * keysym produces no text (function keys, modifiers, arrows ...).
 */
int lm_keysym_to_utf8(uint32_t keysym, char out[8]);

/* A few X11 keysym values clients need for special keys. */
enum {
    LM_KS_BackSpace = 0xff08, LM_KS_Tab = 0xff09, LM_KS_Return = 0xff0d,
    LM_KS_Escape = 0xff1b, LM_KS_Delete = 0xffff,
    LM_KS_Home = 0xff50, LM_KS_Left = 0xff51, LM_KS_Up = 0xff52,
    LM_KS_Right = 0xff53, LM_KS_Down = 0xff54, LM_KS_Page_Up = 0xff55,
    LM_KS_Page_Down = 0xff56, LM_KS_End = 0xff57, LM_KS_Insert = 0xff63,
    LM_KS_KP_Enter = 0xff8d,
    LM_KS_F1 = 0xffbe, /* F1..F12 = LM_KS_F1 + n - 1 */
    LM_KS_Shift_L = 0xffe1, LM_KS_Control_L = 0xffe3, LM_KS_Alt_L = 0xffe9,
    LM_KS_Super_L = 0xffeb,
};

#endif

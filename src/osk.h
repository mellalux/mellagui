#ifndef MELLAGUI_OSK_H
#define MELLAGUI_OSK_H

/*
 * The on-screen keyboard: the table of keys (evdev codes, not characters, so it follows the active layout), their
 * geometry for an output and the label a key shows under the current modifiers. No server dependencies; wm.c owns the
 * state (open, latched modifiers, the key held) and injects the keys through the input module, so they pass the same
 * modifier tracking, shortcuts and sticky keys as a real keyboard.
 *
 * Five rows of 15 units (a unit = 4 quarters): number row, Tab row, Caps row, Shift row, and a bottom row with Hide,
 * Esc, Ctrl, Alt, Space, AltGr and the arrows. The labels come from lm_keymap_keysym(), so Estonian shows õ ä ö ü, German
 * ß and so on; Shift / AltGr change them while they are latched.
 */

#include <stdbool.h>
#include <stdint.h>
#include "util.h"

enum lm_osk_kind {
    LM_OSK_KEY,         /* an ordinary key: press and release the evdev code */
    LM_OSK_SHIFT,       /* the modifiers latch for the next key (a second tap lets go) */
    LM_OSK_CTRL,
    LM_OSK_ALT,
    LM_OSK_ALTGR,
    LM_OSK_CAPS,        /* taps Caps Lock (the input module toggles it) */
    LM_OSK_HIDE,        /* closes the keyboard */
};

#define LM_OSK_MAX_KEYS 80
#define LM_OSK_LABEL 8

struct lm_osk_key {
    uint32_t code;      /* evdev KEY_* (0 for Hide) */
    int row;            /* 0 = number row ... 4 = bottom row */
    enum lm_osk_kind kind;
    struct lm_rect rect;
};

/* Where the keyboard is and how big: the user drags and resizes it. w == 0 = the default (as wide as the output allows up to 1180 px,
 * centred along the bottom); otherwise x / y is the card's top left corner, w its width and kh the height of one key row. */
struct lm_osk_place {
    int x, y, w, kh;
};

struct lm_osk_geo {
    struct lm_rect card;                    /* the whole keyboard, with its margin */
    int kh;                                 /* the height of a key (the place to resize from) */
    int n;
    struct lm_osk_key key[LM_OSK_MAX_KEYS];
};

/* Lays the keys out inside `out` (the output's rectangle) at `place` (NULL = the default); the result is kept inside the output and
 * its size within sane limits. */
void lm_osk_geometry(const struct lm_rect *out, const struct lm_osk_place *place, struct lm_osk_geo *g);
/* The card's height for a key height, and the other way round (for resizing with the mouse). */
int lm_osk_card_h(int kh);
int lm_osk_kh_for_h(int h);
/* The width of the strips along the edges of the card that move / resize it (nothing but margin there). */
int lm_osk_margin(void);

/* The modifier a latching kind stands for (LM_MOD_*), 0 for the others. */
uint32_t lm_osk_kind_mod(enum lm_osk_kind kind);
/* The evdev key that is pressed for a latched modifier kind (Shift -> KEY_LEFTSHIFT ...). */
uint32_t lm_osk_kind_code(enum lm_osk_kind kind);

/* What a key shows: the character under `mods` (LM_MOD_SHIFT / ALTGR, plus Caps in `locks`) in the layout group the keymap code
 * was given before (lm_keymap_set_groups / lm_keymap_client_group), or its name ("Tab", "Enter" ...). */
void lm_osk_label(const struct lm_osk_key *k, uint32_t mods, uint32_t locks, char out[LM_OSK_LABEL * 4]);

#endif

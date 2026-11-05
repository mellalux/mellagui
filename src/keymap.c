#include "keymap.h"

#include <stddef.h>

/* evdev code -> keysyms (unshifted, shifted) for the US layout. */
struct key_entry {
    uint16_t code;
    uint16_t plain, shifted;
};

static const struct key_entry keys[] = {
    { 2, '1', '!' }, { 3, '2', '@' }, { 4, '3', '#' }, { 5, '4', '$' },
    { 6, '5', '%' }, { 7, '6', '^' }, { 8, '7', '&' }, { 9, '8', '*' },
    { 10, '9', '(' }, { 11, '0', ')' }, { 12, '-', '_' }, { 13, '=', '+' },
    { 14, LM_KS_BackSpace, LM_KS_BackSpace }, { 15, LM_KS_Tab, LM_KS_Tab },
    { 16, 'q', 'Q' }, { 17, 'w', 'W' }, { 18, 'e', 'E' }, { 19, 'r', 'R' },
    { 20, 't', 'T' }, { 21, 'y', 'Y' }, { 22, 'u', 'U' }, { 23, 'i', 'I' },
    { 24, 'o', 'O' }, { 25, 'p', 'P' }, { 26, '[', '{' }, { 27, ']', '}' },
    { 28, LM_KS_Return, LM_KS_Return },
    { 30, 'a', 'A' }, { 31, 's', 'S' }, { 32, 'd', 'D' }, { 33, 'f', 'F' },
    { 34, 'g', 'G' }, { 35, 'h', 'H' }, { 36, 'j', 'J' }, { 37, 'k', 'K' },
    { 38, 'l', 'L' }, { 39, ';', ':' }, { 40, '\'', '"' }, { 41, '`', '~' },
    { 43, '\\', '|' },
    { 44, 'z', 'Z' }, { 45, 'x', 'X' }, { 46, 'c', 'C' }, { 47, 'v', 'V' },
    { 48, 'b', 'B' }, { 49, 'n', 'N' }, { 50, 'm', 'M' }, { 51, ',', '<' },
    { 52, '.', '>' }, { 53, '/', '?' }, { 57, ' ', ' ' },
    { 1, LM_KS_Escape, LM_KS_Escape },
    { 102, LM_KS_Home, LM_KS_Home }, { 103, LM_KS_Up, LM_KS_Up },
    { 104, LM_KS_Page_Up, LM_KS_Page_Up }, { 105, LM_KS_Left, LM_KS_Left },
    { 106, LM_KS_Right, LM_KS_Right }, { 107, LM_KS_End, LM_KS_End },
    { 108, LM_KS_Down, LM_KS_Down }, { 109, LM_KS_Page_Down, LM_KS_Page_Down },
    { 110, LM_KS_Insert, LM_KS_Insert }, { 111, LM_KS_Delete, LM_KS_Delete },
    { 96, LM_KS_KP_Enter, LM_KS_KP_Enter },
    { 29, LM_KS_Control_L, LM_KS_Control_L }, { 97, 0xffe4, 0xffe4 },
    { 42, LM_KS_Shift_L, LM_KS_Shift_L }, { 54, 0xffe2, 0xffe2 },
    { 56, LM_KS_Alt_L, LM_KS_Alt_L }, { 100, 0xffea, 0xffea },
    { 125, LM_KS_Super_L, LM_KS_Super_L }, { 126, 0xffec, 0xffec },
    { 58, 0xffe5, 0xffe5 }, { 69, 0xff7f, 0xff7f },
};

/* Keypad: code -> (NumLock/shifted symbol, plain navigation symbol). */
static const struct key_entry keypad[] = {
    { 55, '*', '*' }, { 74, '-', '-' }, { 78, '+', '+' }, { 98, '/', '/' },
    { 71, '7', LM_KS_Home }, { 72, '8', LM_KS_Up }, { 73, '9', LM_KS_Page_Up },
    { 75, '4', LM_KS_Left }, { 76, '5', 0xff9d }, { 77, '6', LM_KS_Right },
    { 79, '1', LM_KS_End }, { 80, '2', LM_KS_Down }, { 81, '3', LM_KS_Page_Down },
    { 82, '0', LM_KS_Insert }, { 83, '.', LM_KS_Delete },
};

uint32_t lm_keymap_us_keysym(uint32_t code, uint32_t mods)
{
    if (code >= 59 && code <= 68)
        return LM_KS_F1 + (code - 59);
    if (code == 87 || code == 88)
        return LM_KS_F1 + 10 + (code - 87);
    for (size_t i = 0; i < sizeof(keypad) / sizeof(keypad[0]); i++) {
        if (keypad[i].code != code)
            continue;
        return (mods & LM_MOD_NUM) ? keypad[i].plain : keypad[i].shifted;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i].code != code)
            continue;
        uint32_t plain = keys[i].plain;
        int letter = plain >= 'a' && plain <= 'z';
        int shift = (mods & LM_MOD_SHIFT) != 0;
        if (letter && (mods & LM_MOD_CAPS))
            shift = !shift;
        return shift ? keys[i].shifted : plain;
    }
    return 0;
}

int lm_keysym_to_utf8(uint32_t ks, char out[8])
{
    uint32_t cp;
    if (ks >= 0x20 && ks <= 0x7e)
        cp = ks;
    else if (ks >= 0xa0 && ks <= 0xff)
        cp = ks;
    else if ((ks & 0xff000000u) == 0x01000000u)
        cp = ks & 0x00ffffffu;
    else
        return out[0] = 0, 0;
    if (cp > 0x10ffff)
        return out[0] = 0, 0;
    int n;
    if (cp < 0x80) { out[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3f)); n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
    } else {
        out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f)); n = 4;
    }
    out[n] = 0;
    return n;
}

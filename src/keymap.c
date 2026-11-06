#include "keymap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "font.h"
#include "keymap_xkb.h"

/* ---- layout tables ----------------------------------------------------------------------------------------
 *
 * The typewriter block (digits, letters, punctuation, the ISO "<>" key) is described per layout as UTF-8
 * strings, one character per level: level 1 (plain), 2 (Shift), 3 (AltGr), 4 (Shift+AltGr). A layout lists only
 * the keys that differ from US; everything else (function, navigation, keypad, modifiers) is layout independent.
 */

struct row {
    uint16_t code;                  /* evdev */
    const char *xkb;                /* XKB key name */
    const char *lv;                 /* levels, UTF-8, 1..4 characters */
};

struct layout {
    const char *id, *name;
    const struct row *rows;         /* overrides of the US rows */
    int nrows;
};

static const struct row us_rows[] = {
    { 41, "TLDE", "`~" },  { 2, "AE01", "1!" },  { 3, "AE02", "2@" },  { 4, "AE03", "3#" },  { 5, "AE04", "4$" },
    { 6, "AE05", "5%" },   { 7, "AE06", "6^" },  { 8, "AE07", "7&" },  { 9, "AE08", "8*" },  { 10, "AE09", "9(" },
    { 11, "AE10", "0)" },  { 12, "AE11", "-_" }, { 13, "AE12", "=+" },
    { 16, "AD01", "qQ" },  { 17, "AD02", "wW" }, { 18, "AD03", "eE" }, { 19, "AD04", "rR" }, { 20, "AD05", "tT" },
    { 21, "AD06", "yY" },  { 22, "AD07", "uU" }, { 23, "AD08", "iI" }, { 24, "AD09", "oO" }, { 25, "AD10", "pP" },
    { 26, "AD11", "[{" },  { 27, "AD12", "]}" },  { 43, "BKSL", "\\|" },
    { 30, "AC01", "aA" },  { 31, "AC02", "sS" }, { 32, "AC03", "dD" }, { 33, "AC04", "fF" }, { 34, "AC05", "gG" },
    { 35, "AC06", "hH" },  { 36, "AC07", "jJ" }, { 37, "AC08", "kK" }, { 38, "AC09", "lL" }, { 39, "AC10", ";:" },
    { 40, "AC11", "'\"" },
    { 86, "LSGT", "<>" },
    { 44, "AB01", "zZ" },  { 45, "AB02", "xX" }, { 46, "AB03", "cC" }, { 47, "AB04", "vV" }, { 48, "AB05", "bB" },
    { 49, "AB06", "nN" },  { 50, "AB07", "mM" }, { 51, "AB08", ",<" }, { 52, "AB09", ".>" }, { 53, "AB10", "/?" },
};
#define N_US_ROWS ((int)(sizeof(us_rows) / sizeof(us_rows[0])))

static const struct row ee_rows[] = {
    { 41, "TLDE", "ˇ~" }, { 3, "AE02", "2\"@" }, { 4, "AE03", "3#£" }, { 5, "AE04", "4¤$" },
    { 6, "AE05", "5%" }, { 7, "AE06", "6&" }, { 8, "AE07", "7/{" }, { 9, "AE08", "8([" }, { 10, "AE09", "9)]" },
    { 11, "AE10", "0=}" }, { 12, "AE11", "+?\\" }, { 13, "AE12", "´`" },
    { 18, "AD03", "eE€" }, { 26, "AD11", "üÜ" }, { 27, "AD12", "õÕ§" },
    { 43, "BKSL", "'*½" },
    { 33, "AC04", "fFšŠ" }, { 39, "AC10", "öÖ" }, { 40, "AC11", "äÄ" },
    { 86, "LSGT", "<>|" },
    { 44, "AB01", "zZžŽ" }, { 51, "AB08", ",;" }, { 52, "AB09", ".:" }, { 53, "AB10", "-_" },
};

static const struct row de_rows[] = {
    { 41, "TLDE", "^°" }, { 3, "AE02", "2\"²" }, { 4, "AE03", "3§³" }, { 5, "AE04", "4$" },
    { 6, "AE05", "5%" }, { 7, "AE06", "6&" }, { 8, "AE07", "7/{" }, { 9, "AE08", "8([" }, { 10, "AE09", "9)]" },
    { 11, "AE10", "0=}" }, { 12, "AE11", "ß?\\" }, { 13, "AE12", "´`" },
    { 16, "AD01", "qQ@" }, { 18, "AD03", "eE€" }, { 21, "AD06", "zZ" }, { 26, "AD11", "üÜ" },
    { 27, "AD12", "+*~" }, { 43, "BKSL", "#'" },
    { 39, "AC10", "öÖ" }, { 40, "AC11", "äÄ" },
    { 86, "LSGT", "<>|" },
    { 44, "AB01", "yY" }, { 50, "AB07", "mMµ" }, { 51, "AB08", ",;" }, { 52, "AB09", ".:" }, { 53, "AB10", "-_" },
};

static const struct row fi_rows[] = {
    { 41, "TLDE", "§½" }, { 3, "AE02", "2\"@" }, { 4, "AE03", "3#£" }, { 5, "AE04", "4¤$" },
    { 6, "AE05", "5%" }, { 7, "AE06", "6&" }, { 8, "AE07", "7/{" }, { 9, "AE08", "8([" }, { 10, "AE09", "9)]" },
    { 11, "AE10", "0=}" }, { 12, "AE11", "+?\\" }, { 13, "AE12", "´`" },
    { 18, "AD03", "eE€" }, { 26, "AD11", "åÅ" }, { 27, "AD12", "¨^~" },
    { 43, "BKSL", "'*" },
    { 39, "AC10", "öÖ" }, { 40, "AC11", "äÄ" },
    { 86, "LSGT", "<>|" },
    { 51, "AB08", ",;" }, { 52, "AB09", ".:" }, { 53, "AB10", "-_" },
};

#define LAYOUT(id, name, rows) { id, name, rows, (int)(sizeof(rows) / sizeof(rows[0])) }
static const struct layout layouts[] = {
    { "us", "English (US)", NULL, 0 },
    LAYOUT("ee", "Estonian", ee_rows),
    LAYOUT("de", "German", de_rows),
    LAYOUT("fi", "Finnish", fi_rows),
};
#define N_LAYOUTS ((int)(sizeof(layouts) / sizeof(layouts[0])))

int lm_layout_count(void) { return N_LAYOUTS; }
const char *lm_layout_id(int l) { return l >= 0 && l < N_LAYOUTS ? layouts[l].id : "??"; }
const char *lm_layout_name(int l) { return l >= 0 && l < N_LAYOUTS ? layouts[l].name : "?"; }

int lm_layout_find(const char *id)
{
    for (int i = 0; i < N_LAYOUTS; i++)
        if (!strcmp(layouts[i].id, id))
            return i;
    return -1;
}

int lm_layout_parse(const char *list, int out[LM_KBD_MAX_LAYOUTS], char *err, size_t errn)
{
    int n = 0;
    if (!list || !*list)
        list = "us";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", list);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        int l = lm_layout_find(tok);
        if (l < 0) {
            if (err && errn)
                snprintf(err, errn, "unknown keyboard layout '%s' (available: us, ee, de, fi)", tok);
            return 0;
        }
        bool dup = false;
        for (int i = 0; i < n; i++)
            dup |= out[i] == l;
        if (dup)
            continue;
        if (n == LM_KBD_MAX_LAYOUTS) {
            if (err && errn)
                snprintf(err, errn, "at most %d keyboard layouts can be configured", LM_KBD_MAX_LAYOUTS);
            return 0;
        }
        out[n++] = l;
    }
    if (n == 0 && err && errn)
        snprintf(err, errn, "empty keyboard layout list");
    return n;
}

bool lm_layout_needs_altgr(const int *l, int n)
{
    return !(n == 1 && l[0] == 0);
}

/* The row of `code` in layout `l` (override, else the US row); NULL when the key is not part of the typewriter block. */
static const struct row *layout_row(int l, uint32_t code)
{
    if (l >= 0 && l < N_LAYOUTS)
        for (int i = 0; i < layouts[l].nrows; i++)
            if (layouts[l].rows[i].code == code)
                return &layouts[l].rows[i];
    for (int i = 0; i < N_US_ROWS; i++)
        if (us_rows[i].code == code)
            return &us_rows[i];
    return NULL;
}

/* Decode the levels of a row; returns how many there are. */
static int row_levels(const struct row *r, uint32_t cp[4])
{
    int n = 0, len = (int)strlen(r->lv);
    for (int i = 0; i < len && n < 4;) {
        int used;
        cp[n++] = lm_utf8_decode(r->lv + i, len - i, &used);
        i += used > 0 ? used : 1;
    }
    return n;
}

static uint32_t cp_upper(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z')
        return cp - 32;
    if (cp >= 0xe0 && cp <= 0xfe && cp != 0xf7)
        return cp - 32;
    if (cp == 0x161 || cp == 0x17e)
        return cp - 1;
    return cp;
}

/* A letter key: level 2 is the capital of level 1 (Caps Lock applies). */
static bool row_is_letter(const uint32_t cp[4], int n)
{
    return n >= 2 && cp[0] != cp[1] && cp_upper(cp[0]) == cp[1];
}

static uint32_t cp_to_keysym(uint32_t cp)
{
    return cp < 0x100 ? cp : 0x01000000u | cp;
}

/* ---- XKB text generation ------------------------------------------------------------------------------ */

struct buf { char *p; size_t n, cap; };

static void buf_add(struct buf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 65536;
        while (b->cap < b->n + l + 1)
            b->cap *= 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p)
            abort();
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void buf_printf(struct buf *b, const char *fmt, ...)
{
    char tmp[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    buf_add(b, tmp);
}

char *lm_keymap_build(const int *sel, int n)
{
    struct buf b = { 0 };
    bool altgr = lm_layout_needs_altgr(sel, n);
    buf_add(&b, xkb_head);
    for (int g = 0; g < n; g++)
        buf_printf(&b, "  name[Group%d] = \"%s\";\n", g + 1, lm_layout_name(sel[g]));
    for (int i = 0; i < N_US_ROWS; i++) {
        const struct row *us = &us_rows[i];
        buf_printf(&b, "  key <%s> {", us->xkb);
        for (int g = 0; g < n; g++) {
            const struct row *r = layout_row(sel[g], us->code);
            uint32_t cp[4];
            int lv = row_levels(r, cp);
            const char *type = lv > 2 ? (row_is_letter(cp, lv) ? "FOUR_LEVEL_ALPHABETIC" : "FOUR_LEVEL")
                                      : (row_is_letter(cp, lv) ? "ALPHABETIC" : "TWO_LEVEL");
            buf_printf(&b, "%s type[Group%d] = \"%s\", symbols[Group%d] = [", g ? "," : "", g + 1, type, g + 1);
            for (int k = 0; k < lv; k++)
                buf_printf(&b, "%s U%04X", k ? "," : "", (unsigned)cp[k]);
            buf_add(&b, " ]");
        }
        buf_add(&b, " };\n");
    }
    if (altgr)
        buf_add(&b, "  key <RALT> { type[Group1] = \"ONE_LEVEL\", symbols[Group1] = [ ISO_Level3_Shift ], "
                    "virtualMods = LevelThree };\n");
    else
        buf_add(&b, "  key <RALT> { [ Alt_R, Meta_R ] };\n");
    buf_add(&b, xkb_shared_keys);
    buf_add(&b, xkb_modmap_rest);
    buf_add(&b, altgr ? "  modifier_map Mod1 { <LALT> };\n  modifier_map Mod5 { <RALT> };\n"
                      : "  modifier_map Mod1 { <LALT>, <RALT> };\n");
    buf_add(&b, "};\n};\n");
    return b.p;
}

/* ---- client side lookup --------------------------------------------------------------------------------- */

/* evdev code -> keysyms (unshifted, shifted) of the layout independent keys. */
struct key_entry {
    uint16_t code;
    uint16_t plain, shifted;
};

static const struct key_entry keys[] = {
    { 14, LM_KS_BackSpace, LM_KS_BackSpace }, { 15, LM_KS_Tab, LM_KS_Tab },
    { 28, LM_KS_Return, LM_KS_Return }, { 57, ' ', ' ' },
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

static int client_group_layout[LM_KBD_MAX_LAYOUTS];     /* XKB group -> layout index, default: all "us" */
static int client_ngroups = 1;
static uint32_t client_group;

void lm_keymap_client_load(int fd, uint32_t size)
{
    if (fd < 0)
        return;
    if (size > 0 && size < (1u << 24)) {
        char *text = malloc(size + 1);
        if (text) {
            size_t off = 0;
            while (off < size) {
                ssize_t r = pread(fd, text + off, size - off, (off_t)off);
                if (r <= 0)
                    break;
                off += (size_t)r;
            }
            text[off] = 0;
            int ng = 0;
            int map[LM_KBD_MAX_LAYOUTS] = { 0 };
            for (int g = 1; g <= LM_KBD_MAX_LAYOUTS; g++) {
                char key[24];
                snprintf(key, sizeof(key), "name[Group%d] = \"", g);
                const char *p = strstr(text, key);
                if (!p)
                    break;
                p += strlen(key);
                size_t l = strcspn(p, "\"");
                int found = 0;
                for (int i = 0; i < N_LAYOUTS; i++)
                    if (strlen(layouts[i].name) == l && !strncmp(layouts[i].name, p, l))
                        found = i;
                map[ng++] = found;
            }
            if (ng > 0) {
                memcpy(client_group_layout, map, sizeof(map));
                client_ngroups = ng;
            }
            free(text);
        }
    }
    close(fd);
}

void lm_keymap_client_group(uint32_t group)
{
    client_group = group;
}

uint32_t lm_keymap_keysym(uint32_t code, uint32_t mods)
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
        return (mods & LM_MOD_SHIFT) ? keys[i].shifted : keys[i].plain;
    }
    int layout = client_group_layout[client_ngroups > 0 ? client_group % (uint32_t)client_ngroups : 0];
    const struct row *r = layout_row(layout, code);
    if (!r)
        return 0;
    uint32_t cp[4];
    int n = row_levels(r, cp);
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    if (row_is_letter(cp, n) && (mods & LM_MOD_CAPS))
        shift = !shift;
    int level = shift ? 1 : 0;
    if (n > 2 && (mods & LM_MOD_ALTGR))
        level += 2;
    if (level >= n)
        return level == 1 ? cp[0] : 0;
    return cp_to_keysym(cp[level]);
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

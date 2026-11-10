/*
 * Keyboard shortcut table. See keys.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <linux/input-event-codes.h>

#include "conf.h"
#include "keymap.h"
#include "keys.h"
#include "log.h"

#define DEFAULT_KEYS \
    "Super+Enter       = terminal\n" \
    "Super+R           = run\n" \
    "Super+I           = settings\n" \
    "Super+Space       = layout-next\n" \
    "Super+Shift+Space = layout-prev\n" \
    "Super+G           = gadget-clipboard\n" \
    "Super+C           = clipboard-pick\n" \
    "Super+V           = gadget-mixer\n" \
    "Super+S           = gadget-system\n" \
    "Super+Q           = close\n" \
    "Super+L           = lock\n" \
    "Super+1           = workspace 1\n" \
    "Super+2           = workspace 2\n" \
    "Super+3           = workspace 3\n" \
    "Super+4           = workspace 4\n" \
    "Super+Shift+1     = move-to-workspace 1\n" \
    "Super+Shift+2     = move-to-workspace 2\n" \
    "Super+Shift+3     = move-to-workspace 3\n" \
    "Super+Shift+4     = move-to-workspace 4\n" \
    "Super+Tab         = focus-next\n" \
    "Super+Shift+Tab   = focus-prev\n" \
    "Super+F           = fullscreen\n" \
    "Super+M           = maximize-toggle\n" \
    "Super+Left        = tile-left\n" \
    "Super+Right       = tile-right\n" \
    "Super+Up          = maximize\n" \
    "Super+Down        = restore\n" \
    "Super+Shift+E     = quit\n" \
    "Super+A           = launcher\n" \
    "Super+K           = osk\n" \
    "Super+Shift+R     = reload-config\n" \
    "Super+Slash       = exec mellagui-keys\n"

const char *lm_keys_default_conf(void)
{
    return "[keys]\n" DEFAULT_KEYS;
}

/* ---- names -------------------------------------------------------------------------------------------------------- */

struct keyname { const char *name; uint32_t code; };

#define L(c) { #c, KEY_##c }
#define D(n) { #n, KEY_##n }
#define F(n) { "F" #n, KEY_F##n }
static const struct keyname key_names[] = {
    L(A), L(B), L(C), L(D), L(E), L(F), L(G), L(H), L(I), L(J), L(K), L(L), L(M),
    L(N), L(O), L(P), L(Q), L(R), L(S), L(T), L(U), L(V), L(W), L(X), L(Y), L(Z),
    { "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 },
    { "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 },
    F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8), F(9), F(10), F(11), F(12),
    F(13), F(14), F(15), F(16), F(17), F(18), F(19), F(20), F(21), F(22), F(23), F(24),
    { "Enter", KEY_ENTER }, { "Return", KEY_ENTER }, { "Tab", KEY_TAB }, { "Space", KEY_SPACE },
    { "Escape", KEY_ESC }, { "Esc", KEY_ESC }, { "Backspace", KEY_BACKSPACE }, { "Delete", KEY_DELETE },
    { "Insert", KEY_INSERT }, { "Home", KEY_HOME }, { "End", KEY_END }, { "PageUp", KEY_PAGEUP },
    { "PageDown", KEY_PAGEDOWN }, { "Left", KEY_LEFT }, { "Right", KEY_RIGHT }, { "Up", KEY_UP }, { "Down", KEY_DOWN },
    { "Minus", KEY_MINUS }, { "Equal", KEY_EQUAL }, { "Comma", KEY_COMMA }, { "Period", KEY_DOT },
    { "Slash", KEY_SLASH }, { "Semicolon", KEY_SEMICOLON }, { "Apostrophe", KEY_APOSTROPHE }, { "Grave", KEY_GRAVE },
    { "Backslash", KEY_BACKSLASH }, { "LeftBrace", KEY_LEFTBRACE }, { "RightBrace", KEY_RIGHTBRACE },
};
#undef L
#undef D
#undef F

static const struct keyname mod_names[] = {
    { "Super", LM_MOD_SUPER }, { "Win", LM_MOD_SUPER }, { "Mod4", LM_MOD_SUPER }, { "Meta", LM_MOD_SUPER },
    { "Ctrl", LM_MOD_CTRL }, { "Control", LM_MOD_CTRL }, { "Alt", LM_MOD_ALT }, { "Shift", LM_MOD_SHIFT },
};

static bool parse_combo(const char *text, uint32_t *mods, uint32_t *code)
{
    char buf[96];
    if (strlen(text) >= sizeof(buf))
        return false;
    strcpy(buf, text);
    *mods = 0;
    *code = 0;
    bool have_key = false;
    for (char *tok = buf; tok;) {
        char *plus = strchr(tok, '+');
        if (plus && plus[1] == 0 && plus != tok)        /* "Super++": not supported, say Plus via Equal */
            return false;
        if (plus)
            *plus = 0;
        while (isspace((unsigned char)*tok))
            tok++;
        for (char *e = tok + strlen(tok); e > tok && isspace((unsigned char)e[-1]);)
            *--e = 0;
        if (have_key || !*tok)
            return false;                               /* something after the key, or an empty part */
        bool found = false;
        if (plus) {                                     /* not the last part: a modifier */
            for (size_t i = 0; i < sizeof(mod_names) / sizeof(mod_names[0]); i++) {
                if (!strcasecmp(tok, mod_names[i].name)) {
                    *mods |= mod_names[i].code;
                    found = true;
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
                if (!strcasecmp(tok, key_names[i].name)) {
                    *code = key_names[i].code;
                    found = have_key = true;
                    break;
                }
            }
        }
        if (!found)
            return false;
        tok = plus ? plus + 1 : NULL;
    }
    return have_key;
}

/* ---- actions ------------------------------------------------------------------------------------------------------ */

enum arg_kind { ARG_NONE, ARG_WORKSPACE, ARG_COMMAND };
struct action_def { const char *name; enum lm_action act; enum arg_kind arg; };
static const struct action_def actions[] = {
    { "none", LM_ACT_NONE, ARG_NONE },
    { "terminal", LM_ACT_TERMINAL, ARG_NONE }, { "run", LM_ACT_RUN, ARG_NONE }, { "settings", LM_ACT_SETTINGS, ARG_NONE },
    { "exec", LM_ACT_EXEC, ARG_COMMAND },
    { "layout-next", LM_ACT_LAYOUT_NEXT, ARG_NONE }, { "layout-prev", LM_ACT_LAYOUT_PREV, ARG_NONE },
    { "gadget-clipboard", LM_ACT_GADGET_CLIPBOARD, ARG_NONE }, { "clipboard-pick", LM_ACT_CLIPBOARD_PICK, ARG_NONE },
    { "gadget-mixer", LM_ACT_GADGET_MIXER, ARG_NONE }, { "gadget-system", LM_ACT_GADGET_SYSTEM, ARG_NONE },
    { "close", LM_ACT_CLOSE, ARG_NONE }, { "lock", LM_ACT_LOCK, ARG_NONE }, { "quit", LM_ACT_QUIT, ARG_NONE },
    { "workspace", LM_ACT_WORKSPACE, ARG_WORKSPACE }, { "move-to-workspace", LM_ACT_MOVE_TO_WORKSPACE, ARG_WORKSPACE },
    { "focus-next", LM_ACT_FOCUS_NEXT, ARG_NONE }, { "focus-prev", LM_ACT_FOCUS_PREV, ARG_NONE },
    { "fullscreen", LM_ACT_FULLSCREEN, ARG_NONE }, { "maximize-toggle", LM_ACT_MAXIMIZE_TOGGLE, ARG_NONE },
    { "maximize", LM_ACT_MAXIMIZE, ARG_NONE }, { "restore", LM_ACT_RESTORE, ARG_NONE },
    { "tile-left", LM_ACT_TILE_LEFT, ARG_NONE }, { "tile-right", LM_ACT_TILE_RIGHT, ARG_NONE },
    { "reload-config", LM_ACT_RELOAD_CONFIG, ARG_NONE }, { "launcher", LM_ACT_LAUNCHER, ARG_NONE }, { "osk", LM_ACT_OSK, ARG_NONE },
};

/* ---- the table ---------------------------------------------------------------------------------------------------- */

static int find_index(const struct lm_keybinds *k, uint32_t mods, uint32_t code)
{
    for (int i = 0; i < k->n; i++)
        if (k->b[i].code == code && k->b[i].mods == mods)
            return i;
    return -1;
}

/* "close" / "workspace 3" / "exec CMD" -> the action fields of nb. */
static bool parse_action(const char *value, struct lm_keybind *nb)
{
    char name[40];
    const char *arg = value;
    size_t nl = strcspn(value, " \t");
    if (nl == 0 || nl >= sizeof(name))
        return false;
    memcpy(name, value, nl);
    name[nl] = 0;
    arg += nl;
    while (isspace((unsigned char)*arg))
        arg++;

    const struct action_def *def = NULL;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (!strcmp(name, actions[i].name))
            def = &actions[i];
    if (!def)
        return false;

    nb->action = def->act;
    nb->num = 0;
    nb->arg[0] = 0;
    if (def->arg == ARG_NONE && *arg)
        return false;
    if (def->arg == ARG_WORKSPACE) {
        char *end;
        long n = strtol(arg, &end, 10);
        if (end == arg || *end || n < 1 || n > 4)      /* LM_NUM_WORKSPACES */
            return false;
        nb->num = (int)n - 1;
    }
    if (def->arg == ARG_COMMAND) {
        if (!*arg || strlen(arg) >= LM_KEYBIND_ARG)
            return false;
        strcpy(nb->arg, arg);
    }
    return true;
}

/* Install or (action none) remove a binding. The order of the others is kept (the panel lists them in table order). */
static bool set_binding(struct lm_keybinds *k, const char *combo, const char *value)
{
    uint32_t mods, code;
    if (!parse_combo(combo, &mods, &code))
        return false;
    struct lm_keybind nb = { .mods = mods, .code = code };
    if (!parse_action(value, &nb))
        return false;

    int i = find_index(k, mods, code);
    if (nb.action == LM_ACT_NONE) {
        if (i >= 0) {
            memmove(&k->b[i], &k->b[i + 1], (size_t)(k->n - i - 1) * sizeof(k->b[0]));
            k->n--;
        }
        return true;
    }
    if (i < 0) {
        if (k->n >= LM_MAX_KEYBINDS) {
            lm_warn("keys: at most %d bindings, '%s' ignored", LM_MAX_KEYBINDS, combo);
            return true;
        }
        i = k->n++;
    }
    k->b[i] = nb;
    return true;
}

bool lm_keys_parse_combo(const char *text, uint32_t *mods, uint32_t *code) { return parse_combo(text, mods, code); }

bool lm_keys_valid(const char *combo, const char *value)
{
    uint32_t mods, code;
    struct lm_keybind nb;
    return parse_combo(combo, &mods, &code) && parse_action(value, &nb) && nb.action != LM_ACT_NONE;
}

bool lm_keys_same_combo(const char *a, const char *b)
{
    uint32_t ma, ca, mb, cb;
    return parse_combo(a, &ma, &ca) && parse_combo(b, &mb, &cb) && ma == mb && ca == cb;
}

const char *lm_keys_action_names(void)
{
    static char text[512];
    if (!text[0]) {
        size_t o = 0;
        for (size_t i = 1; i < sizeof(actions) / sizeof(actions[0]); i++)          /* [0] is "none" */
            o += (size_t)snprintf(text + o, sizeof(text) - o, "%s%s", i > 1 ? " " : "", actions[i].name);
    }
    return text;
}

const struct lm_keybind *lm_keys_builtin(uint32_t code, uint32_t mods)
{
    static struct lm_keybinds defaults;
    if (!defaults.n)
        lm_keys_defaults(&defaults);
    return lm_keys_find(&defaults, code, mods);
}

bool lm_keys_conf(void *ud, const char *section, const char *key, const char *value)
{
    return !strcmp(section, "keys") && set_binding(ud, key, value);
}

void lm_keys_defaults(struct lm_keybinds *k)
{
    k->n = 0;
    lm_conf_parse("[keys]\n" DEFAULT_KEYS, "<built-in keys>", lm_keys_conf, k);
}

const struct lm_keybind *lm_keys_find(const struct lm_keybinds *k, uint32_t code, uint32_t mods)
{
    int i = find_index(k, mods & LM_KEY_MODS_MASK, code);
    return i < 0 ? NULL : &k->b[i];
}

bool lm_keys_combo_text(uint32_t mods, uint32_t code, char *out, size_t n)
{
    static const struct { uint32_t bit; const char *name; } order[] = {
        { LM_MOD_SUPER, "Super" }, { LM_MOD_CTRL, "Ctrl" }, { LM_MOD_ALT, "Alt" }, { LM_MOD_SHIFT, "Shift" } };
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < 4 && o < n; i++)
        if (mods & order[i].bit)
            o += (size_t)snprintf(out + o, n - o, "%s+", order[i].name);
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        if (key_names[i].code == code) {
            if (o < n)
                snprintf(out + o, n - o, "%s", key_names[i].name);
            return true;
        }
    }
    return false;
}

void lm_keys_action_text(const struct lm_keybind *b, char *out, size_t n)
{
    const char *act = "?";
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (actions[i].act == b->action)
            act = actions[i].name;
    if (b->action == LM_ACT_WORKSPACE || b->action == LM_ACT_MOVE_TO_WORKSPACE)
        snprintf(out, n, "%s %d", act, b->num + 1);
    else if (b->action == LM_ACT_EXEC)
        snprintf(out, n, "%s %s", act, b->arg);
    else
        snprintf(out, n, "%s", act);
}

void lm_keys_describe(const struct lm_keybind *b, char *out, size_t n)
{
    char combo[64], act[LM_KEYBIND_ARG + 40];
    if (!lm_keys_combo_text(b->mods, b->code, combo, sizeof(combo)))
        snprintf(combo, sizeof(combo), "?");
    lm_keys_action_text(b, act, sizeof(act));
    snprintf(out, n, "%s = %s", combo, act);
}

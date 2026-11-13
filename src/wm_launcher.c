/* Part of the window manager (see wm_priv.h): the launcher dialog, the text cursor of the form's fields */
#include "wm_priv.h"

/* ---- the launcher dialog -------------------------------------------------------------------------------------------------- */

#define LC_CELL_W LM_LAUNCHER_CELL_W
#define LC_CELL_H 116
#define LC_ICON LM_LAUNCHER_ICON
#define LC_PAD 28
#define LC_HEAD 56
#define LC_FOOT 84
#define LC_BTN_W 110
#define LC_BTN_H 28
#define LC_KIND 36                  /* the icon choices of the form */
#define LC_FORM_W 580
#define LC_FORM_H 440

/* The buttons the launcher shows: the dock's (all but the one that opens the launcher itself) and then the launcher's own ([launcher-apps]). */
bool launcher_is_self(const struct lm_dock_item *it)
{
    return !strcmp(it->cmd, "@launcher");
}

/* The dock's buttons that are not programs: the one that opens this dialog and the one that opens the desktop overview. */
bool dock_special(const struct lm_dock_item *it)
{
    return launcher_is_self(it) || !strcmp(it->cmd, "@overview");
}

/* Search: what is typed in the dialog filters the icons by name (one wm, so file scope). */
static char launcher_filter[48];
static bool launcher_searching;

static bool launcher_shown(const struct lm_dock_item *it)
{
    return !launcher_is_self(it) && (!launcher_filter[0] || strcasestr(it->name, launcher_filter));          /* (the Overview button shows: it is a button like the others) */
}

static bool launcher_name_taken(const char *name);

/* The results of the search for the text typed (search.h): asked again when the text changes or the file index has grown. */
static char search_for[48];
static int search_n, search_indexed;

static void search_sync(void)
{
    if (!launcher_filter[0]) {
        search_n = 0;
        search_for[0] = 0;
        return;
    }
    int idx = lm_search_indexed(NULL);
    if (strcmp(search_for, launcher_filter) || idx != search_indexed) {
        snprintf(search_for, sizeof(search_for), "%s", launcher_filter);
        search_indexed = idx;
        search_n = lm_search_query(launcher_filter, launcher_name_taken);
    }
}

int launcher_count(void)
{
    int n = 0;
    for (int i = 0; i < lm_dock_count(); i++)
        if (launcher_shown(lm_dock_item(i)))
            n++;
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (launcher_shown(lm_launchapps_item(i)))
            n++;
    search_sync();
    return n + search_n;
}

/* The g-th button of the launcher; *dock_index is its index in the dock, -1 for one that is only in the launcher. NULL when there is none. */
const struct lm_dock_item *launcher_item(int g, int *dock_index)
{
    if (dock_index)
        *dock_index = -1;
    for (int i = 0; i < lm_dock_count(); i++)
        if (launcher_shown(lm_dock_item(i)) && g-- == 0) {
            if (dock_index)
                *dock_index = i;
            return lm_dock_item(i);
        }
    for (int i = 0; i < lm_launchapps_count(); i++)
        if (launcher_shown(lm_launchapps_item(i)) && g-- == 0)
            return lm_launchapps_item(i);
    search_sync();
    return g >= 0 && g < search_n ? lm_search_item(g) : NULL;           /* what the search found comes after the buttons */
}

static bool launcher_name_taken(const char *name)
{
    const char *nm, *ic, *cm;
    for (int i = 0; lm_dock_raw_entry(i, &nm, &ic, &cm); i++)
        if (!strcasecmp(nm, name))
            return true;
    for (int i = 0; lm_launchapps_raw_entry(i, &nm, &ic, &cm); i++)
        if (!strcasecmp(nm, name))
            return true;
    return false;
}

static bool launcher_can_add(void)
{
    return lm_launchapps_raw_count() < LM_DOCK_MAX;
}


struct lm_output *launcher_output(const struct lm_wm *wm)
{
    if (!wm->launcher_open || !wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->launcher_out))
            return o;
    return NULL;
}

/* The built-in icon the form's icon text names: -1 when it is empty, -2 when it is something else (a theme name, a file). */
static int launcher_kind_of(const struct lm_wm *wm)
{
    if (!wm->launcher_icon[0])
        return -1;
    int k = lm_dockicon_by_name(wm->launcher_icon);
    return k >= 0 ? k : -2;
}

/* ---- the text cursor of the form's fields ---- */

static int text_width_to(const char *text, int from, int to)
{
    char tmp[LM_DOCK_CMD_MAX];
    int n = LM_MIN(to - from, (int)sizeof(tmp) - 1);
    memcpy(tmp, text + from, (size_t)LM_MAX(n, 0));
    tmp[LM_MAX(n, 0)] = 0;
    return lm_font_text_width(tmp);
}

static int utf8_prev(const char *t, int i)
{
    if (i > 0)
        i--;
    while (i > 0 && ((unsigned char)t[i] & 0xc0) == 0x80)
        i--;
    return i;
}

static int utf8_next(const char *t, int i, int len)
{
    if (i < len)
        i++;
    while (i < len && ((unsigned char)t[i] & 0xc0) == 0x80)
        i++;
    return i;
}

void lm_launcher_field_view(const char *text, int caret, int avail, int *start, int *end)
{
    int len = (int)strlen(text), st = 0;
    caret = LM_CLAMP(caret, 0, len);
    while (st < caret && text_width_to(text, st, caret) > avail - 2)       /* scrolled so that the caret shows */
        st = utf8_next(text, st, len);
    int en = st;
    while (en < len) {
        int nx = utf8_next(text, en, len);
        if (text_width_to(text, st, nx) > avail)
            break;
        en = nx;
    }
    *start = st;
    *end = en;
}

/* The caret of field f, always on a character boundary inside the text (-1 = the end). */
static int lf_caret(const struct lm_wm *wm, int f, const char *text)
{
    int len = (int)strlen(text), c = wm->launcher_caret[f];
    if (c < 0 || c > len)
        c = len;
    while (c > 0 && ((unsigned char)text[c] & 0xc0) == 0x80)
        c--;
    return c;
}

static bool is_word_char(unsigned char c)
{
    return isalnum(c) || c >= 0x80 || c == '_';
}

static int lf_word_prev(const char *t, int i)
{
    while (i > 0 && !is_word_char((unsigned char)t[i - 1]))
        i--;
    while (i > 0 && is_word_char((unsigned char)t[i - 1]))
        i--;
    return i;
}

static int lf_word_next(const char *t, int i, int len)
{
    while (i < len && !is_word_char((unsigned char)t[i]))
        i++;
    while (i < len && is_word_char((unsigned char)t[i]))
        i++;
    return i;
}

/* Puts `s` (n bytes) into text at the caret; false when it does not fit. The caret moves behind it. */
static bool lf_insert(char *text, size_t max, int *caret, const char *s, size_t n)
{
    size_t len = strlen(text);
    if (len + n >= max)
        return false;
    memmove(text + *caret + n, text + *caret, len - (size_t)*caret + 1);
    memcpy(text + *caret, s, n);
    *caret += (int)n;
    return true;
}

static void lf_erase(char *text, int from, int to)
{
    memmove(text + from, text + to, strlen(text + to) + 1);
}

void launcher_info(const struct lm_wm *wm, struct lm_output *o, struct lm_launcher_info *li)
{
    memset(li, 0, sizeof(*li));
    li->rect = lm_output_rect(o);
    li->veil = 1.0f;
    li->form = wm->launcher_form;
    li->editing = wm->launcher_form && wm->launcher_editing;
    li->edit = wm->launcher_edit && !li->form;
    li->hover_button = wm->launcher_hover_btn;
    li->message = wm->launcher_msg;
    li->focus = wm->launcher_focus;
    li->kind = launcher_kind_of(wm);
    li->kind_name = li->kind >= 0 ? lm_dockicon_name(li->kind) : li->kind == -2 ? "yours" : "auto";
    int n = launcher_count(), cells = n + (launcher_can_add() && !launcher_searching ? 1 : 0);
    li->search = launcher_filter;
    li->searching = launcher_searching && !li->form;
    li->n = n;
    li->sel = LM_CLAMP(wm->launcher_sel, -1, cells - 1);
    int cw, ch;
    if (li->form) {
        cw = LM_MIN(LC_FORM_W, li->rect.w - 20);
        ch = LC_FORM_H;
    } else {
        int maxcols = LM_CLAMP((li->rect.w - 60) / LC_CELL_W, 1, 6);
        li->cols = LM_MIN(maxcols, LM_MAX(cells, 1));
        int rows = LM_MAX((cells + li->cols - 1) / li->cols, 1);
        int cell_h = LC_CELL_H;
        if (LC_HEAD + rows * cell_h + LC_FOOT > li->rect.h - 20)       /* a low screen: the rows get closer */
            cell_h = LM_MAX((li->rect.h - 20 - LC_HEAD - LC_FOOT) / rows, LC_ICON + 8 + LM_FONT_H + 6);
        cw = LM_MAX(li->cols * LC_CELL_W + 2 * LC_PAD, 2 * LC_BTN_W + 56);
        ch = LC_HEAD + rows * cell_h + LC_FOOT;
        int x0 = li->rect.x + (li->rect.w - cw) / 2 + (cw - li->cols * LC_CELL_W) / 2;
        int y0 = li->rect.y + (li->rect.h - ch) / 2 + LC_HEAD;
        for (int g = 0; g < cells; g++) {
            struct lm_rect cell = lm_rect_make(x0 + (g % li->cols) * LC_CELL_W, y0 + (g / li->cols) * cell_h, LC_CELL_W, cell_h);
            struct lm_rect box = lm_rect_make(cell.x + (LC_CELL_W - LC_ICON) / 2, cell.y + 6, LC_ICON, LC_ICON);
            if (g == n) {
                li->add = cell;
                continue;
            }
            li->icon[g].box = box;
            li->icon[g].hit = cell;
            li->icon[g].remove = lm_rect_make(box.x - 8, box.y - 8, 24, 24);
            li->icon[g].item = launcher_item(g, NULL);
            li->icon[g].running = li->icon[g].item && !lm_search_owns(li->icon[g].item) && dock_window_count(wm, li->icon[g].item) > 0;
        }
    }
    li->card = lm_rect_make(li->rect.x + (li->rect.w - cw) / 2, li->rect.y + (li->rect.h - ch) / 2, cw, ch);
    li->drag = -1;
    li->drop_pos = -1;
    if (wm->launcher_drag && !li->form && wm->launcher_drag_g < n) {
        li->drag = wm->launcher_drag_g;
        li->drag_box = lm_rect_make((int)wm->drag_x - 28, (int)wm->drag_y - 28, 56, 56);
        launcher_drop_zone(wm, o, &li->drop, &li->marker, &li->drop_pos);
    }
    li->title = lm_rect_make(li->card.x, li->card.y + 18, li->card.w, LM_FONT_H);
    int by = li->card.y + li->card.h - 16 - LC_BTN_H, bx = li->card.x + li->card.w / 2;
    li->button[0] = lm_rect_make(bx - LC_BTN_W - 8, by, LC_BTN_W, LC_BTN_H);
    li->button[1] = lm_rect_make(bx + 8, by, LC_BTN_W, LC_BTN_H);
    li->button_text[0] = li->form ? (li->editing ? "Save" : "Add") : li->edit ? "Done" : "Edit";
    li->button_text[1] = li->form ? "Cancel" : "Close";
    static const char hint_grid[] = "Enter starts   E edit   A add   Type to search   Esc closes", hint_edit[] = "Click an icon to change it   red x removes   E or Done ends   Esc closes",
                      hint_form[] = "Tab next field   Left / Right cursor (or icon)   Enter adds   Esc back",
                      hint_change[] = "Tab next field   Left / Right cursor (or icon)   Enter saves   Esc back";
    static const char hint_search[] = "Type to search   Enter starts   Esc clears";
    li->hint = li->form ? (li->editing ? hint_change : hint_form) : li->searching ? hint_search : li->edit ? hint_edit : hint_grid;
    if (li->searching && li->sel >= 0 && li->sel < n) {                  /* a result of the search: what Enter does with it */
        const struct lm_dock_item *si = li->icon[li->sel].item;
        static char hint_result[PATH_MAX + 48];
        if (si && lm_search_owns(si)) {
            int ri = si->raw <= -1000 ? -1000 - si->raw : -1;
            const char *d = ri >= 0 ? lm_search_detail(ri) : "";
            snprintf(hint_result, sizeof(hint_result), "%s   (Esc clears)", d);
            li->hint = hint_result;
        }
    }
    if (li->form) {
        li->field_label[0] = "Name";
        li->field_label[1] = "Command   (@terminal, mellagui-*, ~/prog, a || b)";
        li->field_label[2] = "Icon   (empty = guessed, theme:firefox, a file, or pick below)";
        li->field_text[0] = wm->launcher_name;
        li->field_text[1] = wm->launcher_cmd;
        li->field_text[2] = wm->launcher_icon;
        for (int i = 0; i < 3; i++)
            li->caret[i] = lf_caret(wm, i, li->field_text[i]);
        int fx = li->card.x + LC_PAD, fw = li->card.w - 2 * LC_PAD;
        li->field[0] = lm_rect_make(fx, li->card.y + 60 + LM_FONT_H + 4, fw, 30);
        li->field[1] = lm_rect_make(fx, li->card.y + 126 + LM_FONT_H + 4, fw, 30);
        li->field[2] = lm_rect_make(fx, li->card.y + 192 + LM_FONT_H + 4, fw, 30);
        for (int i = 0; i < 3; i++)                                     /* the "?" at the end of each field */
            li->help_btn[i] = lm_rect_make(li->field[i].x + li->field[i].w - LM_LAUNCHER_HELP_D - 8, li->field[i].y + (li->field[i].h - LM_LAUNCHER_HELP_D) / 2,
                                           LM_LAUNCHER_HELP_D, LM_LAUNCHER_HELP_D);
        li->help = wm->launcher_help;
        if (li->help >= 1 && li->help <= 3) {
            static const char *const titles[3] = { "Name", "Command", "Icon" };
            static const char *const texts[3] = {
                "The name of the button. It is written under its icon and the search finds it by it. Every button needs a name of its own (capital "
                "and small letters do not count as different).",
                "The program to start. @terminal, @run, @settings ... are MellaGUI's own programs, mellagui-* are found next to MellaGUI, ~/prog is in "
                "your home folder. 'a || b' tries a and, if it is not installed, b. '@terminal -e alpine' runs a text program in a terminal. "
                "env VAR=value comes before the program.",
                "Empty: guessed from the name. A built-in icon: pick one below or type its name. theme:NAME takes an installed program's own icon "
                "(theme:firefox, or theme:vscode|code to try several names). Or the path of a PNG or JPEG picture; a logo with transparent corners "
                "gets a round plate.",
            };
            li->help_title = titles[li->help - 1];
            li->help_text = texts[li->help - 1];
            li->help_card = lm_rect_make(li->card.x + 50, li->card.y + 86, li->card.w - 100, 210);
            li->help_close = lm_rect_make(li->help_card.x + (li->help_card.w - 90) / 2, li->help_card.y + li->help_card.h - 42, 90, 28);
        }
        int kgap = 5, ksz = LM_MIN(LC_KIND, (li->card.w - 2 * LC_PAD - (LM_LAUNCHER_KIND_N - 1) * kgap) / LM_LAUNCHER_KIND_N);       /* all the icon choices fit the card */
        int total = LM_LAUNCHER_KIND_N * ksz + (LM_LAUNCHER_KIND_N - 1) * kgap;
        int kx = li->card.x + (li->card.w - total) / 2, ky = li->card.y + 286;
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            li->kind_box[i] = lm_rect_make(kx + i * (ksz + kgap), ky, ksz, ksz);
    }
}

void launcher_damage(struct lm_wm *wm)
{
    struct lm_output *o = launcher_output(wm);
    if (o) {
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

void launcher_set(struct lm_wm *wm, bool open, struct lm_output *o)
{
    if (open)
        menu_set(wm, false);
    if (open == wm->launcher_open)
        return;
    if (open) {
        if (!o)
            o = lm_output_primary(wm->srv);
        if (!o)
            return;
        launcher_filter[0] = 0;
        launcher_searching = false;
        lm_search_begin(wm->srv->loop);                         /* the programs are read and the file index is started (or brought up to date) */
        about_set(wm, false, NULL);
        power_set(wm, false, NULL);
        sw_close(wm);
        snprintf(wm->launcher_out, sizeof(wm->launcher_out), "%s", o->name);
        wm->launcher_open = true;
        wm->launcher_edit = wm->launcher_form = false;
        wm->launcher_sel = -1;
        wm->launcher_hover_btn = -1;
        wm->launcher_msg[0] = 0;
        launcher_damage(wm);
        if (wm->bubble.active && wm->bubble.closing)
            wm->bubble.closing = false;                          /* it was on its way back to the button: turns round */
        else
            bubble_begin(wm, true);
    } else {
        if (wm->bubble.active && !wm->bubble.closing) {
            wm->bubble.closing = true;                           /* still rising: it goes back from where it is */
            wm->bubble.last = lm_now_ms();
        } else {
            bubble_begin(wm, false);                             /* (the picture of the card as it is now) */
        }
        launcher_damage(wm);
        wm->launcher_open = false;
        wm->launcher_edit = wm->launcher_form = false;
    }
}

void lm_wm_launcher_set(struct lm_wm *wm, bool open) { launcher_set(wm, open, NULL); }
bool lm_wm_launcher_is_open(const struct lm_wm *wm) { return wm->launcher_open; }

size_t lm_wm_launcher_describe(struct lm_wm *wm, char *buf, size_t size)
{
    struct lm_output *o = launcher_output(wm);
    size_t n = 0;
    struct lm_launcher_info li;
    if (o)
        launcher_info(wm, o, &li);
    n += (size_t)snprintf(buf, size, "launcher open=%d edit=%d form=%d n=%d sel=%d drag=%d droppos=%d editing=%d searching=%d search=\"%s\"\n", o != NULL, o ? li.edit : 0,
                          o ? li.form : 0, o ? li.n : 0, o ? li.sel : -1, o ? li.drag : -1, o ? li.drop_pos : -1, o ? li.editing : 0,
                          o ? li.searching : 0, o ? li.search : "");
    if (!o)
        return n;
#define LCR(r) (r).x, (r).y, (r).w, (r).h
    n += (size_t)snprintf(buf + n, size - n, "card x=%d y=%d w=%d h=%d\n", LCR(li.card));
    if (li.drag >= 0 && li.drop.w > 0)
        n += (size_t)snprintf(buf + n, size - n, "drop x=%d y=%d w=%d h=%d\n", LCR(li.drop));
    if (!li.form) {
        for (int g = 0; g < li.n && n < size; g++) {
            n += (size_t)snprintf(buf + n, size - n, "icon %d \"%s\" x=%d y=%d w=%d h=%d", g, li.icon[g].item->name, LCR(li.icon[g].box));
            if (li.edit)
                n += (size_t)snprintf(buf + n, size - n, " remove=%d,%d,%d,%d", LCR(li.icon[g].remove));
            n += (size_t)snprintf(buf + n, size - n, "\n");
        }
        if (li.add.w > 0)
            n += (size_t)snprintf(buf + n, size - n, "add x=%d y=%d w=%d h=%d\n", LCR(li.add));
    } else {
        for (int i = 0; i < 3; i++)
            n += (size_t)snprintf(buf + n, size - n, "field %d x=%d y=%d w=%d h=%d text=\"%s\"\n", i, LCR(li.field[i]), li.field_text[i]);
        for (int i = 0; i < 3; i++)
            n += (size_t)snprintf(buf + n, size - n, "caret %d %d\nhelp %d x=%d y=%d w=%d h=%d\n", i, li.caret[i], i, LCR(li.help_btn[i]));
        n += (size_t)snprintf(buf + n, size - n, "helpcard open=%d x=%d y=%d w=%d h=%d close=%d,%d,%d,%d title=\"%s\"\n", li.help, LCR(li.help_card), LCR(li.help_close),
                              li.help_title ? li.help_title : "");
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            n += (size_t)snprintf(buf + n, size - n, "kind %d x=%d y=%d w=%d h=%d\n", i - 1, LCR(li.kind_box[i]));
        n += (size_t)snprintf(buf + n, size - n, "focus %d kind %d message \"%s\"\n", li.focus, li.kind, li.message);
    }
    for (int i = 0; i < 2 && n < size; i++)
        n += (size_t)snprintf(buf + n, size - n, "button %d x=%d y=%d w=%d h=%d \"%s\"\n", i, LCR(li.button[i]), li.button_text[i]);
#undef LCR
    return n < size ? n : size - 1;
}

/* Where the pointer is in the dialog. */
bool launcher_pick(const struct lm_wm *wm, int ix, int iy, struct lm_pick *out)
{
    struct lm_output *lo = launcher_output(wm);
    if (!lo || lm_lock_active(wm->srv->lock))
        return false;
    struct lm_launcher_info li;
    launcher_info(wm, lo, &li);
    out->rect = li.card;
    if (li.form && li.help) {                                       /* a help card is open: its Close button, a click elsewhere also closes it */
        out->kind = lm_rect_contains(&li.help_card, ix, iy) && !lm_rect_contains(&li.help_close, ix, iy) ? LM_PICK_LAUNCHER_CARD : LM_PICK_LAUNCHER_HELPCLOSE;
        return true;
    }
    out->kind = lm_rect_contains(&li.card, ix, iy) ? LM_PICK_LAUNCHER_CARD : LM_PICK_LAUNCHER_BACKDROP;
    if (out->kind == LM_PICK_LAUNCHER_BACKDROP)
        return true;
    for (int i = 0; i < 2; i++)
        if (lm_rect_contains(&li.button[i], ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_BUTTON;
            out->index = i;
            return true;
        }
    if (li.form) {
        for (int i = 0; i < 3; i++)
            if (lm_rect_contains(&li.help_btn[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_HELP;
                out->index = i;
                return true;
            }
        for (int i = 0; i < 3; i++)
            if (lm_rect_contains(&li.field[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_FIELD;
                out->index = i;
                return true;
            }
        for (int i = 0; i < LM_LAUNCHER_KIND_N; i++)
            if (lm_rect_contains(&li.kind_box[i], ix, iy)) {
                out->kind = LM_PICK_LAUNCHER_KIND;
                out->index = i - 1;
                return true;
            }
        return true;
    }
    for (int g = 0; g < li.n; g++) {
        if (li.edit && lm_rect_contains(&li.icon[g].remove, ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_REMOVE;
            out->index = g;
            return true;
        }
    }
    for (int g = 0; g < li.n; g++)
        if (lm_rect_contains(&li.icon[g].hit, ix, iy)) {
            out->kind = LM_PICK_LAUNCHER_ITEM;
            out->index = g;
            return true;
        }
    if (li.add.w > 0 && lm_rect_contains(&li.add, ix, iy))
        out->kind = LM_PICK_LAUNCHER_ADD;
    return true;
}

static void launcher_open_form(struct lm_wm *wm)
{
    if (!launcher_can_add())
        return;
    launcher_filter[0] = 0;                                         /* (the "+" tile and the counts want the whole list) */
    launcher_searching = false;
    wm->launcher_form = true;
    wm->launcher_editing = false;
    wm->launcher_name[0] = wm->launcher_cmd[0] = wm->launcher_msg[0] = 0;
    wm->launcher_icon[0] = 0;
    wm->launcher_focus = 0;
    wm->launcher_caret[0] = wm->launcher_caret[1] = wm->launcher_caret[2] = -1;
    wm->launcher_help = 0;
    wm->launcher_hover_btn = -1;
    launcher_damage(wm);
}

/* The form again, filled in with the button g (edit mode: a click on its icon): name, command and icon as the file has them. */
void launcher_open_change(struct lm_wm *wm, int g)
{
    const struct lm_dock_item *it = launcher_item(g, NULL);
    const char *nm, *ic, *cm;
    if (!it || !(it->launcher_only ? lm_launchapps_raw_entry(it->raw, &nm, &ic, &cm) : lm_dock_raw_entry(it->raw, &nm, &ic, &cm)))
        return;
    wm->launcher_form = true;
    wm->launcher_editing = true;
    wm->launcher_edit_raw = it->raw;
    wm->launcher_edit_only = it->launcher_only;
    snprintf(wm->launcher_name, sizeof(wm->launcher_name), "%s", nm);
    snprintf(wm->launcher_cmd, sizeof(wm->launcher_cmd), "%s", cm);
    wm->launcher_msg[0] = 0;
    snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", ic);
    wm->launcher_focus = 0;
    wm->launcher_caret[0] = wm->launcher_caret[1] = wm->launcher_caret[2] = -1;
    wm->launcher_help = 0;
    wm->launcher_hover_btn = -1;
    launcher_damage(wm);
}

static void launcher_remove(struct lm_wm *wm, int g)
{
    const struct lm_dock_item *it = launcher_item(g, NULL);
    if (!it || lm_search_owns(it))
        return;
    if (!(it->launcher_only ? lm_settings_launcher_del(wm->srv, it->raw) : lm_settings_dock_del(wm->srv, it->raw)))
        lm_wm_notify(wm, "Launcher", "Could not remove it: there is no config file to write", 5000);
    wm->launcher_sel = LM_MIN(wm->launcher_sel, launcher_count() - 1);
    if (launcher_count() == 0)
        wm->launcher_edit = false;
    launcher_damage(wm);
}

static bool launcher_replace(struct lm_wm *wm, const char *name, const char *icon, const char *cmd)
{
    return wm->launcher_edit_only ? lm_settings_launcher_replace(wm->srv, wm->launcher_edit_raw, name, icon, cmd)
                                  : lm_settings_dock_replace(wm->srv, wm->launcher_edit_raw, name, icon, cmd);
}

static void launcher_submit(struct lm_wm *wm)
{
    char name[LM_DOCK_NAME_MAX], cmd[LM_DOCK_CMD_MAX];
    snprintf(name, sizeof(name), "%s", wm->launcher_name);
    snprintf(cmd, sizeof(cmd), "%s", wm->launcher_cmd);
    for (char *t = name; *t && (*t == ' ');)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(name); l > 0 && name[l - 1] == ' '; l--)
        name[l - 1] = 0;
    for (char *t = cmd; *t && (*t == ' ');)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(cmd); l > 0 && cmd[l - 1] == ' '; l--)
        cmd[l - 1] = 0;
    if (!name[0] || !cmd[0]) {
        snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Give it a name and a command.");
        launcher_damage(wm);
        return;
    }
    char icon[LM_DOCK_ICON_MAX];                                     /* "[theme:firefox]" as the file shows it: the brackets are not part of it */
    snprintf(icon, sizeof(icon), "%s", wm->launcher_icon);
    for (char *t = icon; *t == ' ';)
        memmove(t, t + 1, strlen(t));
    for (size_t l = strlen(icon); l > 0 && icon[l - 1] == ' '; l--)
        icon[l - 1] = 0;
    size_t il = strlen(icon);
    if (il >= 2 && icon[0] == '[' && icon[il - 1] == ']') {
        memmove(icon, icon + 1, il - 2);
        icon[il - 2] = 0;
    }
    int before = launcher_count();
    if (wm->launcher_editing) {                                      /* change the button in place; its own name does not count as taken */
        bool ok = launcher_replace(wm, name, icon, cmd);
        if (!ok) {
            snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Not saved: the name is in use or not allowed, or there is no config file.");
            launcher_damage(wm);
            return;
        }
        wm->launcher_form = false;
        wm->launcher_editing = false;
        wm->launcher_msg[0] = 0;
        wm->launcher_sel = LM_CLAMP(wm->launcher_sel, 0, LM_MAX(launcher_count() - 1, 0));
        if (launcher_count() < before)
            lm_wm_notify(wm, "Launcher", "Saved, but that program was not found, so it is left out for now", 6000);
        launcher_damage(wm);
        return;
    }
    if (launcher_name_taken(name) || !lm_settings_launcher_put(wm->srv, name, icon, cmd)) {
        snprintf(wm->launcher_msg, sizeof(wm->launcher_msg), "Not added: the name is in use or not allowed, the list is full, or there is no config file.");
        launcher_damage(wm);
        return;
    }
    wm->launcher_form = false;
    wm->launcher_msg[0] = 0;
    wm->launcher_sel = LM_MAX(launcher_count() - 1, 0);
    if (launcher_count() <= before)
        lm_wm_notify(wm, "Launcher", "Saved, but that program was not found, so it is left out for now", 6000);
    launcher_damage(wm);
}

/* A click (press and release on the same thing) or Enter on one of the dialog's targets. */
void launcher_activate(struct lm_wm *wm, int kind, int index)
{
    switch (kind) {
    case LM_PICK_LAUNCHER_ITEM: {
        int di;
        const struct lm_dock_item *it = launcher_item(index, &di);
        if (!it)
            break;
        if (lm_search_owns(it)) {                                     /* a result of the search */
            if (wm->launcher_edit)
                break;
            char cmd[LM_DOCK_CMD_MAX];
            snprintf(cmd, sizeof(cmd), "%s", it->cmd);
            launcher_set(wm, false, NULL);
            if (!strncmp(cmd, "@copy:", 6)) {                             /* the calculator's answer goes to the clipboard */
                if (wm->srv->seat) {
                    lm_seat_clip_add(wm->srv->seat, cmd + 6);
                    lm_seat_clip_use(wm->srv->seat, 0);
                }
                lm_wm_notify(wm, "Calculator", cmd + 6, 2500);
            } else {
                lm_server_spawn(wm->srv, cmd);
            }
            break;
        }
        if (wm->launcher_edit) {                                      /* edit mode: change it instead of starting it */
            launcher_open_change(wm, index);
            break;
        }
        launcher_set(wm, false, NULL);
        dock_launch(wm, it, di, true);
        break;
    }
    case LM_PICK_LAUNCHER_REMOVE: launcher_remove(wm, index); break;
    case LM_PICK_LAUNCHER_ADD: launcher_open_form(wm); break;
    case LM_PICK_LAUNCHER_FIELD: {                                    /* the caret goes where the pointer is */
        wm->launcher_focus = index;
        struct lm_output *lo = launcher_output(wm);
        double px = 0, py = 0;
        if (lo && wm->srv->seat) {
            lm_seat_pointer_position(wm->srv->seat, &px, &py);
            struct lm_launcher_info li;
            launcher_info(wm, lo, &li);
            const char *text = li.field_text[index];
            int avail = li.field[index].w - 20 - LM_LAUNCHER_HELP_D - 8, st, en;
            lm_launcher_field_view(text, li.caret[index], avail, &st, &en);
            int tx = li.field[index].x + 10, best = st, bestd = 1 << 30;
            for (int b = st; b <= en;) {
                int d = abs(tx + text_width_to(text, st, b) - (int)px);
                if (d < bestd) {
                    bestd = d;
                    best = b;
                }
                if (b >= en)
                    break;
                b = utf8_next(text, b, (int)strlen(text));
            }
            wm->launcher_caret[index] = best;
        }
        launcher_damage(wm);
        break;
    }
    case LM_PICK_LAUNCHER_HELP:
        wm->launcher_help = index + 1;
        wm->launcher_focus = index;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_HELPCLOSE:
        wm->launcher_help = 0;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_KIND:                                       /* a built-in icon: its name goes into the icon text, "auto" empties it */
        snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", index >= 0 ? lm_dockicon_name(index) : "");
        wm->launcher_caret[2] = -1;
        wm->launcher_focus = 3;
        launcher_damage(wm);
        break;
    case LM_PICK_LAUNCHER_BUTTON:
        if (wm->launcher_form) {
            if (index == 0)
                launcher_submit(wm);
            else {
                wm->launcher_form = false;
                launcher_damage(wm);
            }
        } else if (index == 0) {
            wm->launcher_edit = !wm->launcher_edit;
            launcher_damage(wm);
        } else {
            launcher_set(wm, false, NULL);
        }
        break;
    }
}

/* The text of the form field that has the focus (name, command, icon) and its size; NULL while the icon row has it. */
char *launcher_field_text(struct lm_wm *wm, size_t *max)
{
    switch (wm->launcher_focus) {
    case 0: *max = sizeof(wm->launcher_name); return wm->launcher_name;
    case 1: *max = sizeof(wm->launcher_cmd); return wm->launcher_cmd;
    case 2: *max = sizeof(wm->launcher_icon); return wm->launcher_icon;
    default: return NULL;
    }
}

/* Clipboard actions on the focused field of the form: 0 cut, 1 copy (both take the whole field), 2 paste (the newest copied text as one line, on
 * the end), 3 clear. The compositor keeps the clipboard history itself, so it can read the text without asking a program. */
void launcher_field_clip(struct lm_wm *wm, int what)
{
    size_t max;
    char *text = launcher_field_text(wm, &max);
    if (!text || !wm->srv->seat)
        return;
    if (what == 0 || what == 1) {
        if (text[0]) {
            lm_seat_clip_add(wm->srv->seat, text);
            lm_seat_clip_use(wm->srv->seat, 0);
        }
        if (what == 0) {
            text[0] = 0;
            wm->launcher_caret[wm->launcher_focus] = 0;
        }
    } else if (what == 2) {                                                  /* paste: at the caret, as one line */
        const char *c = lm_seat_clip_count(wm->srv->seat) > 0 ? lm_seat_clip_get(wm->srv->seat, 0) : NULL;
        int caret = lf_caret(wm, wm->launcher_focus, text);
        for (; c && *c; c++) {
            unsigned char ch = (unsigned char)*c;
            if (ch == '\n' || ch == '\r' || ch == '\t')
                ch = ' ';
            if (ch < 0x20 || ch == 0x7f)
                continue;
            if ((ch & 0xc0) != 0x80 && ch >= 0xc0) {                         /* a multi-byte character goes in whole or not at all */
                size_t need = ch >= 0xf0 ? 4 : ch >= 0xe0 ? 3 : 2;
                if (strlen(text) + need >= max)
                    break;
            }
            char one = (char)ch;
            if (!lf_insert(text, max, &caret, &one, 1))
                break;
        }
        wm->launcher_caret[wm->launcher_focus] = caret;
    } else {
        text[0] = 0;
        wm->launcher_caret[wm->launcher_focus] = 0;
    }
    wm->launcher_msg[0] = 0;
    launcher_damage(wm);
}

static void launcher_pop(char *text)
{
    size_t l = strlen(text);
    while (l > 0 && ((unsigned char)text[l - 1] & 0xc0) == 0x80)
        l--;
    if (l > 0)
        l--;
    text[l] = 0;
}

void launcher_key(struct lm_wm *wm, uint32_t code, bool repeat, uint32_t mods)
{
    struct lm_output *lo = launcher_output(wm);
    if (!lo)
        return;
    if (wm->bubble.active && !wm->bubble.closing && code != KEY_ESC && !(code == KEY_A && (mods & LM_MOD_SUPER))) {     /* still rising: a key only finishes it */
        bubble_finish(wm);
        return;
    }
    struct lm_launcher_info li;
    launcher_info(wm, lo, &li);
    bool shift = (mods & LM_MOD_SHIFT) != 0;
    if (code == KEY_A && (mods & LM_MOD_SUPER) && !(mods & ~(uint32_t)LM_MOD_SUPER & LM_KEY_MODS_MASK)) {   /* Super+A again closes it */
        if (!repeat)
            launcher_set(wm, false, NULL);
        return;
    }
    if (wm->launcher_form && wm->launcher_help) {                   /* a help card is open: Esc / Enter / Space close it, nothing else is typed */
        if (!repeat && (code == KEY_ESC || code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE)) {
            wm->launcher_help = 0;
            launcher_damage(wm);
        }
        return;
    }
    bool arrow = code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN;
    if (!wm->launcher_form) {
        int cells = li.n + (li.add.w > 0 ? 1 : 0);
        keymap_sync(wm);
        uint32_t sks = lm_keymap_keysym(code, mods);
        char stxt[8];
        int stl = sks ? lm_keysym_to_utf8(sks, stxt) : 0;
        bool splain = !(mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER));
        bool sprint = splain && stl == 1 && (unsigned char)stxt[0] >= 0x20 && (unsigned char)stxt[0] < 0x7f;
        bool changed = false;
        if (launcher_searching) {                                       /* search mode: the letters go into the text */
            size_t l = strlen(launcher_filter);
            if (code == KEY_BACKSPACE) {
                launcher_pop(launcher_filter);
                changed = true;
            } else if (code == KEY_ESC) {
                if (repeat)
                    return;
                if (l)
                    launcher_filter[0] = 0;
                else
                    launcher_searching = false;
                changed = true;
            } else if (sprint && l + 1 < sizeof(launcher_filter)) {
                launcher_filter[l] = stxt[0];
                launcher_filter[l + 1] = 0;
                changed = true;
            }
        } else if (!repeat && ((sprint && code != KEY_E && code != KEY_A && code != KEY_N && code != KEY_SPACE) ||
                               (code == KEY_F && (mods & LM_MOD_CTRL) && !(mods & (LM_MOD_ALT | LM_MOD_SUPER))))) {
            launcher_searching = true;                                  /* typing a letter other than the shortcuts (E A N) starts a search; "/" and Ctrl+F too */
            launcher_filter[0] = 0;
            if (sprint && code != KEY_SLASH) {
                launcher_filter[0] = stxt[0];
                launcher_filter[1] = 0;
            }
            changed = true;
        }
        if (changed) {
            wm->launcher_sel = launcher_count() > 0 ? 0 : -1;
            launcher_damage(wm);
            return;
        }
        if (repeat && !arrow)
            return;
        if (code == KEY_ESC) {
            launcher_set(wm, false, NULL);
        } else if (arrow || code == KEY_HOME || code == KEY_END) {
            if (cells == 0)
                return;
            int cur = li.sel < 0 ? (code == KEY_LEFT || code == KEY_UP || code == KEY_END ? cells - 1 : 0) : li.sel;
            if (li.sel >= 0) {
                int cols = LM_MAX(li.cols, 1);
                cur = code == KEY_LEFT ? cur - 1 : code == KEY_RIGHT ? cur + 1 : code == KEY_UP ? cur - cols :
                      code == KEY_DOWN ? cur + cols : code == KEY_HOME ? 0 : cells - 1;
                if (code == KEY_DOWN && cur >= cells && li.sel < cells - 1)
                    cur = cells - 1;                                    /* the last, shorter row */
            }
            cur = LM_CLAMP(cur, 0, cells - 1);
            if (cur != wm->launcher_sel) {
                wm->launcher_sel = cur;
                launcher_damage(wm);
            }
        } else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) {
            if (li.sel >= 0 && li.sel < li.n)
                launcher_activate(wm, LM_PICK_LAUNCHER_ITEM, li.sel);
            else if (li.sel == li.n && li.add.w > 0)
                launcher_open_form(wm);
        } else if ((code == KEY_DELETE || code == KEY_BACKSPACE) && li.edit && li.sel >= 0 && li.sel < li.n) {
            launcher_remove(wm, li.sel);
        } else if (code == KEY_E && li.n > 0) {
            wm->launcher_edit = !wm->launcher_edit;
            launcher_damage(wm);
        } else if (code == KEY_A || code == KEY_N) {
            launcher_open_form(wm);
        }
        return;
    }
    /* the add form */
    if (code == KEY_ESC) {
        if (!repeat) {
            wm->launcher_form = false;
            launcher_damage(wm);
        }
        return;
    }
    if (code == KEY_TAB || ((code == KEY_UP || code == KEY_DOWN) && !repeat)) {
        if (repeat)
            return;
        bool back = code == KEY_UP || (code == KEY_TAB && shift);
        wm->launcher_focus = (wm->launcher_focus + (back ? 3 : 1)) % 4;
        if (wm->launcher_focus < 3)
            wm->launcher_caret[wm->launcher_focus] = -1;                /* (a field entered with the keyboard has the caret at its end) */
        launcher_damage(wm);
        return;
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        if (repeat)
            return;
        if (wm->launcher_focus == 0) {
            wm->launcher_focus = 1;
            wm->launcher_caret[1] = -1;
            launcher_damage(wm);
        } else {
            launcher_submit(wm);
        }
        return;
    }
    if (!(mods & (LM_MOD_ALT | LM_MOD_SUPER))) {                    /* the clipboard keys of a text field */
        bool ctl = (mods & LM_MOD_CTRL) != 0;
        if ((ctl && code == KEY_V) || (shift && !ctl && code == KEY_INSERT)) {
            launcher_field_clip(wm, 2);
            return;
        }
        if ((ctl && code == KEY_C) || (ctl && !shift && code == KEY_INSERT)) {
            launcher_field_clip(wm, 1);
            return;
        }
        if ((ctl && !shift && code == KEY_X) || (shift && !ctl && code == KEY_DELETE)) {
            launcher_field_clip(wm, 0);
            return;
        }
    }
    if (wm->launcher_focus == 3) {
        if (code == KEY_LEFT || code == KEY_RIGHT) {
            int k = LM_MAX(launcher_kind_of(wm), -1) + (code == KEY_LEFT ? -1 : 1);
            k = LM_CLAMP(k, -1, LM_DOCKICON_COUNT - 1);
            snprintf(wm->launcher_icon, sizeof(wm->launcher_icon), "%s", k >= 0 ? lm_dockicon_name(k) : "");
            wm->launcher_caret[2] = -1;
            launcher_damage(wm);
        }
        return;
    }
    size_t max = 0;
    char *text = launcher_field_text(wm, &max);
    if (!text)
        return;
    int f = wm->launcher_focus, len = (int)strlen(text), caret = lf_caret(wm, f, text);
    bool ctrl = (mods & LM_MOD_CTRL) != 0;
    if (code == KEY_LEFT) {                                          /* the caret: a character, with Ctrl a word */
        caret = ctrl ? lf_word_prev(text, caret) : utf8_prev(text, caret);
    } else if (code == KEY_RIGHT) {
        caret = ctrl ? lf_word_next(text, caret, len) : utf8_next(text, caret, len);
    } else if (code == KEY_HOME) {
        caret = 0;
    } else if (code == KEY_END) {
        caret = len;
    } else if (code == KEY_BACKSPACE) {                              /* before the caret */
        int from = ctrl ? lf_word_prev(text, caret) : utf8_prev(text, caret);
        lf_erase(text, from, caret);
        caret = from;
    } else if (code == KEY_DELETE) {                                 /* at the caret (Shift+Delete is Cut, handled above) */
        lf_erase(text, caret, ctrl ? lf_word_next(text, caret, len) : utf8_next(text, caret, len));
    } else if (!(mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
        keymap_sync(wm);
        if (code >= KEY_KP7 && code <= KEY_KPDOT)
            mods |= LM_MOD_NUM;
        uint32_t ks = lm_keymap_keysym(code, mods);
        char txt[8];
        int k = ks ? lm_keysym_to_utf8(ks, txt) : 0;
        if (k <= 0 || (unsigned char)txt[0] < 0x20 || !lf_insert(text, max, &caret, txt, (size_t)k))
            return;
    } else {
        return;
    }
    wm->launcher_caret[f] = caret;
    wm->launcher_msg[0] = 0;
    launcher_damage(wm);
}

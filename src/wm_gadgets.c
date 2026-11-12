/* Part of the window manager (see wm_priv.h): the gadgets' places and sizes, kept between runs */
#include "wm_priv.h"
#include "smarthome.h"

/* ---- the gadgets' places and sizes, kept between runs ------------------------------------------------------------------ */

/* Default / minimum / maximum card size per gadget. */
struct lm_gadget_def gadget_def[LM_GADGET_COUNT] = {
    [LM_GADGET_CLIPBOARD] = { 0, 0, GADGET_MIN_W, GADGET_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_MIXER] = { 0, 0, MIXER_MIN_W, MIXER_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_SYSTEM] = { 0, 0, SYS_MIN_W, SYS_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
    [LM_GADGET_SMART] = { 0, 0, SMART_MIN_W, SMART_MIN_H, GADGET_MAX_W, GADGET_MAX_H },
};

/* The starting sizes come from [gadgets] in mellagui.conf (metrics.h), so they are filled in when the wm is created. */
void gadget_def_init(void)
{
    gadget_def[LM_GADGET_CLIPBOARD].w = GADGET_W;
    gadget_def[LM_GADGET_CLIPBOARD].h = GADGET_H;
    gadget_def[LM_GADGET_MIXER].w = LM_MAX(MIXER_W, MIXER_MIN_W);
    gadget_def[LM_GADGET_MIXER].h = MIXER_H;
    gadget_def[LM_GADGET_SYSTEM].w = SYS_W;
    gadget_def[LM_GADGET_SYSTEM].h = SYS_H;
    gadget_def[LM_GADGET_SMART].w = SMART_W;
    gadget_def[LM_GADGET_SMART].h = SMART_H;
}

static const char *const gadget_names[LM_GADGET_COUNT] = { "clipboard", "mixer", "system", "smart" };

/* One line per gadget that was moved or resized: "name x y w h". A gadget at its default place is not listed. */
void gadgets_save(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    if (!path || !*path)
        return;
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
        return;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(dir, 0755) < 0 && errno != EEXIST)
            return;
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lm_warn("gadgets: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < LM_GADGET_COUNT; i++)
        if (wm->gad[i].placed)
            fprintf(f, "%s %d %d %d %d\n", gadget_names[i], wm->gad[i].x, wm->gad[i].y, wm->gad[i].w, wm->gad[i].h);
    if (wm->osk_pl.w > 0)                                           /* the on-screen keyboard: top left corner, width and key height */
        fprintf(f, "keyboard %d %d %d %d\n", wm->osk_pl.x, wm->osk_pl.y, wm->osk_pl.w, wm->osk_pl.kh);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("gadgets: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

void gadgets_load(struct lm_wm *wm)
{
    const char *path = wm->srv->config.gadgets_path;
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f)
        return;
    char line[160], name[24];
    while (fgets(line, sizeof(line), f)) {
        int x, y, w, h;
        if (sscanf(line, "%23s %d %d %d %d", name, &x, &y, &w, &h) != 5)
            continue;
        if (!strcmp(name, "keyboard")) {                            /* (clamped to the output and to the size limits when it is laid out) */
            if (abs(x) <= 20000 && abs(y) <= 20000 && w > 0 && w <= 20000 && h > 0 && h <= 1000) {
                wm->osk_pl.x = x;
                wm->osk_pl.y = y;
                wm->osk_pl.w = w;
                wm->osk_pl.kh = h;
            }
            continue;
        }
        for (int i = 0; i < LM_GADGET_COUNT; i++) {
            if (strcmp(name, gadget_names[i]))
                continue;
            if (abs(x) > 20000 || abs(y) > 20000)
                break;
            wm->gad[i].x = x;                                       /* (the places are clamped to the output when drawn) */
            wm->gad[i].y = y;
            wm->gad[i].w = LM_CLAMP(w, gadget_def[i].min_w, gadget_def[i].max_w);
            wm->gad[i].h = LM_CLAMP(h, gadget_def[i].min_h, gadget_def[i].max_h);
            wm->gad[i].placed = true;
            break;
        }
    }
    fclose(f);
}

/* Largest size gadget `id` may have on `o` (it stays inside the output, below the panel). */
void gadget_max_size(int id, const struct lm_output *o, int *mw, int *mh)
{
    *mw = LM_MIN(gadget_def[id].max_w, o->width - 2 * GADGET_MARGIN);
    *mh = LM_MIN(gadget_def[id].max_h, o->height - LM_PANEL_HEIGHT - 2 * GADGET_MARGIN);
}

/* Where gadget `id` sits: the first output, docked top right below the panel until it is dragged (the mixer docks below
 * the clipboard card while that one is docked too). False when hidden or it does not fit. */
bool lm_wm_gadget_rect_id(const struct lm_wm *wm, int id, struct lm_rect *out)
{
    struct lm_output *o = gadget_output(wm);
    if (id < 0 || id >= LM_GADGET_COUNT || !wm->gad[id].visible || !o)
        return false;
    int mw, mh;
    gadget_max_size(id, o, &mw, &mh);
    if (mw < gadget_def[id].min_w || mh < gadget_def[id].min_h || fullscreen_covers(wm, o))
        return false;
    int w = LM_CLAMP(wm->gad[id].w, gadget_def[id].min_w, mw), h = LM_CLAMP(wm->gad[id].h, gadget_def[id].min_h, mh);
    int x, y;
    if (wm->gad[id].placed) {
        x = LM_CLAMP(wm->gad[id].x, o->x, o->x + o->width - w);
        y = LM_CLAMP(wm->gad[id].y, o->y + LM_PANEL_HEIGHT, o->y + o->height - h);
    } else {
        x = o->x + o->width - w - GADGET_MARGIN;
        y = o->y + LM_PANEL_HEIGHT + GADGET_MARGIN;
        /* docked gadgets stack downwards from the top right in id order; when the next one does not fit below the
         * previous one, it starts a new column to its left */
        for (int j = id - 1; j >= 0; j--) {
            struct lm_rect pr;
            if (wm->gad[j].placed || !lm_wm_gadget_rect_id(wm, j, &pr))
                continue;
            if (pr.y + pr.h + GADGET_MARGIN + h <= o->y + o->height - GADGET_MARGIN) {
                y = pr.y + pr.h + GADGET_MARGIN;                        /* below the previous card */
            } else if (pr.x - GADGET_MARGIN - w >= o->x) {
                x = pr.x - GADGET_MARGIN - w;                           /* a small screen: beside it, at the top */
                y = o->y + LM_PANEL_HEIGHT + GADGET_MARGIN;
            } else {                                                    /* no room at all: cascade, titles stay visible */
                x = LM_MAX(o->x, pr.x - 28);
                y = LM_MIN(pr.y + 28, o->y + o->height - h);
            }
            break;
        }
    }
    *out = lm_rect_make(x, y, w, h);
    return true;
}

/* The card plus its drop shadow (what has to be repainted). */
struct lm_rect gadget_bounds(const struct lm_rect *r)
{
    return lm_rect_make(r->x - LM_GADGET_SHADOW, r->y - LM_GADGET_SHADOW, r->w + 2 * LM_GADGET_SHADOW,
                        r->h + 2 * LM_GADGET_SHADOW);
}

/* Entry slots of a card of height `h`: as many as fit; the entries have one fixed height, the card scrolls through a longer history. */
void gadget_slots(int h, int *slots, int *entry_h)
{
    int body = h - GADGET_TITLE_H - GADGET_PAD;
    *slots = LM_CLAMP(body / GADGET_ENTRY_H, 1, LM_GADGET_ENTRIES);
    *entry_h = GADGET_ENTRY_H;
}

/* The title bar buttons of a clipboard card: [0] up, [1] down, [2] clear all. */
void clip_buttons(const struct lm_rect *card, struct lm_rect b[3])
{
    int y = card->y + (GADGET_TITLE_H - LM_CLIP_BTN) / 2;
    int x = card->x + card->w - 14 - LM_CLIP_BTN;
    for (int i = 2; i >= 0; i--, x -= LM_CLIP_BTN + LM_CLIP_BTN_GAP)
        b[i] = lm_rect_make(x, y, LM_CLIP_BTN, LM_CLIP_BTN);
}

void damage_gadget(struct lm_wm *wm, int id)
{
    struct lm_rect r;
    if (lm_wm_gadget_rect_id(wm, id, &r)) {
        struct lm_rect b = gadget_bounds(&r);
        damage_rect(wm, &b);
    }
}

/* A docked mixer sits below the clipboard card: when that one appears, disappears or changes size, repaint both. */
void damage_gadgets(struct lm_wm *wm)
{
    for (int i = 0; i < LM_GADGET_COUNT; i++)
        damage_gadget(wm, i);
}

/* How many entries fit in the card now (0 when it is hidden) and the history's length. */
int clip_slots(struct lm_wm *wm)
{
    struct lm_rect gr;
    if (!lm_wm_gadget_rect_id(wm, LM_GADGET_CLIPBOARD, &gr))
        return 0;
    int slots, entry_h;
    gadget_slots(gr.h, &slots, &entry_h);
    return slots;
}

/* The first entry the card shows: clip_top kept inside the history. */
int clip_top(struct lm_wm *wm)
{
    int slots = clip_slots(wm), total = lm_seat_clip_count(wm->srv->seat);
    return LM_CLAMP(wm->clip_top, 0, LM_MAX(total - LM_MAX(slots, 1), 0));
}

/* Entries the keyboard can reach: the whole history (the card follows the cursor). */
int clip_nav_count(struct lm_wm *wm)
{
    return clip_slots(wm) > 0 ? lm_seat_clip_count(wm->srv->seat) : 0;
}

/* Scroll so that entry `i` is drawn. */
void clip_show(struct lm_wm *wm, int i)
{
    int slots = LM_MAX(clip_slots(wm), 1), top = clip_top(wm);
    if (i < top)
        top = i;
    else if (i >= top + slots)
        top = i - slots + 1;
    wm->clip_top = top;
}

void clip_nav_set(struct lm_wm *wm, bool on)
{
    if (wm->clip_nav == on)
        return;
    wm->clip_nav = on;
    wm->clip_cur = on ? clip_top(wm) : 0;
    damage_gadget(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_clipboard_changed(struct lm_wm *wm)
{
    int total = lm_seat_clip_count(wm->srv->seat);
    if (total > wm->clip_total)                                 /* something new was copied: the card shows the newest again */
        wm->clip_top = 0;
    wm->clip_total = total;
    wm->clip_top = clip_top(wm);
    if (wm->clip_nav) {                                         /* the list moved under the cursor: keep it inside */
        if (total == 0 || clip_slots(wm) == 0) {
            wm->clip_nav = false;
        } else {
            wm->clip_cur = LM_CLAMP(wm->clip_cur, 0, total - 1);
            clip_show(wm, wm->clip_cur);
        }
    }
    damage_gadget(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_mixer_changed(struct lm_wm *wm)
{
    damage_gadget(wm, LM_GADGET_MIXER);
}

void lm_wm_sysinfo_changed(struct lm_wm *wm)
{
    damage_gadget(wm, LM_GADGET_SYSTEM);
}

void lm_wm_smart_changed(struct lm_wm *wm)
{
    damage_gadget(wm, LM_GADGET_SMART);
}

bool lm_wm_gadget_visible_id(const struct lm_wm *wm, int id)
{
    return id >= 0 && id < LM_GADGET_COUNT && wm->gad[id].visible;
}

bool lm_wm_gadget_visible(const struct lm_wm *wm)
{
    return lm_wm_gadget_visible_id(wm, LM_GADGET_CLIPBOARD);
}

void lm_wm_set_gadget_id(struct lm_wm *wm, int id, bool visible)
{
    if (id < 0 || id >= LM_GADGET_COUNT || wm->gad[id].visible == visible)
        return;
    damage_gadgets(wm);                 /* (still shown, if it was) */
    wm->gad[id].visible = visible;
    damage_gadgets(wm);
    if (id == LM_GADGET_MIXER && wm->srv->mixer)
        lm_mixer_set_active(wm->srv->mixer, visible);
    if (id == LM_GADGET_SYSTEM && wm->srv->sysinfo)
        lm_sysinfo_set_active(wm->srv->sysinfo, visible);
    if (id == LM_GADGET_SMART && wm->srv->smart)
        lm_smart_set_active(wm->srv->smart, visible);
    lm_settings_changed(wm->srv->settings);     /* the panel's checkbox follows Super+G / Super+V */
}

void lm_wm_set_gadget(struct lm_wm *wm, bool visible)
{
    lm_wm_set_gadget_id(wm, LM_GADGET_CLIPBOARD, visible);
}


void damage_panels(struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link) {
        struct lm_rect r = output_panel_rect(o);
        r.h += LM_PANEL_SHADOW;                         /* (the shadow below it) */
        damage_rect(wm, &r);
    }
    dock_damage(wm);                                    /* (windows come and go here: the dock's "running" dots) */
}

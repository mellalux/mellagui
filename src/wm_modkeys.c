/*
 * wm_modkeys.c - sticky keys on the screen: every modifier that waits (latched for the next key) or is locked shows as a see-through key
 * of its own, in a row near the top of the primary output, in the middle from left to right and a tenth of its height down (out of the
 * on-screen keyboard's way when that was moved there). They are only shown: the pointer goes through them.
 */
#include <stdio.h>
#include <string.h>

#include "wm_priv.h"

#define MODKEY_W 128                /* narrower only when the output cannot hold all of them */
#define MODKEY_H 88
#define MODKEY_GAP 14
#define MODKEY_EDGE 16              /* kept free at the sides of the output */
#define MODKEY_BELOW_PANEL 24       /* at least this far under the panel */

bool modkeys_info(const struct lm_wm *wm, struct lm_modkeys_info *mi)
{
    static const struct { uint32_t bit; const char *label; } keys[LM_MODKEYS_MAX] = {
        { LM_MOD_CTRL, "CTRL" }, { LM_MOD_SHIFT, "SHIFT" }, { LM_MOD_ALT, "ALT" }, { LM_MOD_ALTGR, "ALTGR" }, { LM_MOD_SUPER, "SUPER" },
    };
    memset(mi, 0, sizeof(*mi));
    struct lm_input *in = wm->srv->input;
    if (!in || !lm_input_sticky(in) || !wm->srv->backend || lm_lock_active(wm->srv->lock))
        return false;
    struct lm_output *o = lm_output_primary(wm->srv);
    uint32_t latched = lm_input_latched(in), locked = lm_input_sticky_locked(in);
    if (!o || !(latched | locked))
        return false;
    for (int i = 0; i < LM_MODKEYS_MAX; i++) {
        if (!((latched | locked) & keys[i].bit))
            continue;
        snprintf(mi->key[mi->n].label, sizeof(mi->key[0].label), "%s", keys[i].label);
        mi->key[mi->n].locked = (locked & keys[i].bit) != 0;
        mi->n++;
    }
    if (!mi->n)
        return false;
    int kw = LM_MIN(MODKEY_W, (o->width - 2 * MODKEY_EDGE - (LM_MODKEYS_MAX - 1) * MODKEY_GAP) / LM_MODKEYS_MAX);
    int w = mi->n * kw + (mi->n - 1) * MODKEY_GAP;
    int x = o->x + (o->width - w) / 2, y = o->y + LM_MAX(LM_PANEL_HEIGHT + MODKEY_BELOW_PANEL, o->height / 10);
    struct lm_osk_geo kg;
    if (wm->osk_open && osk_geo(wm, &kg)) {                     /* the keyboard was carried up here: under it, or over it when there is no room under it */
        struct lm_rect row = lm_rect_make(x, y, w, MODKEY_H);
        if (lm_rect_overlaps(&row, &kg.card)) {
            y = kg.card.y + kg.card.h + MODKEY_GAP;
            if (y + MODKEY_H > o->y + o->height)
                y = LM_MAX(o->y, kg.card.y - MODKEY_GAP - MODKEY_H);
        }
    }
    for (int i = 0; i < mi->n; i++)
        mi->key[i].rect = lm_rect_make(x + i * (kw + MODKEY_GAP), y, kw, MODKEY_H);
    mi->rect = lm_rect_make(x, y, w, MODKEY_H);
    return true;
}

void modkeys_damage(struct lm_wm *wm)
{
    struct lm_modkeys_info mi;
    bool shown = modkeys_info(wm, &mi);
    if (!lm_rect_is_empty(&wm->modkeys_rect))
        damage_rect(wm, &wm->modkeys_rect);
    wm->modkeys_rect = shown ? mi.rect : (struct lm_rect){ 0 };
    if (shown)
        damage_rect(wm, &wm->modkeys_rect);
}

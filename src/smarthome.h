#ifndef MELLAGUI_SMARTHOME_H
#define MELLAGUI_SMARTHOME_H

/*
 * The data of the desktop gadget "Smart home": the devices the user pinned in mellagui-smart (smart-favourites.conf), their state and the
 * commands the card sends. The compositor does not talk to the cloud: it asks the program. Every LM_SMART_POLL_MS while the gadget is shown
 * it runs `mellagui-smart --favourites` (asynchronously, lm_proc_run; $MELLAGUI_SMART_BIN replaces the program) and reads its lines
 *     favs <TAB> count                       (a program that does not print this line did not answer)
 *     fav <TAB> id <TAB> name <TAB> online <TAB> switch|blind|none <TAB> code <TAB> value
 * and a click runs `mellagui-smart --set ID CODE VALUE` (the card shows the new state at once, the next poll confirms it).
 * Owner: smarthome.c. Single threaded (the server's loop).
 */

#include <stdbool.h>
#include <stddef.h>

struct lm_loop;
struct lm_smart;

#define LM_SMART_MAX 8
#define LM_SMART_POLL_MS 15000

enum lm_smart_kind { LM_SMART_NONE, LM_SMART_SWITCH, LM_SMART_BLIND };

struct lm_smart_item {
    char id[40], name[48], code[32];
    bool online;
    enum lm_smart_kind kind;
    bool on;                            /* a switch */
    int percent;                        /* a blind's position, -1 = unknown */
};

struct lm_smart_state {
    int n;
    struct lm_smart_item item[LM_SMART_MAX];
    bool ready;                         /* a poll has been answered */
    char note[96];                      /* "" or why there is nothing ("not logged in ...", "no device is pinned") */
};

struct lm_smart *lm_smart_create(struct lm_loop *loop, void (*changed)(void *), void *data);
void lm_smart_destroy(struct lm_smart *s);
void lm_smart_set_active(struct lm_smart *s, bool active);
const struct lm_smart_state *lm_smart_state(const struct lm_smart *s);
/* Poll now (the pins changed or the card was opened). */
void lm_smart_refresh(struct lm_smart *s);

/* A switch is turned the other way; a blind gets 0 = open, 1 = stop, 2 = close. */
void lm_smart_toggle(struct lm_smart *s, int index);
void lm_smart_blind(struct lm_smart *s, int index, int what);

/* ---- the card's layout (shared by the painter and the pointer) ----------------------------------------------------------------- */

#define LM_SMART_ROW_H 34

enum lm_smart_part { LM_SMART_NOTHING, LM_SMART_SWITCH_PART, LM_SMART_OPEN, LM_SMART_STOP, LM_SMART_CLOSE };

struct lm_smart_rect { int x, y, w, h; };

struct lm_smart_layout {
    int rows;                           /* rows that fit */
    struct lm_smart_rect name[LM_SMART_MAX];
    struct lm_smart_rect sw[LM_SMART_MAX];          /* a switch's pill */
    struct lm_smart_rect btn[LM_SMART_MAX][3];      /* a blind's open / stop / close */
};

void lm_smart_layout(int x, int y, int w, int h, int title_h, int n, struct lm_smart_layout *l);
enum lm_smart_part lm_smart_hit(const struct lm_smart_layout *l, const struct lm_smart_state *st, int px, int py, int *index);

#endif

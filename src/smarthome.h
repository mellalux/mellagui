#ifndef MELLAGUI_SMARTHOME_H
#define MELLAGUI_SMARTHOME_H

/*
 * The data of the desktop gadget "Smart home": the devices the user pinned in mellagui-smart (smart-favourites.conf), their state and the
 * commands the card sends. The compositor does not talk to the cloud: it asks the program. Every LM_SMART_POLL_MS while the gadget is shown
 * it runs `mellagui-smart --favourites` (asynchronously, lm_proc_run; $MELLAGUI_SMART_BIN replaces the program) and reads its lines
 *     favs <TAB> count                       (a program that does not print this line did not answer)
 *     fav <TAB> id <TAB> name <TAB> online <TAB> switch|blind|vacuum|none <TAB> code <TAB> value [<TAB> battery,dockcode,dockvalue]
 * (a blind's value is its position in per cent, a vacuum's 1 = cleaning; a device with several switches - a power strip - has one `fav` line for each:
 * the same id and name, that switch's code and, as the field after the value, its label "Switch 2" / "USB 1". The card shows such a device as ONE
 * card within the card: its name on the first row and the switches below it side by side (a cell each, as many in a row as fit, the rest on the next line))
 * and a click runs `mellagui-smart --set ID CODE VALUE` (the card shows the new state at once, the next poll confirms it).
 * Owner: smarthome.c. Single threaded (the server's loop).
 */

#include <stdbool.h>
#include <stddef.h>

struct lm_loop;
struct lm_smart;

#define LM_SMART_MAX 24                 /* rows of the card: a device with several switches (a power strip) is one row for each */
#define LM_SMART_POLL_MS 15000

enum lm_smart_kind { LM_SMART_NONE, LM_SMART_SWITCH, LM_SMART_BLIND, LM_SMART_VACUUM };

struct lm_smart_item {
    char id[40], name[48], code[32];
    char label[32];                     /* a switch of a device that has several: "Switch 2" (the name is the device's); "" = the only one */
    bool group_first;                   /* the first row of such a device: the card draws its name above it */
    int group_len;                      /* ... and how many rows belong to it (valid on the first one) */
    bool online;
    enum lm_smart_kind kind;
    bool on;                            /* a switch */
    int percent;                        /* a blind's position, -1 = unknown */
    bool can_pos;                       /* a blind that can be sent to a position (percent_control): the card shows a slider, else open / stop / close buttons */
    /* a robot vacuum: `code` is its start / stop data point, `on` = it is cleaning */
    int battery;                        /* per cent, -1 = unknown */
    char dock_code[32], dock_value[24]; /* what is sent to send it home ("mode" "chargego", "switch_charge" "true"); empty = no dock button */
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
/* A blind with a slider: show `percent` (0..100) at once and, with `send`, tell the motor (percent_control). */
void lm_smart_blind_position(struct lm_smart *s, int index, int percent, bool send);
/* A robot vacuum: 0 = start / stop cleaning (the other way from what it does now), 1 = send it to its dock. */
void lm_smart_vacuum(struct lm_smart *s, int index, int what);

/* ---- the card's layout (shared by the painter and the pointer) ----------------------------------------------------------------- */

#define LM_SMART_ROW_H 34
#define LM_SMART_ROW2_H 58              /* a blind with a slider has two lines: the name, and the slider under it */
#define LM_SMART_HEAD_H 28              /* the name of a device with several switches, above them */
#define LM_SMART_CELL_H 46              /* one of its switches: a cell with its label on top and the pill under it; the cells stand side by side, as many in a row as fit */
#define LM_SMART_CELL_MIN_W 58

enum lm_smart_part { LM_SMART_NOTHING, LM_SMART_SWITCH_PART, LM_SMART_OPEN, LM_SMART_STOP, LM_SMART_CLOSE, LM_SMART_CLEAN, LM_SMART_DOCK, LM_SMART_SLIDER };

struct lm_smart_rect { int x, y, w, h; };

struct lm_smart_layout {
    int rows;                           /* rows that fit */
    struct lm_smart_rect row[LM_SMART_MAX];         /* the whole row (its height depends on the kind) */
    struct lm_smart_rect head[LM_SMART_MAX];        /* the name row above a group of switches (only on its first item; w = 0 elsewhere) */
    struct lm_smart_rect group[LM_SMART_MAX];       /* the card of that group: its name row and all its switches (only on its first item) */
    /* a switch of a group: row[] is its cell (the whole of it is the click area), name[] the label on top, sw[] the pill under it */
    struct lm_smart_rect slider[LM_SMART_MAX];      /* a blind's slider track (with can_pos); the percentage is written right of it */
    struct lm_smart_rect name[LM_SMART_MAX];
    struct lm_smart_rect sw[LM_SMART_MAX];          /* a switch's pill */
    struct lm_smart_rect btn[LM_SMART_MAX][3];      /* a blind's open / stop / close */
    struct lm_smart_rect vac[LM_SMART_MAX][2];      /* a vacuum's start-stop and dock buttons */
};

void lm_smart_layout(int x, int y, int w, int h, int title_h, const struct lm_smart_state *st, struct lm_smart_layout *l);
/* The height of a card that shows every row. */
int lm_smart_height(const struct lm_smart_state *st, int w, int title_h);
/* The percentage a pointer at `px` stands for on row i's slider. */
int lm_smart_slider_value(const struct lm_smart_layout *l, int i, int px);
enum lm_smart_part lm_smart_hit(const struct lm_smart_layout *l, const struct lm_smart_state *st, int px, int py, int *index);

#endif

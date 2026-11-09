#ifndef MELLAGUI_TRAY_H
#define MELLAGUI_TRAY_H

/*
 * The panel's tray: programs put icons and buttons into the top bar through the private `mellagui_tray_v1` global (used by
 * mellagui-record for its blinking dot and the elapsed time).
 *
 *   create_item(new_id)   a button in the bar, left of the keyboard indicators; the compositor answers `size(px)`: the icon edge it
 *                         wants (a bit less than the bar's height)
 *   item.set_icon(buffer) a wl_shm buffer (ARGB8888 straight from the program: premultiplied, or XRGB8888; at most 64x64) is copied
 *                         at once and the buffer released, so one buffer can be reused for a blinking icon; null removes the icon
 *   item.set_text(s)      a label after the icon (at most 31 bytes, control characters become blanks)
 *   item.destroy          the item goes; so does one whose client disconnects
 *   event clicked(button) the button was pressed and let go on the item (272 left, 273 right, 274 middle)
 *
 * Limits: 4 items per client, 16 in all (more are accepted but stay inert: never shown, never clicked). Access is the settings'
 * (lm_settings_client_allowed): on the drm backend only MellaGUI's own programs; any other client gets inert items. Owner: tray.c; the bar's layout, painting
 * and picking are in wm.c / render.c, which read the items through lm_tray_get().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server.h"
#include "shm.h"

#define LM_TRAY_MAX_ITEMS 16
#define LM_TRAY_PER_CLIENT 4
#define LM_TRAY_MAX_ICON 64
#define LM_TRAY_TEXT 32

struct lm_tray;

/* What the bar shows of an item (valid until the next change of the tray). */
struct lm_tray_view {
    int id;                             /* stable, never reused while the process runs */
    const struct lm_texture *icon;      /* premultiplied ARGB; NULL = no icon */
    const char *text;                   /* "" = none */
};

struct lm_tray *lm_tray_create(struct lm_server *srv);
void lm_tray_destroy(struct lm_tray *t);

int lm_tray_count(const struct lm_tray *t);
/* Item i (0 = created first). False when out of range. */
bool lm_tray_get(const struct lm_tray *t, int i, struct lm_tray_view *out);
/* Sends `clicked(button)` to the item with that id. */
void lm_tray_click(struct lm_tray *t, int id, uint32_t button);
/* The edge of the icons the bar asks for. */
int lm_tray_icon_size(void);

#endif

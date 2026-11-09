#ifndef MELLAGUI_WLCLIP_H
#define MELLAGUI_WLCLIP_H

/*
 * Text clipboard for the wlwin programs (the editor, the calculator): our own wl_data_source for copying and a reader for what
 * somebody else offered. Wire it up like this:
 *   - ops.global: call wlclip_global() (it binds wl_data_device_manager and ignores everything else);
 *   - after wlwin_open: wlclip_attach();
 *   - on every keyboard / pointer button event keep `serial` up to date (the compositor wants the serial of the input that
 *     caused the copy);
 *   - wlclip_copy() to offer text, wlclip_paste() to fetch the current selection (our own copy without a round trip).
 */

#include <stddef.h>
#include <stdint.h>

#include "wlwin.h"

struct wlclip {
    struct wlwin *win;
    uint32_t ddm, ddev, dsrc, offer, offer_pending;
    char offer_mimes[16][64];
    int n_offer_mimes;
    char *out;                          /* what we offer */
    uint32_t serial;
};

void wlclip_global(struct wlclip *c, struct wlwin *w, uint32_t name, const char *iface, uint32_t ver);
void wlclip_attach(struct wlclip *c, struct wlwin *w);
void wlclip_copy(struct wlclip *c, const char *text);
/* The text of the selection (malloc'd, `*len` bytes, NUL terminated), or NULL when there is none. At most 1 MB. */
char *wlclip_paste(struct wlclip *c, size_t *len);

#endif

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
 *   - wlclip_drag_files() starts a drag of files (text/uri-list) that other programs - a browser's page, another file manager -
 *     take a drop of; call it while the button is held (a press that moved), after `ptr_serial` got the press' serial.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wlwin.h"

struct wlclip {
    struct wlwin *win;
    uint32_t ddm, ddev, dsrc, offer, offer_pending;
    char offer_mimes[16][64];           /* the types of the selection offer */
    int n_offer_mimes;
    char pend_mimes[16][64];            /* the types of the offer that has just been introduced (a selection, or the offer of a drag over us) */
    int n_pend_mimes;
    char *out;                          /* what we offer */
    uint32_t serial;
    uint32_t ptr_serial;                /* the serial of the last pointer button event (a drag starts from it); 0 = use `serial` */
    /* files dragged over the window (wlclip_accept_drops) */
    void (*drop)(struct wlclip *c, char **paths, int n, double x, double y, void *data);
    void *drop_data;
    uint32_t drop_offer;
    double drop_x, drop_y;
    /* a drag in progress (wlclip_drag_files) */
    uint32_t drag_src, drag_surf, drag_pool, drag_buf;
    char *drag_uris, *drag_text;
    void *drag_map;
    size_t drag_map_size;
};

void wlclip_global(struct wlclip *c, struct wlwin *w, uint32_t name, const char *iface, uint32_t ver);
void wlclip_attach(struct wlclip *c, struct wlwin *w);
void wlclip_copy(struct wlclip *c, const char *text);
/* The text of the selection (malloc'd, `*len` bytes, NUL terminated), or NULL when there is none. At most 1 MB. */
char *wlclip_paste(struct wlclip *c, size_t *len);
/* Whether the selection offers `mime` (an image/png for instance), and its bytes (malloc'd; NULL when it is not offered, longer than `maxlen`, or the
 * source does not finish within two seconds of silence). */
bool wlclip_has_mime(const struct wlclip *c, const char *mime);
uint8_t *wlclip_paste_mime(struct wlclip *c, const char *mime, size_t maxlen, size_t *len);
/* Start dragging files: `uris` is the text/uri-list text ("file:///a\r\nfile:///b\r\n"), `text` what a text/plain drop gets (the paths, a line each),
 * `label` the words on the little icon that follows the pointer (a name, "3 files"). The drag ends with the button; a drop is a copy.
 * Returns false when the compositor has no drag-and-drop or one is going on already. */
bool wlclip_drag_files(struct wlclip *c, const char *uris, const char *text, const char *label);
bool wlclip_dragging(const struct wlclip *c);
/* Take drops of files: a drag of text/uri-list over the window is accepted (copy) and, when it is let go, `drop` gets the local paths (malloc'd, `n` of them,
 * freed by the library) and where the pointer was (surface pixels). Without a call a drag over the window is refused. Needs wlclip_attach to have worked. */
void wlclip_accept_drops(struct wlclip *c, void (*drop)(struct wlclip *c, char **paths, int n, double x, double y, void *data), void *data);

#endif

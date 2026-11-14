#ifndef MELLAGUI_WLWIN_H
#define MELLAGUI_WLWIN_H

/*
 * One software-drawn xdg-toplevel window on top of wlc, shared by the small
 * single-window clients (mellagui-calc, mellagui-run, mellagui-settings): registry
 * binding, xdg-shell boilerplate, a single wl_shm buffer that is redrawn on
 * request, keymap/modifier tracking and a few pixel-drawing helpers.
 *
 * Usage: fill in `ops`, call wlwin_connect(), look at the bound globals
 * (and any extra ones from ops->global), call wlwin_open(), then loop on
 * wlwin_step(). Ask for a repaint by setting `redraw`; ops->paint draws into
 * `pixels` (bw x bh, XRGB8888) with the helpers below.
 *
 * mellagui-term does not use this: it double-buffers and has its own protocol needs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wlc.h"

struct wlwin;

struct wlwin_ops {
    /* Required: draw the whole window (pixels, bw, bh are valid). */
    void (*paint)(struct wlwin *w);
    /* Optional: a registry global the library does not know (bind it with wlc_bind(w->c, w->registry, ...)). */
    void (*global)(struct wlwin *w, uint32_t name, const char *iface, uint32_t version);
    /* Optional: when set, a wl_pointer / wl_keyboard is created and its events forwarded as they are
     * (the library has already handled wl_keyboard.keymap and .modifiers: see `mods`). */
    void (*pointer)(struct wlwin *w, uint32_t op, const union lm_arg *args);
    void (*keyboard)(struct wlwin *w, uint32_t op, const union lm_arg *args);
    /* Optional: the compositor asked the window to close. Return false to keep it open (unsaved work); default: close. */
    bool (*close)(struct wlwin *w);
    /* Optional: the menu of wlwin_menu() was answered: the entry chosen, or -1 when it closed without a choice. */
    void (*menu)(struct wlwin *w, int index);
};

struct wlwin {
    struct wlc *c;
    const struct wlwin_ops *ops;
    void *data;                     /* for the ops callbacks */
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard, menu;       /* menu: mellagui_menu_v1, 0 without it */
    uint32_t surface, xsurface, toplevel, pool, buffer;
    int w, h;                       /* size the compositor asked for (clamped to the minimum) */
    int bw, bh;                     /* the window's drawing area in LOGICAL pixels (what a program lays out in) */
    int inx;                        /* wlwin_set_inset(): the width of a rail at the left; bw is smaller by it, drawing is shifted right and pointer x left by it */
    double rpx, rpy;                /* the pointer in WINDOW pixels (not shifted by the inset), for a program's own rail */
    int pw, ph;                     /* size of `pixels`: bw * scale x bh * scale (= bw x bh unless the program asked for wlwin_hidpi) */
    int scale;                      /* the buffer's scale factor: 1, or the largest wl_output scale for a hidpi window */
    bool hidpi;                     /* wlwin_hidpi() was called: the program draws only through the wlwin_* calls */
    bool argb;                      /* wlwin_argb() was called: the buffer has an alpha channel (transparent corners); it is cleared before every paint */
    uint32_t out_id[8];             /* the wl_outputs bound, and the scale each announced */
    int out_scale[8], nout;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    uint32_t mods;                  /* LM_MOD_* bits, from wl_keyboard.modifiers */
    int def_w, def_h, min_w, min_h;
    bool fixed_size;                /* maximized or fullscreen: the compositor decides the size, a program must not grow the window */
    bool want_fullscreen;           /* set before wlwin_open: the toplevel asks for full screen before its first commit */
    double zoom;                    /* the content's scale (wlwin_set_zoom; 0 or 1 = none): bw x bh is the window size divided by it */
    const char *name;               /* program name for messages, e.g. "mellagui-calc" */
};

/*
 * Connect, fetch the registry and bind compositor/shm/xdg_wm_base/wl_seat (plus whatever ops->global binds),
 * then wait for it with one roundtrip. Returns 0, or -1 (the error is on stderr) when the connection fails.
 * The caller may roundtrip again (e.g. for a value burst after binding a private global).
 */
int wlwin_connect(struct wlwin *w, const char *name, const struct wlwin_ops *ops, void *data);

/* Create the surface and the toplevel (title, app id, default and minimum size). Returns 0, or -1 with a message
 * when wl_compositor / wl_shm / xdg_wm_base are missing. */
int wlwin_open(struct wlwin *w, const char *title, const char *app_id, int def_w, int def_h, int min_w, int min_h);

/* One loop iteration: repaint if requested and possible, then wait up to timeout_ms (-1 = block) for events.
 * Returns false once the window should end (closed, or the connection died). */
bool wlwin_step(struct wlwin *w, int timeout_ms);

void wlwin_destroy(struct wlwin *w);

/*
 * HiDPI. A program that calls this once (at the start of main) draws on a screen with a scale of 2 or more into a buffer
 * that many times as big, so its text and buttons are as sharp as the shell's: `bw` x `bh` stay the LOGICAL size
 * every layout counts in, and the wlwin_* drawing calls take logical coordinates and do the multiplying. Such a program
 * must NOT touch `pixels` itself (its stride is `pw`, its size `pw` x `ph`): it uses wlwin_round_fill / wlwin_dim /
 * wlwin_blit below. Without the call the buffer is bw x bh and the compositor enlarges it (soft, but nothing breaks).
 */
void wlwin_hidpi(void);                                  /* call it first in main(): it is a property of the program, every window it opens is hidpi */
/* A program that calls this (first in main) draws into a premultiplied ARGB buffer, cleared to transparent before every paint; wlwin_fill and
 * wlwin_round_fill keep the alpha right (a card with rounded corners over the desktop). */
void wlwin_argb(void);
/* Ask the compositor to move / resize the window with the pointer that has the button down (the serial of the button event). `edges` are
 * LM_EDGE_* bits of xdg_toplevel.resize_edge (1 top, 2 bottom, 4 left, 8 right: LM_EDGE_*). */
void wlwin_move(struct wlwin *w, uint32_t serial);
void wlwin_resize(struct wlwin *w, uint32_t serial, uint32_t edges);

/* Show a menu at the pointer, drawn by the compositor (it may reach beyond the window): `entries` one per line, a leading '~' = dim. The answer
 * comes to ops->menu. False when the compositor has no such menu (the caller does the action some other way or does nothing). */
bool wlwin_menu(struct wlwin *w, const char *entries);

/* The content of the window is drawn `percent` per cent as big as it is laid out (a gadget card whose contents can be made bigger or smaller):
 * a program still lays out in bw x bh and draws with the wlwin_* calls, but bw x bh is now the window's size divided by the zoom, and the pointer
 * coordinates the program gets are converted to match. Combine it with wlwin_request_size to make the window follow. */
void wlwin_set_zoom(struct wlwin *w, int percent);

/* Make the window this big (a program zooming its text): the next repaint uses a buffer of that size and the compositor
 * follows it. Ignored while `fixed_size`. Returns true when the size was changed. */
bool wlwin_request_size(struct wlwin *w, int width, int height);

/* ---- drawing (all clip to the buffer) ------------------------------------------------------------------------------ */

void wlwin_fill(struct wlwin *w, int x, int y, int width, int height, uint32_t rgb);
/* A box with rounded corners (a pill when r >= min(width, height) / 2), blended over what is there. */
void wlwin_round_fill(struct wlwin *w, int x, int y, int width, int height, int r, uint32_t rgb);
/* A coloured button as a bead of water (dark rim, light gathered towards the bottom, a small glint): the look of every MellaGUI program's coloured buttons.
 * Same arguments as wlwin_round_fill. */
void wlwin_drop(struct wlwin *w, int x, int y, int bw, int bh, int r, uint32_t rgb);
/* Everything drawn so far mixed with black: t = 0 nothing, 1 all black (a modal card dims the page behind it). */
void wlwin_dim(struct wlwin *w, double t);
/* An XRGB / ARGB picture of sw x sh pixels (stride in pixels) scaled to the logical rectangle (x, y, width, height). */
/* Room for a side rail (wlrail.h) at the left: the program goes on drawing and hit-testing in a drawing area that starts `left` pixels in (bw is that much
 * smaller, every wlwin_* call is shifted, pointer positions are too), and keeps the rail to itself: it paints it with wlrail_paint (which draws in window
 * pixels) and finds the pointer over it through w->rpx / w->rpy. Call before wlwin_open (the default and minimum width grow by `left`). */
void wlwin_set_inset(struct wlwin *w, int left);
/* The scale factor of the buffer (1 when the program is not hidpi or the screen is not scaled): a program that makes pictures for its window
 * (an image viewer, a camera) makes them wlwin_scale() times as big as their logical size and puts them with wlwin_blit_px. */
static inline int wlwin_scale(const struct wlwin *w) { return w->scale > 1 ? w->scale : 1; }
/* One character in a cell of cw x ch LOGICAL pixels (a text grid: an editor, a table), drawn at the buffer's scale: the glyph is rasterised for
 * cw * scale x ch * scale pixels, so the grid stays exact at any scale. */
void wlwin_cell(struct wlwin *w, int x, int y, uint32_t cp, int cw, int ch, uint32_t rgb, bool bold);
/* An opaque picture of sw x sh PHYSICAL pixels (stride in pixels) with its top left corner at the logical position (x, y), clipped to the rectangle
 * (cx, cy, cw, ch) (logical). */
void wlwin_blit_px(struct wlwin *w, int x, int y, const uint32_t *src, int sw, int sh, int stride, int cx, int cy, int cw, int ch);
void wlwin_blit(struct wlwin *w, int x, int y, int width, int height, const uint32_t *src, int sw, int sh, int stride);
/* One glyph scaled up by `scale` (>= 1). */
void wlwin_glyph(struct wlwin *w, int x, int y, uint32_t cp, int scale, uint32_t rgb);
/* UTF-8 text; max_w > 0 stops before the first character that would not fit completely in that many pixels. */
/* A label: proportional when there is a font for it (font.h), so measure it with lm_font_text_width() * scale. */
void wlwin_text(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w);
/* A label in the bold weight (headings); measure with lm_font_text_width_bold(). The bitmap font has one weight. */
void wlwin_text_bold(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w);
/* Text at a fraction of the normal size (0.5 .. 1, e.g. 0.8 for fine print); the proportional font only - with the bitmap font it is the normal size. */
void wlwin_text_small(struct wlwin *w, int x, int y, const char *s, double size, uint32_t rgb, int max_w);
/* Text in cells of LM_FONT_W * scale (tables, text fields): always one cell per character. */
void wlwin_text_cells(struct wlwin *w, int x, int y, const char *s, int scale, uint32_t rgb, int max_w);

#endif

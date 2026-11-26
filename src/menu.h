#ifndef MELLAGUI_MENU_H
#define MELLAGUI_MENU_H

/*
 * mellagui_menu_v1 (MellaGUI private): a program shows a small menu at the pointer, drawn by the compositor (wm.c, the same card as the dock
 * icon's menu), so it can reach beyond the program's own window: the right-click menu of a text field (Cut, Copy, Paste ...).
 *
 *   popup(entries)   the entries, one per line; a leading '~' shows an entry dim (it cannot be chosen); at most LM_MENU_MAX, 47 bytes each
 *   chosen(index)    the user chose that entry (the menu is gone)
 *   warp(x, y)       move the pointer to (x, y) of the surface it is over (surface pixels; only while that surface is this program's own): a program
 *                    whose window grows under the button that was pressed (a text size button at the window's edge) takes the pointer along, so the
 *                    button can be pressed again where it now is
 *   raise(surface)   bring the window of one of this program's own surfaces forward (its workspace is switched to, a minimized window comes back):
 *                    the single `mellagui-view` window, asked by a second start to show another picture, comes up without an xdg-activation token
 *   place(surface, x, y)   (since 2) where a CARD WINDOW of this program opens, from the top left corner of its parent window's content: a card is a
 *                    toplevel with a parent whose app id ends in ".card" (a dialog), ".panel" (a floating panel) or ".tool" (a panel that stays beside the program). The wm gives
 *                    it no frame, keeps it above its parent, lets the user drag it anywhere (xdg_toplevel.move) and never gives it the keyboard
 *                    (the parent keeps it). Once the user has moved it, `place` is not obeyed.
 *   closed()         the menu went away without a choice (a click elsewhere, Esc, another menu, the screen was locked)
 *
 * One menu at a time; a new popup closes the old one (which gets `closed`). Served only to MellaGUI's own programs (lm_settings_client_allowed);
 * the others get an inert object that answers `closed` at once. Owner: menu.c.
 */

struct lm_server;
struct lm_menu;

struct lm_menu *lm_menu_create(struct lm_server *srv);
void lm_menu_destroy(struct lm_menu *m);

/* From wm.c: the user chose entry `index` / the menu was closed. */
void lm_menu_chosen(struct lm_server *srv, int index);
void lm_menu_closed(struct lm_server *srv);

#endif

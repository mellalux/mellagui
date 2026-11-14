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

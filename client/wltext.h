#ifndef MELLAGUI_WLTEXT_H
#define MELLAGUI_WLTEXT_H

/*
 * Single line text fields for the wlwin programs (Run, the settings panel, the file manager's and the editor's prompts): a plain NUL terminated
 * UTF-8 buffer that the program keeps as it likes, edited with a caret and a selection, the clipboard and a right-click menu. The caret and the
 * selection are kept here by the address of the buffer; when the program sets the text itself (another length) the caret goes to the end.
 *
 *   wltext_edit()         a key: characters, BackSpace / Delete (Ctrl: a word), Left / Right / Home / End (Shift: select; Ctrl: by word), Ctrl+A
 *                         select all, Ctrl+C / X / V, Ctrl+Insert, Shift+Delete, Shift+Insert. Copy and Cut take the selection, the whole field when
 *                         nothing is selected; Paste replaces the selection and goes in as one line. True when the key was one of those.
 *   wltext_click()        a click at `rel_px` pixels from the left of the text (Shift: extend the selection)
 *   wltext_paint()        draws the text with the selection and the caret, scrolled so that the caret shows
 *   wltext_menu()         the entries of the field's right-click menu for wlwin_menu(): Cut / Copy / Paste / Select all / Clear (dim when they do nothing)
 *   wltext_menu_do()      do the entry that ops->menu got
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wlclip;
struct wlwin;

char *wltext_paste_text(struct wlclip *clip, size_t *len);
/* True when the key is one of the clipboard keys. */
bool wltext_is_clipkey(uint32_t ks, uint32_t mods);
bool wltext_edit(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods);
/* Only the clipboard keys of wltext_edit(). */
bool wltext_key(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods);

void wltext_end(char *buf);                                 /* the caret to the end, nothing selected */
bool wltext_selection(const char *buf, size_t *from, size_t *to);
size_t wltext_cursor(const char *buf);
void wltext_select_all(char *buf);
void wltext_click(char *buf, int rel_px, bool extend);
void wltext_paint(struct wlwin *w, int x, int y, int maxw, const char *buf, bool focus, uint32_t fg, uint32_t selbg, uint32_t caret);

const char *wltext_menu(const char *buf);
void wltext_menu_do(struct wlclip *clip, char *buf, size_t max, int index);
/* Appends `text` (`n` bytes) to `buf` without overflowing or cutting a UTF-8 character in two. */
void wltext_append(char *buf, size_t max, const char *text, size_t n);

#endif

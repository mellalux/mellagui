#ifndef MELLAGUI_WLTEXT_H
#define MELLAGUI_WLTEXT_H

/*
 * Clipboard and right-click menu for the single line text fields of the wlwin programs (Run, the settings panel, the file manager's and the
 * editor's prompts). The fields are plain buffers: what is typed goes on the end (Run has a cursor), there is no selection, so the clipboard
 * works on the whole field: Copy takes all of it, Cut takes all of it and empties the field, Paste puts the clipboard (as one line) in.
 *
 *   wltext_paste_text()   the clipboard as one line: line ends and tabs become spaces, other control bytes go; malloc'd, NULL when empty
 *   wltext_key()          Ctrl+C / X / V, Ctrl+Insert, Shift+Delete, Shift+Insert on a buffer; true when the key was one of them
 *   wltext_menu()         the entries of the field's right-click menu for wlwin_menu(): Cut / Copy / Paste / Clear (dim ones when they do nothing)
 *   wltext_menu_do()      do the entry that ops->menu got
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wlclip;

char *wltext_paste_text(struct wlclip *clip, size_t *len);
/* True when the key is one of the clipboard keys wltext_key() knows. */
bool wltext_is_clipkey(uint32_t ks, uint32_t mods);
/* `buf` holds `max` bytes, NUL terminated, UTF-8. Paste appends (at most what fits, on a character boundary). */
bool wltext_key(struct wlclip *clip, char *buf, size_t max, uint32_t ks, uint32_t mods);
const char *wltext_menu(const char *buf);
void wltext_menu_do(struct wlclip *clip, char *buf, size_t max, int index);
/* Appends `text` (`n` bytes) to `buf` without overflowing or cutting a UTF-8 character in two. */
void wltext_append(char *buf, size_t max, const char *text, size_t n);

#endif

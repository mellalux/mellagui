#ifndef MELLAGUI_ICONFIND_H
#define MELLAGUI_ICONFIND_H

/*
 * Finds the icon an installed program brings along (a PNG file), so the dock can show Firefox's or VS Code's own icon. Looked for, for every
 * name in turn (names are separated by `|`, "vscode|code"):
 *   <root>/icons/hicolor/<size>/apps/<name>.png and <root>/pixmaps/<name>.png     for the roots $XDG_DATA_HOME (~/.local/share),
 *                                                                                 /usr/share, /usr/local/share, /var/lib/snapd/desktop,
 *                                                                                 /var/lib/flatpak/exports/share, ~/.local/share/flatpak/exports/share
 *   /snap/<any>/current/meta/gui/<name>.png and /snap/<name>/current/default256.png (snap packages keep their icon inside)
 * The size closest to 256 px wins (big ones are scaled down by the dock). SVG icons are not read. $MELLAGUI_ICON_PATH (colon separated
 * roots of the first kind; "snap" there stands for the /snap search) replaces the list above: the tests point it at a folder of their own.
 * Owner: iconfind.c.
 */

#include <stdbool.h>
#include <stddef.h>

/* true and the path of the PNG in `path`, or false when none of the names has an icon. */
bool lm_icon_find(const char *names, char *path, size_t n);

#endif

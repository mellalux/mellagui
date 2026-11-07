/*
 * screenshot.h - Print / Ctrl+Print / Alt+Print. The scene is painted again into a private buffer (without the
 * cursor), encoded as PNG (png.c), optionally saved under config.screenshot_dir and always offered as the clipboard
 * selection ("image/png", served by the compositor itself). Owner: screenshot.c; the keys live in wm.c.
 */
#ifndef MELLAGUI_SCREENSHOT_H
#define MELLAGUI_SCREENSHOT_H

#include <stdbool.h>
#include "server.h"
#include "util.h"

/* `rect` in global coordinates (an output, or a window frame). Returns false when nothing could be captured. */
bool lm_screenshot_take(struct lm_server *srv, const struct lm_rect *rect, bool to_file);

#endif

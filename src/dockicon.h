#ifndef MELLAGUI_DOCKICON_H
#define MELLAGUI_DOCKICON_H

/*
 * The dock's built-in icons: a round disc in the icon's colour with a white glyph, all drawn with signed-distance shapes so they are
 * crisp at any size (the dock magnifies them). Pure arithmetic, no state; render.c asks for one pixel at a time.
 */

#include <stdint.h>

enum lm_dockicon {
    LM_DOCKICON_GENERIC,        /* four rounded squares: "an application" */
    LM_DOCKICON_TERMINAL,
    LM_DOCKICON_CALCULATOR,
    LM_DOCKICON_FILES,
    LM_DOCKICON_EDITOR,
    LM_DOCKICON_IMAGES,
    LM_DOCKICON_SETTINGS,
    LM_DOCKICON_CODE,
    LM_DOCKICON_WEB,
    LM_DOCKICON_RUN,
    LM_DOCKICON_LAUNCHER,       /* a grid of nine dots: opens the launcher dialog */
    LM_DOCKICON_CAMERA,         /* the webcam viewer */
    LM_DOCKICON_RECORD,         /* the screen recorder: a record symbol (ring with a dot) */
    LM_DOCKICON_MONITOR,        /* the process manager: a screen with bars */
    LM_DOCKICON_COUNT
};

/* Name of a built-in icon as written in mellagui.conf ("terminal", "calculator" ...), NULL when kind is out of range. */
const char *lm_dockicon_name(int kind);
/* One pixel (premultiplied ARGB) of a plain round plate of colour `rgb` (0xRRGGBB) with the same shadow and top light as the built-in icons, for
 * the logos of installed programs: (dx, dy) from the centre, radius R, lift as in lm_dockicon_pixel. */
uint32_t lm_dockicon_plate_pixel(uint32_t rgb, float dx, float dy, float R, float lift);
/* The kind called `name` (case-insensitive), or -1. */
int lm_dockicon_by_name(const char *name);
/* A kind guessed from an entry's name ("Text Editor" -> editor, "Firefox" -> web ...); LM_DOCKICON_GENERIC when nothing fits. */
int lm_dockicon_guess(const char *entry_name);

/* The icon's pixel at (dx, dy) from its centre, for a disc of radius R (all in pixels, fractional is fine): premultiplied ARGB,
 * 0 outside. The disc has a soft shadow beyond its edge (up to about R * 0.3, shifted down) and a light-to-dark shading; `lift` (0..1)
 * brightens the disc a little (the hovered icon). */
uint32_t lm_dockicon_pixel(int kind, float dx, float dy, float R, float lift);

#endif

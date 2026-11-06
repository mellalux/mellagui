/*
 * edid.h - what the EDID says about a monitor's own (native) shape, and when the video mode the kernel offers does not match
 * it. No DRM types: unit-testable (tests/keymap_test.c). Used by backend_drm.c.
 *
 * Background: a GPU/kernel may cut the monitor's native mode away (pixel clock limit, e.g. 2560x1080 over old HDMI) and
 * leave only other shapes (16:9). The monitor then stretches that picture over its whole panel. The EDID's first detailed
 * timing is the monitor's preferred = native mode; if its shape differs from the preferred mode the kernel offers, a mode of
 * the native shape (generated CVT, cvt.h) with the same width avoids the stretching.
 */
#ifndef LM_EDID_H
#define LM_EDID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_edid_native {
    int w, h;                   /* first detailed timing: active pixels */
    int mm_w, mm_h;             /* its image size in mm (0 when not given) */
};

/* Parses the base block. False for a block that is not EDID (header) or whose first descriptor is not a timing. */
bool lm_edid_native(const uint8_t *edid, size_t len, struct lm_edid_native *out);

/*
 * The mode `pref_w` x `pref_h` the kernel offers as preferred, the monitor's native timing `nat` and the connector's
 * physical size (0 = unknown): returns the height of a mode with width pref_w and the native shape, or 0 = leave the
 * preferred mode alone. Nothing is changed when the shapes agree within 8 %, when there is no native timing, or when the
 * physical size contradicts the native timing by more than 12 % (a broken EDID: projectors and TVs often claim 16x9 cm).
 */
int lm_edid_fix_height(int pref_w, int pref_h, const struct lm_edid_native *nat, int panel_mm_w, int panel_mm_h);

#endif

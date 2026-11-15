/*
 * cvt.h - VESA CVT 1.2 "reduced blanking" (v1) timings for a custom video mode (no DRM types: unit-testable).
 * Used by backend_drm.c for `--mode WxH[@Hz]` when the monitor lists no mode of that size (e.g. 21:9 on a GPU whose
 * HDMI pixel clock limit cuts the panel's own mode away).
 */
#ifndef LM_CVT_H
#define LM_CVT_H

#include <stdint.h>

struct lm_cvt {
    uint32_t clock_khz;
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    uint16_t refresh;
};

/* width is rounded down to a multiple of 8. False for sizes/rates outside 320..8192 x 200..4320 and 24..240 Hz. */
int lm_cvt_rb(int width, int height, int refresh, struct lm_cvt *out);
/* The highest of the usual refresh rates (60, 59, 50, 48, 30, 25, 24; at most max_hz) at which the mode's pixel clock stays
 * within max_clock_khz; false when none fits. */
int lm_cvt_rb_fit(int width, int height, int max_hz, uint32_t max_clock_khz, struct lm_cvt *out);
/* The same, but when the monitor declares its vertical range (vmin > 0, from the EDID) ANY whole rate from min(max_hz, vmax) down to max(vmin, 24)
 * counts, the highest one that fits first - not only the usual ones: a 2560x1080 over a 162 MHz link gets 53 Hz instead of 50. */
int lm_cvt_rb_fit_range(int width, int height, int max_hz, uint32_t max_clock_khz, int vmin, int vmax, struct lm_cvt *out);

#endif

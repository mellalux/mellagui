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

#endif

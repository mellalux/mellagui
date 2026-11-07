#include "cvt.h"

#include <math.h>

int lm_cvt_rb(int width, int height, int refresh, struct lm_cvt *out)
{
    width &= ~7;
    if (width < 320 || width > 8192 || height < 200 || height > 4320 || refresh < 24 || refresh > 240)
        return 0;
    const double min_vblank_us = 460.0;
    const int hblank = 160, hfront = 48, hsync = 32, vfront = 3, min_vback = 6;
    int vsync = 10;                                         /* vertical sync width by aspect ratio (CVT table) */
    if (height * 4 == width * 3)
        vsync = 4;
    else if (height * 16 == width * 9)
        vsync = 5;
    else if (height * 16 == width * 10)
        vsync = 6;
    else if (height * 5 == width * 4 || height * 15 == width * 9)
        vsync = 7;
    double h_period_us = ((1000000.0 / refresh) - min_vblank_us) / height;
    int vbi = (int)(min_vblank_us / h_period_us) + 1;
    int min_vbi = vfront + vsync + min_vback;
    if (vbi < min_vbi)
        vbi = min_vbi;
    int htotal = width + hblank, vtotal = height + vbi;
    double clk_khz = (double)htotal * vtotal * refresh / 1000.0;
    out->clock_khz = (uint32_t)(floor(clk_khz / 250.0) * 250.0);     /* CVT: multiple of 0.25 MHz, rounded down */
    out->hdisplay = (uint16_t)width;
    out->hsync_start = (uint16_t)(width + hfront);
    out->hsync_end = (uint16_t)(width + hfront + hsync);
    out->htotal = (uint16_t)htotal;
    out->vdisplay = (uint16_t)height;
    out->vsync_start = (uint16_t)(height + vfront);
    out->vsync_end = (uint16_t)(height + vfront + vsync);
    out->vtotal = (uint16_t)vtotal;
    out->refresh = (uint16_t)refresh;
    return 1;
}

int lm_cvt_rb_fit(int width, int height, int max_hz, uint32_t max_clock_khz, struct lm_cvt *out)
{
    static const int rates[] = { 60, 59, 50, 48, 30, 25, 24 };          /* rates a monitor is likely to accept, not 52 */
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (rates[i] <= max_hz && lm_cvt_rb(width, height, rates[i], out) && out->clock_khz <= max_clock_khz)
            return 1;
    return 0;
}

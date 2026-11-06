#include "edid.h"

#include <math.h>
#include <string.h>

bool lm_edid_native(const uint8_t *e, size_t len, struct lm_edid_native *out)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    if (!e || len < 128 || memcmp(e, header, 8))
        return false;
    const uint8_t *b = e + 54;                              /* first of the four 18-byte descriptors */
    int clock10k = b[0] | (b[1] << 8);
    if (!clock10k)                                          /* a monitor descriptor, not a timing */
        return false;
    out->w = b[2] | ((b[4] & 0xf0) << 4);
    out->h = b[5] | ((b[7] & 0xf0) << 4);
    out->mm_w = b[12] | ((b[14] & 0xf0) << 4);
    out->mm_h = b[13] | ((b[14] & 0x0f) << 8);
    return out->w >= 320 && out->h >= 200;
}

int lm_edid_fix_height(int pref_w, int pref_h, const struct lm_edid_native *nat, int panel_mm_w, int panel_mm_h)
{
    if (!nat || nat->w < 320 || nat->h < 200 || pref_w < 320 || pref_h < 200)
        return 0;
    double an = (double)nat->w / nat->h, ap = (double)pref_w / pref_h;
    if (fabs(an / ap - 1.0) <= 0.08)
        return 0;                                           /* the kernel's preferred mode has the native shape */
    int mw = nat->mm_w > 0 ? nat->mm_w : panel_mm_w, mh = nat->mm_h > 0 ? nat->mm_h : panel_mm_h;
    if (mw > 0 && mh > 0 && fabs(((double)mw / mh) / an - 1.0) > 0.12)
        return 0;                                           /* the EDID contradicts itself: do not guess */
    int h = (int)lround((pref_w & ~7) / an / 2.0) * 2;
    return h >= 200 ? h : 0;
}

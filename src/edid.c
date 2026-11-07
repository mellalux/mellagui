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

bool lm_edid_info(const uint8_t *e, size_t len, struct lm_edid_info *out)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memset(out, 0, sizeof(*out));
    if (!e || len < 128 || memcmp(e, header, 8))
        return false;
    int id = (e[8] << 8) | e[9];                            /* three letters of five bits, 1 = A */
    for (int i = 0; i < 3; i++) {
        int ch = (id >> (10 - 5 * i)) & 31;
        out->vendor[i] = ch >= 1 && ch <= 26 ? (char)('A' + ch - 1) : '?';
    }
    out->product = e[10] | (e[11] << 8);
    out->year = e[17] ? 1990 + e[17] : 0;
    out->mm_w = e[21] * 10;
    out->mm_h = e[22] * 10;
    for (int d = 0; d < 4; d++) {                           /* descriptors: 0xfc = monitor name */
        const uint8_t *b = e + 54 + 18 * d;
        if (b[0] || b[1] || b[2] || b[3] != 0xfc)
            continue;
        int n = 0;
        for (int i = 5; i < 18 && b[i] != 0x0a && b[i] && n < (int)sizeof(out->name) - 1; i++)
            out->name[n++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '?';
        while (n > 0 && out->name[n - 1] == ' ')
            n--;
        out->name[n] = 0;
    }
    return true;
}

const char *lm_edid_vendor_name(const char *pnp)
{
    static const struct { const char *id, *name; } t[] = {
        { "ACR", "Acer" }, { "AOC", "AOC" }, { "APP", "Apple" }, { "AUS", "ASUS" }, { "AUO", "AU Optronics" },
        { "BNQ", "BenQ" }, { "BOE", "BOE" }, { "CMN", "Innolux" }, { "DEL", "Dell" }, { "GSM", "LG" },
        { "HWP", "HP" }, { "HPN", "HP" }, { "IVM", "iiyama" }, { "LEN", "Lenovo" }, { "LGD", "LG Display" },
        { "MSI", "MSI" }, { "NEC", "NEC" }, { "PHL", "Philips" }, { "SAM", "Samsung" }, { "SDC", "Samsung Display" },
        { "SHP", "Sharp" }, { "SNY", "Sony" }, { "VSC", "ViewSonic" }, { "ENC", "Eizo" }, { "GBT", "Gigabyte" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(pnp, t[i].id))
            return t[i].name;
    return pnp;
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

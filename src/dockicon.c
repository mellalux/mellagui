/*
 * The dock's built-in icons. See dockicon.h. Everything works in units of the disc radius (the disc is the unit circle, y points down)
 * and every glyph is a signed distance: negative inside. The edge is anti-aliased over one pixel (`px` units wide).
 */
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "dockicon.h"
#include "metrics.h"
#include "theme.h"

static const struct { const char *name; uint32_t color; } icons[LM_DOCKICON_COUNT] = {
    [LM_DOCKICON_GENERIC] = { "generic", 0 },
    [LM_DOCKICON_TERMINAL] = { "terminal", 0 },
    [LM_DOCKICON_CALCULATOR] = { "calculator", 0 },
    [LM_DOCKICON_FILES] = { "files", 0 },
    [LM_DOCKICON_EDITOR] = { "editor", 0 },
    [LM_DOCKICON_IMAGES] = { "images", 0 },
    [LM_DOCKICON_SETTINGS] = { "settings", 0 },
    [LM_DOCKICON_CODE] = { "code", 0 },
    [LM_DOCKICON_WEB] = { "web", 0 },
    [LM_DOCKICON_RUN] = { "run", 0 },
    [LM_DOCKICON_LAUNCHER] = { "launcher", 0 },
    [LM_DOCKICON_CAMERA] = { "camera", 0 },
    [LM_DOCKICON_RECORD] = { "record", 0 },
    [LM_DOCKICON_MONITOR] = { "monitor", 0 },
    [LM_DOCKICON_OVERVIEW] = { "overview", 0 },
    [LM_DOCKICON_KEYBOARD] = { "keyboard", 0 },
    [LM_DOCKICON_BELL] = { "bell", 0 },
    [LM_DOCKICON_BLUETOOTH] = { "bluetooth", 0 },
    [LM_DOCKICON_SHOT] = { "shot", 0 },
    [LM_DOCKICON_DISK] = { "disk", 0 },
};

/* The disc colour of a built-in icon: the palette's icon-* colours. */
static uint32_t icon_color(int kind)
{
    switch (kind) {
    case LM_DOCKICON_TERMINAL: return lm_theme.icon_terminal;
    case LM_DOCKICON_CALCULATOR: return lm_theme.icon_calculator;
    case LM_DOCKICON_FILES: return lm_theme.icon_files;
    case LM_DOCKICON_EDITOR: return lm_theme.icon_editor;
    case LM_DOCKICON_IMAGES: return lm_theme.icon_images;
    case LM_DOCKICON_SETTINGS: return lm_theme.icon_settings;
    case LM_DOCKICON_CODE: return lm_theme.icon_code;
    case LM_DOCKICON_WEB: return lm_theme.icon_web;
    case LM_DOCKICON_RUN: return lm_theme.icon_run;
    case LM_DOCKICON_LAUNCHER: return lm_theme.icon_launcher;
    case LM_DOCKICON_CAMERA: return lm_theme.icon_camera;
    case LM_DOCKICON_RECORD: return lm_theme.icon_record;
    case LM_DOCKICON_MONITOR: return lm_theme.icon_monitor;
    case LM_DOCKICON_OVERVIEW: return lm_theme.icon_overview;
    case LM_DOCKICON_KEYBOARD: return lm_theme.icon_keyboard;
    case LM_DOCKICON_BELL: return lm_theme.icon_bell;
    case LM_DOCKICON_BLUETOOTH: return lm_theme.icon_bluetooth;
    case LM_DOCKICON_SHOT: return lm_theme.icon_shot;
    case LM_DOCKICON_DISK: return lm_theme.icon_disk;
    default: return lm_theme.icon_generic;
    }
}

const char *lm_dockicon_name(int kind)
{
    return kind >= 0 && kind < LM_DOCKICON_COUNT ? icons[kind].name : NULL;
}

int lm_dockicon_by_name(const char *name)
{
    for (int i = 0; i < LM_DOCKICON_COUNT; i++)
        if (!strcasecmp(icons[i].name, name))
            return i;
    return -1;
}

int lm_dockicon_guess(const char *entry_name)
{
    static const struct { const char *part; int kind; } hints[] = {
        { "overview", LM_DOCKICON_OVERVIEW }, { "bluetooth", LM_DOCKICON_BLUETOOTH }, { "shortcut", LM_DOCKICON_KEYBOARD }, { "keyboard", LM_DOCKICON_KEYBOARD },
        { "notification", LM_DOCKICON_BELL }, { "bell", LM_DOCKICON_BELL }, { "screenshot", LM_DOCKICON_SHOT }, { "disk", LM_DOCKICON_DISK },
        { "record", LM_DOCKICON_RECORD }, { "process", LM_DOCKICON_MONITOR }, { "monitor", LM_DOCKICON_MONITOR }, { "task", LM_DOCKICON_MONITOR },
        { "cam", LM_DOCKICON_CAMERA },   /* before "web": a Webcam is a camera */
        { "term", LM_DOCKICON_TERMINAL }, { "console", LM_DOCKICON_TERMINAL }, { "shell", LM_DOCKICON_TERMINAL },
        { "calc", LM_DOCKICON_CALCULATOR }, { "file", LM_DOCKICON_FILES }, { "folder", LM_DOCKICON_FILES },
        { "edit", LM_DOCKICON_EDITOR }, { "text", LM_DOCKICON_EDITOR }, { "note", LM_DOCKICON_EDITOR },
        { "image", LM_DOCKICON_IMAGES }, { "photo", LM_DOCKICON_IMAGES }, { "picture", LM_DOCKICON_IMAGES }, { "view", LM_DOCKICON_IMAGES },
        { "setting", LM_DOCKICON_SETTINGS }, { "config", LM_DOCKICON_SETTINGS }, { "prefer", LM_DOCKICON_SETTINGS },
        { "code", LM_DOCKICON_CODE }, { "firefox", LM_DOCKICON_WEB }, { "browser", LM_DOCKICON_WEB }, { "chrom", LM_DOCKICON_WEB },
        { "web", LM_DOCKICON_WEB }, { "internet", LM_DOCKICON_WEB }, { "run", LM_DOCKICON_RUN }, { "launcher", LM_DOCKICON_LAUNCHER },
    };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
        if (strcasestr(entry_name, hints[i].part))
            return hints[i].kind;
    return LM_DOCKICON_GENERIC;
}

/* ---- signed distances ------------------------------------------------------------------------------------------------ */

static float fminf2(float a, float b) { return a < b ? a : b; }
static float fmaxf2(float a, float b) { return a > b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float sd_circle(float u, float v, float cx, float cy, float r)
{
    return hypotf(u - cx, v - cy) - r;
}

/* Rounded box with half sizes hw, hh and corner radius r. */
static float sd_box(float u, float v, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(u - cx) - (hw - r), qy = fabsf(v - cy) - (hh - r);
    float ox = fmaxf2(qx, 0.0f), oy = fmaxf2(qy, 0.0f);
    return hypotf(ox, oy) + fminf2(fmaxf2(qx, qy), 0.0f) - r;
}

/* A line segment with round ends, `h` = half its thickness. */
static float sd_line(float u, float v, float ax, float ay, float bx, float by, float h)
{
    float vx = bx - ax, vy = by - ay, wx = u - ax, wy = v - ay;
    float t = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return hypotf(u - (ax + t * vx), v - (ay + t * vy)) - h;
}

static float sd_tri(float u, float v, float ax, float ay, float bx, float by, float cx, float cy)
{
    float ex[3] = { bx - ax, cx - bx, ax - cx }, ey[3] = { by - ay, cy - by, ay - cy };
    float px[3] = { u - ax, u - bx, u - cx }, py[3] = { v - ay, v - by, v - cy };
    float d = 1e9f, s = 0.0f;
    for (int i = 0; i < 3; i++) {
        float t = clampf((px[i] * ex[i] + py[i] * ey[i]) / (ex[i] * ex[i] + ey[i] * ey[i]), 0.0f, 1.0f);
        float qx = px[i] - ex[i] * t, qy = py[i] - ey[i] * t;
        d = fminf2(d, qx * qx + qy * qy);
        float cr = ex[i] * py[i] - ey[i] * px[i];
        s += cr > 0.0f ? 1.0f : -1.0f;
    }
    return (fabsf(s) == 3.0f ? -1.0f : 1.0f) * sqrtf(d);
}

static float sd_sub(float a, float b) { return fmaxf2(a, -b); }       /* a without b */

/* ---- the glyphs ------------------------------------------------------------------------------------------------------ */

static float glyph(int kind, float u, float v)
{
    switch (kind) {
    case LM_DOCKICON_TERMINAL: {
        float d = sd_line(u, v, -0.40f, -0.24f, -0.12f, 0.0f, 0.075f);
        d = fminf2(d, sd_line(u, v, -0.12f, 0.0f, -0.40f, 0.24f, 0.075f));
        return fminf2(d, sd_line(u, v, 0.02f, 0.27f, 0.42f, 0.27f, 0.075f));
    }
    case LM_DOCKICON_CALCULATOR: {
        float d = sd_box(u, v, 0, 0, 0.31f, 0.44f, 0.08f);
        d = sd_sub(d, sd_box(u, v, 0, -0.26f, 0.21f, 0.09f, 0.03f));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                d = sd_sub(d, sd_circle(u, v, -0.15f + 0.15f * (float)c, 0.0f + 0.15f * (float)r + 0.02f, 0.045f));
        return d;
    }
    case LM_DOCKICON_FILES: {
        float d = sd_box(u, v, 0, 0.09f, 0.46f, 0.29f, 0.06f);
        return fminf2(d, sd_box(u, v, -0.26f, -0.25f, 0.20f, 0.09f, 0.04f));
    }
    case LM_DOCKICON_EDITOR: {
        float d = sd_box(u, v, 0, 0, 0.32f, 0.44f, 0.07f);
        for (int i = 0; i < 4; i++)
            d = sd_sub(d, sd_box(u, v, i == 3 ? -0.07f : 0.0f, -0.24f + 0.16f * (float)i, i == 3 ? 0.13f : 0.2f, 0.028f, 0.012f));
        return d;
    }
    case LM_DOCKICON_IMAGES: {
        float ring = sd_sub(sd_box(u, v, 0, 0, 0.46f, 0.36f, 0.07f), sd_box(u, v, 0, 0, 0.38f, 0.28f, 0.03f));
        float in = sd_circle(u, v, 0.19f, -0.11f, 0.075f);
        in = fminf2(in, sd_tri(u, v, -0.36f, 0.26f, -0.09f, -0.05f, 0.18f, 0.26f));
        in = fminf2(in, sd_tri(u, v, 0.0f, 0.26f, 0.19f, 0.05f, 0.36f, 0.26f));
        in = fmaxf2(in, sd_box(u, v, 0, 0, 0.40f, 0.30f, 0.03f));
        return fminf2(ring, in);
    }
    case LM_DOCKICON_SETTINGS: {
        float d = hypotf(u, v), th = atan2f(v, u);
        float c = cosf(8.0f * th), x = clampf(2.2f * c, -1.0f, 1.0f);
        float ro = 0.37f + 0.06f * (x + 1.0f);
        float dro = fabsf(2.2f * c) < 1.0f ? 0.06f * 2.2f * 8.0f * -sinf(8.0f * th) : 0.0f;
        float g = sqrtf(1.0f + (dro / (d + 0.05f)) * (dro / (d + 0.05f)));
        return fmaxf2((d - ro) / g, 0.17f - d);
    }
    case LM_DOCKICON_CODE: {
        float d = sd_line(u, v, -0.20f, -0.22f, -0.44f, 0.0f, 0.065f);
        d = fminf2(d, sd_line(u, v, -0.44f, 0.0f, -0.20f, 0.22f, 0.065f));
        d = fminf2(d, sd_line(u, v, 0.20f, -0.22f, 0.44f, 0.0f, 0.065f));
        d = fminf2(d, sd_line(u, v, 0.44f, 0.0f, 0.20f, 0.22f, 0.065f));
        return fminf2(d, sd_line(u, v, 0.07f, -0.28f, -0.07f, 0.28f, 0.05f));
    }
    case LM_DOCKICON_WEB: {
        float r = hypotf(u, v);
        float d = fabsf(r - 0.46f) - 0.04f;
        float inside = r - 0.46f;
        float e = hypotf(u / 0.21f, v / 0.46f);
        d = fminf2(d, fabsf(e - 1.0f) * 0.21f - 0.03f);
        d = fminf2(d, fmaxf2(fabsf(v) - 0.03f, inside));
        d = fminf2(d, fmaxf2(fabsf(fabsf(v) - 0.25f) - 0.03f, inside));
        return fminf2(d, fmaxf2(fabsf(u) - 0.03f, inside));
    }
    case LM_DOCKICON_LAUNCHER: {
        float d = 1e9f;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                d = fminf2(d, sd_box(u, v, -0.28f + 0.28f * (float)c, -0.28f + 0.28f * (float)r, 0.095f, 0.095f, 0.03f));
        return d;
    }
    case LM_DOCKICON_CAMERA: {
        float d = sd_box(u, v, 0, 0.05f, 0.44f, 0.27f, 0.07f);
        d = fminf2(d, sd_box(u, v, -0.22f, -0.22f, 0.13f, 0.07f, 0.03f));
        d = sd_sub(d, sd_circle(u, v, 0.0f, 0.05f, 0.18f));
        return fminf2(d, sd_circle(u, v, 0.0f, 0.05f, 0.095f));
    }
    case LM_DOCKICON_RECORD:                            /* a ring with a dot in it */
        return fminf2(fabsf(hypotf(u, v) - 0.40f) - 0.05f, sd_circle(u, v, 0.0f, 0.0f, 0.21f));
    case LM_DOCKICON_MONITOR: {                         /* a screen with three bars */
        float d = sd_sub(sd_box(u, v, 0, -0.04f, 0.46f, 0.34f, 0.07f), sd_box(u, v, 0, -0.04f, 0.37f, 0.25f, 0.03f));
        d = fminf2(d, sd_box(u, v, 0, 0.39f, 0.14f, 0.045f, 0.02f));
        d = fminf2(d, sd_box(u, v, -0.19f, 0.05f, 0.055f, 0.12f, 0.015f));
        d = fminf2(d, sd_box(u, v, 0.0f, -0.02f, 0.055f, 0.19f, 0.015f));
        return fminf2(d, sd_box(u, v, 0.19f, -0.09f, 0.055f, 0.08f, 0.015f));
    }
    case LM_DOCKICON_KEYBOARD: {                        /* a keyboard: a frame, two rows of keys and the space bar */
        float d = sd_sub(sd_box(u, v, 0, 0.02f, 0.44f, 0.29f, 0.07f), sd_box(u, v, 0, 0.02f, 0.385f, 0.235f, 0.03f));
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 5; c++)
                d = fminf2(d, sd_box(u, v, -0.24f + 0.12f * (float)c, -0.075f + 0.115f * (float)r, 0.04f, 0.04f, 0.012f));
        return fminf2(d, sd_box(u, v, 0, 0.17f, 0.17f, 0.035f, 0.012f));
    }
    case LM_DOCKICON_BELL: {                            /* a bell: a dome, a flare, a lip, a knob and a clapper */
        float d = fminf2(sd_circle(u, v, 0, -0.07f, 0.24f), sd_box(u, v, 0, 0.07f, 0.24f, 0.14f, 0.0f));
        d = fminf2(d, sd_tri(u, v, -0.24f, 0.07f, 0.24f, 0.07f, 0.34f, 0.2f));
        d = fminf2(d, sd_tri(u, v, -0.24f, 0.07f, -0.34f, 0.2f, 0.34f, 0.2f));
        d = fminf2(d, sd_box(u, v, 0, 0.2f, 0.34f, 0.045f, 0.03f));
        d = fminf2(d, sd_circle(u, v, 0, -0.33f, 0.05f));
        return fminf2(d, sd_circle(u, v, 0, 0.3f, 0.07f));
    }
    case LM_DOCKICON_BLUETOOTH: {                       /* the rune: a vertical stroke with two arrowheads */
        float h = 0.04f;
        float d = sd_line(u, v, -0.17f, -0.17f, 0.17f, 0.17f, h);
        d = fminf2(d, sd_line(u, v, 0.17f, 0.17f, 0.0f, 0.33f, h));
        d = fminf2(d, sd_line(u, v, 0.0f, 0.33f, 0.0f, -0.33f, h));
        d = fminf2(d, sd_line(u, v, 0.0f, -0.33f, 0.17f, -0.17f, h));
        return fminf2(d, sd_line(u, v, 0.17f, -0.17f, -0.17f, 0.17f, h));
    }
    case LM_DOCKICON_SHOT: {                            /* four viewfinder corners round a dot */
        float d = sd_circle(u, v, 0, 0, 0.08f);
        for (int k = 0; k < 4; k++) {
            float sx = (k & 1) ? 0.3f : -0.3f, sy = (k & 2) ? 0.3f : -0.3f;
            d = fminf2(d, sd_line(u, v, sx, sy, sx * 0.35f, sy, 0.04f));
            d = fminf2(d, sd_line(u, v, sx, sy, sx, sy * 0.35f, 0.04f));
        }
        return d;
    }
    case LM_DOCKICON_DISK: {                            /* a pie with a slice pulled out: where the space went */
        float body = fmaxf2(sd_circle(u, v, 0, 0, 0.36f), fminf2(u, -v));
        float uo = u - 0.07f, vo = v + 0.07f;
        float slice = fmaxf2(sd_circle(uo, vo, 0, 0, 0.36f), fmaxf2(-uo, vo));
        return fminf2(body, slice);
    }
    case LM_DOCKICON_OVERVIEW: {                        /* four windows side by side: frames with a title bar each (Exposé) */
        float d = 1e9f;
        static const float cxs[4] = { -0.235f, 0.235f, -0.235f, 0.235f }, cys[4] = { -0.215f, -0.215f, 0.225f, 0.225f };
        static const float hws[4] = { 0.215f, 0.19f, 0.19f, 0.215f }, hhs[4] = { 0.17f, 0.17f, 0.17f, 0.17f };
        for (int k = 0; k < 4; k++) {
            float ring = sd_sub(sd_box(u, v, cxs[k], cys[k], hws[k], hhs[k], 0.05f), sd_box(u, v, cxs[k], cys[k], hws[k] - 0.045f, hhs[k] - 0.045f, 0.02f));
            d = fminf2(d, ring);
            d = fminf2(d, sd_box(u, v, cxs[k], cys[k] - hhs[k] + 0.085f, hws[k] - 0.05f, 0.022f, 0.01f));      /* the title bar */
        }
        return d;
    }
    case LM_DOCKICON_RUN:
        return sd_tri(u, v, -0.15f, -0.27f, -0.15f, 0.27f, 0.29f, 0.0f) - 0.05f;
    default: {                                          /* GENERIC: four rounded squares */
        float qx = fabsf(u) - 0.2f, qy = fabsf(v) - 0.2f;
        return sd_box(qx, qy, 0, 0, 0.14f, 0.14f, 0.05f);
    }
    }
}

static float cov(float sd, float px)
{
    return clampf(0.5f - sd / px, 0.0f, 1.0f);
}

/* The icon's outline, in units of R: the unit circle at roundness 100, a rounded square (half size 1) below. Negative inside. */
static float shape_sd(float u, float v)
{
    float rr = (float)lm_metrics.button_roundness / 100.0f;
    if (rr >= 0.999f)
        return hypotf(u, v) - 1.0f;
    return sd_box(u, v, 0.0f, 0.0f, 1.0f, 1.0f, rr);
}

float lm_dockicon_shape_coverage(float dx, float dy, float R)
{
    if (R <= 0.0f)
        return 0.0f;
    return cov(shape_sd(dx / R, dy / R), 1.0f / R);
}

uint32_t lm_dockicon_plate_pixel(uint32_t base, float dx, float dy, float R, float lift)
{
    if (R <= 0.0f)
        return 0;
    float u = dx / R, v = dy / R, px = 1.0f / R;
    float shadow = 0.0f;
    float sd = shape_sd(u, v - 0.07f);
    if (sd < 0.3f)
        shadow = 0.42f * (1.0f - clampf(sd / 0.3f, 0.0f, 1.0f)) * (1.0f - clampf(sd / 0.3f, 0.0f, 1.0f));
    float dc = cov(shape_sd(u, v), px);
    if (dc <= 0.0f && shadow <= 0.004f)
        return 0;
    float shade = 1.04f - 0.16f * (v * 0.5f + 0.5f);   /* a plain plate: a little lighter at the top, darker at the foot */
    float out[3];
    for (int i = 0; i < 3; i++) {
        float c = (float)((base >> (16 - 8 * i)) & 255) * shade;
        c += (255.0f - c) * 0.20f * lift;
        out[i] = clampf(c, 0.0f, 255.0f) * dc;
    }
    float a = dc + shadow * (1.0f - dc);
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | ((uint32_t)(out[0] + 0.5f) << 16) | ((uint32_t)(out[1] + 0.5f) << 8) |
           (uint32_t)(out[2] + 0.5f);
}

uint32_t lm_dockicon_pixel(int kind, float dx, float dy, float R, float lift)
{
    if (kind < 0 || kind >= LM_DOCKICON_COUNT || R <= 0.0f)
        return 0;
    float u = dx / R, v = dy / R, px = 1.0f / R;
    float shadow = 0.0f;
    float sd = shape_sd(u, v - 0.07f);                  /* the shadow: the shape moved a little down, fading out over 0.3 R */
    if (sd < 0.3f)
        shadow = 0.42f * (1.0f - clampf(sd / 0.3f, 0.0f, 1.0f)) * (1.0f - clampf(sd / 0.3f, 0.0f, 1.0f));
    float dc = cov(shape_sd(u, v), px);
    if (dc <= 0.0f && shadow <= 0.004f)
        return 0;

    uint32_t base = icon_color(kind);
    float shade = 1.14f - 0.28f * (v * 0.5f + 0.5f);   /* lighter at the top */
    float rgb[3];
    for (int i = 0; i < 3; i++) {
        float c = (float)((base >> (16 - 8 * i)) & 255) * shade;
        c += (255.0f - c) * 0.20f * lift;
        rgb[i] = clampf(c, 0.0f, 255.0f);
    }
    float g = dc > 0.0f ? cov(glyph(kind, u, v), px) : 0.0f;
    float a = dc + shadow * (1.0f - dc);
    float out[3];
    for (int i = 0; i < 3; i++)
        out[i] = (rgb[i] * (1.0f - g) + 255.0f * g) * dc;     /* premultiplied: the shadow is black and adds no colour */
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | ((uint32_t)(out[0] + 0.5f) << 16) | ((uint32_t)(out[1] + 0.5f) << 8) |
           (uint32_t)(out[2] + 0.5f);
}

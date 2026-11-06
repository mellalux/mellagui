/*
 * keymap_test: layout tables, XKB text generator and the client side keysym lookup (src/keymap.c).
 * Built as build/keymap-test (make), run by tests/test_lumen.py and by hand. No display needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cvt.h"
#include "edid.h"
#include "keymap.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static void expect_ks(const char *what, uint32_t code, uint32_t mods, uint32_t want)
{
    uint32_t got = lm_keymap_keysym(code, mods);
    CHECK(got == want, "%s: code %u mods 0x%x -> 0x%x, want 0x%x", what, code, mods, got, want);
}

static void load_keymap(const int *sel, int n)
{
    char *text = lm_keymap_build(sel, n);
    int fd = memfd_create("keymap-test", 0);
    size_t len = strlen(text) + 1;
    if (fd < 0 || write(fd, text, len) != (ssize_t)len) {
        perror("memfd");
        exit(2);
    }
    free(text);
    lm_keymap_client_load(fd, (uint32_t)len);     /* closes fd */
}

int main(void)
{
    char err[128];
    int l[LM_KBD_MAX_LAYOUTS];

    /* parsing */
    CHECK(lm_layout_parse("us,ee", l, err, sizeof(err)) == 2 && l[0] == 0 && l[1] == lm_layout_find("ee"), "parse us,ee");
    CHECK(lm_layout_parse(NULL, l, err, sizeof(err)) == 1 && l[0] == 0, "parse NULL = us");
    CHECK(lm_layout_parse("de, de ,fi", l, err, sizeof(err)) == 2, "duplicates dropped");
    CHECK(lm_layout_parse("us,xx", l, err, sizeof(err)) == 0 && strstr(err, "xx"), "unknown layout: %s", err);
    CHECK(lm_layout_parse("us,ee,de,fi,us", l, err, sizeof(err)) == 4, "5 entries with a duplicate still fit");
    CHECK(!lm_layout_needs_altgr((int[]){ 0 }, 1) && lm_layout_needs_altgr((int[]){ 1 }, 1) &&
          lm_layout_needs_altgr((int[]){ 0, 1 }, 2), "needs_altgr");

    /* before any keymap: US */
    expect_ks("a", 30, 0, 'a');
    expect_ks("A", 30, LM_MOD_SHIFT, 'A');
    expect_ks("caps a", 30, LM_MOD_CAPS, 'A');
    expect_ks("caps shift a", 30, LM_MOD_CAPS | LM_MOD_SHIFT, 'a');
    expect_ks("caps 1", 2, LM_MOD_CAPS, '1');
    expect_ks("at", 3, LM_MOD_SHIFT, '@');
    expect_ks("return", 28, 0, LM_KS_Return);
    expect_ks("F1", 59, 0, LM_KS_F1);
    expect_ks("kp numlock", 71, LM_MOD_NUM, '7');
    expect_ks("kp home", 71, 0, LM_KS_Home);

    /* generated text: structure */
    int all[4] = { 0, 1, 2, 3 };
    char *text = lm_keymap_build(all, 4);
    int depth = 0, ok = 1;
    for (const char *p = text; *p; p++) {
        depth += *p == '{';
        depth -= *p == '}';
        ok &= depth >= 0;
    }
    CHECK(ok && depth == 0, "unbalanced braces in the keymap (depth %d)", depth);
    CHECK(strstr(text, "name[Group4] = \"Finnish\";") && !strstr(text, "Group5"), "group names");
    CHECK(strstr(text, "key <AD11> { type[Group1] = \"TWO_LEVEL\", symbols[Group1] = [ U005B, U007B ], "
                       "type[Group2] = \"ALPHABETIC\", symbols[Group2] = [ U00FC, U00DC ]"),
          "AD11 row");
    CHECK(strstr(text, "type[Group2] = \"FOUR_LEVEL_ALPHABETIC\", symbols[Group2] = [ U0066, U0046, U0161, U0160 ]"),
          "Estonian s-caron on AltGr+f");
    CHECK(strstr(text, "ISO_Level3_Shift") && strstr(text, "modifier_map Mod5 { <RALT> };"), "AltGr key");
    CHECK(!strstr(text, "Alt_R, Meta_R ] };\n  key <KPMU>"), "no plain Alt_R key when AltGr is on");
    free(text);
    text = lm_keymap_build(all, 1);
    CHECK(strstr(text, "key <RALT> { [ Alt_R, Meta_R ] };") && strstr(text, "modifier_map Mod1 { <LALT>, <RALT> };") &&
          !strstr(text, "ISO_Level3_Shift ], virtualMods"), "lone us keeps right Alt = Alt");
    CHECK(!strstr(text, "FOUR_LEVEL\", symbols"), "lone us uses no third level");
    free(text);

    /* client lookup per group */
    load_keymap(all, 4);
    lm_keymap_client_group(0);
    expect_ks("us q", 16, 0, 'q');
    expect_ks("us y", 21, 0, 'y');
    lm_keymap_client_group(1);                              /* Estonian */
    expect_ks("ee u-diaeresis", 26, 0, 0xfc);
    expect_ks("ee U-diaeresis", 26, LM_MOD_SHIFT, 0xdc);
    expect_ks("ee o-tilde", 27, 0, 0xf5);
    expect_ks("ee O-tilde caps", 27, LM_MOD_CAPS, 0xd5);
    expect_ks("ee o-diaeresis", 39, 0, 0xf6);
    expect_ks("ee a-diaeresis", 40, 0, 0xe4);
    expect_ks("ee s-caron", 33, LM_MOD_ALTGR, 0x01000161);
    expect_ks("ee S-caron", 33, LM_MOD_ALTGR | LM_MOD_SHIFT, 0x01000160);
    expect_ks("ee S-caron caps", 33, LM_MOD_ALTGR | LM_MOD_CAPS, 0x01000160);
    expect_ks("ee z-caron", 44, LM_MOD_ALTGR, 0x0100017e);
    expect_ks("ee f", 33, 0, 'f');
    expect_ks("ee F", 33, LM_MOD_SHIFT, 'F');
    expect_ks("ee quotedbl", 3, LM_MOD_SHIFT, '"');
    expect_ks("ee at", 3, LM_MOD_ALTGR, '@');
    expect_ks("ee slash", 8, LM_MOD_SHIFT, '/');
    expect_ks("ee brace", 8, LM_MOD_ALTGR, '{');
    expect_ks("ee euro", 18, LM_MOD_ALTGR, 0x010020ac);
    expect_ks("ee altgr on a 2-level key is ignored", 16, LM_MOD_ALTGR, 'q');
    expect_ks("ee level 4 absent", 18, LM_MOD_ALTGR | LM_MOD_SHIFT, 0);
    expect_ks("ee return", 28, LM_MOD_ALTGR, LM_KS_Return);
    lm_keymap_client_group(2);                              /* German */
    expect_ks("de z", 21, 0, 'z');
    expect_ks("de y", 44, 0, 'y');
    expect_ks("de sharp s", 12, 0, 0xdf);
    expect_ks("de question", 12, LM_MOD_SHIFT, '?');
    expect_ks("de backslash", 12, LM_MOD_ALTGR, '\\');
    expect_ks("de at", 16, LM_MOD_ALTGR, '@');
    expect_ks("de a-diaeresis", 40, 0, 0xe4);
    expect_ks("de hash", 43, 0, '#');
    lm_keymap_client_group(3);                              /* Finnish */
    expect_ks("fi a-ring", 26, 0, 0xe5);
    expect_ks("fi A-ring", 26, LM_MOD_SHIFT, 0xc5);
    expect_ks("fi o-diaeresis", 39, 0, 0xf6);
    lm_keymap_client_group(0);
    expect_ks("back to us", 26, 0, '[');

    /* a single-layout keymap maps group 0 only */
    int ee_only[1] = { 1 };
    load_keymap(ee_only, 1);
    lm_keymap_client_group(0);
    expect_ks("ee-only o-tilde", 27, 0, 0xf5);
    lm_keymap_client_group(3);                              /* out of range wraps */
    expect_ks("wrapped group", 27, 0, 0xf5);

    /* UTF-8 of the results */
    char u[8];
    CHECK(lm_keysym_to_utf8(0x01000161, u) == 2 && !strcmp(u, "\xc5\xa1"), "utf8 of s-caron");
    CHECK(lm_keysym_to_utf8(0xf5, u) == 2 && !strcmp(u, "\xc3\xb5"), "utf8 of o-tilde");
    CHECK(lm_keysym_to_utf8(0x010020ac, u) == 3, "utf8 of euro");

    /* CVT reduced blanking: reference values from the VESA CVT 1.2 spec / well-known modelines */
    struct lm_cvt c;
    CHECK(lm_cvt_rb(1920, 1080, 60, &c) && c.clock_khz == 138500 && c.htotal == 2080 && c.vtotal == 1111 && c.vsync_end - c.vsync_start == 5,
          "cvt-rb 1920x1080: %u kHz %ux%u", c.clock_khz, c.htotal, c.vtotal);
    CHECK(lm_cvt_rb(2560, 1440, 60, &c) && c.clock_khz == 241500 && c.vtotal == 1481, "cvt-rb 2560x1440: %u kHz vtotal %u", c.clock_khz, c.vtotal);
    CHECK(lm_cvt_rb(1920, 1200, 60, &c) && c.clock_khz == 154000 && c.vtotal == 1235, "cvt-rb 1920x1200: %u kHz vtotal %u", c.clock_khz, c.vtotal);
    CHECK(lm_cvt_rb(1920, 810, 60, &c) && c.clock_khz > 100000 && c.clock_khz < 120000 && c.hsync_end - c.hsync_start == 32 &&
          c.vsync_end - c.vsync_start == 10, "cvt-rb 1920x810 (21:9): %u kHz", c.clock_khz);
    CHECK(lm_cvt_rb(1921, 1080, 60, &c) && c.hdisplay == 1920, "width rounded down to a multiple of 8");
    CHECK(!lm_cvt_rb(100, 100, 60, &c) && !lm_cvt_rb(1920, 1080, 5, &c), "absurd sizes/rates are refused");

    /* EDID: native shape vs the mode the kernel offers (first 128 bytes of a real 21:9 monitor, serial number removed) */
    static const uint8_t edid_21x9[128] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0xac, 0x18, 0x43, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x23, 0x01, 0x03, 0x80, 0x5d, 0x27, 0x78, 0x2a, 0xd9, 0xa5, 0xb0, 0x4f, 0x3d, 0xb1, 0x24, 0x0e, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0x71, 0x4f, 0x81, 0x00, 0x81, 0x80, 0xa9, 0x40, 0xb3, 0x00, 0xd1, 0xc0, 0xd1, 0x00, 0xe1, 0xc0, 0xd4, 0x46, 0x00, 0xa0, 0xa0, 0x38, 0x1f, 0x40, 0x30, 0x20, 0x3a, 0x00, 0xa1, 0x88, 0x31, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44, 0x45, 0x4c, 0x4c, 0x20, 0x55, 0x34, 0x30, 0x32, 0x35, 0x51, 0x57, 0x0a, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x78, 0x19, 0xff, 0x96, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x5b };
    struct lm_edid_native nat;
    CHECK(lm_edid_native(edid_21x9, sizeof(edid_21x9), &nat) && nat.w == 2560 && nat.h == 1080 && nat.mm_w == 929 && nat.mm_h == 392,
          "edid native: %dx%d %dx%d mm", nat.w, nat.h, nat.mm_w, nat.mm_h);
    CHECK(lm_edid_fix_height(2048, 1152, &nat, 930, 390) == 864, "21:9 panel offered 16:9 2048x1152 -> 2048x864 (%d)",
          lm_edid_fix_height(2048, 1152, &nat, 930, 390));
    CHECK(lm_edid_fix_height(1920, 1080, &nat, 0, 0) == 810, "same without physical size -> 1920x810");
    CHECK(lm_edid_fix_height(2560, 1080, &nat, 930, 390) == 0, "the native mode itself: untouched");
    CHECK(lm_edid_fix_height(2560, 1100, &nat, 930, 390) == 0, "within 8 percent of the native shape: untouched");
    struct lm_edid_native bare = { 2560, 1080, 0, 0 };
    CHECK(lm_edid_fix_height(2048, 1152, &bare, 930, 390) == 864, "native timing without its own size: the connector's size agrees");
    CHECK(lm_edid_fix_height(2048, 1152, &bare, 160, 90) == 0, "physical size contradicts the native timing (a broken EDID): untouched");
    CHECK(lm_edid_fix_height(2048, 1152, NULL, 930, 390) == 0, "no native timing: untouched");
    uint8_t other[128];
    memcpy(other, edid_21x9, sizeof(other));
    other[54 + 2] = 0x80; other[54 + 4] = 0x70;               /* 1920 wide ... */
    other[54 + 5] = 0x38; other[54 + 7] = 0x40;               /* ... 1080 high: a plain 16:9 monitor */
    other[54 + 12] = 0x56; other[54 + 13] = 0x50; other[54 + 14] = 0x21;    /* 598 x 336 mm */
    CHECK(lm_edid_native(other, sizeof(other), &nat) && nat.w == 1920 && nat.h == 1080 && nat.mm_w == 598 && nat.mm_h == 336, "edid native (patched): %dx%d mm %dx%d", nat.w, nat.h, nat.mm_w, nat.mm_h);
    CHECK(lm_edid_fix_height(1920, 1080, &nat, 0, 0) == 0, "16:9 monitor with 16:9 mode: untouched");
    CHECK(lm_edid_fix_height(1600, 1200, &nat, 0, 0) == 900, "16:9 monitor offered only 4:3 -> 1600x900 (%d)",
          lm_edid_fix_height(1600, 1200, &nat, 0, 0));
    memset(other, 0, 8);
    CHECK(!lm_edid_native(other, sizeof(other), &nat), "bad header is refused");
    memcpy(other, edid_21x9, sizeof(other));
    other[54] = other[55] = 0;
    CHECK(!lm_edid_native(other, sizeof(other), &nat), "a monitor descriptor is not a timing");

    if (!fails)
        puts("keymap-test ok");
    return fails ? 1 : 0;
}

/*
 * keymap_test: layout tables, XKB text generator and the client side keysym lookup (src/keymap.c).
 * Built as build/keymap-test (make), run by tests/test_mellagui.py and by hand. No display needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cvt.h"
#include "ddc.h"
#include "edid.h"
#include "keymap.h"
#include "audio.h"
#include "mixer.h"
#include "sysinfo.h"
#include "sha256.h"

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
    /* the 21:9 panel behind an HDMI link that takes 156.75 MHz (its largest listed mode, 2048x1152@60): 2560x1080 only at 50 Hz */
    CHECK(lm_cvt_rb_fit(2560, 1080, 60, 156750, &c) && c.refresh == 50 && c.clock_khz == 150250, "fit 2560x1080: %u Hz %u kHz", c.refresh, c.clock_khz);
    CHECK(lm_cvt_rb_fit(2560, 1080, 60, 200000, &c) && c.refresh == 60, "a faster link keeps 60 Hz");
    CHECK(!lm_cvt_rb_fit(2560, 1080, 60, 50000, &c), "nothing fits a 50 MHz link");

    /* EDID: native shape vs the mode the kernel offers (first 128 bytes of a real 21:9 monitor, serial number removed) */
    static const uint8_t edid_21x9[128] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0xac, 0x18, 0x43, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x23, 0x01, 0x03, 0x80, 0x5d, 0x27, 0x78, 0x2a, 0xd9, 0xa5, 0xb0, 0x4f, 0x3d, 0xb1, 0x24, 0x0e, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0x71, 0x4f, 0x81, 0x00, 0x81, 0x80, 0xa9, 0x40, 0xb3, 0x00, 0xd1, 0xc0, 0xd1, 0x00, 0xe1, 0xc0, 0xd4, 0x46, 0x00, 0xa0, 0xa0, 0x38, 0x1f, 0x40, 0x30, 0x20, 0x3a, 0x00, 0xa1, 0x88, 0x31, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44, 0x45, 0x4c, 0x4c, 0x20, 0x55, 0x34, 0x30, 0x32, 0x35, 0x51, 0x57, 0x0a, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x78, 0x19, 0xff, 0x96, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x5b };
    struct lm_edid_native nat;
    /* DDC/CI packets (the bytes a real monitor accepted / sent) */
    {
        uint8_t req[7];
        CHECK(lm_ddc_build_get(LM_VCP_BRIGHTNESS, req) == 5 && req[0] == 0x51 && req[1] == 0x82 && req[2] == 0x01 && req[3] == 0x10 && req[4] == 0xac,
              "ddc get request: %02x %02x %02x %02x %02x", req[0], req[1], req[2], req[3], req[4]);
        CHECK(lm_ddc_build_set(LM_VCP_BRIGHTNESS, 50, req) == 7 && req[1] == 0x84 && req[2] == 0x03 && req[4] == 0x00 && req[5] == 0x32 && req[6] == 0x9a,
              "ddc set request: checksum %02x", req[6]);
        static const uint8_t ok[11] = { 0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x30, 0xf0 };
        struct lm_ddc_vcp v;
        CHECK(lm_ddc_parse_get(ok, sizeof(ok), 0x10, &v) == LM_DDC_OK && v.max == 100 && v.cur == 48, "ddc reply: max %d cur %d", v.max, v.cur);
        CHECK(lm_ddc_parse_get(ok, sizeof(ok), 0x12, &v) == LM_DDC_BAD, "ddc reply for another code");
        uint8_t bad[11];
        memcpy(bad, ok, sizeof(bad));
        bad[10] ^= 1;
        CHECK(lm_ddc_parse_get(bad, sizeof(bad), 0x10, &v) == LM_DDC_BAD, "ddc reply with a wrong checksum");
        CHECK(lm_ddc_parse_get(ok, 5, 0x10, &v) == LM_DDC_BAD, "ddc short reply");
        /* capabilities: the string a Dell U4025QW sent (shortened), and a chunk reply */
        static const char caps[] = "(prot(monitor)type(LCD)model(U4025QW)cmds(01 02 03 07 0C E3 F3)vcp(02 04 10 12 14(01 04 05 06 08 09 0B 0C) 16 60(19 0F 11 ) 62 66(00F2) AC D6(01 04 05) DC(00 03 05 ))mswhql(1)mccs_ver(2.1))";
        int vals[16];
        CHECK(lm_ddc_caps_has(caps, 0x10) && lm_ddc_caps_has(caps, 0x62) && lm_ddc_caps_has(caps, 0xac) && lm_ddc_caps_has(caps, 0xd6),
              "caps: brightness, volume ... listed");
        CHECK(!lm_ddc_caps_has(caps, 0x8d) && !lm_ddc_caps_has(caps, 0x11) && !lm_ddc_caps_has(caps, 0x01), "caps: mute is not listed");
        int n = lm_ddc_caps_values(caps, 0x60, vals, 16);
        CHECK(n == 3 && vals[0] == 0x19 && vals[1] == 0x0f && vals[2] == 0x11, "caps: input values %d: %x %x %x", n, vals[0], vals[1], vals[2]);
        n = lm_ddc_caps_values(caps, 0x14, vals, 16);
        CHECK(n == 8 && vals[0] == 1 && vals[7] == 0x0c, "caps: preset values %d", n);
        CHECK(lm_ddc_caps_values(caps, 0x10, vals, 16) == 0 && lm_ddc_caps_values(caps, 0x99, vals, 16) == 0, "caps: no value list");
        CHECK(!lm_ddc_caps_has("", 0x10) && !lm_ddc_caps_has(NULL, 0x10) && !lm_ddc_caps_has("(prot(monitor))", 0x10), "caps: nothing to find");
        {
            uint8_t chunk[16] = { 0x6e, 0x80 | 8, 0xe3, 0x00, 0x20, '(', 'p', 'r', 'o', 't' }, req6[6];
            uint8_t ck = 0x50;
            for (int i = 0; i < 10; i++)
                ck ^= chunk[i];
            chunk[10] = ck;
            char data[40];
            CHECK(lm_ddc_parse_caps(chunk, 11, 0x20, data, sizeof(data)) == 5 && !memcmp(data, "(prot", 5), "caps chunk");
            CHECK(lm_ddc_parse_caps(chunk, 11, 0x40, data, sizeof(data)) == -1, "caps chunk at another offset");
            chunk[10] ^= 1;
            CHECK(lm_ddc_parse_caps(chunk, 11, 0x20, data, sizeof(data)) == -1, "caps chunk with a wrong checksum");
            CHECK(lm_ddc_build_caps(0x20, req6) == 6 && req6[1] == 0x83 && req6[2] == 0xf3 && req6[3] == 0 && req6[4] == 0x20 &&
                  req6[5] == (uint8_t)(0x6e ^ 0x51 ^ 0x83 ^ 0xf3 ^ 0x20), "caps request");
        }
        static const uint8_t unsup[11] = { 0x6e, 0x88, 0x02, 0x01, 0x99, 0, 0, 0, 0, 0, 0x2c };
        CHECK(lm_ddc_parse_get(unsup, sizeof(unsup), 0x99, &v) == LM_DDC_UNSUPPORTED, "ddc unsupported feature");
    }
    struct lm_edid_info einfo;
    CHECK(lm_edid_info(edid_21x9, sizeof(edid_21x9), &einfo) && !strcmp(einfo.vendor, "DEL") && !strcmp(einfo.name, "DELL U4025QW") &&
          einfo.product == 0x4318 && einfo.year == 2025 && einfo.mm_w == 930 && einfo.mm_h == 390,
          "edid info: '%s' '%s' product %x year %d %dx%d mm", einfo.vendor, einfo.name, einfo.product, einfo.year, einfo.mm_w, einfo.mm_h);
    CHECK(!strcmp(lm_edid_vendor_name("DEL"), "Dell") && !strcmp(lm_edid_vendor_name("ZZZ"), "ZZZ"), "edid vendor names");
    CHECK(!lm_edid_info((const uint8_t *)"not an edid block", 18, &einfo), "edid info: not EDID");
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

    /* wpctl status (PipeWire without pactl): the sinks of the Audio section, the default one marked with * */
    {
        static const char *wp =
            "PipeWire 'pipewire-0' [1.0.5, mella@host, cookie:123]\n \xe2\x94\x94\xe2\x94\x80 Clients:\n        33. WirePlumber                         [1.0.5, mella@host, pid:1]\n\n"
            "Audio\n \xe2\x94\x9c\xe2\x94\x80 Devices:\n \xe2\x94\x82      49. Built-in Audio                      [alsa]\n \xe2\x94\x82  \n"
            " \xe2\x94\x9c\xe2\x94\x80 Sinks:\n \xe2\x94\x82  *   53. Built-in Audio Analog Stereo        [vol: 0.40]\n \xe2\x94\x82      54. Built-in Audio Digital Stereo (HDMI) [vol: 1.00 MUTED]\n"
            " \xe2\x94\x82  \n \xe2\x94\x9c\xe2\x94\x80 Sources:\n \xe2\x94\x82  *   55. Built-in Audio Analog Stereo        [vol: 1.00]\n"
            " \xe2\x94\x94\xe2\x94\x80 Streams:\n\nVideo\n \xe2\x94\x9c\xe2\x94\x80 Sinks:\n \xe2\x94\x82      99. Not audio\n";
        char list[512], cur[16];
        int n = lm_audio_parse_wpctl_sinks(wp, list, sizeof(list), cur, sizeof(cur));
        CHECK(n == 2, "wpctl sinks: %d (%s)", n, list);
        CHECK(!strcmp(cur, "53"), "wpctl default sink %s", cur);
        CHECK(!strcmp(list, "53|Built-in Audio Analog Stereo;54|Built-in Audio Digital Stereo (HDMI)"), "wpctl list: %s", list);
        CHECK(lm_audio_parse_wpctl_sinks("", list, sizeof(list), cur, sizeof(cur)) == 0 && !list[0] && !cur[0], "empty wpctl output");
    }

    /* PipeWire card profiles: real `pw-cli` output of an Intel HDA card (trimmed to a few profiles) */
    {
        static const char *pw_ls =
        "	id 40, type PipeWire:Interface:Device/3\n"
        " 		device.description = \"0825\"\n"
        " 		device.name = \"v4l2_device.pci-0000_00_14.0-usb-0_1.4.1.1_1.0\"\n"
        " 		media.class = \"Video/Device\"\n"
        "	id 42, type PipeWire:Interface:Device/3\n"
        " 		device.description = \"Webcam C270\"\n"
        " 		device.name = \"alsa_card.usb-046d_0825_3F3A0E40-02\"\n"
        " 		device.nick = \"USB Device 0x46d:0x825\"\n"
        " 		media.class = \"Audio/Device\"\n"
        "	id 43, type PipeWire:Interface:Device/3\n"
        " 		device.description = \"Built-in Audio\"\n"
        " 		device.name = \"alsa_card.pci-0000_00_1b.0\"\n"
        " 		device.nick = \"HDA Intel PCH\"\n"
        " 		media.class = \"Audio/Device\"\n"
        ;
        static const char *pw_enum =
        "  Object: size 160, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 0\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"off\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Off\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 0\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 16\n"
        "        Int 0\n"
        "  Object: size 424, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 1\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:analog-stereo+input:analog-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Analog Stereo Duplex\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 6565\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 1        (Spa:Enum:ParamAvailability:no)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 0\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 9\n"
        "  Object: size 456, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 2\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:analog-stereo+input:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Analog Stereo Output + Digital Stereo (IEC958) Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 6555\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 8\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 9\n"
        "  Object: size 296, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 3\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:analog-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Analog Stereo Output\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 6500\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 120\n"
        "        Int 1\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 9\n"
        "  Object: size 448, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 4\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:hdmi-stereo+input:analog-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (HDMI) Output + Analog Stereo Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5965\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 1        (Spa:Enum:ParamAvailability:no)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 0\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 11\n"
        "  Object: size 456, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 5\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:hdmi-stereo+input:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (HDMI) Output + Digital Stereo (IEC958) Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5955\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 8\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 11\n"
        "  Object: size 304, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 6\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:hdmi-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (HDMI) Output\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5900\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 120\n"
        "        Int 1\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 11\n"
        "  Object: size 312, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 11\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:hdmi-stereo-extra1\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (HDMI 2) Output\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5700\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 1        (Spa:Enum:ParamAvailability:no)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 120\n"
        "        Int 1\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 12\n"
        "  Object: size 432, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 14\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:iec958-stereo+input:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo Duplex (IEC958)\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5555\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 8\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 10\n"
        "  Object: size 304, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 15\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (IEC958) Output\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 5500\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 120\n"
        "        Int 1\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 10\n"
        "  Object: size 464, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 16\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:hdmi-surround-extra1+input:analog-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Surround 5.1 (HDMI 2) Output + Analog Stereo Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 665\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 1        (Spa:Enum:ParamAvailability:no)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 0\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 13\n"
        "  Object: size 304, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 29\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"input:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Digital Stereo (IEC958) Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 55\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 120\n"
        "        Int 1\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 8\n"
        "  Object: size 400, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:EnumProfile (8)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 30\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"pro-audio\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Pro Audio\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 1\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 0        (Spa:Enum:ParamAvailability:unknown)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 240\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 2\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 6\n"
        "            Int 7\n"
        "        Struct: size 112\n"
        "          String \"Audio/Sink\"\n"
        "          Int 5\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 1\n"
        "            Int 2\n"
        "            Int 3\n"
        "            Int 4\n"
        "            Int 5\n"
        ;
        static const char *pw_cur =
        "  Object: size 480, type Spa:Pod:Object:Param:Profile (262151), id Spa:Enum:ParamId:Profile (9)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:index (1), flags 00000000\n"
        "      Int 2\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:name (2), flags 00000000\n"
        "      String \"output:analog-stereo+input:iec958-stereo\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:description (3), flags 00000000\n"
        "      String \"Analog Stereo Output + Digital Stereo (IEC958) Input\"\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:priority (4), flags 00000000\n"
        "      Int 6555\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:available (5), flags 00000000\n"
        "      Id 2        (Spa:Enum:ParamAvailability:yes)\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:classes (7), flags 00000000\n"
        "      Struct: size 224\n"
        "        Int 2\n"
        "        Struct: size 96\n"
        "          String \"Audio/Source\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 8\n"
        "        Struct: size 96\n"
        "          String \"Audio/Sink\"\n"
        "          Int 1\n"
        "          String \"card.profile.devices\"\n"
        "          Array: child.size 4, child.type Spa:Int\n"
        "            Int 9\n"
        "    Prop: key Spa:Pod:Object:Param:Profile:save (8), flags 00000000\n"
        "      Bool false\n"
        ;
        struct lm_audio_card cards[4];
        int ncards = lm_audio_parse_cards(pw_ls, cards, 4);
        CHECK(ncards == 2 && cards[0].id == 42 && !strcmp(cards[0].desc, "Webcam C270") && cards[1].id == 43 && !strcmp(cards[1].desc, "Built-in Audio"),
              "cards: %d (the video devices are not audio cards)", ncards);
        static struct lm_audio_profile pr[32];
        int np = lm_audio_parse_profiles(pw_enum, pr, 32);
        CHECK(np == 13, "profiles: %d", np);
        CHECK(pr[0].index == 0 && !strcmp(pr[0].name, "off") && pr[0].available, "profile 0 is off, available");
        CHECK(pr[1].index == 1 && !pr[1].available && !strcmp(pr[1].desc, "Analog Stereo Duplex"), "profile 1: unavailable analog duplex");
        int cur = lm_audio_parse_current_profile(pw_cur);
        CHECK(cur == 2, "current profile %d", cur);
        int picked[8], cp;
        int m = lm_audio_pick_profiles(pr, np, cur, picked, 8, &cp);
        CHECK(m == 3, "outputs offered: %d (analog, hdmi, iec958; hdmi 2 is unplugged, surround is skipped)", m);
        CHECK(!strcmp(pr[picked[0]].name, "output:analog-stereo") && !strcmp(pr[picked[1]].name, "output:hdmi-stereo") &&
              !strcmp(pr[picked[2]].name, "output:iec958-stereo"), "pure output profiles are preferred: %s %s %s", pr[picked[0]].name, pr[picked[1]].name, pr[picked[2]].name);
        CHECK(cp == 0, "the active output type (analog) is the current one: %d", cp);
        CHECK(lm_audio_pick_profiles(pr, 0, -1, picked, 8, &cp) == 0 && cp == -1, "no profiles");
        CHECK(lm_audio_parse_cards("", cards, 4) == 0 && lm_audio_parse_profiles("", pr, 32) == 0 && lm_audio_parse_current_profile("") == -1, "empty output");
    }

    /* mixer: parsers of pactl / amixer output, layout and hit testing */
    static const char *sinks_text =
        "Sink #47\n\tState: SUSPENDED\n\tName: alsa_output.pci-0000_00_1b.0.analog-stereo\n\tDescription: Built-in Audio Analog Stereo\n"
        "\tMute: no\n\tVolume: front-left: 41943 /  64% / -11.14 dB,   front-right: 41943 /  64% / -11.14 dB\n\t        balance 0.00\n"
        "\tBase Volume: 65536 / 100% / 0.00 dB\nSink #48\n\tName: alsa_output.hdmi-stereo\n\tDescription: Built-in Audio Digital Stereo (HDMI)\n"
        "\tMute: yes\n\tVolume: front-left: 65536 / 100% / 0.00 dB\n\tBase Volume: 1 / 7% / 0 dB\nSink #49\n\tState: x\n";
    struct lm_mixer_sink sk[4];
    int ns = lm_mixer_parse_sinks(sinks_text, sk, 4);
    CHECK(ns == 2, "sinks: %d (a sink without a name is dropped)", ns);
    CHECK(!strcmp(sk[0].name, "alsa_output.pci-0000_00_1b.0.analog-stereo") && !strcmp(sk[0].desc, "Built-in Audio Analog Stereo") &&
          sk[0].volume == 64 && !sk[0].mute, "sink 0: %s %d", sk[0].desc, sk[0].volume);
    CHECK(sk[1].volume == 100 && sk[1].mute && strstr(sk[1].desc, "HDMI"), "sink 1: base volume ignored, muted");
    static const char *inputs_text =
        "Sink Input #61\n\tDriver: PipeWire\n\tSink: 47\n\tMute: no\n\tVolume: front-left: 65536 / 100% / 0.00 dB,   front-right: 65536 / 100% / 0.00 dB\n"
        "\tProperties:\n\t\tmedia.name = \"AudioStream\"\n\t\tapplication.name = \"Firefox\"\n"
        "Sink Input #62\n\tMute: yes\n\tVolume: front-left: 19660 /  30% / -31 dB\n\tProperties:\n\t\tmedia.name = \"Music\"\n";
    struct lm_mixer_stream st[4];
    int nst = lm_mixer_parse_streams(inputs_text, st, 4);
    CHECK(nst == 2 && st[0].id == 61 && !strcmp(st[0].name, "Firefox") && st[0].volume == 100 && !st[0].mute, "stream 0: %s", st[0].name);
    CHECK(st[1].id == 62 && !strcmp(st[1].name, "Music") && st[1].volume == 30 && st[1].mute, "stream 1 falls back to media.name: %s", st[1].name);
    CHECK(lm_mixer_parse_streams("", st, 4) == 0 && lm_mixer_parse_sinks("", sk, 4) == 0, "empty output");
    int av = -1;
    bool am = true;
    CHECK(lm_mixer_parse_wpctl("Volume: 0.64\n", &av, &am) && av == 64 && !am, "wpctl: %d", av);
    CHECK(lm_mixer_parse_wpctl("Volume: 0.30 [MUTED]\n", &av, &am) && av == 30 && am, "wpctl muted");
    CHECK(lm_mixer_parse_wpctl("Volume: 1.00\n", &av, &am) && av == 100 && !am, "wpctl 100");
    CHECK(!lm_mixer_parse_wpctl("Could not connect to PipeWire\n", &av, &am), "wpctl without PipeWire");
    av = -1;
    am = true;
    CHECK(lm_mixer_parse_amixer("Simple mixer control 'Master',0\n  Front Left: Playback 40 [75%] [-18.00dB] [on]\n  Front Right: Playback 40 [75%] [-18.00dB] [on]\n",
                                &av, &am) && av == 75 && !am, "amixer on: %d", av);
    CHECK(lm_mixer_parse_amixer("  Mono: Playback 0 [0%] [off]\n", &av, &am) && av == 0 && am, "amixer off");
    CHECK(!lm_mixer_parse_amixer("amixer: Unable to find simple control 'Master',0\n", &av, &am), "amixer without Master");
    CHECK(lm_mixer_slider_value(100, 200, 100) == 0 && lm_mixer_slider_value(100, 200, 300) == 100 &&
          lm_mixer_slider_value(100, 200, 200) == 50 && lm_mixer_slider_value(100, 200, 40) == 0 && lm_mixer_slider_value(100, 200, 999) == 100,
          "slider value");
    {
        struct lm_rect card = lm_rect_make(50, 50, 300, 300);
        struct lm_mixer_layout l;
        lm_mixer_layout(&card, 32, 4, true, &l);
        CHECK(l.nstreams == 4 && l.output.w > 0 && l.master_slider.w > 100, "layout: %d stream rows", l.nstreams);
        int x = l.master_slider.x + 10, y = l.master_slider.y + 5, idx;
        CHECK(lm_mixer_hit(&l, x, y, &idx) == LM_MIXER_MASTER_SLIDER, "hit master slider");
        CHECK(lm_mixer_hit(&l, l.master_mute.x + 3, l.master_mute.y + 3, &idx) == LM_MIXER_MASTER_MUTE, "hit master mute");
        CHECK(lm_mixer_hit(&l, l.output.x + 5, l.output.y + 5, &idx) == LM_MIXER_OUTPUT, "hit output");
        CHECK(lm_mixer_hit(&l, l.stream[2].slider.x + 4, l.stream[2].slider.y + 4, &idx) == LM_MIXER_STREAM_SLIDER && idx == 2, "hit stream 2 slider");
        CHECK(lm_mixer_hit(&l, l.stream[1].mute.x + 2, l.stream[1].mute.y + 2, &idx) == LM_MIXER_STREAM_MUTE && idx == 1, "hit stream 1 mute");
        CHECK(lm_mixer_hit(&l, 60, card.y + 5, &idx) == LM_MIXER_NOTHING, "title bar is not a control");
        struct lm_rect small = lm_rect_make(0, 0, 300, 140);
        lm_mixer_layout(&small, 32, 4, true, &l);
        CHECK(l.nstreams < 4 && l.nstreams >= 0, "a small card shows fewer rows (%d)", l.nstreams);
        lm_mixer_layout(&card, 32, 0, false, &l);
        CHECK(l.output.w == 0 && l.nstreams == 0, "no output row without pulse");
    }

    /* system monitor: /proc parsers and the temperature sensor search */
    {
        struct lm_cpu_times t1, t2, c1[4], c2[4];
        int n1 = lm_sysinfo_parse_stat("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 60 0 20 400 20 0 0 0 0 0\ncpu1 40 0 30 400 30 0 0 0 0 0\nintr 5\n", &t1, c1, 4);
        CHECK(n1 == 2 && t1.total == 1000 && t1.busy == 150, "stat: %d cores total %llu busy %llu", n1, t1.total, t1.busy);
        lm_sysinfo_parse_stat("cpu  300 0 150 1400 150 0 0 0 0 0\ncpu0 160 0 70 700 70 0 0 0 0 0\ncpu1 140 0 80 700 80 0 0 0 0 0\n", &t2, c2, 4);
        CHECK(lm_sysinfo_cpu_percent(&t1, &t2) == 30, "cpu percent %d (busy +300 of +1000)", lm_sysinfo_cpu_percent(&t1, &t2));
        CHECK(lm_sysinfo_cpu_percent(&t2, &t2) == 0 && lm_sysinfo_cpu_percent(&t2, &t1) == 0, "no time passed / went backwards: 0");
        struct lm_cpu_times idle_a = { 10, 1000 }, idle_b = { 10, 2000 }, full_a = { 0, 0 }, full_b = { 500, 500 };
        CHECK(lm_sysinfo_cpu_percent(&idle_a, &idle_b) == 0 && lm_sysinfo_cpu_percent(&full_a, &full_b) == 100, "0 and 100 percent");
        long tot, av, st, sf;
        CHECK(lm_sysinfo_parse_meminfo("MemTotal:       16281472 kB\nMemFree:         1309544 kB\nMemAvailable:   13637628 kB\nSwapTotal: 2097148 kB\nSwapFree: 2097100 kB\n",
                                       &tot, &av, &st, &sf) && tot == 16281472 && av == 13637628 && st == 2097148 && sf == 2097100, "meminfo");
        CHECK(lm_sysinfo_parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 250 kB\n", &tot, &av, &st, &sf) && av == 400 && st == 0,
              "meminfo without MemAvailable: %ld", av);
        CHECK(!lm_sysinfo_parse_meminfo("nothing\n", &tot, &av, &st, &sf), "meminfo without MemTotal");
        char devbuf[64];
        CHECK(lm_sysinfo_parse_cpu_model("processor\t: 0\nmodel name\t: Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz\n", devbuf, sizeof(devbuf)) &&
              !strcmp(devbuf, "Intel Core i5-8250U @ 1.60GHz"), "cpu model: '%s'", devbuf);
        CHECK(lm_sysinfo_parse_cpu_model("Hardware\t: BCM2835\nModel\t\t: Raspberry Pi 4\n", devbuf, sizeof(devbuf)) && !strcmp(devbuf, "Raspberry Pi 4"),
              "arm cpuinfo: '%s'", devbuf);
        CHECK(!lm_sysinfo_parse_cpu_model("flags : fpu\n", devbuf, sizeof(devbuf)), "cpuinfo without a model");
        CHECK(lm_sysinfo_parse_os_name("NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 24.04.2 LTS\"\n", devbuf, sizeof(devbuf)) && !strcmp(devbuf, "Ubuntu 24.04.2 LTS"),
              "os-release: '%s'", devbuf);
        CHECK(lm_sysinfo_parse_os_name("NAME=Alpine\n", devbuf, sizeof(devbuf)) && !strcmp(devbuf, "Alpine"), "os-release NAME only");
        CHECK(lm_sysinfo_parse_uptime("12345.67 9999.1\n") == 12345 && lm_sysinfo_parse_uptime("x") == -1, "uptime");
        lm_sysinfo_format_uptime(3 * 86400 + 4 * 3600 + 5, devbuf, sizeof(devbuf));
        CHECK(!strcmp(devbuf, "3d 4h"), "uptime days: '%s'", devbuf);
        lm_sysinfo_format_uptime(5 * 3600 + 12 * 60, devbuf, sizeof(devbuf));
        CHECK(!strcmp(devbuf, "5h 12m"), "uptime hours: '%s'", devbuf);
        lm_sysinfo_format_uptime(7 * 60 + 9, devbuf, sizeof(devbuf));
        CHECK(!strcmp(devbuf, "7m"), "uptime minutes: '%s'", devbuf);
        char dir[] = "/tmp/mellagui-sys-XXXXXX";
        CHECK(mkdtemp(dir) != NULL, "mkdtemp");
        char cmd[512], label[48];
        int deg = 0;
        snprintf(cmd, sizeof(cmd), "mkdir -p %s/sys/class/hwmon/hwmon0 %s/sys/class/hwmon/hwmon1 %s/sys/class/thermal/thermal_zone0", dir, dir, dir);
        CHECK(system(cmd) == 0, "mkdir tree");
        CHECK(!lm_sysinfo_read_temp(dir, &deg, label, sizeof(label)), "no sensors at all");
        snprintf(cmd, sizeof(cmd), "cd %s/sys/class && echo 'hidpp_battery_55' > hwmon/hwmon1/name && echo 99000 > hwmon/hwmon1/temp1_input && "
                 "echo x86_pkg_temp > thermal/thermal_zone0/type && echo 61500 > thermal/thermal_zone0/temp", dir);
        CHECK(system(cmd) == 0, "write thermal zone");
        CHECK(lm_sysinfo_read_temp(dir, &deg, label, sizeof(label)) && deg == 62 && !strcmp(label, "x86_pkg_temp"),
              "thermal zone of a CPU type (a battery hwmon is not a CPU): %d %s", deg, label);
        snprintf(cmd, sizeof(cmd), "cd %s/sys/class/hwmon/hwmon0 && echo coretemp > name && echo 72000 > temp2_input && echo 'Core 0' > temp2_label && "
                 "echo 81000 > temp1_input && echo 'Package id 0' > temp1_label && echo 75000 > temp3_input && echo 'Core 1' > temp3_label", dir);
        CHECK(system(cmd) == 0, "write coretemp");
        CHECK(lm_sysinfo_read_temp(dir, &deg, label, sizeof(label)) && deg == 81 && strstr(label, "coretemp") && strstr(label, "Package"),
              "coretemp package sensor wins: %d %s", deg, label);
        snprintf(cmd, sizeof(cmd), "rm -f %s/sys/class/hwmon/hwmon0/temp1_label && echo 70000 > %s/sys/class/hwmon/hwmon0/temp1_input", dir, dir);
        CHECK(system(cmd) == 0, "unlabel");
        CHECK(lm_sysinfo_read_temp(dir, &deg, label, sizeof(label)) && deg == 75, "no package label: the hottest input (%d)", deg);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        CHECK(system(cmd) == 0, "cleanup");
    }

    /* SHA-256, HMAC and PBKDF2 (the screen lock keeps the password only as a PBKDF2 hash): published test vectors */
    {
        uint8_t d[40];
        char hex[2 * sizeof(d) + 1];
        lm_sha256("abc", 3, d);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc: %s", hex);
        lm_sha256("", 0, d);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty: %s", hex);
        const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";    /* crosses a block boundary */
        lm_sha256(two, strlen(two), d);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 448 bits: %s", hex);
        uint8_t key[20];
        memset(key, 0x0b, sizeof(key));
        lm_hmac_sha256(key, sizeof(key), "Hi There", 8, d);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "hmac rfc4231 #1: %s", hex);
        lm_pbkdf2_sha256("password", 8, "salt", 4, 1, d, 32);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"), "pbkdf2 1: %s", hex);
        lm_pbkdf2_sha256("password", 8, "salt", 4, 2, d, 32);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"), "pbkdf2 2: %s", hex);
        lm_pbkdf2_sha256("password", 8, "salt", 4, 4096, d, 32);
        lm_hex_encode(d, 32, hex);
        CHECK(!strcmp(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"), "pbkdf2 4096: %s", hex);
        lm_pbkdf2_sha256("passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, d, 40);
        lm_hex_encode(d, 40, hex);
        CHECK(!strcmp(hex, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"), "pbkdf2 long: %s", hex);
        uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 5 };
        CHECK(lm_ct_equal(a, a, 4) && !lm_ct_equal(a, b, 4), "ct_equal");
        uint8_t back[3];
        CHECK(lm_hex_decode("00ff7A", back, 3) && back[0] == 0 && back[1] == 0xff && back[2] == 0x7a, "hex decode");
        CHECK(!lm_hex_decode("00ff7", back, 3) && !lm_hex_decode("00ff7g", back, 3), "hex decode rejects bad input");
    }

    if (!fails)
        puts("keymap-test ok");
    return fails ? 1 : 0;
}

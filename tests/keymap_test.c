/*
 * keymap_test: layout tables, XKB text generator and the client side keysym lookup (src/keymap.c).
 * Built as build/keymap-test (make), run by tests/test_lumen.py and by hand. No display needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

    if (!fails)
        puts("keymap-test ok");
    return fails ? 1 : 0;
}

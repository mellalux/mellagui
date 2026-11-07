/*
 * mellagui-notify - show a notification card (top right of the screen).
 *
 *   mellagui-notify [-t MILLISECONDS] TITLE [BODY...]    -t 0 = stays until it is clicked; default 6000
 *
 * It sets the write-only `notify` key of the compositor's private mellagui_settings_v1 interface, so (like the settings panel) it is
 * only served to MellaGUI's own programs, i.e. the executables next to the compositor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "util.h"
#include "wlwin.h"

struct app {
    struct wlwin win;
    uint32_t settings;
};

static void paint_unused(struct wlwin *w) { (void)w; }

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;            /* the value bursts are of no interest here */
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    (void)ver;
    struct app *a = w->data;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
}

int main(int argc, char **argv)
{
    int ms = 6000, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-t")) {
        ms = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc || ms < 0) {
        fprintf(stderr, "usage: mellagui-notify [-t MILLISECONDS] TITLE [BODY...]\n");
        return 2;
    }
    char title[128], body[256] = "", msg[512];
    snprintf(title, sizeof(title), "%s", argv[i++]);
    for (; i < argc; i++) {
        size_t l = strlen(body);
        snprintf(body + l, sizeof(body) - l, "%s%s", l ? " " : "", argv[i]);
    }
    for (char *p = title; *p; p++)
        if ((unsigned char)*p < 0x20)
            *p = ' ';
    for (char *p = body; *p; p++)
        if (*p == '\t' || *p == '\r')
            *p = ' ';
    snprintf(msg, sizeof(msg), "%s\t%s\t%d", title, body, ms);

    static const struct wlwin_ops ops = { .paint = paint_unused, .global = global_event };
    struct app a = { 0 };
    if (wlwin_connect(&a.win, "mellagui-notify", &ops, &a) < 0)
        return 1;
    if (!a.settings) {
        fprintf(stderr, "mellagui-notify: the compositor has no mellagui_settings_v1 (or it is not served to this program)\n");
        wlwin_destroy(&a.win);
        return 1;
    }
    WLC_SEND(a.win.c, a.settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "notify" }, { .s = msg });
    int rc = wlc_roundtrip(a.win.c) < 0 ? 1 : 0;
    wlwin_destroy(&a.win);
    return rc;
}

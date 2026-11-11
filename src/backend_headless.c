/*
 * Offscreen backend used by automated tests: one output rendered into
 * memory, a 60 Hz frame clock after each present, and a line-based control
 * socket (see backend.h) to dump frames and inject input.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "keys.h"
#include "backend.h"
#include "idleinhibit.h"
#include "saver.h"
#include "search.h"
#include "input.h"
#include "log.h"
#include "mixer.h"
#include "sysinfo.h"
#include "render.h"
#include "seat.h"
#include "nightlight.h"
#include "sound.h"
#include "worker.h"
#include "notifylog.h"
#include "settings.h"
#include "lock.h"
#include "wm.h"

#define CTL_MAX_CLIENTS 8
#define CTL_LINE_MAX 4096

struct ctl_client {
    int fd;
    struct lm_source *source;
    char buf[CTL_LINE_MAX];
    size_t len;
    struct headless *h;
};

struct hl_extra {                       /* --output WxH: a fixed-size output besides the first one */
    struct lm_output out;
    struct lm_framebuffer fb;
};

struct headless {
    struct lm_backend base;
    struct lm_output out;
    struct lm_framebuffer fb;
    struct hl_extra extra[3];
    int nextra;
    uint64_t frames;
    struct lm_timer *ready_timer;
    int ctl_fd;
    struct lm_source *ctl_source;
    struct ctl_client *clients[CTL_MAX_CLIENTS];
    bool output_live;
    int w, h;
    int init_w, init_h;         /* the size asked for on the command line = the "preferred" and "automatic" mode */
};

static struct headless *HL(struct lm_backend *b) { return (struct headless *)b; }

/* ---- backend ops ------------------------------------------------------------------- */

static void ready_fire(void *data)
{
    struct headless *h = data;
    uint32_t t = lm_now_msec32();
    h->base.listener.output_ready(h->base.listener.data, &h->out, t);
    for (int i = 0; i < h->nextra; i++)
        h->base.listener.output_ready(h->base.listener.data, &h->extra[i].out, t);
}

static struct lm_framebuffer *hl_get_framebuffer(struct lm_output *out)
{
    struct headless *h = HL(out->backend);
    struct lm_framebuffer *fb = &h->fb;
    for (int i = 0; i < h->nextra; i++)
        if (out == &h->extra[i].out)
            fb = &h->extra[i].fb;
    fb->age = h->frames ? 1 : 0;
    return fb;
}

static int hl_present(struct lm_output *out, const struct lm_rect *damage, int n)
{
    struct headless *h = HL(out->backend);
    (void)damage;
    (void)n;
    h->frames++;
    lm_timer_arm_ms(h->ready_timer, 16, 0);
    return 0;
}

int lm_backend_headless_dump_output(struct lm_backend *b, const char *path, int index)
{
    struct headless *h = HL(b);
    if (b->server && b->server->render)
        lm_render_readback(b->server->render);
    const struct lm_framebuffer *fb = index == 0 ? &h->fb : index >= 1 && index <= h->nextra ? &h->extra[index - 1].fb : NULL;
    if (!fb)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", fb->width, fb->height);
    uint8_t *row = malloc((size_t)fb->width * 3);
    if (!row) {
        fclose(f);
        return -1;
    }
    for (int y = 0; y < fb->height; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)fb->pixels + (size_t)y * (size_t)fb->stride);
        for (int x = 0; x < fb->width; x++) {
            row[x * 3] = (uint8_t)((src[x] >> 16 & 255) * b->tint[0] + 0.5f);            /* the night light, as a display's gamma table would */
            row[x * 3 + 1] = (uint8_t)((src[x] >> 8 & 255) * b->tint[1] + 0.5f);
            row[x * 3 + 2] = (uint8_t)((src[x] & 255) * b->tint[2] + 0.5f);
        }
        fwrite(row, 3, (size_t)fb->width, f);
    }
    free(row);
    return fclose(f) == 0 ? 0 : -1;
}

int lm_backend_headless_dump(struct lm_backend *b, const char *path)
{
    return lm_backend_headless_dump_output(b, path, 0);
}

/* ---- control socket ------------------------------------------------------------------- */

static void ctl_reply(struct ctl_client *c, const char *fmt, ...) LM_PRINTF(2, 3);
static void ctl_reply(struct ctl_client *c, const char *fmt, ...)
{
    char buf[4608];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for (int off = 0; off < n;) {
        ssize_t k = write(c->fd, buf + off, (size_t)(n - off));
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (int)k;
    }
}

static void ctl_flush(struct headless *h)
{
    if (h->base.server->render)
        lm_render_flush(h->base.server->render);
}

static int g_last_vt;                   /* last Ctrl+Alt+Fn request, shown by the `vt` command (the lock's tests) */

static void ctl_command(struct ctl_client *c, char *line)
{
    struct headless *h = c->h;
    struct lm_server *srv = h->base.server;
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) {
        ctl_reply(c, "err empty command\n");
        return;
    }
    char *a1 = strtok_r(NULL, " \t", &save);
    char *a2 = strtok_r(NULL, " \t", &save);
    char *a3 = strtok_r(NULL, " \t", &save);

    if (!strcmp(cmd, "dump")) {
        if (!a1) { ctl_reply(c, "err usage: dump <path> [output]\n"); return; }
        ctl_flush(h);
        if (lm_backend_headless_dump_output(&h->base, a1, a2 ? atoi(a2) : 0) < 0)
            ctl_reply(c, "err cannot write %s: %s\n", a1, strerror(errno));
        else
            ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "sync")) {
        ctl_flush(h);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "key")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: key <code> <0|1>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_KEY };
        ev.u.key.code = (uint32_t)strtoul(a1, NULL, 0);
        ev.u.key.pressed = atoi(a2) != 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "motion")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: motion <dx> <dy>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_MOTION };
        ev.u.motion.dx = atof(a1);
        ev.u.motion.dy = atof(a2);
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "abs")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: abs <x> <y>\n"); return; }
        if (srv->seat)
            lm_seat_pointer_warp(srv->seat, atof(a1), atof(a2));
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "button")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: button <code> <0|1>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_BUTTON };
        ev.u.button.code = (uint32_t)strtoul(a1, NULL, 0);
        ev.u.button.pressed = atoi(a2) != 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "axis")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: axis <v|h> <value> <discrete>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_AXIS };
        ev.u.axis.orientation = a1[0] == 'h' ? LM_POINTER_AXIS_HORIZONTAL : LM_POINTER_AXIS_VERTICAL;
        ev.u.axis.value = atof(a2);
        ev.u.axis.discrete = a3 ? atoi(a3) : 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "windows")) {
        char *text = malloc(65536);
        if (!text) { ctl_reply(c, "err out of memory\n"); return; }
        if (srv->wm)
            lm_wm_describe(srv->wm, text, 65536);
        else
            text[0] = 0;
        size_t len = strlen(text);
        for (size_t off = 0; off < len;) {
            ssize_t k = write(c->fd, text + off, len - off);
            if (k < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)k;
        }
        free(text);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "kbd")) {
        if (!srv->input) { ctl_reply(c, "err no input\n"); return; }
        ctl_reply(c, "ok layout=%s slot=%d sticky=%d latched=%u slocked=%u locks=%u sound=%s sounds=%u\n",
                  lm_input_layout_id(srv->input, lm_input_layout(srv->input)), lm_input_layout(srv->input),
                  lm_input_sticky(srv->input) ? 1 : 0, lm_input_latched(srv->input),
                  lm_input_sticky_locked(srv->input), lm_input_locks(srv->input), lm_input_last_sound(srv->input),
                  lm_input_sound_count(srv->input));
    } else if (!strcmp(cmd, "clip")) {
        if (a1 && !strcmp(a1, "clear"))
            lm_seat_clip_clear(srv->seat);
        else if (a1 && !strcmp(a1, "add") && a2) {                   /* clip add TEXT: as if it had been copied (single spaces only) */
            char text[300];
            snprintf(text, sizeof(text), "%s%s%s%s%s", a2, a3 ? " " : "", a3 ? a3 : "", save && *save ? " " : "", save && *save ? save : "");
            lm_seat_clip_add(srv->seat, text);
        }
        else if (a1 && !strcmp(a1, "remove") && a2)
            lm_seat_clip_remove(srv->seat, atoi(a2));
        /* one "clip <i> <text>" line per history entry (\ and newline escaped), then "ok n=<count> gadget=<0|1>" */
        int n = lm_seat_clip_count(srv->seat);
        for (int i = 0; i < n; i++) {
            const char *t = lm_seat_clip_get(srv->seat, i);
            char line[480];
            size_t o = (size_t)snprintf(line, sizeof(line), "clip %d ", i);
            for (; *t && o + 3 < sizeof(line); t++) {
                if (*t == '\n' || *t == '\\') {
                    line[o++] = '\\';
                    line[o++] = *t == '\n' ? 'n' : '\\';
                } else if (*t == '\r') {
                    line[o++] = ' ';
                } else {
                    line[o++] = *t;
                }
            }
            line[o++] = '\n';
            ctl_reply(c, "%.*s", (int)o, line);
        }
        int top = 0, slots = 0;
        if (srv->wm)
            lm_wm_clip_view(srv->wm, &top, &slots);
        ctl_reply(c, "ok n=%d gadget=%d top=%d slots=%d\n", n, srv->wm && lm_wm_gadget_visible(srv->wm) ? 1 : 0, top, slots);
    } else if (!strcmp(cmd, "settings")) {
        /* settings                -> ok key=value ...
         * settings <key> <value>  -> same, after the change ("err rejected" if it was refused) */
        if (!srv->settings) { ctl_reply(c, "err no settings\n"); return; }
        bool ok = true;
        if (a1 && a2)
            ok = lm_settings_set(srv->settings, a1, !strcmp(a2, "\"\"") ? "" : a2);        /* "" = the empty value */
        char buf[1536];
        lm_settings_dump(srv->settings, buf, sizeof(buf));
        ctl_reply(c, "%s %s\n", ok ? "ok" : "err rejected", buf);
    } else if (!strcmp(cmd, "outputs")) {
        /* outputs -> one "output <name> x=<x> y=<y> w=<w> h=<h>" line each (in the order of the list), then "ok" */
        struct lm_output *o;
        lm_list_for_each(o, &h->base.outputs, link)
            ctl_reply(c, "output %s x=%d y=%d w=%d h=%d\n", o->name, o->x, o->y, o->width, o->height);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "unplug")) {
        /* unplug -> the first output goes away like a monitor whose KVM switch looks at another computer (tests); its fb is kept */
        if (h->output_live) {
            h->output_live = false;
            if (h->base.listener.output_removed)
                h->base.listener.output_removed(h->base.listener.data, &h->out);
            lm_list_remove(&h->out.link);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "replug")) {
        /* replug [WxH] -> the first output is back, at that size when given (the monitor may wake up in another mode) */
        int nw = h->w, nh = h->h;
        if (a1 && sscanf(a1, "%dx%d", &nw, &nh) != 2) { ctl_reply(c, "err usage: replug [WxH]\n"); return; }
        if (nw < 16 || nh < 16 || nw > 8192 || nh > 8192) { ctl_reply(c, "err size\n"); return; }
        if (!h->output_live) {
            if (nw != h->w || nh != h->h) {
                uint32_t *px = calloc((size_t)nw * (size_t)nh, 4);
                if (!px) { ctl_reply(c, "err memory\n"); return; }
                free(h->fb.pixels);
                h->fb = (struct lm_framebuffer){ .width = nw, .height = nh, .stride = nw * 4, .pixels = px };
                h->w = nw;
                h->h = nh;
                h->out.width = nw;
                h->out.height = nh;
                h->out.phys_width_mm = nw * 254 / 960;
                h->out.phys_height_mm = nh * 254 / 960;
            }
            lm_list_insert(&h->base.outputs, &h->out.link);         /* the first one again */
            h->output_live = true;
            if (h->base.listener.output_added)
                h->base.listener.output_added(h->base.listener.data, &h->out);
            lm_timer_arm_ms(h->ready_timer, 1, 0);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "loop")) {
        /* loop -> "loop stalls=<n> worst_ms=<n> threshold=<ms> worker=<jobs not finished>" (the stall meter, loop.h; worker.h), then "ok";
         * `loop stall MS` blocks the loop for that long (a test of the meter), `loop reset` forgets the counts */
        if (a1 && !strcmp(a1, "stall")) {
            int ms = a2 ? atoi(a2) : 0;
            if (ms > 0 && ms <= 2000)
                usleep((unsigned)ms * 1000u);
        }
        struct lm_loop_stats st;
        lm_loop_stats(srv->loop, &st);
        ctl_reply(c, "loop stalls=%u worst_ms=%u threshold=%u worker=%d\nok\n", st.stalls, st.worst_ms, st.threshold_ms, lm_work_pending());
    } else if (!strcmp(cmd, "idle")) {
        /* idle -> "idle inhibitors=<n> inhibited=<0|1>" (zwp_idle_inhibit_manager_v1), then "ok" */
        ctl_reply(c, "idle inhibitors=%d inhibited=%d\nok\n", lm_idle_inhibit_count(srv->idle_inhibit), lm_idle_inhibited(srv->idle_inhibit));
    } else if (!strcmp(cmd, "saver")) {
        /* saver [start|stop] -> "saver running=<0|1> enabled=<0|1> minutes=<n> lock=<n> shield=<0|1> style=room|flat" after the optional change, then "ok" (the screensaver) */
        if (srv->saver && a1 && !strcmp(a1, "start"))
            lm_saver_start(srv->saver);
        else if (srv->saver && a1 && !strcmp(a1, "stop"))
            lm_saver_stop(srv->saver);
        ctl_reply(c, "saver running=%d enabled=%d minutes=%d lock=%d shield=%d style=%s\n", srv->saver && lm_saver_running(srv->saver), srv->config.saver,
                  srv->config.saver_min, srv->config.saver_lock_min, srv->saver && lm_saver_shields(srv->saver),
                  srv->config.saver_style ? "flat" : "room");
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "sound")) {
        /* sound -> "sound last=<name> n=<count>": the last interface sound that was noted (a dock click: "dock"), then "ok" */
        ctl_reply(c, "sound last=%s n=%u\nok\n", lm_sound_last(), lm_sound_noted());
    } else if (!strcmp(cmd, "notes")) {
        /* notes -> one "note <id> <unread 0|1> <title>\t<body>" line per entry of the notification history, newest first (notifylog.h), then "ok n=<count> unread=<n> dnd=<0|1>" */
        const struct lm_notelog *nl = srv->notelog;
        for (int i = 0; i < lm_notelog_count(nl) && i < LM_NOTELOG_MAX; i++) {
            const struct lm_notelog_entry *e = lm_notelog_get(nl, i);
            ctl_reply(c, "note %d %d %s\t%s\n", e->id, e->unread, e->title, e->body);
        }
        ctl_reply(c, "ok n=%d unread=%d dnd=%d\n", lm_notelog_count(nl), lm_notelog_unread(nl), srv->config.dnd);
    } else if (!strcmp(cmd, "night")) {
        /* night -> "night mode=off|on|auto kelvin=<K> from=HH:MM to=HH:MM active=<0|1> level=<0..1> tint=<r>,<g>,<b>" (night light, nightlight.h; the tint is what the display shows now), then "ok" */
        const struct lm_config *cf = &srv->config;
        ctl_reply(c, "night mode=%s kelvin=%d from=%s to=%s active=%d level=%.2f tint=%.3f,%.3f,%.3f\nok\n",
                  cf->night_mode == 1 ? "on" : cf->night_mode == 2 ? "auto" : "off", cf->night_kelvin, cf->night_from, cf->night_to,
                  lm_night_active(srv->night), lm_night_level(srv->night), srv->backend->tint[0], srv->backend->tint[1], srv->backend->tint[2]);
    } else if (!strcmp(cmd, "anim")) {
        /* anim -> "anim n=<running> dur=<ms>" and one "item kind=open|close|minimize|restore x= y= w= h= alpha=" line each (window animations), then "ok" */
        static char text[1024];
        if (srv->wm) {
            lm_wm_anim_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "overview")) {
        /* overview [open|close] -> "overview open= closing= n= sel= cols= prog= ws=" and one "item i x= y= w= h= slot=x,y,w,h title=" line each (the desktop overview), then "ok" */
        static char otext[2048];
        if (srv->wm && a1 && !strcmp(a1, "open"))
            lm_wm_overview_set(srv->wm, true);
        else if (srv->wm && a1 && !strcmp(a1, "close"))
            lm_wm_overview_set(srv->wm, false);
        if (srv->wm) {
            lm_wm_overview_describe(srv->wm, otext, sizeof(otext));
            ctl_reply(c, "%s", otext);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "about")) {
        /* about [open|close] -> "about open=<0|1>" (after the optional change), while open "card x= y= w= h=", "close x= y= w= h=" and one "row <label>|<value>" line each, then "ok" (the About dialog of the logo button) */
        if (srv->wm && a1 && !strcmp(a1, "open"))
            lm_wm_about_set(srv->wm, true);
        else if (srv->wm && a1 && !strcmp(a1, "close"))
            lm_wm_about_set(srv->wm, false);
        ctl_reply(c, "about open=%d\n", srv->wm && lm_wm_about_is_open(srv->wm));
        if (srv->wm && lm_wm_about_is_open(srv->wm)) {
            static char text[4096];
            lm_wm_about_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "osk")) {
        /* osk [open|close|toggle|reset] -> the lines of lm_wm_osk_describe ("osk open=.. latched=.. n=..", "card ...", one "key ..." line per key), then "ok" */
        static char text[16384];
        if (srv->wm && a1 && !strcmp(a1, "open"))
            lm_wm_osk_set(srv->wm, true);
        else if (srv->wm && a1 && !strcmp(a1, "close"))
            lm_wm_osk_set(srv->wm, false);
        else if (srv->wm && a1 && !strcmp(a1, "reset"))
            lm_wm_osk_reset(srv->wm);
        else if (srv->wm && a1 && !strcmp(a1, "toggle"))
            lm_wm_osk_set(srv->wm, !lm_wm_osk_is_open(srv->wm));
        if (srv->wm) {
            lm_wm_osk_describe(srv->wm, text, sizeof(text));
            for (const char *l = text; *l;) {                   /* (ctl_reply's buffer is small: a line at a time) */
                const char *e = strchr(l, '\n');
                int len = e ? (int)(e - l) + 1 : (int)strlen(l);
                ctl_reply(c, "%.*s", len, l);
                l += len;
            }
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "cascade")) {
        /* cascade -> the next window is placed at the first cascade position again (tests), then "ok" */
        lm_wm_reset_cascade(srv->wm);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "dockmenu")) {
        /* dockmenu -> "dockmenu open= item= sel=" and, while it is open, "card ..." and one "entry ..." line each (the dock's right-click menu), then "ok" */
        static char text[2048];
        if (srv->wm) {
            lm_wm_menu_describe(srv->wm, text, sizeof(text));
            for (const char *l = text; *l;) {
                const char *e = strchr(l, '\n');
                int len = e ? (int)(e - l) + 1 : (int)strlen(l);
                ctl_reply(c, "%.*s", len, l);
                l += len;
            }
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "clock")) {
        /* clock [calendar] -> "clock x= y= w= h= tasks=N calendar=0|1|2 text=..." (calendar: 0 no window, 1 hidden, 2 shown); the word `calendar`
         * first shows / hides it as a click on the clock does. Then "ok". */
        static char text[512];
        if (srv->wm && a1 && !strcmp(a1, "calendar"))
            lm_wm_calendar_toggle(srv->wm, NULL);
        if (srv->wm) {
            lm_wm_clock_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "calc")) {
        /* calc EXPRESSION -> "calc ok=1 value=<text>" or "calc ok=0" (the launcher's calculator, search.h), then "ok" */
        double v;
        char num[48], expr[256];
        snprintf(expr, sizeof(expr), "%s %s %s %s", a1 ? a1 : "", a2 ? a2 : "", a3 ? a3 : "", save ? save : "");
        if (lm_calc_eval(expr, &v)) {
            lm_calc_format(v, num, sizeof(num));
            ctl_reply(c, "calc ok=1 value=%s\n", num);
        } else {
            ctl_reply(c, "calc ok=0\n");
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "quick")) {
        /* quick [toggle] -> "quick x= y= w= h= window=0|1|2" (the panel's quick settings button and its card: 0 none, 1 hidden, 2 shown), then "ok" */
        static char text[256];
        if (srv->wm && a1 && !strcmp(a1, "toggle"))
            lm_wm_quick_toggle(srv->wm, NULL);
        if (srv->wm) {
            lm_wm_quick_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "tray")) {
        /* tray -> the lines of lm_wm_tray_describe ("tray n=..", one "item .." line each), then "ok" */
        static char text[4096];
        if (srv->wm) {
            lm_wm_tray_describe(srv->wm, text, sizeof(text));
            for (const char *l = text; *l;) {
                const char *e = strchr(l, '\n');
                int len = e ? (int)(e - l) + 1 : (int)strlen(l);
                ctl_reply(c, "%.*s", len, l);
                l += len;
            }
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "launcher")) {
        /* launcher [open|close] -> the lines of lm_wm_launcher_describe ("launcher open=.. edit=.. form=.. n=.. sel=..", card, icons, buttons ...), then "ok" */
        static char text[8192];
        if (srv->wm && a1 && !strcmp(a1, "open"))
            lm_wm_launcher_set(srv->wm, true);
        else if (srv->wm && a1 && !strcmp(a1, "close"))
            lm_wm_launcher_set(srv->wm, false);
        if (srv->wm) {
            lm_wm_launcher_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "dock")) {
        /* dock -> "dock shown=.. hover=.. n=.." and one "icon ..." line per button, then "ok" (see lm_wm_dock_describe) */
        static char text[8192];
        if (srv->wm) {
            lm_wm_dock_describe(srv->wm, text, sizeof(text));
            ctl_reply(c, "%s", text);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "setraw")) {
        /* setraw <key> <value...> -> set a setting whose value has spaces or control characters (the dock-* ones); single spaces only */
        if (!srv->settings || !a1 || !a2) { ctl_reply(c, "err usage: setraw <key> <value>\n"); return; }
        char val[1400];
        snprintf(val, sizeof(val), "%s%s%s%s%s", a2, a3 ? " " : "", a3 ? a3 : "", save && *save ? " " : "", save && *save ? save : "");
        ctl_reply(c, lm_settings_set(srv->settings, a1, val) ? "ok\n" : "err rejected\n");
    } else if (!strcmp(cmd, "reload")) {
        /* reload -> read mellagui.conf again (what SIGUSR2 and the reload-config shortcut do); "ok <rejected lines>" */
        int bad = srv->reload_config ? srv->reload_config(srv, false) : -1;
        if (bad < 0)
            ctl_reply(c, "err no configuration file\n");
        else
            ctl_reply(c, "ok %d\n", bad);
    } else if (!strcmp(cmd, "keys")) {
        /* keys -> one "key <Mods+Key = action [arg]>" line per shortcut (unordered), then "ok" */
        for (int i = 0; i < srv->config.keys.n; i++) {
            char line[400];
            lm_keys_describe(&srv->config.keys.b[i], line, sizeof(line));
            ctl_reply(c, "key %s\n", line);
        }
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "commands")) {
        /* commands -> "terminal=... run=... settings=... view=... saver=..." one per line, then "ok" */
        ctl_reply(c, "terminal=%s\nrun=%s\nsettings=%s\nview=%s\nsaver=%s\nok\n", srv->config.terminal_cmd, srv->config.run_cmd,
                  srv->config.settings_cmd, srv->config.view_cmd, srv->config.saver_cmd ? srv->config.saver_cmd : "");
    } else if (!strcmp(cmd, "notify")) {
        /* notify <ms> <title> [body words...] */
        if (!srv->settings || !a1 || !a2) { ctl_reply(c, "err usage: notify <ms> <title> [body]\n"); return; }
        char msg[400];
        snprintf(msg, sizeof(msg), "%s\t%s%s%s\t%s", a2, a3 ? a3 : "", (a3 && save) ? " " : "", save ? save : "", a1);
        ctl_reply(c, lm_settings_set(srv->settings, "notify", msg) ? "ok\n" : "err rejected\n");
    } else if (!strcmp(cmd, "setting")) {
        /* setting <key>           -> "ok <value>" (also the long list values, which `settings` leaves out) */
        char buf[4096];
        if (!srv->settings || !a1 || !lm_settings_get(srv->settings, a1, buf, sizeof(buf))) {
            ctl_reply(c, "err unknown setting\n");
            return;
        }
        ctl_reply(c, "ok %s\n", buf);
    } else if (!strcmp(cmd, "vt")) {
        /* vt -> "ok <n>": the console Ctrl+Alt+Fn asked for last (0 = none); the headless backend only records it */
        ctl_reply(c, "ok %d\n", g_last_vt);
    } else if (!strcmp(cmd, "power")) {
        /* power -> "ok on" | "ok standby": whether the monitors are in standby (the lock screen's blank timeout) */
        ctl_reply(c, "ok %s\n", srv->backend && srv->backend->blanked ? "standby" : "on");
    } else if (!strcmp(cmd, "lock")) {
        /* lock                      -> "ok kind=.. locked=.. timeout=.. start=.. typed=.. fails=.. wait=.. status=.. message=.."
         * lock now                  lock the screen ("err no lock set" when there is no password / PIN)
         * lock set OLD KIND NEW [MINUTES [START [BLANK [VT]]]]   change the lock like the settings key `lock-set` (OLD, NEW without blanks;
         *                           "-" = empty; KIND password|pin|keep|none) */
        if (!srv->lock) { ctl_reply(c, "err no lock\n"); return; }
        bool ok = true;
        if (a1 && !strcmp(a1, "now")) {
            ok = lm_lock_now(srv->lock);
        } else if (a1 && !strcmp(a1, "quit")) {            /* what Super+Shift+E does: ask for the secret, then quit */
            ok = lm_lock_request_quit(srv->lock);
        } else if (a1 && (!strcmp(a1, "suspend") || !strcmp(a1, "reboot") || !strcmp(a1, "poweroff"))) {      /* the session dialog's buttons when a secret is set */
            ok = lm_lock_request_action(srv->lock, !strcmp(a1, "suspend") ? LM_LOCK_ACT_SUSPEND : !strcmp(a1, "reboot") ? LM_LOCK_ACT_REBOOT : LM_LOCK_ACT_POWEROFF);
        } else if (a1 && !strcmp(a1, "set")) {
            char *t[7] = { a2, a3, strtok_r(NULL, " \t", &save), NULL, NULL, NULL, NULL };
            for (int i = 3; i < 7; i++)
                t[i] = t[i - 1] ? strtok_r(NULL, " \t", &save) : NULL;
            for (int i = 0; i < 7; i++)
                if (t[i] && !strcmp(t[i], "-"))
                    t[i] = (char *)"";
            ok = t[0] && t[1] && t[2] &&
                 lm_lock_apply(srv->lock, t[0], t[1], t[2], t[3] ? t[3] : "", t[4] ? t[4] : "", t[5] ? t[5] : "",
                              t[6] ? t[6] : "");
        }
        char buf[512];
        lm_lock_describe(srv->lock, buf, sizeof(buf));
        ctl_reply(c, "%s %s\n", ok ? "ok" : "err rejected", buf);
    } else if (!strcmp(cmd, "gadget")) {
        if (!a1 || !srv->wm) { ctl_reply(c, "err usage: gadget <0|1>\n"); return; }
        lm_wm_set_gadget(srv->wm, a1[0] == '1');
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "mixer")) {
        /* mixer <0|1>  show / hide the volume mixer gadget; always replies the mixer's state:
         * "ok visible=<0|1> backend=<none|pulse|alsa> ready=<0|1> volume=<n> mute=<0|1> output=<text> streams=<n>" */
        if (!srv->wm || !srv->mixer) { ctl_reply(c, "err no mixer\n"); return; }
        if (a1)
            lm_wm_set_gadget_id(srv->wm, LM_GADGET_MIXER, a1[0] == '1');
        const struct lm_mixer_state *ms = lm_mixer_state(srv->mixer);
        ctl_reply(c, "ok visible=%d backend=%s ready=%d volume=%d mute=%d streams=%d output=%s\n",
                  lm_wm_gadget_visible_id(srv->wm, LM_GADGET_MIXER) ? 1 : 0,
                  ms->backend == LM_MIXER_PULSE ? "pulse" : ms->backend == LM_MIXER_WPCTL ? "wpctl" : ms->backend == LM_MIXER_ALSA ? "alsa" : "none", ms->ready ? 1 : 0,
                  ms->volume, ms->mute ? 1 : 0, ms->nstreams, ms->output[0] ? ms->output : "-");
    } else if (!strcmp(cmd, "sysinfo")) {
        /* sysinfo [0|1]  show / hide the system monitor gadget; always replies the sampled values:
         * "ok visible=<0|1> ready=<0|1> cpu=<%> cores=<n> mem=<%> mem_total_kb=<n> swap=<%> temp=<C or -1> load=<x.xx> hist=<n>" */
        if (!srv->wm || !srv->sysinfo) { ctl_reply(c, "err no sysinfo\n"); return; }
        if (a1)
            lm_wm_set_gadget_id(srv->wm, LM_GADGET_SYSTEM, a1[0] == '1');
        const struct lm_sysinfo_state *ss = lm_sysinfo_state(srv->sysinfo);
        ctl_reply(c, "ok visible=%d ready=%d cpu=%d cores=%d mem=%d mem_total_kb=%ld swap=%d temp=%d load=%.2f hist=%d\n",
                  lm_wm_gadget_visible_id(srv->wm, LM_GADGET_SYSTEM) ? 1 : 0, ss->ready ? 1 : 0, ss->cpu, ss->ncores, ss->mem_pct,
                  ss->mem_total_kb, ss->swap_pct, ss->temp_c, ss->load1, ss->nhist);
        /* the device strings contain blanks, so they have their own command: "sysdev" */
    } else if (!strcmp(cmd, "sysdev")) {
        /* sysdev  replies "ok maker=<..>|model=<..>|cpu=<..>|os=<..>|kernel=<..>|host=<..>|uptime=<s>" ('|' separated, values may hold blanks) */
        if (!srv->sysinfo) { ctl_reply(c, "err no sysinfo\n"); return; }
        const struct lm_sysinfo_state *ss = lm_sysinfo_state(srv->sysinfo);
        ctl_reply(c, "ok maker=%s|model=%s|cpu=%s|os=%s|kernel=%s|host=%s|uptime=%ld\n", ss->maker, ss->model, ss->cpu_model,
                  ss->os_name, ss->kernel, ss->host, ss->uptime_s);
    } else if (!strcmp(cmd, "gadget-rect")) {
        struct lm_rect r;
        if (srv->wm && lm_wm_gadget_rect_id(srv->wm, a1 && !strcmp(a1, "mixer") ? LM_GADGET_MIXER :
                                            a1 && !strcmp(a1, "system") ? LM_GADGET_SYSTEM : LM_GADGET_CLIPBOARD, &r))
            ctl_reply(c, "ok %d %d %d %d\n", r.x, r.y, r.w, r.h);
        else
            ctl_reply(c, "ok hidden\n");
    } else if (!strcmp(cmd, "layout")) {
        if (!a1 || !srv->input) { ctl_reply(c, "err usage: layout <slot|next|prev>\n"); return; }
        if (!strcmp(a1, "next") || !strcmp(a1, "prev"))
            lm_input_cycle_layout(srv->input, a1[0] == 'n' ? 1 : -1);
        else
            lm_input_set_layout(srv->input, atoi(a1));
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "sticky")) {
        if (!a1 || !srv->input) { ctl_reply(c, "err usage: sticky <0|1>\n"); return; }
        lm_input_set_sticky(srv->input, atoi(a1) != 0);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "quit")) {
        ctl_reply(c, "ok\n");
        lm_server_quit(srv);
    } else {
        ctl_reply(c, "err unknown command %s\n", cmd);
    }
}

static void ctl_close(struct ctl_client *c)
{
    struct headless *h = c->h;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (h->clients[i] == c)
            h->clients[i] = NULL;
    lm_source_remove(c->source);
    close(c->fd);
    free(c);
}

static void ctl_client_cb(int fd, uint32_t events, void *data)
{
    struct ctl_client *c = data;
    if (events & EPOLLIN) {
        ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            ctl_close(c);
            return;
        }
        c->len += (size_t)n;
        char *nl;
        while ((nl = memchr(c->buf, '\n', c->len))) {
            *nl = 0;
            char line[CTL_LINE_MAX];
            snprintf(line, sizeof(line), "%s", c->buf);
            size_t used = (size_t)(nl - c->buf) + 1;
            memmove(c->buf, c->buf + used, c->len - used);
            c->len -= used;
            ctl_command(c, line);
        }
        if (c->len >= sizeof(c->buf) - 1) {
            ctl_reply(c, "err line too long\n");
            ctl_close(c);
            return;
        }
    } else if (events & (EPOLLHUP | EPOLLERR)) {
        ctl_close(c);
    }
}

static void ctl_accept_cb(int fd, uint32_t events, void *data)
{
    struct headless *h = data;
    (void)events;
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0)
        return;
    int slot = -1;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (!h->clients[i]) { slot = i; break; }
    struct ctl_client *c = slot >= 0 ? calloc(1, sizeof(*c)) : NULL;
    if (!c) {
        close(cfd);
        return;
    }
    c->fd = cfd;
    c->h = h;
    c->source = lm_loop_add_fd(h->base.server->loop, cfd, EPOLLIN, ctl_client_cb, c);
    if (!c->source) {
        close(cfd);
        free(c);
        return;
    }
    h->clients[slot] = c;
}

static int ctl_listen(struct headless *h, const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(sa.sun_path)) {
        lm_error("headless: control path too long: %s", path);
        return -1;
    }
    strcpy(sa.sun_path, path);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0) {
        lm_error("headless: cannot listen on %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    h->ctl_fd = fd;
    h->ctl_source = lm_loop_add_fd(h->base.server->loop, fd, EPOLLIN, ctl_accept_cb, h);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------------------ */

static int hl_init(struct lm_backend *b)
{
    struct headless *h = HL(b);
    struct lm_server *srv = b->server;
    if (srv->config.drm_mode_w > 0) {                        /* the saved resolution setting wins over --size */
        h->w = srv->config.drm_mode_w;
        h->h = srv->config.drm_mode_h;
    }
    h->fb.width = h->w;
    h->fb.height = h->h;
    h->fb.stride = h->w * 4;
    h->fb.pixels = calloc((size_t)h->w * (size_t)h->h, 4);
    if (!h->fb.pixels)
        return -1;
    if (srv->config.control_path && ctl_listen(h, srv->config.control_path) < 0)
        return -1;
    h->ready_timer = lm_loop_add_timer(srv->loop, ready_fire, h);

    memset(&h->out, 0, sizeof(h->out));
    h->out.backend = b;
    snprintf(h->out.name, sizeof(h->out.name), "headless-0");
    snprintf(h->out.make, sizeof(h->out.make), "MellaGUI");
    snprintf(h->out.model, sizeof(h->out.model), "headless");
    h->out.width = h->w;
    h->out.height = h->h;
    h->out.refresh_mhz = 60000;
    h->out.phys_width_mm = h->w * 254 / 960;
    h->out.phys_height_mm = h->h * 254 / 960;
    h->out.scale = 1;
    lm_list_append(&b->outputs, &h->out.link);
    h->output_live = true;
    b->session_active = true;
    if (b->listener.output_added)
        b->listener.output_added(b->listener.data, &h->out);
    for (int i = 0; i < srv->config.headless_nextra && i < 3; i++) {             /* --output WxH */
        struct hl_extra *x = &h->extra[i];
        int w = srv->config.headless_extra[i][0], hh = srv->config.headless_extra[i][1];
        x->fb = (struct lm_framebuffer){ .width = w, .height = hh, .stride = w * 4, .pixels = calloc((size_t)w * (size_t)hh, 4) };
        if (!x->fb.pixels)
            return -1;
        x->out.backend = b;
        snprintf(x->out.name, sizeof(x->out.name), "headless-%d", i + 1);
        snprintf(x->out.make, sizeof(x->out.make), "MellaGUI");
        snprintf(x->out.model, sizeof(x->out.model), "headless");
        x->out.width = w;
        x->out.height = hh;
        x->out.refresh_mhz = 60000;
        x->out.phys_width_mm = w * 254 / 960;
        x->out.phys_height_mm = hh * 254 / 960;
        x->out.scale = 1;
        lm_list_append(&b->outputs, &x->out.link);
        h->nextra = i + 1;
        if (b->listener.output_added)
            b->listener.output_added(b->listener.data, &x->out);
    }
    lm_timer_arm_ms(h->ready_timer, 1, 0);
    return 0;
}

static void hl_destroy(struct lm_backend *b)
{
    struct headless *h = HL(b);
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (h->clients[i])
            ctl_close(h->clients[i]);
    for (int i = h->nextra - 1; i >= 0; i--) {
        if (b->listener.output_removed)
            b->listener.output_removed(b->listener.data, &h->extra[i].out);
        lm_list_remove(&h->extra[i].out.link);
        free(h->extra[i].fb.pixels);
    }
    if (h->output_live) {
        if (b->listener.output_removed)
            b->listener.output_removed(b->listener.data, &h->out);
        lm_list_remove(&h->out.link);
    }
    if (h->ctl_source)
        lm_source_remove(h->ctl_source);
    if (h->ctl_fd > 0) {
        close(h->ctl_fd);
        if (b->server->config.control_path)
            unlink(b->server->config.control_path);
    }
    if (h->ready_timer)
        lm_timer_destroy(h->ready_timer);
    free(h->fb.pixels);
    free(h);
}

/* A few sizes to pick from in tests (the settings panel's resolution list). */
static int hl_list_modes(struct lm_output *out, struct lm_mode_info *modes, int max)
{
    struct headless *h = HL(out->backend);
    static const int sizes[][2] = { { 1920, 1080 }, { 1280, 720 }, { 1024, 768 }, { 800, 600 }, { 2048, 858 } };
    int n = 0;
    if (n < max)
        modes[n++] = (struct lm_mode_info){ h->init_w, h->init_h, 60, true, false };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && n < max; i++)
        if (sizes[i][0] != h->init_w || sizes[i][1] != h->init_h)
            modes[n++] = (struct lm_mode_info){ sizes[i][0], sizes[i][1], 60, false, i == 4 };
    return n;
}

/* Resize the (single) output: new framebuffer, then the same notifications a DRM mode change sends. */
static int hl_apply_mode(struct lm_output *out)
{
    struct headless *h = HL(out->backend);
    const struct lm_config *cfg = &out->backend->server->config;
    int w = cfg->drm_mode_w > 0 ? cfg->drm_mode_w : h->init_w, hh = cfg->drm_mode_w > 0 ? cfg->drm_mode_h : h->init_h;
    if (w == h->w && hh == h->h)
        return 0;
    if (w < 64 || hh < 64 || w > 16384 || hh > 16384)
        return -1;
    uint32_t *px = calloc((size_t)w * (size_t)hh, 4);
    if (!px)
        return -1;
    free(h->fb.pixels);
    h->w = w;
    h->h = hh;
    h->fb.pixels = px;
    h->fb.width = w;
    h->fb.height = hh;
    h->fb.stride = w * 4;
    h->frames = 0;                                  /* buffer age 0: everything is redrawn */
    out->width = w;
    out->height = hh;
    out->phys_width_mm = w * 254 / 960;
    out->phys_height_mm = hh * 254 / 960;
    if (out->backend->listener.output_changed)
        out->backend->listener.output_changed(out->backend->listener.data, out);
    lm_timer_arm_ms(h->ready_timer, 1, 0);
    return 0;
}

static int hl_switch_vt(struct lm_backend *b, int vt)
{
    (void)b;
    g_last_vt = vt;
    return 0;
}

static int hl_set_power(struct lm_backend *b, bool on)
{
    (void)b;
    (void)on;                           /* nothing to switch off; lm_backend_set_power() keeps the flag for the `power` command */
    return 0;
}

static int hl_set_tint(struct lm_backend *b)
{
    (void)b;                                /* dump applies b->tint when it writes a picture */
    return 0;
}

static const struct lm_backend_ops headless_ops = {
    .name = "headless",
    .init = hl_init,
    .destroy = hl_destroy,
    .get_framebuffer = hl_get_framebuffer,
    .present = hl_present,
    .switch_vt = hl_switch_vt,
    .list_modes = hl_list_modes,
    .apply_mode = hl_apply_mode,
    .set_power = hl_set_power,
    .set_tint = hl_set_tint,
};

struct lm_backend *lm_backend_headless_create(struct lm_server *srv, int w, int h)
{
    struct headless *hl = calloc(1, sizeof(*hl));
    if (!hl)
        return NULL;
    hl->base.ops = &headless_ops;
    hl->base.server = srv;
    hl->base.tint[0] = hl->base.tint[1] = hl->base.tint[2] = 1.0f;
    lm_list_init(&hl->base.outputs);
    hl->w = w > 0 ? w : 1280;
    hl->h = h > 0 ? h : 720;
    hl->init_w = hl->w;
    hl->init_h = hl->h;
    hl->ctl_fd = -1;
    return &hl->base;
}

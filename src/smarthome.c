#include "smarthome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "log.h"
#include "loop.h"

struct lm_smart {
    struct lm_loop *loop;
    void (*changed)(void *);
    void *data;
    struct lm_smart_state st;
    bool active;
    struct lm_timer *poll;
    struct lm_proc *job;                /* the poll in flight */
    unsigned gen;                       /* bumped by every click: a poll that began earlier is stale about that device */
    unsigned poll_gen;
    struct lm_timer *soon;              /* a poll a moment after a command */
};

static void notify(struct lm_smart *s)
{
    if (s->changed)
        s->changed(s->data);
}

/* The program's path: $MELLAGUI_SMART_BIN, else mellagui-smart in the folder of this executable, else the one on PATH. */
static void program(char *out, size_t n)
{
    const char *e = getenv("MELLAGUI_SMART_BIN");
    if (e && *e) {
        snprintf(out, n, "%s", e);
        return;
    }
    char self[470];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len > 0) {
        self[len] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            char path[500];
            snprintf(path, sizeof(path), "%s/mellagui-smart", self);
            if (access(path, X_OK) == 0) {
                snprintf(out, n, "%s", path);
                return;
            }
        }
    }
    snprintf(out, n, "mellagui-smart");
}

static void parse(struct lm_smart *s, const char *text)
{
    struct lm_smart_state ns;
    memset(&ns, 0, sizeof(ns));
    ns.ready = true;
    char *copy = strdup(text), *save = NULL;
    if (!copy)
        return;
    bool header = strncmp(text, "favs", 4) == 0;                  /* the program says how many are pinned first: no header = no answer */
    for (char *line = strtok_r(copy, "\n", &save); line && ns.n < LM_SMART_MAX; line = strtok_r(NULL, "\n", &save)) {
        char *f[8];
        int k = 0;
        for (char *p = line; k < 8;) {
            f[k++] = p;
            p = strchr(p, '\t');
            if (!p)
                break;
            *p++ = 0;
        }
        if (k < 7 || strcmp(f[0], "fav"))
            continue;
        struct lm_smart_item *it = &ns.item[ns.n++];
        snprintf(it->id, sizeof(it->id), "%s", f[1]);
        snprintf(it->name, sizeof(it->name), "%s", f[2]);
        snprintf(it->code, sizeof(it->code), "%s", f[4 + 1]);
        it->online = atoi(f[3]) != 0;
        it->kind = !strcmp(f[4], "switch") ? LM_SMART_SWITCH : !strcmp(f[4], "blind") ? LM_SMART_BLIND : LM_SMART_NONE;
        int v = atoi(f[6]);
        it->on = it->kind == LM_SMART_SWITCH && v != 0;
        it->percent = it->kind == LM_SMART_BLIND ? v : -1;
        if (!strcmp(f[4], "vacuum")) {
            it->kind = LM_SMART_VACUUM;
            it->on = v != 0;
            it->battery = -1;
            if (k >= 8) {                                           /* battery,dockcode,dockvalue */
                char *c1 = strchr(f[7], ',');
                if (c1) {
                    *c1++ = 0;
                    it->battery = atoi(f[7]);
                    char *c2 = strchr(c1, ',');
                    if (c2) {
                        *c2++ = 0;
                        snprintf(it->dock_code, sizeof(it->dock_code), "%s", c1);
                        snprintf(it->dock_value, sizeof(it->dock_value), "%s", c2);
                    }
                }
            }
        }
    }
    free(copy);
    if (!header) {
        snprintf(ns.note, sizeof(ns.note), "Smart home does not answer (log in with the Smart home program).");
        s->st = ns;
        return;
    }
    if (!ns.n)
        snprintf(ns.note, sizeof(ns.note), "Nothing is pinned yet. In Smart home open a device and press \"Show it on the desktop\".");
    s->st = ns;
}

static void poll_done(void *data, const char *out)
{
    struct lm_smart *s = data;
    s->job = NULL;
    if (s->poll_gen != s->gen) {                                /* the user clicked meanwhile: this answer predates it */
        lm_smart_refresh(s);
        return;
    }
    parse(s, out ? out : "");
    notify(s);
}

void lm_smart_refresh(struct lm_smart *s)
{
    if (!s || s->job)
        return;
    char prog[512];
    program(prog, sizeof(prog));
    char *argv[] = { prog, (char *)"--favourites", NULL };
    s->poll_gen = s->gen;
    s->job = lm_proc_run(s->loop, argv, 30000, poll_done, s);
}

static void poll_cb(void *data)
{
    struct lm_smart *s = data;
    if (s->active)
        lm_smart_refresh(s);
}

static void soon_cb(void *data)
{
    struct lm_smart *s = data;
    if (s->active)
        lm_smart_refresh(s);
}

struct lm_smart *lm_smart_create(struct lm_loop *loop, void (*changed)(void *), void *data)
{
    struct lm_smart *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->loop = loop;
    s->changed = changed;
    s->data = data;
    s->poll = lm_loop_add_timer(loop, poll_cb, s);
    s->soon = lm_loop_add_timer(loop, soon_cb, s);
    return s;
}

void lm_smart_destroy(struct lm_smart *s)
{
    if (!s)
        return;
    if (s->job)
        lm_proc_cancel(s->job);
    if (s->poll)
        lm_timer_destroy(s->poll);
    if (s->soon)
        lm_timer_destroy(s->soon);
    free(s);
}

void lm_smart_set_active(struct lm_smart *s, bool active)
{
    if (!s || s->active == active)
        return;
    s->active = active;
    if (active) {
        lm_timer_arm_ms(s->poll, LM_SMART_POLL_MS, LM_SMART_POLL_MS);
        lm_smart_refresh(s);
    } else {
        lm_timer_arm_ms(s->poll, 0, 0);
    }
}

const struct lm_smart_state *lm_smart_state(const struct lm_smart *s)
{
    return &s->st;
}

static void send_set_code(struct lm_smart *s, const struct lm_smart_item *it, const char *code, const char *value)
{
    char prog[512];
    program(prog, sizeof(prog));
    char *argv[] = { prog, (char *)"--set", (char *)it->id, (char *)code, (char *)value, NULL };
    lm_proc_run(s->loop, argv, 30000, NULL, NULL);                 /* fire and forget; the poll a few seconds later shows what happened */
    s->gen++;
    if (s->soon)
        lm_timer_arm_ms(s->soon, 4000, 0);
}

static void send_set(struct lm_smart *s, const struct lm_smart_item *it, const char *value)
{
    send_set_code(s, it, it->code, value);
}

void lm_smart_toggle(struct lm_smart *s, int index)
{
    if (!s || index < 0 || index >= s->st.n || s->st.item[index].kind != LM_SMART_SWITCH || !s->st.item[index].online)
        return;
    struct lm_smart_item *it = &s->st.item[index];
    it->on = !it->on;
    send_set(s, it, it->on ? "true" : "false");
    notify(s);
}

void lm_smart_blind(struct lm_smart *s, int index, int what)
{
    static const char *const words[3] = { "open", "stop", "close" };
    if (!s || index < 0 || index >= s->st.n || s->st.item[index].kind != LM_SMART_BLIND || !s->st.item[index].online || what < 0 || what > 2)
        return;
    send_set(s, &s->st.item[index], words[what]);
}

void lm_smart_vacuum(struct lm_smart *s, int index, int what)
{
    if (!s || index < 0 || index >= s->st.n || s->st.item[index].kind != LM_SMART_VACUUM || !s->st.item[index].online)
        return;
    struct lm_smart_item *it = &s->st.item[index];
    if (what == 0) {
        it->on = !it->on;                                           /* (shown at once, the next poll confirms it) */
        send_set(s, it, it->on ? "true" : "false");
        notify(s);
    } else if (what == 1 && it->dock_code[0]) {
        it->on = false;
        send_set_code(s, it, it->dock_code, it->dock_value);
        notify(s);
    }
}

/* ---- layout ----------------------------------------------------------------------------------------------------------------------- */

void lm_smart_layout(int x, int y, int w, int h, int title_h, int n, struct lm_smart_layout *l)
{
    memset(l, 0, sizeof(*l));
    int room = (h - title_h - 8) / LM_SMART_ROW_H;
    l->rows = room < 0 ? 0 : room > LM_SMART_MAX ? LM_SMART_MAX : room;
    if (n < l->rows)
        l->rows = n;
    for (int i = 0; i < l->rows; i++) {
        int ry = y + title_h + 4 + i * LM_SMART_ROW_H;
        l->sw[i] = (struct lm_smart_rect){ x + w - 12 - 44, ry + 6, 44, 22 };
        for (int b = 0; b < 3; b++)
            l->btn[i][b] = (struct lm_smart_rect){ x + w - 12 - 3 * 30 + b * 30, ry + 5, 26, 24 };
        l->vac[i][0] = (struct lm_smart_rect){ x + w - 12 - 90, ry + 5, 46, 24 };
        l->vac[i][1] = (struct lm_smart_rect){ x + w - 12 - 40, ry + 5, 40, 24 };
        l->name[i] = (struct lm_smart_rect){ x + 14, ry, w - 28 - 3 * 30 - 8, LM_SMART_ROW_H };
    }
}

static bool inside(const struct lm_smart_rect *r, int px, int py)
{
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

enum lm_smart_part lm_smart_hit(const struct lm_smart_layout *l, const struct lm_smart_state *st, int px, int py, int *index)
{
    for (int i = 0; i < l->rows && i < st->n; i++) {
        if (st->item[i].kind == LM_SMART_SWITCH && inside(&l->sw[i], px, py)) {
            *index = i;
            return LM_SMART_SWITCH_PART;
        }
        if (st->item[i].kind == LM_SMART_VACUUM)
            for (int b = 0; b < 2; b++)
                if (inside(&l->vac[i][b], px, py) && (b == 0 || st->item[i].dock_code[0])) {
                    *index = i;
                    return b == 0 ? LM_SMART_CLEAN : LM_SMART_DOCK;
                }
        if (st->item[i].kind == LM_SMART_BLIND)
            for (int b = 0; b < 3; b++)
                if (inside(&l->btn[i][b], px, py)) {
                    *index = i;
                    return (enum lm_smart_part)(LM_SMART_OPEN + b);
                }
    }
    return LM_SMART_NOTHING;
}

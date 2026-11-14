#include "smarthome.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
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
        if (!strcmp(f[4], "switch") && k >= 8)
            snprintf(it->label, sizeof(it->label), "%s", f[7]);
        it->kind = !strcmp(f[4], "switch") ? LM_SMART_SWITCH : !strcmp(f[4], "blind") ? LM_SMART_BLIND : LM_SMART_NONE;
        int v = atoi(f[6]);
        it->on = it->kind == LM_SMART_SWITCH && v != 0;
        it->percent = it->kind == LM_SMART_BLIND ? v : -1;
        it->can_pos = it->kind == LM_SMART_BLIND && k >= 8 && atoi(f[7]) != 0;
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
    for (int i = 0; i < ns.n; i++)                                  /* the switches of one device (same id, each with a label, one after the other) are a group */
        if (ns.item[i].label[0] && (i == 0 || !ns.item[i - 1].label[0] || strcmp(ns.item[i - 1].id, ns.item[i].id))) {
            ns.item[i].group_first = true;
            int m = 1;
            while (i + m < ns.n && ns.item[i + m].label[0] && !strcmp(ns.item[i + m].id, ns.item[i].id))
                m++;
            ns.item[i].group_len = m;
        }
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

/* Starts a program that has nothing to do with this one (a double fork: nobody waits for it, and it keeps none of our files). */
static void spawn_detached(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            close_range(3, ~0U, 0);
            execv(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

void lm_smart_open_card(struct lm_smart *s, int index)
{
    (void)s;
    if (!s || index < 0 || index >= s->st.n)
        return;
    char prog[512];
    program(prog, sizeof(prog));
    char *argv[] = { prog, (char *)"--device", (char *)s->st.item[index].id, NULL };
    spawn_detached(argv);
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

void lm_smart_blind_position(struct lm_smart *s, int index, int percent, bool send)
{
    if (!s || index < 0 || index >= s->st.n || s->st.item[index].kind != LM_SMART_BLIND || !s->st.item[index].online || !s->st.item[index].can_pos)
        return;
    struct lm_smart_item *it = &s->st.item[index];
    it->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (send) {
        char v[8];
        snprintf(v, sizeof(v), "%d", it->percent);
        send_set_code(s, it, "percent_control", v);
    }
    notify(s);
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

/* How many switches of a group stand in a row on a card w wide. */
static int group_cols(int w)
{
    int n = (w - 16 - 8) / LM_SMART_CELL_MIN_W;
    return n < 1 ? 1 : n;
}

/* The height one item (or a whole group, on its first item) takes, and the gap that follows it. */
static int block_height(const struct lm_smart_state *st, int i, int w)
{
    const struct lm_smart_item *it = &st->item[i];
    if (it->label[0]) {
        if (!it->group_first)
            return 0;                                               /* (the other switches of a group are part of its first) */
        int cols = group_cols(w), lines = (it->group_len + cols - 1) / cols;
        return LM_SMART_HEAD_H + lines * LM_SMART_CELL_H + 4;
    }
    return it->kind == LM_SMART_BLIND && it->can_pos ? LM_SMART_ROW2_H : LM_SMART_ROW_H;
}

void lm_smart_layout(int x, int y, int w, int h, int title_h, const struct lm_smart_state *st, struct lm_smart_layout *l)
{
    memset(l, 0, sizeof(*l));
    int ry = y + title_h + 4, bottom = y + h - 4;
    for (int i = 0; i < st->n && i < LM_SMART_MAX;) {
        const struct lm_smart_item *it = &st->item[i];
        int rh = block_height(st, i, w);
        if (ry + rh - 4 > bottom)                                   /* (a group is shown whole or not at all) */
            break;
        if (it->label[0]) {                                         /* a device with several switches: its name, then its switches side by side */
            int m = it->group_len > 0 ? it->group_len : 1, cols = group_cols(w), lines = (m + cols - 1) / cols;
            int gw = w - 16 - 8, cw = gw / (m < cols ? m : cols);   /* the cells share the width (a group with 2 switches has two wide cells) */
            l->group[i] = (struct lm_smart_rect){ x + 8, ry, w - 16, LM_SMART_HEAD_H + lines * LM_SMART_CELL_H };
            l->head[i] = (struct lm_smart_rect){ x + 8, ry, w - 16, LM_SMART_HEAD_H };
            for (int q = 0; q < m && i + q < LM_SMART_MAX; q++) {
                int r = i + q, cx = x + 8 + 4 + (q % cols) * cw, cy = ry + LM_SMART_HEAD_H + (q / cols) * LM_SMART_CELL_H;
                l->rows = r + 1;
                l->row[r] = (struct lm_smart_rect){ cx, cy, cw, LM_SMART_CELL_H };
                l->name[r] = (struct lm_smart_rect){ cx, cy + 3, cw, 16 };
                l->sw[r] = (struct lm_smart_rect){ cx + (cw - 44) / 2, cy + 20, 44, 22 };
            }
            i += m;
            ry += rh;
            continue;
        }
        bool two = it->kind == LM_SMART_BLIND && it->can_pos;
        l->rows = i + 1;
        l->row[i] = (struct lm_smart_rect){ x + 8, ry, w - 16, rh - 4 };
        l->sw[i] = (struct lm_smart_rect){ x + w - 12 - 44, ry + 6, 44, 22 };
        for (int b = 0; b < 3; b++)
            l->btn[i][b] = (struct lm_smart_rect){ x + w - 12 - 3 * 30 + b * 30, ry + 5, 26, 24 };
        l->vac[i][0] = (struct lm_smart_rect){ x + w - 12 - 90, ry + 5, 46, 24 };
        l->vac[i][1] = (struct lm_smart_rect){ x + w - 12 - 40, ry + 5, 40, 24 };
        l->name[i] = (struct lm_smart_rect){ x + 14, ry, w - 28 - (two ? 0 : 3 * 30 + 8), LM_SMART_ROW_H };
        if (two)                                                    /* the slider on the second line, the percentage to its right */
            l->slider[i] = (struct lm_smart_rect){ x + 22, ry + 30, w - 22 - 22 - 48, 20 };
        ry += rh;
        i++;
    }
}

int lm_smart_height(const struct lm_smart_state *st, int w, int title_h)
{
    int h = title_h + 4;
    for (int i = 0; i < st->n && i < LM_SMART_MAX; i++)
        h += block_height(st, i, w);
    return h;
}

int lm_smart_slider_value(const struct lm_smart_layout *l, int i, int px)
{
    const struct lm_smart_rect *s = &l->slider[i];
    if (s->w <= 0)
        return 0;
    int v = (px - s->x) * 100 / s->w;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool inside(const struct lm_smart_rect *r, int px, int py)
{
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

enum lm_smart_part lm_smart_hit(const struct lm_smart_layout *l, const struct lm_smart_state *st, int px, int py, int *index)
{
    for (int i = 0; i < l->rows && i < st->n; i++) {
        if (st->item[i].kind == LM_SMART_SWITCH && st->item[i].online && inside(st->item[i].label[0] ? &l->row[i] : &l->sw[i], px, py)) {
            *index = i;
            return LM_SMART_SWITCH_PART;
        }
        if (st->item[i].kind == LM_SMART_VACUUM)
            for (int b = 0; b < 2; b++)
                if (inside(&l->vac[i][b], px, py) && (b == 0 || st->item[i].dock_code[0])) {
                    *index = i;
                    return b == 0 ? LM_SMART_CLEAN : LM_SMART_DOCK;
                }
        if (st->item[i].kind == LM_SMART_BLIND && st->item[i].can_pos && st->item[i].online) {
            struct lm_smart_rect hit = { l->slider[i].x - 8, l->slider[i].y - 4, l->slider[i].w + 16, l->slider[i].h + 8 };
            if (inside(&hit, px, py)) {
                *index = i;
                return LM_SMART_SLIDER;
            }
        } else if (st->item[i].kind == LM_SMART_BLIND)
            for (int b = 0; b < 3; b++)
                if (inside(&l->btn[i][b], px, py)) {
                    *index = i;
                    return (enum lm_smart_part)(LM_SMART_OPEN + b);
                }
    }
    for (int i = 0; i < l->rows && i < st->n; i++) {                /* beside every control: the card itself */
        const struct lm_smart_item *it = &st->item[i];
        bool hit = it->label[0] ? (inside(&l->head[i], px, py) || (l->group[i].w > 0 && inside(&l->group[i], px, py) && !inside(&l->row[i], px, py))) : inside(&l->row[i], px, py);
        if (hit) {
            *index = i;
            return LM_SMART_CARD;
        }
    }
    return LM_SMART_NOTHING;
}

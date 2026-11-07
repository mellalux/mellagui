#include "mixer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "loop.h"

#define POLL_MS 3000
#define CMD_TIMEOUT_MS 2500
#define QUEUE_MAX 8

/* ---- parsers --------------------------------------------------------------------------------------------------- */

static void copy_trim(char *dst, size_t n, const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '"')
        s++;
    size_t len = strcspn(s, "\r\n");
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '"'))
        len--;
    snprintf(dst, n, "%.*s", (int)len, s);
}

/* "Volume: front-left: 49152 /  75% / -7.50 dB, front-right: ..." -> 75 (first channel); -1 when there is none. */
static int percent_in(const char *line)
{
    const char *p = strchr(line, '/');
    if (!p)
        return -1;
    p++;
    while (*p == ' ')
        p++;
    if (!isdigit((unsigned char)*p))
        return -1;
    return atoi(p);
}

static const char *after(const char *line, const char *key)
{
    size_t n = strlen(key);
    return strncmp(line, key, n) ? NULL : line + n;
}

int lm_mixer_parse_sinks(const char *text, struct lm_mixer_sink *out, int max)
{
    int n = 0;
    struct lm_mixer_sink *cur = NULL;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p = eol ? eol + 1 : p + len;
        const char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        const char *v;
        if (after(l, "Sink #")) {
            cur = n < max ? &out[n++] : NULL;
            if (cur)
                memset(cur, 0, sizeof(*cur));
        } else if (!cur) {
            continue;
        } else if ((v = after(l, "Name: "))) {
            copy_trim(cur->name, sizeof(cur->name), v);
        } else if ((v = after(l, "Description: "))) {
            copy_trim(cur->desc, sizeof(cur->desc), v);
        } else if ((v = after(l, "Mute: "))) {
            cur->mute = !strncmp(v, "yes", 3);
        } else if ((v = after(l, "Volume: "))) {                /* (not "Base Volume:") */
            int pc = percent_in(v);
            cur->volume = pc < 0 ? 0 : pc > 150 ? 150 : pc;
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++)                                 /* a sink needs a name to be addressed */
        if (out[i].name[0]) {
            if (m != i)
                out[m] = out[i];
            m++;
        }
    return m;
}

int lm_mixer_parse_streams(const char *text, struct lm_mixer_stream *out, int max)
{
    int n = 0;
    struct lm_mixer_stream *cur = NULL;
    char media[48] = "";
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p = eol ? eol + 1 : p + len;
        const char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        const char *v;
        if ((v = after(l, "Sink Input #"))) {
            if (cur && !cur->name[0])
                snprintf(cur->name, sizeof(cur->name), "%s", media[0] ? media : "stream");
            cur = n < max ? &out[n++] : NULL;
            media[0] = 0;
            if (cur) {
                memset(cur, 0, sizeof(*cur));
                cur->id = (unsigned)atoi(v);
            }
        } else if (!cur) {
            continue;
        } else if ((v = after(l, "Mute: "))) {
            cur->mute = !strncmp(v, "yes", 3);
        } else if ((v = after(l, "Volume: "))) {
            int pc = percent_in(v);
            cur->volume = pc < 0 ? 0 : pc > 150 ? 150 : pc;
        } else if ((v = after(l, "application.name = "))) {
            copy_trim(cur->name, sizeof(cur->name), v);
        } else if ((v = after(l, "media.name = "))) {
            copy_trim(media, sizeof(media), v);
        }
    }
    if (cur && !cur->name[0])
        snprintf(cur->name, sizeof(cur->name), "%s", media[0] ? media : "stream");
    return n;
}

bool lm_mixer_parse_wpctl(const char *text, int *volume, bool *mute)
{
    const char *p = strstr(text, "Volume:");
    if (!p)
        return false;
    p += 7;
    while (*p == ' ')
        p++;
    if (!isdigit((unsigned char)*p))
        return false;
    *volume = (int)(atof(p) * 100.0 + 0.5);
    *mute = strstr(p, "[MUTED]") != NULL;
    return true;
}

bool lm_mixer_parse_amixer(const char *text, int *volume, bool *mute)
{
    const char *p = text;
    bool found = false;
    while ((p = strchr(p, '['))) {
        p++;
        if (isdigit((unsigned char)*p)) {
            const char *q = p;
            while (isdigit((unsigned char)*q))
                q++;
            if (*q == '%') {
                if (!found)
                    *volume = atoi(p);
                found = true;
            }
        } else if (!strncmp(p, "off]", 4)) {
            *mute = true;
        } else if (!strncmp(p, "on]", 3)) {
            *mute = false;
        }
    }
    return found;
}

int lm_mixer_slider_value(int x0, int w, double x)
{
    if (w <= 0)
        return 0;
    double f = (x - x0) / (double)w;
    int v = (int)(f * 100.0 + 0.5);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

#define PAD 12
#define ROW_MASTER 34
#define ROW_OUTPUT 28
#define ROW_STREAM 36
#define HINT_H 16
#define PCT_W 46                       /* room for the percentage text right of a slider */

void lm_mixer_layout(const struct lm_rect *card, int title_h, int want, bool with_output, struct lm_mixer_layout *out)
{
    memset(out, 0, sizeof(*out));
    int x = card->x + PAD, w = card->w - 2 * PAD, y = card->y + title_h + 4;
    const int btn = 24;
    out->master_mute = lm_rect_make(x, y + (ROW_MASTER - btn) / 2, btn, btn);
    out->master_slider = lm_rect_make(x + btn + 12, y + (ROW_MASTER - 14) / 2, w - btn - 12 - PCT_W, 14);   /* "100%" follows it */
    y += ROW_MASTER;
    if (with_output) {
        out->output = lm_rect_make(x, y, w, ROW_OUTPUT - 4);
        y += ROW_OUTPUT;
    }
    int bottom = card->y + card->h - 8;
    int n = 0;
    y += 4;
    while (n < want && n < LM_MIXER_MAX_STREAMS && y + ROW_STREAM <= bottom) {
        out->stream[n].label = lm_rect_make(x, y, w, 16);
        out->stream[n].mute = lm_rect_make(x, y + 16, 22, 20);
        out->stream[n].slider = lm_rect_make(x + 22 + 10, y + 20, w - 22 - 10 - PCT_W, 12);
        y += ROW_STREAM;
        n++;
    }
    out->nstreams = n;
    if (y + HINT_H <= bottom + 4)
        out->hint = lm_rect_make(x, y, w, HINT_H);
}

static bool inside(const struct lm_rect *r, int x, int y, int slop)
{
    return r->w > 0 && x >= r->x - slop && x < r->x + r->w + slop && y >= r->y - slop && y < r->y + r->h + slop;
}

enum lm_mixer_part lm_mixer_hit(const struct lm_mixer_layout *l, int x, int y, int *index)
{
    if (index)
        *index = -1;
    if (inside(&l->master_mute, x, y, 2))
        return LM_MIXER_MASTER_MUTE;
    if (inside(&l->master_slider, x, y, 6))
        return LM_MIXER_MASTER_SLIDER;
    if (inside(&l->output, x, y, 0))
        return LM_MIXER_OUTPUT;
    for (int i = 0; i < l->nstreams; i++) {
        if (inside(&l->stream[i].mute, x, y, 2)) {
            if (index) *index = i;
            return LM_MIXER_STREAM_MUTE;
        }
        if (inside(&l->stream[i].slider, x, y, 6)) {
            if (index) *index = i;
            return LM_MIXER_STREAM_SLIDER;
        }
    }
    return LM_MIXER_NOTHING;
}

/* ---- the live mixer --------------------------------------------------------------------------------------------- */

struct cmd {
    char key[32];                       /* a newer command with the same key replaces a queued older one (slider drags) */
    char *argv[6];
};

struct lm_mixer {
    struct lm_loop *loop;
    void (*changed)(void *);
    void *data;
    struct lm_mixer_state st;           /* what is shown */
    struct lm_mixer_state nxt;          /* a refresh in progress; replaces `st` only if the user did nothing meanwhile */
    unsigned gen, refresh_gen;          /* bumped by every user action: a refresh that began earlier is stale */
    bool active;
    struct lm_timer *poll;
    struct lm_proc *job;                /* the refresh step or the command in flight */
    int step;                           /* refresh: 0 sinks, 1 default sink, 2 streams, 3 amixer, 4 wpctl; -1 = idle */
    struct lm_mixer_sink sinks_tmp[LM_MIXER_MAX_SINKS];
    int nsinks_tmp;
    char default_tmp[96];
    struct cmd queue[QUEUE_MAX];
    int nqueue;
    bool cmd_in_flight;
    bool dirty;                         /* refresh wanted again once the current one ends */
};

static void notify(struct lm_mixer *m)
{
    if (m->changed)
        m->changed(m->data);
}

static void cmd_free(struct cmd *c)
{
    for (int i = 0; c->argv[i]; i++)
        free(c->argv[i]);
    memset(c, 0, sizeof(*c));
}

static void start_refresh(struct lm_mixer *m);
static void run_next_cmd(struct lm_mixer *m);

static void cmd_done(void *data, const char *out)
{
    struct lm_mixer *m = data;
    (void)out;
    m->job = NULL;
    m->cmd_in_flight = false;
    if (m->nqueue) {
        run_next_cmd(m);
        return;
    }
    start_refresh(m);                                           /* show what the system really did */
}

static void run_next_cmd(struct lm_mixer *m)
{
    if (m->job || !m->nqueue)
        return;
    struct cmd c = m->queue[0];
    memmove(&m->queue[0], &m->queue[1], sizeof(m->queue[0]) * (size_t)(--m->nqueue));
    m->cmd_in_flight = true;
    m->job = lm_proc_run(m->loop, c.argv, CMD_TIMEOUT_MS, cmd_done, m);
    cmd_free(&c);
    if (!m->job) {
        m->cmd_in_flight = false;
        if (m->nqueue)
            run_next_cmd(m);
    }
}

/* Queue `argv` (NULL-terminated, copied). While a refresh runs, the command waits for it. */
static void queue_cmd(struct lm_mixer *m, const char *key, const char *const *argv)
{
    struct cmd c;
    memset(&c, 0, sizeof(c));
    snprintf(c.key, sizeof(c.key), "%s", key);
    for (int i = 0; argv[i] && i < 5; i++)
        c.argv[i] = strdup(argv[i]);
    int at = -1;
    for (int i = 0; i < m->nqueue; i++)
        if (!strcmp(m->queue[i].key, key))
            at = i;
    if (at >= 0) {
        cmd_free(&m->queue[at]);
        m->queue[at] = c;
    } else if (m->nqueue < QUEUE_MAX) {
        m->queue[m->nqueue++] = c;
    } else {
        cmd_free(&c);
        return;
    }
    if (!m->job)
        run_next_cmd(m);
}

static void refresh_step(struct lm_mixer *m);

static void refresh_finish(struct lm_mixer *m)
{
    m->step = -1;
    m->job = NULL;
    if (m->gen == m->refresh_gen)
        m->st = m->nxt;                                         /* (else the answer predates the user's change: drop it) */
    m->st.ready = true;
    notify(m);
    if (m->nqueue)
        run_next_cmd(m);
    else if (m->dirty) {
        m->dirty = false;
        start_refresh(m);
    }
}

static void refresh_cb(void *data, const char *out)
{
    struct lm_mixer *m = data;
    m->job = NULL;
    struct lm_mixer_state *st = &m->nxt;
    switch (m->step) {
    case 0:
        m->nsinks_tmp = lm_mixer_parse_sinks(out, m->sinks_tmp, LM_MIXER_MAX_SINKS);
        m->step = m->nsinks_tmp > 0 ? 1 : 4;                    /* no pactl / pulse server: try PipeWire's wpctl, then ALSA */
        break;
    case 1: {
        copy_trim(m->default_tmp, sizeof(m->default_tmp), out);
        st->backend = LM_MIXER_PULSE;
        st->nsinks = m->nsinks_tmp;
        memcpy(st->sinks, m->sinks_tmp, sizeof(st->sinks));
        st->cur = 0;
        for (int i = 0; i < st->nsinks; i++)
            if (!strcmp(st->sinks[i].name, m->default_tmp))
                st->cur = i;
        st->volume = st->sinks[st->cur].volume;
        st->mute = st->sinks[st->cur].mute;
        snprintf(st->output, sizeof(st->output), "%s", st->sinks[st->cur].desc[0] ? st->sinks[st->cur].desc : st->sinks[st->cur].name);
        m->step = 2;
        break;
    }
    case 2:
        st->nstreams = lm_mixer_parse_streams(out, st->streams, LM_MIXER_MAX_STREAMS);
        refresh_finish(m);
        return;
    case 4: {                                                   /* wpctl get-volume @DEFAULT_AUDIO_SINK@ */
        int vol = 0;
        bool mute = false;
        if (lm_mixer_parse_wpctl(out, &vol, &mute)) {
            st->backend = LM_MIXER_WPCTL;
            st->volume = vol;
            st->mute = mute;
            st->nsinks = st->nstreams = 0;
            snprintf(st->output, sizeof(st->output), "PipeWire");
            refresh_finish(m);
            return;
        }
        m->step = 3;
        break;
    }
    case 3: {                                                   /* amixer sget Master */
        int vol = 0;
        bool mute = false;
        if (lm_mixer_parse_amixer(out, &vol, &mute)) {
            st->backend = LM_MIXER_ALSA;
            st->volume = vol;
            st->mute = mute;
            st->nsinks = st->nstreams = 0;
            snprintf(st->output, sizeof(st->output), "ALSA Master");
        } else {
            st->backend = LM_MIXER_NONE;
            st->nsinks = st->nstreams = 0;
            st->output[0] = 0;
        }
        refresh_finish(m);
        return;
    }
    }
    refresh_step(m);
}

static void refresh_step(struct lm_mixer *m)
{
    char *list_sinks[] = { "pactl", "list", "sinks", NULL };
    char *get_default[] = { "pactl", "get-default-sink", NULL };
    char *list_inputs[] = { "pactl", "list", "sink-inputs", NULL };
    char *amixer[] = { "amixer", "sget", "Master", NULL };
    char *wpctl[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
    char *const *argv = m->step == 0 ? list_sinks : m->step == 1 ? get_default : m->step == 2 ? list_inputs :
                        m->step == 4 ? wpctl : amixer;
    m->job = lm_proc_run(m->loop, argv, CMD_TIMEOUT_MS, refresh_cb, m);
    if (!m->job) {                                              /* cannot fork: treat as "nothing there" */
        m->nxt.backend = LM_MIXER_NONE;
        refresh_finish(m);
    }
}

static void start_refresh(struct lm_mixer *m)
{
    if (m->job || m->cmd_in_flight) {
        m->dirty = true;
        return;
    }
    if (m->nqueue) {
        run_next_cmd(m);
        return;
    }
    m->nxt = m->st;
    m->refresh_gen = m->gen;
    m->step = 0;
    refresh_step(m);
}

static void poll_cb(void *data)
{
    struct lm_mixer *m = data;
    if (m->active)
        start_refresh(m);
}

struct lm_mixer *lm_mixer_create(struct lm_loop *loop, void (*changed)(void *), void *data)
{
    struct lm_mixer *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->loop = loop;
    m->changed = changed;
    m->data = data;
    m->step = -1;
    m->poll = lm_loop_add_timer(loop, poll_cb, m);
    return m;
}

void lm_mixer_destroy(struct lm_mixer *m)
{
    if (!m)
        return;
    if (m->job)
        lm_proc_cancel(m->job);
    if (m->poll)
        lm_timer_destroy(m->poll);
    for (int i = 0; i < m->nqueue; i++)
        cmd_free(&m->queue[i]);
    free(m);
}

void lm_mixer_set_active(struct lm_mixer *m, bool active)
{
    if (!m || m->active == active)
        return;
    m->active = active;
    if (active) {
        lm_timer_arm_ms(m->poll, POLL_MS, POLL_MS);
        start_refresh(m);
    } else {
        lm_timer_arm_ms(m->poll, 0, 0);
    }
}

const struct lm_mixer_state *lm_mixer_state(const struct lm_mixer *m)
{
    return &m->st;
}

/* ---- actions --------------------------------------------------------------------------------------------------- */

static void pct(char *out, size_t n, int v)
{
    snprintf(out, n, "%d%%", v < 0 ? 0 : v > 150 ? 150 : v);
}

void lm_mixer_set_master(struct lm_mixer *m, int percent)
{
    m->gen++;
    struct lm_mixer_state *st = &m->st;
    if (st->backend == LM_MIXER_NONE)
        return;
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    st->volume = percent;                                       /* at once: the slider follows the pointer */
    if (st->backend == LM_MIXER_PULSE)
        st->sinks[st->cur].volume = percent;
    notify(m);
    char v[16];
    pct(v, sizeof(v), percent);
    if (st->backend == LM_MIXER_PULSE) {
        const char *argv[] = { "pactl", "set-sink-volume", st->sinks[st->cur].name, v, NULL };
        queue_cmd(m, "master", argv);
    } else if (st->backend == LM_MIXER_WPCTL) {
        const char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL };
        queue_cmd(m, "master", argv);
    } else {
        const char *argv[] = { "amixer", "-q", "sset", "Master", v, NULL };
        queue_cmd(m, "master", argv);
    }
}

void lm_mixer_toggle_master_mute(struct lm_mixer *m)
{
    m->gen++;
    struct lm_mixer_state *st = &m->st;
    if (st->backend == LM_MIXER_NONE)
        return;
    st->mute = !st->mute;
    if (st->backend == LM_MIXER_PULSE)
        st->sinks[st->cur].mute = st->mute;
    notify(m);
    if (st->backend == LM_MIXER_PULSE) {
        const char *argv[] = { "pactl", "set-sink-mute", st->sinks[st->cur].name, st->mute ? "1" : "0", NULL };
        queue_cmd(m, "master-mute", argv);
    } else if (st->backend == LM_MIXER_WPCTL) {
        const char *argv[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", st->mute ? "1" : "0", NULL };
        queue_cmd(m, "master-mute", argv);
    } else {
        const char *argv[] = { "amixer", "-q", "sset", "Master", st->mute ? "mute" : "unmute", NULL };
        queue_cmd(m, "master-mute", argv);
    }
}

void lm_mixer_set_stream(struct lm_mixer *m, int index, int percent)
{
    m->gen++;
    struct lm_mixer_state *st = &m->st;
    if (st->backend != LM_MIXER_PULSE || index < 0 || index >= st->nstreams)
        return;
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    st->streams[index].volume = percent;
    notify(m);
    char id[16], v[16], key[32];
    snprintf(id, sizeof(id), "%u", st->streams[index].id);
    pct(v, sizeof(v), percent);
    snprintf(key, sizeof(key), "stream-%u", st->streams[index].id);
    const char *argv[] = { "pactl", "set-sink-input-volume", id, v, NULL };
    queue_cmd(m, key, argv);
}

void lm_mixer_toggle_stream_mute(struct lm_mixer *m, int index)
{
    m->gen++;
    struct lm_mixer_state *st = &m->st;
    if (st->backend != LM_MIXER_PULSE || index < 0 || index >= st->nstreams)
        return;
    st->streams[index].mute = !st->streams[index].mute;
    notify(m);
    char id[16], key[32];
    snprintf(id, sizeof(id), "%u", st->streams[index].id);
    snprintf(key, sizeof(key), "stream-mute-%u", st->streams[index].id);
    const char *argv[] = { "pactl", "set-sink-input-mute", id, st->streams[index].mute ? "1" : "0", NULL };
    queue_cmd(m, key, argv);
}

void lm_mixer_next_output(struct lm_mixer *m)
{
    m->gen++;
    struct lm_mixer_state *st = &m->st;
    if (st->backend != LM_MIXER_PULSE || st->nsinks < 2)
        return;
    int next = (st->cur + 1) % st->nsinks;
    st->cur = next;                                             /* shown at once; the refresh confirms it */
    st->volume = st->sinks[next].volume;
    st->mute = st->sinks[next].mute;
    snprintf(st->output, sizeof(st->output), "%s", st->sinks[next].desc[0] ? st->sinks[next].desc : st->sinks[next].name);
    notify(m);
    const char *argv[] = { "pactl", "set-default-sink", st->sinks[next].name, NULL };
    queue_cmd(m, "default-sink", argv);
}

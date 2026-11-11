/* Part of the window manager (see wm_priv.h): notifications */
#include "wm_priv.h"

/* ---- notifications ---------------------------------------------------------------------------------------------------- */

#define NOTE_W (lm_metrics.note_width)
#define NOTE_GAP (lm_metrics.note_gap)

static int64_t now_ms64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wraps `body` into at most LM_NOTIFY_LINES lines of LM_NOTIFY_COLS characters (breaking at blanks, "..." when it is cut). */
int note_wrap(const char *body, char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1])
{
    int n = 0;
    const char *p = body;
    while (*p == ' ')
        p++;
    while (*p && n < LM_NOTIFY_LINES) {
        const char *q = p;
        int cols = 0, last_blank = -1;
        const char *start = p;
        while (*q && *q != '\n' && cols < LM_NOTIFY_COLS) {
            if (*q == ' ')
                last_blank = (int)(q - start);
            do
                q++;
            while ((*q & 0xc0) == 0x80);
            cols++;
        }
        size_t len = (size_t)(q - start);
        if (*q && *q != '\n' && *q != ' ' && last_blank > 0)
            len = (size_t)last_blank;                                   /* break at the last blank */
        if (len >= sizeof(line[0]))
            len = sizeof(line[0]) - 1;
        memcpy(line[n], start, len);
        line[n][len] = 0;
        n++;
        p = start + len;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    if (*p && n > 0) {                                                 /* text left over: mark the cut */
        size_t l = strlen(line[n - 1]);
        if (l + 3 < sizeof(line[0]) && l >= 3) {
            line[n - 1][l - 3] = 0;
            strcat(line[n - 1], "...");
        }
    }
    return n;
}

static int note_height(const struct note *nt)
{
    char line[LM_NOTIFY_LINES][LM_NOTIFY_COLS * 4 + 1];
    int n = note_wrap(nt->body, line);
    return 8 + 20 + n * LM_FONT_H + (n ? 6 : 0) + 6;
}

/* The card rectangles, newest first, stacked down from the top right of the first output; false without an output. */
bool note_rects(const struct lm_wm *wm, struct lm_rect *r)
{
    struct lm_output *o = gadget_output(wm);
    if (!o)
        return false;
    int y = o->y + LM_PANEL_HEIGHT + 8;
    for (int i = 0; i < wm->nnotes; i++) {
        int h = note_height(&wm->notes[i]);
        r[i] = lm_rect_make(o->x + o->width - NOTE_W - 12, y, NOTE_W, h);
        y += h + NOTE_GAP;
    }
    return true;
}

static void note_damage(struct lm_wm *wm)
{
    struct lm_rect r[LM_NOTIFY_MAX];
    if (!note_rects(wm, r))
        return;
    for (int i = 0; i < wm->nnotes; i++)
        damage_rect(wm, &r[i]);
}

static void note_timer_arm(struct lm_wm *wm)
{
    if (!wm->note_timer)
        return;
    int64_t soon = 0, now = now_ms64();
    for (int i = 0; i < wm->nnotes; i++)
        if (wm->notes[i].expire && (!soon || wm->notes[i].expire < soon))
            soon = wm->notes[i].expire;
    if (soon)
        lm_timer_arm_ms(wm->note_timer, (uint32_t)LM_MAX(soon - now, 1), 0);
}

static void note_remove(struct lm_wm *wm, int idx)
{
    note_damage(wm);                                                   /* the stack closes up: everything below moves */
    memmove(&wm->notes[idx], &wm->notes[idx + 1], sizeof(wm->notes[0]) * (size_t)(wm->nnotes - idx - 1));
    wm->nnotes--;
    note_damage(wm);
}

void note_dismiss(struct lm_wm *wm, int id)
{
    for (int i = 0; i < wm->nnotes; i++)
        if (wm->notes[i].id == id) {
            note_remove(wm, i);
            note_timer_arm(wm);
            return;
        }
}

void note_tick(void *data)
{
    struct lm_wm *wm = data;
    int64_t now = now_ms64();
    for (int i = wm->nnotes - 1; i >= 0; i--)
        if (wm->notes[i].expire && wm->notes[i].expire <= now)
            note_remove(wm, i);
    note_timer_arm(wm);
}

static void notify_card(struct lm_wm *wm, const char *title, const char *body, int timeout_ms);

/* A notification: kept in the history (notifylog.h) and, unless "do not disturb" is on, shown as a card. */
void lm_wm_notify(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (!wm)
        return;
    if (wm->srv->notelog) {
        lm_notelog_add(wm->srv->notelog, title, body, time(NULL));
        if (wm->srv->settings)
            lm_settings_changed(wm->srv->settings);                    /* the readers of notes-N / notes-unread */
    }
    if (!wm->srv->config.dnd)
        notify_card(wm, title, body, timeout_ms);
}

/* The same without the history and whatever "do not disturb" says: feedback for the user's own switch. */
void lm_wm_notify_now(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (wm)
        notify_card(wm, title, body, timeout_ms);
}

static void notify_card(struct lm_wm *wm, const char *title, const char *body, int timeout_ms)
{
    if (!wm || !gadget_output(wm))
        return;
    note_damage(wm);
    if (wm->nnotes == LM_NOTIFY_MAX)                                   /* the oldest (last) one makes room */
        wm->nnotes--;
    memmove(&wm->notes[1], &wm->notes[0], sizeof(wm->notes[0]) * (size_t)wm->nnotes);
    struct note *nt = &wm->notes[0];
    memset(nt, 0, sizeof(*nt));
    nt->id = ++wm->next_note_id;
    snprintf(nt->title, sizeof(nt->title), "%.*s", LM_NOTIFY_COLS * 4, title ? title : "");
    snprintf(nt->body, sizeof(nt->body), "%s", body ? body : "");
    for (char *c = nt->title; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = ' ';
    nt->expire = timeout_ms > 0 ? now_ms64() + timeout_ms : 0;
    wm->nnotes++;
    note_damage(wm);
    note_timer_arm(wm);
}

#include "notifylog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lm_notelog {
    struct lm_notelog_entry e[LM_NOTELOG_MAX];      /* newest first */
    int n;
    int next_id;
};

struct lm_notelog *lm_notelog_create(void)
{
    return calloc(1, sizeof(struct lm_notelog));
}

void lm_notelog_destroy(struct lm_notelog *l)
{
    free(l);
}

static void copy_clean(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap + 1, "%.*s", (int)cap, src ? src : "");
    /* a cut in the middle of a UTF-8 character would leave a broken tail: drop it */
    size_t len = strlen(dst);
    if (len == cap) {
        size_t k = len;
        while (k > 0 && ((unsigned char)dst[k - 1] & 0xc0) == 0x80)
            k--;
        if (k > 0 && (unsigned char)dst[k - 1] >= 0xc0) {
            unsigned char lead = (unsigned char)dst[k - 1];
            size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
            if (len - (k - 1) < need)
                dst[k - 1] = 0;
        }
    }
    for (char *c = dst; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = ' ';
}

int lm_notelog_add(struct lm_notelog *l, const char *title, const char *body, time_t now)
{
    if (!l)
        return 0;
    if (l->n == LM_NOTELOG_MAX)
        l->n--;
    memmove(&l->e[1], &l->e[0], sizeof(l->e[0]) * (size_t)l->n);
    struct lm_notelog_entry *e = &l->e[0];
    memset(e, 0, sizeof(*e));
    e->id = ++l->next_id;
    e->time = now;
    e->unread = true;
    copy_clean(e->title, LM_NOTELOG_TITLE, title);
    copy_clean(e->body, LM_NOTELOG_BODY, body);
    l->n++;
    return e->id;
}

int lm_notelog_count(const struct lm_notelog *l)
{
    return l ? l->n : 0;
}

int lm_notelog_unread(const struct lm_notelog *l)
{
    int u = 0;
    for (int i = 0; l && i < l->n; i++)
        u += l->e[i].unread;
    return u;
}

const struct lm_notelog_entry *lm_notelog_get(const struct lm_notelog *l, int i)
{
    return l && i >= 0 && i < l->n ? &l->e[i] : NULL;
}

bool lm_notelog_remove(struct lm_notelog *l, int id)
{
    for (int i = 0; l && i < l->n; i++)
        if (l->e[i].id == id) {
            memmove(&l->e[i], &l->e[i + 1], sizeof(l->e[0]) * (size_t)(l->n - i - 1));
            l->n--;
            return true;
        }
    return false;
}

void lm_notelog_clear(struct lm_notelog *l)
{
    if (l)
        l->n = 0;
}

void lm_notelog_mark_read(struct lm_notelog *l)
{
    for (int i = 0; l && i < l->n; i++)
        l->e[i].unread = false;
}

void lm_notelog_text(const struct lm_notelog *l, int chunk, char *out, size_t n)
{
    size_t o = 0;
    if (!n)
        return;
    out[0] = 0;
    for (int i = chunk * LM_NOTELOG_CHUNK; l && i < l->n && i < (chunk + 1) * LM_NOTELOG_CHUNK; i++) {
        const struct lm_notelog_entry *e = &l->e[i];
        struct tm tm;
        localtime_r(&e->time, &tm);
        int w = snprintf(out + o, n - o, "%s%d\x1f%02d:%02d\x1f%s\x1f%s", o ? "\n" : "", e->id, tm.tm_hour, tm.tm_min, e->title, e->body);
        if (w < 0 || (size_t)w >= n - o)
            break;
        o += (size_t)w;
    }
}

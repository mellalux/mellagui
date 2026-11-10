#ifndef MELLAGUI_NOTIFYLOG_H
#define MELLAGUI_NOTIFYLOG_H

/*
 * The history of notifications (lm_wm_notify): the last LM_NOTELOG_MAX of them, newest first, kept in memory while the compositor runs.
 * A card on the screen goes after its time or a click; the history keeps the text so that it can be read later (mellagui-notes, the
 * bell of mellagui-status). "Do not disturb" (config.dnd) only keeps the cards from showing: everything is still logged and counted as
 * unread. The program side reads it through the settings values notes-0 .. notes-4 (lm_notelog_text) and changes it with
 * notes-del ID, notes-clear and notes-read.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define LM_NOTELOG_MAX 50
#define LM_NOTELOG_TITLE 60             /* bytes kept of a title ... */
#define LM_NOTELOG_BODY 160             /* ... and of a body */
#define LM_NOTELOG_CHUNK 10             /* entries in one settings value (notes-0 .. notes-4) */

struct lm_notelog_entry {
    int id;                             /* grows with every entry, never reused */
    time_t time;
    bool unread;
    char title[LM_NOTELOG_TITLE + 1];
    char body[LM_NOTELOG_BODY + 1];
};

struct lm_notelog;

struct lm_notelog *lm_notelog_create(void);
void lm_notelog_destroy(struct lm_notelog *l);

/* Adds an entry on top (control characters become spaces, the oldest entry goes when it is full). Returns its id. */
int lm_notelog_add(struct lm_notelog *l, const char *title, const char *body, time_t now);
int lm_notelog_count(const struct lm_notelog *l);
int lm_notelog_unread(const struct lm_notelog *l);
/* Entry i, 0 = the newest; NULL past the end. */
const struct lm_notelog_entry *lm_notelog_get(const struct lm_notelog *l, int i);
bool lm_notelog_remove(struct lm_notelog *l, int id);
void lm_notelog_clear(struct lm_notelog *l);
void lm_notelog_mark_read(struct lm_notelog *l);

/* Entries chunk * 10 .. chunk * 10 + 9 as lines "id<US>HH:MM<US>title<US>body" (local time; "" past the end) for the settings value notes-<chunk>. */
void lm_notelog_text(const struct lm_notelog *l, int chunk, char *out, size_t n);

#endif

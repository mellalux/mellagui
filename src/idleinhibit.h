#ifndef MELLAGUI_IDLEINHIBIT_H
#define MELLAGUI_IDLEINHIBIT_H

/*
 * zwp_idle_inhibit_manager_v1 / zwp_idle_inhibitor_v1: a program says "this surface is being watched, do not treat the lack of
 * input as idleness" (a film, a presentation). While at least one inhibitor's surface is visible (its window is mapped, not
 * minimized, on the current workspace) the screensaver does not start and the automatic screen lock does not engage; both count
 * their idle time from the moment the last inhibitor goes away or its surface is hidden. An inhibitor whose surface was destroyed
 * does nothing (as the protocol says). Offered to every client. Owner: idleinhibit.c.
 */

#include <stdbool.h>

struct lm_server;
struct lm_idle_inhibit;

struct lm_idle_inhibit *lm_idle_inhibit_create(struct lm_server *srv);
void lm_idle_inhibit_destroy(struct lm_idle_inhibit *ii);

/* true when some inhibitor's surface is visible right now (NULL = false). */
bool lm_idle_inhibited(const struct lm_idle_inhibit *ii);
/* Number of live inhibitors, visible or not (control socket `idle`). */
int lm_idle_inhibit_count(const struct lm_idle_inhibit *ii);

#endif

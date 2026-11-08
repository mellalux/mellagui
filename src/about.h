#ifndef MELLAGUI_ABOUT_H
#define MELLAGUI_ABOUT_H

/*
 * The About dialog's text: what MellaGUI is running on. Collected when the dialog opens (nothing is kept up to date while it is
 * open). Pure reading of /proc, uname and the server's own state; no side effects.
 */

#define LM_ABOUT_ROWS 12
#define LM_ABOUT_LABEL 12
#define LM_ABOUT_VALUE 64

struct lm_server;

struct lm_about_row {
    char label[LM_ABOUT_LABEL];
    char value[LM_ABOUT_VALUE];
};

/* Fills up to `max` rows ("Computer", "User", "System", "Uptime", "Processor", "Memory", "Display", "Graphics", "Session", "Config" ...)
 * and returns how many. Rows whose information is unavailable are left out. */
int lm_about_collect(struct lm_server *srv, struct lm_about_row *rows, int max);

#endif

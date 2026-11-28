#ifndef MELLAGUI_CCTVCTL_H
#define MELLAGUI_CCTVCTL_H

/*
 * Steering a network camera: moving it, its remembered positions, the night picture and the lamp. A thread with a session of its own sends the
 * commands in the order they were asked for, so the window never waits.
 *
 * A camera that has been told to move goes on until it is told to stop. Therefore: a stop is sent when it is asked for, when a movement has lasted
 * CCTV_MOVE_MAX_MS, and when the thing is freed (also by a program that is closing); a stop that cannot be delivered is tried again.
 * A refused login ends it: nothing more is sent (cameras lock accounts).
 *
 * The night values are the numbers of "Camera.Param" DayNightColor; the lamp is "Camera.WhiteLight" WorkMode.
 */

#include <stdbool.h>

#include "cctvrec.h"

#define CCTV_MOVE_MAX_MS 20000
#define CCTV_SPEED 5
#define CCTV_POSITIONS 4                /* the remembered views the window offers: positions 1..4 */

enum { CCTV_NIGHT_AUTO = 0, CCTV_NIGHT_COLOUR = 1, CCTV_NIGHT_GREY = 2, CCTV_NIGHT_SMART = 3, CCTV_NIGHT_LAMP = 4, CCTV_NIGHT_INFRARED = 5 };

struct cctv_view {
    bool known;                         /* the camera has told its settings */
    int night;                          /* CCTV_NIGHT_*, -1 = it has no such setting */
    char lamp[24];                      /* "Close", "KeepOpen", "Auto" ..., "" = it has no lamp */
    char msg[160];                      /* the last thing worth saying ("" = nothing) */
    unsigned said;                      /* goes up with every message */
    unsigned stored[CCTV_POSITIONS + 1]; /* [n] goes up when the camera has taken a view as position n */
    unsigned serial;                    /* goes up with every change */
};

struct cctv_ctl;

struct cctv_ctl *cctv_ctl_start(const struct cctv_target *target);
/* "DirectionLeft", "DirectionUp" ... starts a movement (stopping the one before); NULL stops. */
void cctv_ctl_move(struct cctv_ctl *c, const char *direction);
/* Remember the view as position n (1..), or go back to it. */
void cctv_ctl_position(struct cctv_ctl *c, int n, bool save);
/* Change the night picture (night >= 0) or the lamp (lamp != NULL); both are read back from the camera afterwards. */
void cctv_ctl_night(struct cctv_ctl *c, int night, const char *lamp);
/* Ask the camera for its settings. */
void cctv_ctl_query(struct cctv_ctl *c);
void cctv_ctl_view(struct cctv_ctl *c, struct cctv_view *out);
/* Ask it to end (a movement is stopped first); returns at once. */
void cctv_ctl_quit(struct cctv_ctl *c);
/* Quits, waits and frees. */
void cctv_ctl_free(struct cctv_ctl *c);

#endif

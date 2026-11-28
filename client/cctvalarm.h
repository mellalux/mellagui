#ifndef MELLAGUI_CCTVALARM_H
#define MELLAGUI_CCTVALARM_H

/*
 * Watching the cameras for alarms. What is wanted of each camera is a line "NAME [notify] [record]" of a small file; the WATCHER is a process without a
 * window (mellagui-cctv --watch) that lives while some camera wants something, one for a session. It keeps a session with every such camera, asks to be
 * told its alarms, shows a card ($MELLAGUI_CCTV_NOTIFY, else mellagui-notify) and records the movement into the folder of the recordings: from the
 * alarm until CCTV_AFTER_MS ($MELLAGUI_CCTV_AFTER_MS) after the last one, a file at most CCTV_LONGEST_MS long. A lost connection is made again; a camera
 * that refuses the login is left alone until the watcher is started anew.
 */

#include <stdbool.h>

#include "cctvrec.h"

#define CCTV_AFTER_MS 30000
#define CCTV_LONGEST_MS 300000
#define CCTV_NOTIFY_EVERY_MS 60000      /* at most one card a minute for a camera and a kind of alarm */

struct cctv_want {
    bool notify, record;
};

/* want[i] = what the file says about the camera called name[i] (nothing when it is not there). */
void cctv_wants_load(const char *path, const char *const *name, struct cctv_want *want, int n);
/* Writes the file (0600). False when that fails. */
bool cctv_wants_save(const char *path, const char *const *name, const struct cctv_want *want, int n);

/* The watcher: returns when nothing is wanted, the Wayland session is gone or it is told to end (SIGTERM / SIGINT). 0, or 1 when another one watches
 * already or it cannot begin. */
int cctv_watch(const struct cctv_target *t, int n, const char *wants_path, const char *dir, const char *ffmpeg);

#endif

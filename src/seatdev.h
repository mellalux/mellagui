#ifndef LUMEN_SEATDEV_H
#define LUMEN_SEATDEV_H

/*
 * Privileged device access ("seat session"). Owner: seatdev.c.
 *
 * Baseline: open(2) the device directly. Optionally, when a systemd-logind
 * session is usable, devices are obtained with
 * org.freedesktop.login1.Session.TakeDevice through sd-bus functions loaded
 * by dlopen("libsystemd.so.0") with hand-declared prototypes (NO systemd
 * headers). Everything about logind is optional: any failure (no library, no
 * session, no D-Bus) silently falls back to direct open, logged at debug level.
 * This is what lets an ordinary user, not in group 'input', still get
 * /dev/input/event* and /dev/dri/card* on the active seat.
 *
 * With logind, VT switching is arbitrated by logind: devices receive
 * PauseDevice / ResumeDevice signals (reported through the listener below)
 * and DRM master is handed over implicitly. Without logind the DRM backend
 * does its own VT_PROCESS handling.
 */

#include <stdbool.h>
#include "loop.h"

struct lm_seatdev;

struct lm_seatdev_listener {
    void *data;
    /* Device temporarily revoked (VT switched away). Stop using `fd`. */
    void (*paused)(void *data, int fd);
    /* Device available again; `new_fd` replaces the old fd (may be == fd). */
    void (*resumed)(void *data, int old_fd, int new_fd);
};

/*
 * Never returns NULL for "logind not available": the object then simply does
 * direct opens. NULL only on out-of-memory. Registers sd-bus fd(s) with the
 * loop when logind is in use.
 */
struct lm_seatdev *lm_seatdev_create(struct lm_loop *loop);
void lm_seatdev_destroy(struct lm_seatdev *sd);

/* True if logind TakeDevice is in use. */
bool lm_seatdev_uses_logind(const struct lm_seatdev *sd);
/* Session currently active (always true without logind). */
bool lm_seatdev_session_active(const struct lm_seatdev *sd);

/*
 * Open `path` (flags like O_RDWR | O_CLOEXEC | O_NONBLOCK for evdev). Returns
 * an fd >= 0 or -errno. `listener` may be NULL; if given it is copied.
 * With logind the device node is stat()ed for its major/minor, then taken.
 * On direct-open EACCES on an evdev node: the caller gets -EACCES and prints
 * the group 'input' hint; seatdev itself stays quiet at debug level.
 */
int lm_seatdev_open(struct lm_seatdev *sd, const char *path, int flags,
                    const struct lm_seatdev_listener *listener);
/* Release (ReleaseDevice if taken via logind) and close. */
void lm_seatdev_close(struct lm_seatdev *sd, int fd);

/*
 * Ask logind to switch to VT `vt` (Seat.SwitchTo). Returns 0 on success,
 * -1 if logind is not in use (caller falls back to VT_ACTIVATE).
 */
int lm_seatdev_switch_vt(struct lm_seatdev *sd, int vt);

#endif

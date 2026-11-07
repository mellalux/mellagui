#ifndef LUMEN_SEATDEV_H
#define LUMEN_SEATDEV_H

/*
 * Device access for the backends. Owner: seatdev.c.
 *
 * Only direct open(2) is implemented (logind TakeDevice is not), so an
 * unprivileged user needs to be in groups 'video' and 'input'. All device
 * opens and closes go through here, so a logind path could be added without
 * touching the callers.
 */

struct lm_seatdev;

/* NULL only on out-of-memory. */
struct lm_seatdev *lm_seatdev_create(void);
void lm_seatdev_destroy(struct lm_seatdev *sd);

/*
 * Open `path` (flags like O_RDWR | O_NONBLOCK for evdev; O_CLOEXEC is added).
 * Returns an fd >= 0 or -errno. On EACCES for an evdev node the caller prints
 * the group 'input' hint; seatdev itself stays quiet.
 */
int lm_seatdev_open(struct lm_seatdev *sd, const char *path, int flags);
void lm_seatdev_close(struct lm_seatdev *sd, int fd);

#endif

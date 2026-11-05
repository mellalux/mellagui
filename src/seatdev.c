/*
 * Device access. Only the direct open(2) path is implemented: logind
 * TakeDevice (see seatdev.h) is not, so an unprivileged user needs to be in
 * groups 'video' and 'input'. The API leaves room for it without changes.
 */
#include "seatdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "log.h"

struct lm_seatdev {
    struct lm_loop *loop;
};

struct lm_seatdev *lm_seatdev_create(struct lm_loop *loop)
{
    struct lm_seatdev *sd = calloc(1, sizeof(*sd));
    if (!sd)
        return NULL;
    sd->loop = loop;
    lm_debug("seatdev: using direct device access (no logind)");
    return sd;
}

void lm_seatdev_destroy(struct lm_seatdev *sd)
{
    free(sd);
}

bool lm_seatdev_uses_logind(const struct lm_seatdev *sd)
{
    (void)sd;
    return false;
}

bool lm_seatdev_session_active(const struct lm_seatdev *sd)
{
    (void)sd;
    return true;
}

int lm_seatdev_open(struct lm_seatdev *sd, const char *path, int flags,
                    const struct lm_seatdev_listener *listener)
{
    (void)sd;
    (void)listener;
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    return fd;
}

void lm_seatdev_close(struct lm_seatdev *sd, int fd)
{
    (void)sd;
    if (fd >= 0)
        close(fd);
}

int lm_seatdev_switch_vt(struct lm_seatdev *sd, int vt)
{
    (void)sd;
    (void)vt;
    return -1;
}

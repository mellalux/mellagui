/* Device access: direct open(2) only, see seatdev.h. */
#include "seatdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "log.h"

struct lm_seatdev {
    int unused; /* no state yet; a logind session would live here */
};

struct lm_seatdev *lm_seatdev_create(void)
{
    struct lm_seatdev *sd = calloc(1, sizeof(*sd));
    if (sd)
        lm_debug("seatdev: using direct device access (no logind)");
    return sd;
}

void lm_seatdev_destroy(struct lm_seatdev *sd)
{
    free(sd);
}

int lm_seatdev_open(struct lm_seatdev *sd, const char *path, int flags)
{
    (void)sd;
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

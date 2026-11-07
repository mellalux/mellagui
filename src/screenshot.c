#include "screenshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "png.h"
#include "render.h"
#include "seat.h"
#include "sound.h"

/* Writes the PNG as dir/mellagui-YYYYMMDD-HHMMSS[-N].png (0600: a screen can show secrets). Returns false on failure. */
static bool save_file(const char *dir, const uint8_t *png, size_t len, char *path, size_t pathsz)
{
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        lm_warn("screenshot: cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    int fd = -1;
    for (int n = 0; n < 100 && fd < 0; n++) {
        if (n == 0)
            snprintf(path, pathsz, "%s/mellagui-%s.png", dir, stamp);
        else
            snprintf(path, pathsz, "%s/mellagui-%s-%d.png", dir, stamp, n);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) {
            lm_warn("screenshot: cannot write %s: %s", path, strerror(errno));
            return false;
        }
    }
    if (fd < 0)
        return false;
    const uint8_t *p = png;
    size_t left = len;
    while (left > 0) {
        ssize_t k = write(fd, p, left);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            lm_warn("screenshot: write to %s failed: %s", path, strerror(errno));
            close(fd);
            unlink(path);
            return false;
        }
        p += k;
        left -= (size_t)k;
    }
    return close(fd) == 0;
}

bool lm_screenshot_take(struct lm_server *srv, const struct lm_rect *rect, bool to_file)
{
    int w = 0, h = 0;
    uint32_t *px = srv->render ? lm_render_capture(srv->render, rect, &w, &h) : NULL;      /* freed below */
    if (!px) {
        lm_warn("screenshot: nothing to capture");
        return false;
    }
    size_t len = 0;
    uint8_t *png = lm_png_encode_xrgb(px, w, h, (size_t)w * 4, &len);
    if (!png) {
        free(px);
        lm_warn("screenshot: PNG encoding failed (%dx%d)", w, h);
        return false;
    }
    char path[640] = "";
    bool saved = false;
    if (to_file && srv->config.screenshot_dir && *srv->config.screenshot_dir)
        saved = save_file(srv->config.screenshot_dir, png, len, path, sizeof(path));
    bool clip = srv->seat && lm_seat_set_selection_data(srv->seat, "image/png", png, len);   /* takes png over */
    if (!srv->seat)
        free(png);
    lm_info("screenshot %dx%d%s%s%s", w, h, saved ? " -> " : "", saved ? path : "", clip ? " (clipboard)" : "");
    if (saved || clip) {
        const char *slash = strrchr(path, '/');
        lm_render_shot_feedback(srv->render, rect, px, w, h, saved ? (slash ? slash + 1 : path) : "Copied to clipboard");
    }
    free(px);
    if (srv->config.sound && strcmp(srv->config.backend, "headless") != 0)
        lm_sound_play(LM_SOUND_SHOT);
    return saved || clip;
}

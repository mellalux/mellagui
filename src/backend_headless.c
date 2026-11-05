/*
 * Offscreen backend used by automated tests: one output rendered into
 * memory, a 60 Hz frame clock after each present, and a line-based control
 * socket (see backend.h) to dump frames and inject input.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "log.h"
#include "render.h"
#include "seat.h"
#include "wm.h"

#define CTL_MAX_CLIENTS 8
#define CTL_LINE_MAX 4096

struct ctl_client {
    int fd;
    struct lm_source *source;
    char buf[CTL_LINE_MAX];
    size_t len;
    struct headless *h;
};

struct headless {
    struct lm_backend base;
    struct lm_output out;
    struct lm_framebuffer fb;
    uint64_t frames;
    struct lm_timer *ready_timer;
    int ctl_fd;
    struct lm_source *ctl_source;
    struct ctl_client *clients[CTL_MAX_CLIENTS];
    bool output_live;
    int w, h;
};

static struct headless *HL(struct lm_backend *b) { return (struct headless *)b; }

/* ---- backend ops ------------------------------------------------------------------- */

static void ready_fire(void *data)
{
    struct headless *h = data;
    h->base.listener.output_ready(h->base.listener.data, &h->out, lm_now_msec32());
}

static struct lm_framebuffer *hl_get_framebuffer(struct lm_output *out)
{
    struct headless *h = HL(out->backend);
    h->fb.age = h->frames ? 1 : 0;
    return &h->fb;
}

static int hl_present(struct lm_output *out, const struct lm_rect *damage, int n)
{
    struct headless *h = HL(out->backend);
    (void)damage;
    (void)n;
    h->frames++;
    lm_timer_arm_ms(h->ready_timer, 16, 0);
    return 0;
}

int lm_backend_headless_dump(struct lm_backend *b, const char *path)
{
    struct headless *h = HL(b);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", h->fb.width, h->fb.height);
    uint8_t *row = malloc((size_t)h->fb.width * 3);
    if (!row) {
        fclose(f);
        return -1;
    }
    for (int y = 0; y < h->fb.height; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)h->fb.pixels + (size_t)y * (size_t)h->fb.stride);
        for (int x = 0; x < h->fb.width; x++) {
            row[x * 3] = (uint8_t)(src[x] >> 16);
            row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            row[x * 3 + 2] = (uint8_t)src[x];
        }
        fwrite(row, 3, (size_t)h->fb.width, f);
    }
    free(row);
    return fclose(f) == 0 ? 0 : -1;
}

/* ---- control socket ------------------------------------------------------------------- */

static void ctl_reply(struct ctl_client *c, const char *fmt, ...) LM_PRINTF(2, 3);
static void ctl_reply(struct ctl_client *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for (int off = 0; off < n;) {
        ssize_t k = write(c->fd, buf + off, (size_t)(n - off));
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (int)k;
    }
}

static void ctl_flush(struct headless *h)
{
    if (h->base.server->render)
        lm_render_flush(h->base.server->render);
}

static void ctl_command(struct ctl_client *c, char *line)
{
    struct headless *h = c->h;
    struct lm_server *srv = h->base.server;
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) {
        ctl_reply(c, "err empty command\n");
        return;
    }
    char *a1 = strtok_r(NULL, " \t", &save);
    char *a2 = strtok_r(NULL, " \t", &save);
    char *a3 = strtok_r(NULL, " \t", &save);

    if (!strcmp(cmd, "dump")) {
        if (!a1) { ctl_reply(c, "err usage: dump <path>\n"); return; }
        ctl_flush(h);
        if (lm_backend_headless_dump(&h->base, a1) < 0)
            ctl_reply(c, "err cannot write %s: %s\n", a1, strerror(errno));
        else
            ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "sync")) {
        ctl_flush(h);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "key")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: key <code> <0|1>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_KEY };
        ev.u.key.code = (uint32_t)strtoul(a1, NULL, 0);
        ev.u.key.pressed = atoi(a2) != 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "motion")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: motion <dx> <dy>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_MOTION };
        ev.u.motion.dx = atof(a1);
        ev.u.motion.dy = atof(a2);
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "abs")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: abs <x> <y>\n"); return; }
        if (srv->seat)
            lm_seat_pointer_warp(srv->seat, atof(a1), atof(a2));
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "button")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: button <code> <0|1>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_BUTTON };
        ev.u.button.code = (uint32_t)strtoul(a1, NULL, 0);
        ev.u.button.pressed = atoi(a2) != 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "axis")) {
        if (!a1 || !a2) { ctl_reply(c, "err usage: axis <v|h> <value> <discrete>\n"); return; }
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_AXIS };
        ev.u.axis.orientation = a1[0] == 'h' ? LM_POINTER_AXIS_HORIZONTAL : LM_POINTER_AXIS_VERTICAL;
        ev.u.axis.value = atof(a2);
        ev.u.axis.discrete = a3 ? atoi(a3) : 0;
        if (srv->input)
            lm_input_inject(srv->input, &ev);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "windows")) {
        char *text = malloc(65536);
        if (!text) { ctl_reply(c, "err out of memory\n"); return; }
        if (srv->wm)
            lm_wm_describe(srv->wm, text, 65536);
        else
            text[0] = 0;
        size_t len = strlen(text);
        for (size_t off = 0; off < len;) {
            ssize_t k = write(c->fd, text + off, len - off);
            if (k < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)k;
        }
        free(text);
        ctl_reply(c, "ok\n");
    } else if (!strcmp(cmd, "quit")) {
        ctl_reply(c, "ok\n");
        lm_server_quit(srv);
    } else {
        ctl_reply(c, "err unknown command %s\n", cmd);
    }
}

static void ctl_close(struct ctl_client *c)
{
    struct headless *h = c->h;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (h->clients[i] == c)
            h->clients[i] = NULL;
    lm_source_remove(c->source);
    close(c->fd);
    free(c);
}

static void ctl_client_cb(int fd, uint32_t events, void *data)
{
    struct ctl_client *c = data;
    if (events & EPOLLIN) {
        ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            ctl_close(c);
            return;
        }
        c->len += (size_t)n;
        char *nl;
        while ((nl = memchr(c->buf, '\n', c->len))) {
            *nl = 0;
            char line[CTL_LINE_MAX];
            snprintf(line, sizeof(line), "%s", c->buf);
            size_t used = (size_t)(nl - c->buf) + 1;
            memmove(c->buf, c->buf + used, c->len - used);
            c->len -= used;
            ctl_command(c, line);
        }
        if (c->len >= sizeof(c->buf) - 1) {
            ctl_reply(c, "err line too long\n");
            ctl_close(c);
            return;
        }
    } else if (events & (EPOLLHUP | EPOLLERR)) {
        ctl_close(c);
    }
}

static void ctl_accept_cb(int fd, uint32_t events, void *data)
{
    struct headless *h = data;
    (void)events;
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0)
        return;
    int slot = -1;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (!h->clients[i]) { slot = i; break; }
    struct ctl_client *c = slot >= 0 ? calloc(1, sizeof(*c)) : NULL;
    if (!c) {
        close(cfd);
        return;
    }
    c->fd = cfd;
    c->h = h;
    c->source = lm_loop_add_fd(h->base.server->loop, cfd, EPOLLIN, ctl_client_cb, c);
    if (!c->source) {
        close(cfd);
        free(c);
        return;
    }
    h->clients[slot] = c;
}

static int ctl_listen(struct headless *h, const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(sa.sun_path)) {
        lm_error("headless: control path too long: %s", path);
        return -1;
    }
    strcpy(sa.sun_path, path);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0) {
        lm_error("headless: cannot listen on %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    h->ctl_fd = fd;
    h->ctl_source = lm_loop_add_fd(h->base.server->loop, fd, EPOLLIN, ctl_accept_cb, h);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------------------ */

static int hl_init(struct lm_backend *b)
{
    struct headless *h = HL(b);
    struct lm_server *srv = b->server;
    h->fb.width = h->w;
    h->fb.height = h->h;
    h->fb.stride = h->w * 4;
    h->fb.pixels = calloc((size_t)h->w * (size_t)h->h, 4);
    if (!h->fb.pixels)
        return -1;
    if (srv->config.control_path && ctl_listen(h, srv->config.control_path) < 0)
        return -1;
    h->ready_timer = lm_loop_add_timer(srv->loop, ready_fire, h);

    memset(&h->out, 0, sizeof(h->out));
    h->out.backend = b;
    snprintf(h->out.name, sizeof(h->out.name), "headless-0");
    snprintf(h->out.make, sizeof(h->out.make), "Lumen");
    snprintf(h->out.model, sizeof(h->out.model), "headless");
    h->out.width = h->w;
    h->out.height = h->h;
    h->out.refresh_mhz = 60000;
    h->out.phys_width_mm = h->w * 254 / 960;
    h->out.phys_height_mm = h->h * 254 / 960;
    h->out.scale = 1;
    lm_list_append(&b->outputs, &h->out.link);
    h->output_live = true;
    b->session_active = true;
    if (b->listener.output_added)
        b->listener.output_added(b->listener.data, &h->out);
    lm_timer_arm_ms(h->ready_timer, 1, 0);
    return 0;
}

static void hl_destroy(struct lm_backend *b)
{
    struct headless *h = HL(b);
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (h->clients[i])
            ctl_close(h->clients[i]);
    if (h->output_live) {
        if (b->listener.output_removed)
            b->listener.output_removed(b->listener.data, &h->out);
        lm_list_remove(&h->out.link);
    }
    if (h->ctl_source)
        lm_source_remove(h->ctl_source);
    if (h->ctl_fd > 0) {
        close(h->ctl_fd);
        if (b->server->config.control_path)
            unlink(b->server->config.control_path);
    }
    if (h->ready_timer)
        lm_timer_destroy(h->ready_timer);
    free(h->fb.pixels);
    free(h);
}

static const struct lm_backend_ops headless_ops = {
    .name = "headless",
    .init = hl_init,
    .destroy = hl_destroy,
    .get_framebuffer = hl_get_framebuffer,
    .present = hl_present,
    .switch_vt = NULL,
};

struct lm_backend *lm_backend_headless_create(struct lm_server *srv, int w, int h)
{
    struct headless *hl = calloc(1, sizeof(*hl));
    if (!hl)
        return NULL;
    hl->base.ops = &headless_ops;
    hl->base.server = srv;
    lm_list_init(&hl->base.outputs);
    hl->w = w > 0 ? w : 1280;
    hl->h = h > 0 ? h : 720;
    hl->ctl_fd = -1;
    return &hl->base;
}

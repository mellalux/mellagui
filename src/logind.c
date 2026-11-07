#include "logind.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "logind.c speaks little-endian D-Bus only"
#endif

#define MAX_MSG (1u << 20)
#define CALL_TIMEOUT_MS 3000
#define MAX_FDS 16
#define MAX_QUEUE 32

#define DBUS_DEST "org.freedesktop.DBus"
#define LOGIN_DEST "org.freedesktop.login1"
#define LOGIN_PATH "/org/freedesktop/login1"
#define LOGIN_MANAGER "org.freedesktop.login1.Manager"
#define LOGIN_SESSION "org.freedesktop.login1.Session"
#define LOGIN_SEAT "org.freedesktop.login1.Seat"

enum { MSG_CALL = 1, MSG_RETURN = 2, MSG_ERROR = 3, MSG_SIGNAL = 4 };
enum { FLAG_NO_REPLY = 1 };

struct sigev {
    bool resume;
    uint32_t major, minor;
    char type[16];
    int fd;
};

struct lm_logind {
    int fd;
    struct lm_loop *loop;
    struct lm_source *src;
    struct lm_idle *idle;
    bool controlled;
    uint32_t serial;
    uint8_t *rbuf;
    size_t rlen, rcap;
    int fds[MAX_FDS];
    int nfds;
    struct sigev q[MAX_QUEUE];
    int nq;
    char session[256];
    char seat[256];                     /* the session's Seat object ("seat/self" does not exist for seats) */
    bool watching;
    dev_t wdev;
    lm_logind_pause_fn wpause;
    lm_logind_resume_fn wresume;
    void *wdata;
    char errname[96];                   /* of the last D-Bus error reply */
    struct { int fd; uint32_t major, minor; } devs[64];     /* what TakeDevice handed out, to release by fd */
    int ndevs;
};

struct msg {
    int type;
    uint32_t serial, reply_serial, nfds_hdr;
    char path[256], iface[96], member[96], errname[96], sig[32];
    int fds[MAX_FDS];
    int nfds;
    uint8_t *body;
    size_t body_len;
};

static void msg_free(struct msg *m)
{
    for (int i = 0; i < m->nfds; i++)
        if (m->fds[i] >= 0)
            close(m->fds[i]);
    free(m->body);
    memset(m, 0, sizeof(*m));
}

/* ---- marshalling ------------------------------------------------------------------------------------------------- */

struct mb {
    uint8_t *p;
    size_t len, cap;
    bool oom;
};

static void mb_put(struct mb *m, const void *data, size_t n)
{
    if (m->oom)
        return;
    if (m->len + n > m->cap) {
        size_t nc = (m->len + n) * 2 + 64;
        uint8_t *t = realloc(m->p, nc);
        if (!t) {
            m->oom = true;
            return;
        }
        m->p = t;
        m->cap = nc;
    }
    if (n)
        memcpy(m->p + m->len, data, n);
    m->len += n;
}

static void mb_align(struct mb *m, size_t a)
{
    static const uint8_t zero[8] = { 0 };
    size_t pad = (a - m->len % a) % a;
    mb_put(m, zero, pad);
}

static void mb_u8(struct mb *m, uint8_t v) { mb_put(m, &v, 1); }

static void mb_u32(struct mb *m, uint32_t v)
{
    mb_align(m, 4);
    mb_put(m, &v, 4);
}

static void mb_string(struct mb *m, const char *s)              /* s and o */
{
    uint32_t n = (uint32_t)strlen(s);
    mb_u32(m, n);
    mb_put(m, s, (size_t)n + 1);
}

static void mb_sig(struct mb *m, const char *s)                 /* g */
{
    mb_u8(m, (uint8_t)strlen(s));
    mb_put(m, s, strlen(s) + 1);
}

/* One header field: (code, variant). */
static void mb_field(struct mb *m, uint8_t code, char type, const char *s)
{
    mb_align(m, 8);
    mb_u8(m, code);
    char sg[2] = { type, 0 };
    mb_sig(m, sg);
    if (type == 'g')
        mb_sig(m, s);
    else
        mb_string(m, s);
}

struct rd {
    const uint8_t *p;
    size_t len, pos;
    bool bad;
};

static void rd_align(struct rd *r, size_t a)
{
    size_t n = (r->pos + a - 1) & ~(a - 1);
    if (n > r->len) {
        r->bad = true;
        r->pos = r->len;
    } else {
        r->pos = n;
    }
}

static uint8_t rd_u8(struct rd *r)
{
    if (r->bad || r->pos + 1 > r->len) {
        r->bad = true;
        return 0;
    }
    return r->p[r->pos++];
}

static uint32_t rd_u32(struct rd *r)
{
    rd_align(r, 4);
    if (r->bad || r->pos + 4 > r->len) {
        r->bad = true;
        return 0;
    }
    uint32_t v;
    memcpy(&v, r->p + r->pos, 4);
    r->pos += 4;
    return v;
}

static const char *rd_string(struct rd *r)                      /* s and o: NUL-terminated inside the buffer */
{
    uint32_t n = rd_u32(r);
    if (r->bad || (size_t)n + 1 > r->len - r->pos || r->p[r->pos + n] != 0) {
        r->bad = true;
        return "";
    }
    const char *s = (const char *)r->p + r->pos;
    r->pos += (size_t)n + 1;
    return s;
}

static const char *rd_sigstr(struct rd *r)                      /* g */
{
    uint8_t n = rd_u8(r);
    if (r->bad || (size_t)n + 1 > r->len - r->pos || r->p[r->pos + n] != 0) {
        r->bad = true;
        return "";
    }
    const char *s = (const char *)r->p + r->pos;
    r->pos += (size_t)n + 1;
    return s;
}

/* ---- connection --------------------------------------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    int64_t end = now_ms() + CALL_TIMEOUT_MS;
    while (n > 0) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                int64_t left = end - now_ms();
                if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
                    return -ETIMEDOUT;
                continue;
            }
            return -errno;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Reads what is there (non-blocking) into rbuf, collecting passed descriptors. 1 = data, 0 = nothing, <0 = error / EOF. */
static int fill(struct lm_logind *l)
{
    if (l->rcap - l->rlen < 4096) {
        size_t nc = l->rcap ? l->rcap * 2 : 8192;
        uint8_t *t = realloc(l->rbuf, nc);
        if (!t)
            return -ENOMEM;
        l->rbuf = t;
        l->rcap = nc;
    }
    char ctl[CMSG_SPACE(sizeof(int) * MAX_FDS)];
    struct iovec iov = { .iov_base = l->rbuf + l->rlen, .iov_len = l->rcap - l->rlen };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof(ctl) };
    ssize_t n;
    do
        n = recvmsg(l->fd, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    while (n < 0 && errno == EINTR);
    if (n < 0)
        return errno == EAGAIN ? 0 : -errno;
    if (n == 0)
        return -ECONNRESET;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)(void *)CMSG_DATA(c);
        for (int i = 0; i < cnt; i++) {
            if (l->nfds < MAX_FDS)
                l->fds[l->nfds++] = fds[i];
            else
                close(fds[i]);
        }
    }
    l->rlen += (size_t)n;
    return 1;
}

/* Takes the next complete message out of rbuf. 1 = *m filled, 0 = incomplete, <0 = protocol error. */
static int extract(struct lm_logind *l, struct msg *m)
{
    if (l->rlen < 16)
        return 0;
    const uint8_t *b = l->rbuf;
    if (b[0] != 'l' || b[3] != 1)
        return -EPROTO;
    uint32_t blen, flen;
    memcpy(&blen, b + 4, 4);
    memcpy(&flen, b + 12, 4);
    if (blen > MAX_MSG || flen > MAX_MSG)
        return -EPROTO;
    size_t body_off = (16 + (size_t)flen + 7) & ~(size_t)7, total = body_off + blen;
    if (l->rlen < total)
        return 0;
    memset(m, 0, sizeof(*m));
    m->type = b[1];
    memcpy(&m->serial, b + 8, 4);
    struct rd r = { .p = b, .len = 16 + (size_t)flen, .pos = 16 };
    while (r.pos < r.len) {
        rd_align(&r, 8);
        if (r.bad)
            return -EPROTO;
        if (r.pos >= r.len)
            break;
        uint8_t code = rd_u8(&r);
        const char *sg = rd_sigstr(&r);
        if (r.bad || strlen(sg) != 1)
            return -EPROTO;
        const char *s = "";
        uint32_t u = 0;
        switch (sg[0]) {
        case 'o': case 's': s = rd_string(&r); break;
        case 'g': s = rd_sigstr(&r); break;
        case 'u': u = rd_u32(&r); break;
        default: return -EPROTO;
        }
        if (r.bad)
            return -EPROTO;
        switch (code) {
        case 1: snprintf(m->path, sizeof(m->path), "%s", s); break;
        case 2: snprintf(m->iface, sizeof(m->iface), "%s", s); break;
        case 3: snprintf(m->member, sizeof(m->member), "%s", s); break;
        case 4: snprintf(m->errname, sizeof(m->errname), "%s", s); break;
        case 5: m->reply_serial = u; break;
        case 8: snprintf(m->sig, sizeof(m->sig), "%s", s); break;
        case 9: m->nfds_hdr = u; break;
        }
    }
    m->body = malloc(blen ? blen : 1);
    if (!m->body)
        return -ENOMEM;
    memcpy(m->body, b + body_off, blen);
    m->body_len = blen;
    for (m->nfds = 0; (uint32_t)m->nfds < m->nfds_hdr && m->nfds < MAX_FDS; m->nfds++) {
        if (l->nfds == 0) {
            msg_free(m);
            return -EPROTO;
        }
        m->fds[m->nfds] = l->fds[0];
        memmove(l->fds, l->fds + 1, sizeof(int) * (size_t)(--l->nfds));
    }
    memmove(l->rbuf, l->rbuf + total, l->rlen - total);
    l->rlen -= total;
    return 1;
}

static void queue_signal(struct lm_logind *l, struct msg *m)
{
    if (strcmp(m->iface, LOGIN_SESSION))
        return;
    bool pause = !strcmp(m->member, "PauseDevice") && !strcmp(m->sig, "uus");
    bool resume = !strcmp(m->member, "ResumeDevice") && !strcmp(m->sig, "uuh");
    if (!pause && !resume)
        return;
    struct rd r = { .p = m->body, .len = m->body_len };
    struct sigev e = { .resume = resume, .fd = -1 };
    e.major = rd_u32(&r);
    e.minor = rd_u32(&r);
    if (pause) {
        snprintf(e.type, sizeof(e.type), "%s", rd_string(&r));
    } else {
        uint32_t idx = rd_u32(&r);
        if (!r.bad && idx < (uint32_t)m->nfds) {
            e.fd = m->fds[idx];
            m->fds[idx] = -1;                       /* now ours */
        }
    }
    if (r.bad || l->nq >= MAX_QUEUE) {
        if (e.fd >= 0)
            close(e.fd);
        return;
    }
    l->q[l->nq++] = e;
    if (l->idle)
        lm_idle_schedule(l->idle);
}

/* A call: sends it and (unless FLAG_NO_REPLY) waits for the reply. Signals arriving meanwhile are queued. */
static int bus_call(struct lm_logind *l, const char *dest, const char *path, const char *iface, const char *member,
                    const char *sig, const struct mb *body, uint32_t flags, struct msg *reply)
{
    struct mb h = { 0 };
    uint32_t serial = ++l->serial;
    uint8_t fixed[16] = { 'l', MSG_CALL, (uint8_t)flags, 1 };
    uint32_t blen = body ? (uint32_t)body->len : 0;
    memcpy(fixed + 4, &blen, 4);
    memcpy(fixed + 8, &serial, 4);
    mb_put(&h, fixed, 16);
    mb_field(&h, 1, 'o', path);
    mb_field(&h, 2, 's', iface);
    mb_field(&h, 3, 's', member);
    mb_field(&h, 6, 's', dest);
    if (body && body->len)
        mb_field(&h, 8, 'g', sig);
    uint32_t flen = (uint32_t)(h.len - 16);
    if (!h.oom)
        memcpy(h.p + 12, &flen, 4);
    mb_align(&h, 8);
    if (body)
        mb_put(&h, body->p, body->len);
    if (h.oom) {
        free(h.p);
        return -ENOMEM;
    }
    int rc = send_all(l->fd, h.p, h.len);
    free(h.p);
    if (rc < 0)
        return rc;
    if (flags & FLAG_NO_REPLY)
        return 0;

    int64_t end = now_ms() + CALL_TIMEOUT_MS;
    for (;;) {
        struct msg m;
        int e;
        while ((e = extract(l, &m)) == 1) {
            if ((m.type == MSG_RETURN || m.type == MSG_ERROR) && m.reply_serial == serial) {
                if (m.type == MSG_ERROR) {
                    snprintf(l->errname, sizeof(l->errname), "%s", m.errname);
                    msg_free(&m);
                    return -EREMOTEIO;
                }
                *reply = m;
                return 0;
            }
            if (m.type == MSG_SIGNAL)
                queue_signal(l, &m);
            msg_free(&m);
        }
        if (e < 0)
            return e;
        int64_t left = end - now_ms();
        struct pollfd pf = { .fd = l->fd, .events = POLLIN };
        if (left <= 0 || poll(&pf, 1, (int)left) == 0)
            return -ETIMEDOUT;
        int f = fill(l);
        if (f < 0)
            return f;
    }
}

/* ---- the calls we need ---------------------------------------------------------------------------------------------- */

static int take_device(struct lm_logind *l, uint32_t major_, uint32_t minor_, bool *inactive)
{
    struct mb b = { 0 };
    mb_u32(&b, major_);
    mb_u32(&b, minor_);
    struct msg r;
    int rc = bus_call(l, LOGIN_DEST, l->session, LOGIN_SESSION, "TakeDevice", "uu", &b, 0, &r);
    free(b.p);
    if (rc < 0)
        return rc == -EREMOTEIO && strstr(l->errname, "NotSupported") ? -ENODEV : (rc == -EREMOTEIO ? -EPERM : rc);
    struct rd rr = { .p = r.body, .len = r.body_len };
    uint32_t idx = rd_u32(&rr), inact = rd_u32(&rr);
    int fd = -1;
    if (!rr.bad && idx < (uint32_t)r.nfds) {
        fd = r.fds[idx];
        r.fds[idx] = -1;
    }
    msg_free(&r);
    if (fd < 0)
        return -EPROTO;
    if (inactive)
        *inactive = inact != 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (l->ndevs < (int)(sizeof(l->devs) / sizeof(l->devs[0])))
        l->devs[l->ndevs++] = (typeof(l->devs[0])){ fd, major_, minor_ };
    return fd;
}

static void release_device(struct lm_logind *l, uint32_t major_, uint32_t minor_)
{
    struct mb b = { 0 };
    mb_u32(&b, major_);
    mb_u32(&b, minor_);
    bus_call(l, LOGIN_DEST, l->session, LOGIN_SESSION, "ReleaseDevice", "uu", &b, FLAG_NO_REPLY, NULL);
    free(b.p);
}

void lm_logind_pause_done(struct lm_logind *l, dev_t dev)
{
    struct mb b = { 0 };
    mb_u32(&b, major(dev));
    mb_u32(&b, minor(dev));
    bus_call(l, LOGIN_DEST, l->session, LOGIN_SESSION, "PauseDeviceComplete", "uu", &b, FLAG_NO_REPLY, NULL);
    free(b.p);
}

/* The Seat property of the session: a variant holding (so), the seat's id and object path. */
static void fetch_seat(struct lm_logind *l)
{
    struct mb b = { 0 };
    mb_string(&b, LOGIN_SESSION);
    mb_string(&b, "Seat");
    struct msg r;
    int rc = bus_call(l, LOGIN_DEST, l->session, "org.freedesktop.DBus.Properties", "Get", "ss", &b, 0, &r);
    free(b.p);
    if (rc < 0)
        return;
    struct rd rr = { .p = r.body, .len = r.body_len };
    const char *sg = rd_sigstr(&rr);
    if (!rr.bad && !strcmp(sg, "(so)")) {
        rd_align(&rr, 8);
        rd_string(&rr);                                 /* id */
        const char *path = rd_string(&rr);
        if (!rr.bad && strcmp(path, "/"))              /* a session without a seat reports "/" */
            snprintf(l->seat, sizeof(l->seat), "%s", path);
    }
    msg_free(&r);
}

int lm_logind_switch_vt(struct lm_logind *l, int vt)
{
    if (vt < 1 || vt > 12)
        return -1;
    if (!l->seat[0]) {
        lm_warn("logind: this session has no seat, cannot switch to VT %d", vt);
        return -1;
    }
    struct mb b = { 0 };
    mb_u32(&b, (uint32_t)vt);
    struct msg r;
    int rc = bus_call(l, LOGIN_DEST, l->seat, LOGIN_SEAT, "SwitchTo", "u", &b, 0, &r);
    free(b.p);
    if (rc < 0) {
        lm_warn("logind: SwitchTo(%d) failed: %s", vt, rc == -EREMOTEIO ? l->errname : strerror(-rc));
        return -1;
    }
    msg_free(&r);
    return 0;
}

void lm_logind_watch(struct lm_logind *l, dev_t dev, lm_logind_pause_fn pause, lm_logind_resume_fn resume, void *data)
{
    l->watching = true;
    l->wdev = dev;
    l->wpause = pause;
    l->wresume = resume;
    l->wdata = data;
}

static void dispatch(struct lm_logind *l, const struct sigev *e)
{
    dev_t dev = makedev(e->major, e->minor);
    if (l->watching && dev == l->wdev) {
        if (e->resume) {
            if (l->wresume)
                l->wresume(l->wdata, e->fd);
            else if (e->fd >= 0)
                close(e->fd);
        } else if (l->wpause) {
            l->wpause(l->wdata, e->type);
        }
        return;
    }
    if (e->resume) {
        if (e->fd >= 0)
            close(e->fd);
    } else if (!strcmp(e->type, "pause")) {
        lm_logind_pause_done(l, dev);
    }
}

static void flush(struct lm_logind *l)
{
    while (l->nq > 0) {                                 /* one at a time: a callback may make calls that queue more */
        struct sigev e = l->q[0];
        memmove(l->q, l->q + 1, sizeof(l->q[0]) * (size_t)(--l->nq));
        dispatch(l, &e);
    }
}

static void idle_cb(void *data)
{
    flush(data);
}

static void readable(int fd, uint32_t events, void *data)
{
    struct lm_logind *l = data;
    (void)fd;
    (void)events;
    int f;
    while ((f = fill(l)) > 0) {
        struct msg m;
        int e;
        while ((e = extract(l, &m)) == 1) {
            if (m.type == MSG_SIGNAL)
                queue_signal(l, &m);
            msg_free(&m);
        }
        if (e < 0) {
            f = e;
            break;
        }
    }
    if (f < 0) {
        lm_warn("logind: connection lost (%s); the devices stay open but VT switching is no longer managed", strerror(-f));
        lm_source_remove(l->src);
        l->src = NULL;
    }
    flush(l);
}

/* ---- open / close --------------------------------------------------------------------------------------------------- */

static int connect_bus(void)
{
    const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "/run/dbus/system_bus_socket";
    if (addr && *addr) {
        const char *p = strstr(addr, "unix:path=");
        if (!p)
            return -EPROTONOSUPPORT;
        p += strlen("unix:path=");
        size_t n = strcspn(p, ",;");
        if (n == 0 || n >= sizeof(path))
            return -EINVAL;
        memcpy(path, p, n);
        path[n] = 0;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

static int read_line(int fd, char *buf, size_t n)       /* up to and including '\n', byte by byte: nothing past it */
{
    size_t i = 0;
    int64_t end = now_ms() + CALL_TIMEOUT_MS;
    while (i + 1 < n) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int64_t left = end - now_ms();
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
            return -ETIMEDOUT;
        char c;
        ssize_t k = recv(fd, &c, 1, 0);
        if (k <= 0)
            return k < 0 ? -errno : -ECONNRESET;
        buf[i++] = c;
        if (c == '\n')
            break;
    }
    buf[i] = 0;
    return (int)i;
}

static int authenticate(int fd)
{
    char hex[64], line[256], cmd[160];
    int hn = snprintf(hex, sizeof(hex), "%u", (unsigned)geteuid());
    char enc[128] = "";
    for (int i = 0; i < hn; i++)
        snprintf(enc + 2 * i, sizeof(enc) - (size_t)(2 * i), "%02x", (unsigned char)hex[i]);
    int n = snprintf(cmd, sizeof(cmd), "%cAUTH EXTERNAL %s\r\n", 0, enc);
    int rc = send_all(fd, cmd, (size_t)n);
    if (rc < 0)
        return rc;
    if ((rc = read_line(fd, line, sizeof(line))) < 0)
        return rc;
    if (strncmp(line, "OK ", 3))
        return -EACCES;
    rc = send_all(fd, "NEGOTIATE_UNIX_FD\r\n", 19);
    if (rc < 0)
        return rc;
    if ((rc = read_line(fd, line, sizeof(line))) < 0)
        return rc;
    if (strncmp(line, "AGREE_UNIX_FD", 13))
        return -ENOTSUP;
    return send_all(fd, "BEGIN\r\n", 7);
}

struct lm_logind *lm_logind_open(struct lm_loop *loop, char *err, size_t errsz)
{
    struct lm_logind *l = calloc(1, sizeof(*l));
    const char *why = "out of memory";
    int rc = -ENOMEM;
    if (!l)
        goto fail;
    l->loop = loop;
    l->fd = connect_bus();
    if (l->fd < 0) {
        rc = l->fd;
        why = "cannot connect to the system bus";
        l->fd = -1;
        goto fail;
    }
    if ((rc = authenticate(l->fd)) < 0) {
        why = "D-Bus authentication failed";
        goto fail;
    }
    fcntl(l->fd, F_SETFL, fcntl(l->fd, F_GETFL) | O_NONBLOCK);

    struct msg r;
    if ((rc = bus_call(l, DBUS_DEST, "/org/freedesktop/DBus", DBUS_DEST, "Hello", "", NULL, 0, &r)) < 0) {
        why = "D-Bus Hello failed";
        goto fail;
    }
    msg_free(&r);

    struct mb b = { 0 };
    const char *sid = getenv("XDG_SESSION_ID");
    if (sid && *sid) {
        mb_string(&b, sid);
        rc = bus_call(l, LOGIN_DEST, LOGIN_PATH, LOGIN_MANAGER, "GetSession", "s", &b, 0, &r);
    } else {
        mb_u32(&b, (uint32_t)getpid());
        rc = bus_call(l, LOGIN_DEST, LOGIN_PATH, LOGIN_MANAGER, "GetSessionByPID", "u", &b, 0, &r);
    }
    free(b.p);
    if (rc < 0) {
        why = "this process is not in a logind session";
        goto fail;
    }
    struct rd rr = { .p = r.body, .len = r.body_len };
    snprintf(l->session, sizeof(l->session), "%s", rd_string(&rr));
    bool bad = rr.bad;
    msg_free(&r);
    if (bad || !l->session[0]) {
        rc = -EPROTO;
        why = "unexpected reply from logind";
        goto fail;
    }

    struct mb tb = { 0 };
    mb_u32(&tb, 0);                                     /* force = false */
    rc = bus_call(l, LOGIN_DEST, l->session, LOGIN_SESSION, "TakeControl", "b", &tb, 0, &r);
    free(tb.p);
    if (rc < 0) {
        why = "cannot take control of the session (another compositor already controls it?)";
        goto fail;
    }
    msg_free(&r);
    l->controlled = true;
    fetch_seat(l);

    l->idle = lm_loop_add_idle(loop, idle_cb, l);
    l->src = lm_loop_add_fd(loop, l->fd, EPOLLIN, readable, l);
    if (!l->src || !l->idle) {
        rc = -ENOMEM;
        why = "event loop registration failed";
        goto fail;
    }
    lm_info("logind: controlling session %s", l->session);
    return l;

fail:
    if (err && errsz)
        snprintf(err, errsz, "%s (%s)", why, rc == -EREMOTEIO ? l->errname : strerror(-rc));
    if (l)
        lm_logind_close(l);
    return NULL;
}

void lm_logind_close(struct lm_logind *l)
{
    if (!l)
        return;
    if (l->controlled)
        bus_call(l, LOGIN_DEST, l->session, LOGIN_SESSION, "ReleaseControl", "", NULL, FLAG_NO_REPLY, NULL);
    if (l->src)
        lm_source_remove(l->src);
    if (l->idle)
        lm_idle_destroy(l->idle);
    if (l->fd >= 0)
        close(l->fd);
    for (int i = 0; i < l->nfds; i++)
        close(l->fds[i]);
    for (int i = 0; i < l->nq; i++)
        if (l->q[i].fd >= 0)
            close(l->q[i].fd);
    free(l->rbuf);
    free(l);
}

/* ---- devices -------------------------------------------------------------------------------------------------------- */

int lm_dev_open(struct lm_logind *l, const char *path, int flags, bool *inactive)
{
    if (inactive)
        *inactive = false;
    if (!l) {
        int fd = open(path, flags | O_CLOEXEC);
        return fd < 0 ? -errno : fd;
    }
    struct stat st;
    if (stat(path, &st) < 0)
        return -errno;
    if (!S_ISCHR(st.st_mode))
        return -ENODEV;
    return take_device(l, major(st.st_rdev), minor(st.st_rdev), inactive);
}

void lm_dev_close(struct lm_logind *l, int fd)
{
    if (fd < 0)
        return;
    for (int i = 0; l && i < l->ndevs; i++) {
        if (l->devs[i].fd != fd)
            continue;
        release_device(l, l->devs[i].major, l->devs[i].minor);
        l->devs[i] = l->devs[--l->ndevs];
        break;
    }
    close(fd);
}

#include "dvrip.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "md5.h"

#define HDR 20
#define WAIT_MS 6000

enum { MSG_LOGIN = 1000, MSG_KEEPALIVE = 1006, MSG_MONITOR = 1410, MSG_CLAIM = 1413 };

void dv_hash(const char *password, char out[9])
{
    static const char set[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    uint8_t d[LM_MD5_LEN];
    lm_md5(password, strlen(password), d);
    for (int i = 0; i < 8; i++)
        out[i] = set[(d[2 * i] + d[2 * i + 1]) % 62];
    out[8] = 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static bool append(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
    if (!n)
        return true;
    if (*len + n > *cap) {
        size_t want = *cap ? *cap : 65536;
        while (want < *len + n)
            want *= 2;
        uint8_t *b = realloc(*buf, want);
        if (!b)
            return false;
        *buf = b;
        *cap = want;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

/* ---- frames ----------------------------------------------------------------------------------------------------------- */

/* The frame at p (n bytes there): its whole size, 0 = more bytes are needed, -1 = no frame starts here. */
static long frame_at(const uint8_t *p, size_t n, struct dv_frame *f)
{
    static const uint8_t start[3] = { 0, 0, 1 };
    if (memcmp(p, start, n < 3 ? n : 3))
        return -1;
    if (n < 4)
        return 0;
    size_t head, len;
    memset(f, 0, sizeof(*f));
    switch (p[3]) {
    case 0xfc: case 0xfe:                   /* a key frame: codec, rate, width / 8, height / 8, time, length */
        head = 16;
        if (n < head)
            return 0;
        f->kind = DV_VIDEO_KEY;
        f->codec = p[4] & 0x0f;
        f->w = (p[6] + ((p[4] >> 4) & 3) * 256) * 8;        /* (pictures wider than 2040 carry the high bits beside the codec) */
        f->h = (p[7] + ((p[4] >> 6) & 3) * 256) * 8;
        len = le32(p + 12);
        break;
    case 0xfd:
        head = 8;
        if (n < head)
            return 0;
        f->kind = DV_VIDEO;
        len = le32(p + 4);
        break;
    case 0xfa: case 0xf9:
        head = 8;
        if (n < head)
            return 0;
        f->kind = p[3] == 0xfa ? DV_AUDIO : DV_INFO;
        len = (size_t)p[6] | (size_t)p[7] << 8;
        break;
    default:
        return -1;
    }
    if (len > DV_MAX_BODY)
        return -1;
    if (n < head + len)
        return 0;
    f->data = p + head;
    f->len = len;
    return (long)(head + len);
}

bool dv_demux_feed(struct dv_demux *d, const void *data, size_t len, dv_frame_fn fn, void *ud)
{
    if (!append(&d->buf, &d->len, &d->cap, data, len))
        return false;
    size_t at = 0;
    while (at < d->len) {
        struct dv_frame f;
        long n = frame_at(d->buf + at, d->len - at, &f);
        if (n == 0)
            break;
        if (n < 0) {                        /* lost our place: look for the next start */
            at++;
            d->skipped++;
            continue;
        }
        fn(ud, &f);
        at += (size_t)n;
    }
    if (at)
        memmove(d->buf, d->buf + at, d->len - at);
    d->len -= at;
    return true;
}

void dv_demux_free(struct dv_demux *d)
{
    free(d->buf);
    memset(d, 0, sizeof(*d));
}

bool dv_packets_feed(struct dv_packets *p, const void *data, size_t len, dv_packet_fn fn, void *ud)
{
    if (!append(&p->buf, &p->len, &p->cap, data, len))
        return false;
    size_t at = 0;
    while (p->len - at >= HDR) {
        const uint8_t *h = p->buf + at;
        uint32_t n = le32(h + 16);
        if (h[0] != 0xff || n > DV_MAX_BODY)
            return false;
        if (p->len - at < HDR + (size_t)n)
            break;
        fn(ud, h[14] | h[15] << 8, h + HDR, n);
        at += HDR + (size_t)n;
    }
    if (at)
        memmove(p->buf, p->buf + at, p->len - at);
    p->len -= at;
    return true;
}

void dv_packets_free(struct dv_packets *p)
{
    free(p->buf);
    memset(p, 0, sizeof(*p));
}

/* ---- the connection --------------------------------------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits for `events` on fd until the deadline; false when it passes or the caller wakes us. */
static bool wait_fd(int fd, short events, int64_t deadline, int wake_fd)
{
    for (;;) {
        struct pollfd p[2] = { { .fd = fd, .events = events }, { .fd = wake_fd, .events = POLLIN } };
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return false;
        int n = poll(p, wake_fd >= 0 ? 2 : 1, (int)left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || (wake_fd >= 0 && p[1].revents))
            return false;
        return true;
    }
}

static int dial(const char *host, int port, int wake_fd, char *err, size_t errsz)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *list = NULL;
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    int g = getaddrinfo(host, service, &hints, &list);
    if (g) {
        snprintf(err, errsz, "%s: %s", host, gai_strerror(g));
        return -1;
    }
    int fd = -1, e = 0;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) {
            e = errno;
            continue;
        }
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS && wait_fd(fd, POLLOUT, now_ms() + WAIT_MS, wake_fd)) {
            socklen_t l = sizeof(e);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e == 0)
                break;
        } else {
            e = errno == EINPROGRESS ? ETIMEDOUT : errno;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0)
        snprintf(err, errsz, "cannot connect to %s: %s", host, strerror(e ? e : ETIMEDOUT));
    return fd;
}

static bool send_msg(int fd, uint32_t session, uint32_t seq, int msg, const char *body, int wake_fd)
{
    size_t n = strlen(body) + 2, total = HDR + n, done = 0;
    uint8_t *m = calloc(1, total);
    if (!m)
        return false;
    m[0] = 0xff;
    put32(m + 4, session);
    put32(m + 8, seq);
    m[14] = (uint8_t)msg;
    m[15] = (uint8_t)(msg >> 8);
    put32(m + 16, (uint32_t)n);
    memcpy(m + HDR, body, n - 2);
    m[total - 2] = '\n';
    int64_t deadline = now_ms() + WAIT_MS;
    while (done < total) {
        ssize_t k = send(fd, m + done, total - done, MSG_NOSIGNAL);
        if (k > 0)
            done += (size_t)k;
        else if (!(k < 0 && (errno == EAGAIN || errno == EINTR) && wait_fd(fd, POLLOUT, deadline, wake_fd)))
            break;
    }
    free(m);
    return done == total;
}

static bool read_all(int fd, uint8_t *buf, size_t n, int64_t deadline, int wake_fd)
{
    size_t done = 0;
    while (done < n) {
        if (!wait_fd(fd, POLLIN, deadline, wake_fd))
            return false;
        ssize_t k = recv(fd, buf + done, n - done, 0);
        if (k > 0)
            done += (size_t)k;
        else if (!(k < 0 && (errno == EAGAIN || errno == EINTR)))
            return false;
    }
    return true;
}

/* One answer's JSON (exactly one message is read, so a stream behind it stays in the socket). NULL when there is none. */
static struct json *read_reply(int fd, int wake_fd)
{
    int64_t deadline = now_ms() + WAIT_MS;
    uint8_t h[HDR];
    if (!read_all(fd, h, HDR, deadline, wake_fd))
        return NULL;
    uint32_t n = le32(h + 16);
    if (h[0] != 0xff || n > 1u << 20)
        return NULL;
    uint8_t *body = malloc((size_t)n + 1);
    if (!body)
        return NULL;
    struct json *j = NULL;
    if (read_all(fd, body, n, deadline, wake_fd)) {
        size_t len = n;
        while (len && (body[len - 1] == 0 || body[len - 1] == '\n'))
            len--;
        body[len] = 0;
        j = json_parse((const char *)body);
    }
    free(body);
    return j;
}

void dv_init(struct dv_session *s)
{
    memset(s, 0, sizeof(*s));
    s->ctl = s->data = -1;
}

static void quote(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

int dv_login(struct dv_session *s, const char *host, int port, const char *user, const char *password, int wake_fd, char *err, size_t errsz)
{
    dv_close(s);
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->port = port;
    s->ctl = dial(host, port, wake_fd, err, errsz);
    if (s->ctl < 0)
        return -1;
    char body[512], u[200], hash[9];
    quote(u, sizeof(u), user);
    dv_hash(password, hash);
    snprintf(body, sizeof(body), "{\"EncryptType\":\"MD5\",\"LoginType\":\"DVRIP-Web\",\"UserName\":\"%s\",\"PassWord\":\"%s\"}", u, hash);
    struct json *j = send_msg(s->ctl, 0, s->seq++, MSG_LOGIN, body, wake_fd) ? read_reply(s->ctl, wake_fd) : NULL;
    long long ret = j ? (long long)json_num(j, "Ret", -1) : -1;
    if (ret != 100) {
        if (ret > 0)
            snprintf(err, errsz, "%s (code %lld)", ret == 203 ? "wrong password" : ret == 204 ? "unknown user" : ret == 205 ? "unknown user, or the account is locked" :
                     ret == 206 ? "this computer is blocked by the camera" : "the camera refused the login", ret);
        else
            snprintf(err, errsz, "no answer to the login");
        json_free(j);
        dv_close(s);
        return ret > 0 && ret < 100000 ? (int)ret : -1;
    }
    snprintf(s->idtext, sizeof(s->idtext), "%s", json_str(j, "SessionID", "0x00000000"));
    s->id = (uint32_t)strtoul(s->idtext, NULL, 16);
    long long alive = (long long)json_num(j, "AliveInterval", 30);
    s->alive_s = alive < 4 ? 4 : alive > 300 ? 300 : (int)alive;
    json_free(j);
    /* the id goes back into JSON: only the characters of a hexadecimal number */
    if (strspn(s->idtext, "0123456789abcdefABCDEFx") != strlen(s->idtext))
        snprintf(s->idtext, sizeof(s->idtext), "0x%08x", s->id);
    return 0;
}

int dv_start(struct dv_session *s, bool sub, int wake_fd, char *err, size_t errsz)
{
    if (s->ctl < 0) {
        snprintf(err, errsz, "not logged in");
        return -1;
    }
    s->data = dial(s->host, s->port, wake_fd, err, errsz);
    if (s->data < 0)
        return -1;
    char body[512];
    const char *fmt = "{\"Name\":\"OPMonitor\",\"SessionID\":\"%s\",\"OPMonitor\":{\"Action\":\"%s\",\"Parameter\":"
                      "{\"Channel\":0,\"CombinMode\":\"NONE\",\"StreamType\":\"%s\",\"TransMode\":\"TCP\"}}}";
    const char *type = sub ? "Extra1" : "Main";
    snprintf(body, sizeof(body), fmt, s->idtext, "Claim", type);
    struct json *j = send_msg(s->data, s->id, 0, MSG_CLAIM, body, wake_fd) ? read_reply(s->data, wake_fd) : NULL;
    long long ret = j ? (long long)json_num(j, "Ret", -1) : -1;
    json_free(j);
    if (ret != 100) {
        snprintf(err, errsz, "the camera did not give the %s stream (code %lld)", sub ? "second" : "main", ret);
        return -1;
    }
    snprintf(body, sizeof(body), fmt, s->idtext, "Start", type);
    if (!send_msg(s->ctl, s->id, s->seq++, MSG_MONITOR, body, wake_fd)) {
        snprintf(err, errsz, "the connection was lost");
        return -1;
    }
    return 0;
}

int dv_keepalive(struct dv_session *s)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"Name\":\"KeepAlive\",\"SessionID\":\"%s\"}", s->idtext);
    return s->ctl >= 0 && send_msg(s->ctl, s->id, s->seq++, MSG_KEEPALIVE, body, -1) ? 0 : -1;
}

void dv_close(struct dv_session *s)
{
    if (s->data >= 0)
        close(s->data);
    if (s->ctl >= 0)
        close(s->ctl);                      /* (the camera ends the session with the connection) */
    s->ctl = s->data = -1;
    s->seq = 0;
}

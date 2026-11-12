#include "tuyalan.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aes.h"
#include "json.h"
#include "md5.h"
#include "sha256.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The key of the broadcasts: the MD5 of a text every Tuya program carries. */
static void udp_key(uint8_t key[16])
{
    lm_md5("yGAdlopoPVldABfn", 16, key);
}

bool tuyalan_decode(const uint8_t *msg, size_t n, char *json, size_t jn)
{
    uint8_t key[16], plain[1500];
    size_t len = 0;
    if (n < 28 || n > sizeof(plain) || jn < 2)
        return false;
    udp_key(key);
    uint32_t prefix = be32(msg);
    if (prefix == 0x000055aa) {
        uint32_t length = be32(msg + 12);
        if (length < 12 || 16 + (size_t)length > n)
            return false;
        const uint8_t *p = msg + 20;                            /* after the return code */
        len = length - 12;                                      /* without the return code, the crc and the suffix */
        if (len > 0 && p[0] == '{') {
            memcpy(plain, p, len);
        } else {
            struct lm_aes128 a;
            if (len == 0 || len % 16)
                return false;
            lm_aes128_init(&a, key);
            for (size_t i = 0; i < len; i += 16)
                lm_aes128_decrypt_block(&a, p + i, plain + i);
            uint8_t pad = plain[len - 1];
            if (pad < 1 || pad > 16)
                return false;
            len -= pad;
        }
    } else if (prefix == 0x00006699) {
        uint32_t length = be32(msg + 14);
        if (length < 28 || 18 + (size_t)length + 4 > n)
            return false;
        len = length - 28;                                      /* without the nonce and the tag */
        if (!lm_aes128_gcm_decrypt(key, msg + 18, msg + 4, 14, msg + 30, len, msg + 30 + len, plain))
            return false;
    } else {
        return false;
    }
    const uint8_t *s = plain;
    if (len > 4 && s[0] != '{' && s[4] == '{') {                /* a return code in front */
        s += 4;
        len -= 4;
    }
    while (len > 0 && s[len - 1] == 0)
        len--;
    if (len == 0 || s[0] != '{' || len >= jn)
        return false;
    memcpy(json, s, len);
    json[len] = 0;
    return true;
}

static int open_port(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int tuyalan_scan(int ms, struct tuyalan_found *out, int max, int stats[2], char *err, size_t errsz)
{
    int none[2];
    if (!stats)
        stats = none;
    stats[0] = stats[1] = 0;
    struct pollfd pf[2] = { { .fd = open_port(6666), .events = POLLIN }, { .fd = open_port(6667), .events = POLLIN } };
    int n = 0;
    if (pf[0].fd < 0 && pf[1].fd < 0) {
        snprintf(err, errsz, "The UDP ports 6666 / 6667 cannot be opened: %s", strerror(errno));
        return -1;
    }
    int64_t end = mono_ms() + ms;
    for (;;) {
        int64_t left = end - mono_ms();
        if (left <= 0 || poll(pf, 2, (int)left) < 0)
            break;
        for (int i = 0; i < 2; i++) {
            uint8_t msg[1500];
            char text[1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            if (pf[i].fd < 0 || !(pf[i].revents & POLLIN))
                continue;
            ssize_t r = recvfrom(pf[i].fd, msg, sizeof(msg), 0, (struct sockaddr *)&from, &fl);
            if (r <= 0)
                continue;
            stats[0]++;
            if (!tuyalan_decode(msg, (size_t)r, text, sizeof(text))) {
                stats[1]++;
                continue;
            }
            struct json *j = json_parse(text);
            const char *id = json_str(j, "gwId", ""), *ip = json_str(j, "ip", "");
            char addr[48];
            inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
            bool known = !*id || strlen(id) >= sizeof(out[0].id);
            for (int k = 0; k < n && !known; k++)
                known = !strcmp(out[k].id, id);
            if (!known && n < max) {
                struct tuyalan_found *f = &out[n++];
                memset(f, 0, sizeof(*f));
                snprintf(f->id, sizeof(f->id), "%s", id);
                snprintf(f->ip, sizeof(f->ip), "%.47s", *ip ? ip : addr);
                snprintf(f->version, sizeof(f->version), "%.7s", json_str(j, "version", "?"));
                snprintf(f->product_key, sizeof(f->product_key), "%.39s", json_str(j, "productKey", ""));
            }
            json_free(j);
        }
    }
    for (int i = 0; i < 2; i++)
        if (pf[i].fd >= 0)
            close(pf[i].fd);
    return n;
}

/* ---- commands ----------------------------------------------------------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* AES-128-ECB with PKCS#7 padding; returns the length (a multiple of 16, always longer than n). */
static size_t ecb_encrypt(const uint8_t key[16], const uint8_t *in, size_t n, uint8_t *out)
{
    struct lm_aes128 a;
    uint8_t blk[16];
    size_t total = (n / 16 + 1) * 16;
    lm_aes128_init(&a, key);
    for (size_t off = 0; off < total; off += 16) {
        size_t have = off < n ? (n - off < 16 ? n - off : 16) : 0;
        memcpy(blk, in + off, have);
        memset(blk + have, (int)(total - n), 16 - have);
        lm_aes128_encrypt_block(&a, blk, out + off);
    }
    return total;
}

/* The reverse, in place; the length without the padding, or -1. */
static long ecb_decrypt(const uint8_t key[16], uint8_t *buf, size_t n)
{
    struct lm_aes128 a;
    if (n == 0 || n % 16)
        return -1;
    lm_aes128_init(&a, key);
    for (size_t off = 0; off < n; off += 16)
        lm_aes128_decrypt_block(&a, buf + off, buf + off);
    uint8_t pad = buf[n - 1];
    return pad >= 1 && pad <= 16 ? (long)(n - pad) : -1;
}

#define FRAME_MAX 2048

/* 000055AA seq cmd length payload (crc32 | hmac-sha256) 0000AA55; the HMAC when hmac_key is given (3.4). */
static size_t build_frame(uint8_t *out, uint32_t seq, uint32_t cmd, const uint8_t *payload, size_t plen, const uint8_t *hmac_key)
{
    put32(out, 0x000055aa);
    put32(out + 4, seq);
    put32(out + 8, cmd);
    put32(out + 12, (uint32_t)(plen + (hmac_key ? 36 : 8)));
    memcpy(out + 16, payload, plen);
    size_t n = 16 + plen;
    if (hmac_key) {
        lm_hmac_sha256(hmac_key, 16, out, n, out + n);
        n += 32;
    } else {
        put32(out + n, crc32_of(out, n));
        n += 4;
    }
    put32(out + n, 0x0000aa55);
    return n + 4;
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            poll(&pf, 1, 500);
            continue;
        }
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

struct conn {
    int fd;
    uint8_t buf[FRAME_MAX];
    size_t len;
};

/* The next whole frame from the device: its command, and the part between the header and the checksum in *body / *blen (it stays valid until the next
 * call). 0 = nothing came within the time, -1 = the connection ended or the bytes are no frame. */
static int read_frame(struct conn *c, int64_t deadline, uint32_t *cmd, uint8_t **body, size_t *blen, bool hmac)
{
    static _Thread_local uint8_t frame[FRAME_MAX];
    for (;;) {
        if (c->len >= 16) {
            uint32_t length = be32(c->buf + 12);
            size_t tail = hmac ? 36 : 8;
            if (be32(c->buf) != 0x000055aa || length < tail || 16 + (size_t)length > FRAME_MAX)
                return -1;
            if (c->len >= 16 + (size_t)length) {
                size_t total = 16 + (size_t)length;
                memcpy(frame, c->buf, total);
                memmove(c->buf, c->buf + total, c->len - total);
                c->len -= total;
                *cmd = be32(frame + 8);
                *body = frame + 16;
                *blen = length - tail;
                return 1;
            }
        }
        int64_t left = deadline - mono_ms();
        struct pollfd pf = { .fd = c->fd, .events = POLLIN };
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
            return 0;
        ssize_t r = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (r <= 0)
            return -1;
        c->len += (size_t)r;
    }
}

static int connect_to(const char *ip, int timeout_ms, char *err, size_t errsz)
{
    const char *pe = getenv("MELLAGUI_TUYA_LAN_PORT");
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)(pe && atoi(pe) > 0 ? atoi(pe) : 6668)) };
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        snprintf(err, errsz, "'%s' is no address", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        struct pollfd pf = { .fd = fd, .events = POLLOUT };
        int e = errno;
        socklen_t el = sizeof(e);
        if (e == EINPROGRESS && poll(&pf, 1, timeout_ms) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0)
            return fd;
        snprintf(err, errsz, "The device at %s does not answer in this network%s%s", ip, e && e != EINPROGRESS ? ": " : "", e && e != EINPROGRESS ? strerror(e) : "");
        close(fd);
        return -1;
    }
    return fd;
}

static bool version_ok(const char *version, const char *local_key, bool *v34, char *err, size_t errsz)
{
    *v34 = !strcmp(version, "3.4");
    if ((!*v34 && strcmp(version, "3.3")) || strlen(local_key) != 16) {
        snprintf(err, errsz, "Protocol %s is not spoken here", version);
        return false;
    }
    return true;
}

/* Protocol 3.4: agrees on a session key - our random number, the device's, both proved with the local key. */
static bool start_session(struct conn *c, const uint8_t key[16], uint8_t session[16], uint32_t *seq, char *err, size_t errsz)
{
    uint8_t mine[16], theirs[16], mac[32], enc[64], out[160], *body;
    size_t blen;
    uint32_t cmd;
    if (getentropy(mine, 16) != 0)
        for (int i = 0; i < 16; i++)
            mine[i] = (uint8_t)(rand() ^ (mono_ms() >> i));
    size_t n = build_frame(out, (*seq)++, 3, enc, ecb_encrypt(key, mine, 16, enc), key);
    if (!send_all(c->fd, out, n) || read_frame(c, mono_ms() + 2500, &cmd, &body, &blen, true) != 1 || cmd != 4) {
        snprintf(err, errsz, "The device does not start a session (is another program connected to it?)");
        return false;
    }
    if (blen % 16 == 4) {                                       /* (a return code in front) */
        body += 4;
        blen -= 4;
    }
    long pl = ecb_decrypt(key, body, blen);
    lm_hmac_sha256(key, 16, mine, 16, mac);
    if (pl < 48 || !lm_ct_equal(mac, body + 16, 32)) {
        snprintf(err, errsz, "The device does not accept the local key (was it paired again? log out and in to fetch the new one)");
        return false;
    }
    memcpy(theirs, body, 16);
    lm_hmac_sha256(key, 16, theirs, 16, mac);
    n = build_frame(out, (*seq)++, 5, enc, ecb_encrypt(key, mac, 32, enc), key);
    if (!send_all(c->fd, out, n)) {
        snprintf(err, errsz, "The connection to the device broke");
        return false;
    }
    struct lm_aes128 a;
    uint8_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = mine[i] ^ theirs[i];
    lm_aes128_init(&a, key);
    lm_aes128_encrypt_block(&a, x, session);
    return true;
}

int tuyalan_set(const char *ip, const char *version, const char *dev_id, const char *local_key, const char *dps, char *err, size_t errsz)
{
    bool v34;
    uint8_t key[16], session[16] = { 0 }, plain[FRAME_MAX], enc[FRAME_MAX], out[FRAME_MAX], *body;
    size_t blen;
    uint32_t cmd, seq = 1;
    if (!version_ok(version, local_key, &v34, err, errsz))
        return 0;
    if (strlen(dps) + strlen(dev_id) * 2 + 120 > 1200) {
        snprintf(err, errsz, "The value is too long");
        return 0;
    }
    memcpy(key, local_key, 16);
    struct conn c = { .fd = connect_to(ip, 2000, err, errsz) };
    if (c.fd < 0)
        return 0;
    int ret = 0;
    if (v34 && !start_session(&c, key, session, &seq, err, errsz))
        goto done;
    /* the command */
    size_t n;
    uint32_t sent_cmd;
    if (v34) {
        memcpy(plain, "3.4\0\0\0\0\0\0\0\0\0\0\0\0", 15);
        int jl = snprintf((char *)plain + 15, sizeof(plain) - 15, "{\"protocol\":5,\"t\":%lld,\"data\":{\"dps\":%s}}", (long long)time(NULL), dps);
        sent_cmd = 13;
        n = build_frame(out, seq++, sent_cmd, enc, ecb_encrypt(session, plain, 15 + (size_t)jl, enc), session);
    } else {
        int jl = snprintf((char *)plain, sizeof(plain), "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}", dev_id, dev_id, (long long)time(NULL), dps);
        memcpy(enc, "3.3\0\0\0\0\0\0\0\0\0\0\0\0", 15);
        sent_cmd = 7;
        n = build_frame(out, seq++, sent_cmd, enc, 15 + ecb_encrypt(key, plain, (size_t)jl, enc + 15), NULL);
    }
    if (!send_all(c.fd, out, n)) {
        snprintf(err, errsz, "The connection to the device broke");
        goto done;
    }
    ret = -1;
    snprintf(err, errsz, "The device did not confirm the command");
    int64_t deadline = mono_ms() + 2500;
    while (read_frame(&c, deadline, &cmd, &body, &blen, v34) == 1) {
        uint32_t rc = 0;
        if (v34 ? blen % 16 == 4 : blen >= 4) {
            rc = be32(body);
            body += 4;
            blen -= 4;
        }
        if (cmd == 8) {                                         /* it reports new values: the command arrived */
            ret = 1;
            break;
        }
        if (cmd != sent_cmd)
            continue;
        if (rc == 0) {
            ret = 1;
        } else {
            char text[80] = "";
            if (!v34 && blen > 0 && blen < sizeof(text)) {      /* (3.3 says why in plain text) */
                memcpy(text, body, blen);
                text[blen] = 0;
            }
            snprintf(err, errsz, "The device refused the command%s%s", text[0] ? ": " : "", text);
        }
        break;
    }
done:
    close(c.fd);
    lm_secure_zero(key, sizeof(key));
    lm_secure_zero(session, sizeof(session));
    if (ret == 1)
        err[0] = 0;
    return ret;
}

/* What a frame of the device says, as text in out: the JSON it carries (decrypted), or the plain words of a refusal. *rc = its return code. */
static void frame_text(bool v34, const uint8_t key[16], uint8_t *body, size_t blen, uint32_t *rc, char *out, size_t outsz)
{
    *rc = 0;
    out[0] = 0;
    if (blen >= 4 && (be32(body) & 0xffffff00u) == 0) {         /* (a small number in front is the return code) */
        *rc = be32(body);
        body += 4;
        blen -= 4;
    }
    if (!v34 && blen >= 15 && !memcmp(body, "3.3", 3)) {         /* (a report of new values has the version in clear text) */
        body += 15;
        blen -= 15;
    }
    if (blen == 0 || blen >= outsz)
        return;
    memcpy(out, body, blen);                                    /* a refusal of 3.3 is plain text */
    out[blen] = 0;
    long pl = ecb_decrypt(key, body, blen);
    if (pl <= 0)
        return;
    if (pl >= 15 && !memcmp(body, v34 ? "3.4" : "3.3", 3)) {
        body += 15;
        pl -= 15;
    }
    if (pl > 0 && body[0] == '{') {
        memcpy(out, body, (size_t)pl);
        out[pl] = 0;
    }
}

bool tuyalan_get(const char *ip, const char *version, const char *dev_id, const char *local_key, const char *ask, char *json, size_t jn, char *err,
                 size_t errsz)
{
    bool v34, ok = false, asked_again = false;
    uint8_t key[16], session[16] = { 0 }, plain[FRAME_MAX], enc[FRAME_MAX], out[FRAME_MAX], *body;
    size_t blen;
    uint32_t cmd, seq = 1;
    if (!version_ok(version, local_key, &v34, err, errsz))
        return false;
    if (strlen(dev_id) * 3 + (ask ? strlen(ask) : 0) + 120 > 1200) {
        snprintf(err, errsz, "The question is too long");
        return false;
    }
    memcpy(key, local_key, 16);
    struct conn c = { .fd = connect_to(ip, 1500, err, errsz) };
    if (c.fd < 0)
        return false;
    if (v34 && !start_session(&c, key, session, &seq, err, errsz))
        goto done;
    /* the question has no version header in either protocol */
    int jl = snprintf((char *)plain, sizeof(plain), "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\"}", dev_id, dev_id, dev_id, (long long)time(NULL));
    uint32_t sent_cmd = v34 ? 16 : 10;
    size_t n = build_frame(out, seq++, sent_cmd, enc, ecb_encrypt(v34 ? session : key, plain, (size_t)jl, enc), v34 ? session : NULL);
    if (!send_all(c.fd, out, n)) {
        snprintf(err, errsz, "The connection to the device broke");
        goto done;
    }
    snprintf(err, errsz, "The device did not say its values");
    int64_t deadline = mono_ms() + 2500;
    while (read_frame(&c, deadline, &cmd, &body, &blen, v34) == 1) {
        uint32_t rc;
        static _Thread_local char text[FRAME_MAX];
        frame_text(v34, v34 ? session : key, body, blen, &rc, text, sizeof(text));
        if (text[0] == '{' && strstr(text, "\"dps\"")) {
            if (strlen(text) < jn) {
                snprintf(json, jn, "%s", text);
                ok = true;
            } else {
                snprintf(err, errsz, "The device's values do not fit");
            }
            break;
        }
        /* Some 3.3 devices (ids of 22 characters) do not know the question: they are told "set these values to nothing" and report them. */
        if (!v34 && !asked_again && ask && *ask && strstr(text, "data unvalid")) {
            asked_again = true;
            jl = snprintf((char *)plain, sizeof(plain), "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}", dev_id, dev_id, (long long)time(NULL), ask);
            memcpy(enc, "3.3\0\0\0\0\0\0\0\0\0\0\0\0", 15);
            sent_cmd = 13;
            n = build_frame(out, seq++, sent_cmd, enc, 15 + ecb_encrypt(key, plain, (size_t)jl, enc + 15), NULL);
            if (!send_all(c.fd, out, n))
                break;
            deadline = mono_ms() + 2500;
            continue;
        }
        if (cmd == sent_cmd && rc != 0) {
            snprintf(err, errsz, "The device does not say its values%s%.80s", text[0] && text[0] != '{' ? ": " : "", text[0] != '{' ? text : "");
            if (!(sent_cmd == 13))                              /* (after the second question a report may still follow the refusal) */
                break;
        }
    }
done:
    close(c.fd);
    lm_secure_zero(key, sizeof(key));
    lm_secure_zero(session, sizeof(session));
    if (ok)
        err[0] = 0;
    return ok;
}

int tuyalan_selftest(void)
{
    int bad = 0;
    static const char text[] = "{\"ip\":\"192.168.1.23\",\"gwId\":\"bf00test\",\"version\":\"3.3\"}";
    uint8_t key[16], msg[400], padded[128];
    char out[300];
    size_t tl = strlen(text), pl = (tl / 16 + 1) * 16;
    udp_key(key);
    /* the old frame, encrypted */
    struct lm_aes128 a;
    lm_aes128_init(&a, key);
    memcpy(padded, text, tl);
    memset(padded + tl, (int)(pl - tl), pl - tl);
    memset(msg, 0, sizeof(msg));
    msg[2] = 0x55; msg[3] = 0xaa;
    msg[11] = 0x13;
    msg[15] = (uint8_t)(pl + 12);
    for (size_t i = 0; i < pl; i += 16)
        lm_aes128_encrypt_block(&a, padded + i, msg + 20 + i);
    msg[20 + pl + 6] = 0xaa; msg[20 + pl + 7] = 0x55;
    if (!tuyalan_decode(msg, 28 + pl, out, sizeof(out)) || strcmp(out, text)) {
        fprintf(stderr, "tuyalan selftest: the encrypted 55AA broadcast\n");
        bad++;
    }
    /* the old frame, plain */
    memcpy(msg + 20, text, tl);
    msg[15] = (uint8_t)(tl + 12);
    if (!tuyalan_decode(msg, 28 + tl, out, sizeof(out)) || strcmp(out, text)) {
        fprintf(stderr, "tuyalan selftest: the plain 55AA broadcast\n");
        bad++;
    }
    /* the new frame: a return code in front of the JSON */
    uint8_t body[128] = { 0 };
    memcpy(body + 4, text, tl);
    memset(msg, 0, sizeof(msg));
    msg[2] = 0x66; msg[3] = 0x99;
    msg[13] = 0x13;
    msg[17] = (uint8_t)(12 + tl + 4 + 16);
    memcpy(msg + 18, "0123456789ab", 12);
    lm_aes128_gcm_encrypt(key, msg + 18, msg + 4, 14, body, tl + 4, msg + 30, msg + 30 + tl + 4);
    size_t total = 30 + tl + 4 + 16 + 4;
    msg[total - 2] = 0x99; msg[total - 1] = 0x66;
    if (!tuyalan_decode(msg, total, out, sizeof(out)) || strcmp(out, text)) {
        fprintf(stderr, "tuyalan selftest: the 6699 broadcast\n");
        bad++;
    }
    msg[40] ^= 1;
    if (tuyalan_decode(msg, total, out, sizeof(out)) || tuyalan_decode(msg, 10, out, sizeof(out))) {
        fprintf(stderr, "tuyalan selftest: a damaged broadcast is accepted\n");
        bad++;
    }
    if (crc32_of((const uint8_t *)"123456789", 9) != 0xcbf43926u) {
        fprintf(stderr, "tuyalan selftest: crc32\n");
        bad++;
    }
    uint8_t enc[64], frame[128];
    size_t el = ecb_encrypt(key, (const uint8_t *)"0123456789abcdef", 16, enc);
    if (el != 32 || ecb_decrypt(key, enc, el) != 16 || memcmp(enc, "0123456789abcdef", 16)) {
        fprintf(stderr, "tuyalan selftest: ecb with padding\n");
        bad++;
    }
    size_t fl = build_frame(frame, 7, 13, (const uint8_t *)"abcd", 4, NULL);
    if (fl != 28 || be32(frame + 12) != 12 || be32(frame + 20) != crc32_of(frame, 20) || be32(frame + 24) != 0x0000aa55) {
        fprintf(stderr, "tuyalan selftest: the frame\n");
        bad++;
    }
    if (build_frame(frame, 7, 13, (const uint8_t *)"abcd", 4, key) != 56) {
        fprintf(stderr, "tuyalan selftest: the frame with an HMAC\n");
        bad++;
    }
    return bad;
}

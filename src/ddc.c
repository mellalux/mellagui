#include "ddc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DDC_ADDR 0x37
#define HOST_SRC 0x51           /* "host" source address on the wire */
#define DEST_CK  0x6e           /* the monitor's write address (0x37 << 1): start value of request checksums */
#define REPLY_CK 0x50           /* start value of reply checksums */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static uint8_t checksum(uint8_t start, const uint8_t *p, size_t n)
{
    uint8_t ck = start;
    for (size_t i = 0; i < n; i++)
        ck ^= p[i];
    return ck;
}

int lm_ddc_build_get(uint8_t code, uint8_t out[5])
{
    out[0] = HOST_SRC;
    out[1] = 0x80 | 2;                  /* length of what follows */
    out[2] = 0x01;                      /* get VCP feature */
    out[3] = code;
    out[4] = checksum(DEST_CK, out, 4);
    return 5;
}

int lm_ddc_build_set(uint8_t code, int value, uint8_t out[7])
{
    out[0] = HOST_SRC;
    out[1] = 0x80 | 4;
    out[2] = 0x03;                      /* set VCP feature */
    out[3] = code;
    out[4] = (uint8_t)((value >> 8) & 0xff);
    out[5] = (uint8_t)(value & 0xff);
    out[6] = checksum(DEST_CK, out, 6);
    return 7;
}

enum lm_ddc_result lm_ddc_parse_get(const uint8_t *r, size_t len, uint8_t code, struct lm_ddc_vcp *out)
{
    if (!r || len < 11 || r[0] != DEST_CK || r[1] != (0x80 | 8) || r[2] != 0x02)
        return LM_DDC_BAD;
    if (checksum(REPLY_CK, r, 10) != r[10])
        return LM_DDC_BAD;
    if (r[3] == 0x01)                   /* "unsupported VCP code" */
        return r[4] == code ? LM_DDC_UNSUPPORTED : LM_DDC_BAD;
    if (r[3] != 0x00 || r[4] != code)
        return LM_DDC_BAD;
    out->code = code;
    out->max = (r[6] << 8) | r[7];
    out->cur = (r[8] << 8) | r[9];
    return LM_DDC_OK;
}

int lm_ddc_build_caps(int offset, uint8_t out[6])
{
    out[0] = HOST_SRC;
    out[1] = 0x80 | 3;
    out[2] = 0xf3;                      /* capabilities request */
    out[3] = (uint8_t)((offset >> 8) & 0xff);
    out[4] = (uint8_t)(offset & 0xff);
    out[5] = checksum(DEST_CK, out, 5);
    return 6;
}

int lm_ddc_parse_caps(const uint8_t *r, size_t len, int offset, char *data, int max)
{
    if (!r || len < 6 || r[0] != DEST_CK || !(r[1] & 0x80) || r[2] != 0xe3)
        return -1;
    int n = (r[1] & 0x7f) - 3;                              /* what follows the opcode: offset (2) + data */
    if (n < 0 || (size_t)(n + 6) > len || n > max)
        return -1;
    if (checksum(REPLY_CK, r, (size_t)n + 5) != r[n + 5])
        return -1;
    if (((r[3] << 8) | r[4]) != offset)
        return -1;
    memcpy(data, r + 5, (size_t)n);
    return n;
}

/* Finds the vcp( ... ) group of the string; returns the position after its "(" or NULL. */
static const char *vcp_group(const char *caps)
{
    for (const char *p = caps; (p = strstr(p, "vcp(")); p += 4)
        if (p == caps || p[-1] == '(' || p[-1] == ' ' || p[-1] == ')')
            return p + 4;
    return NULL;
}

/* Walks the features of the vcp group: calls fn(code, values-text or NULL) until it returns true. */
static bool vcp_walk(const char *caps, bool (*fn)(int code, const char *vals, size_t vlen, void *arg), void *arg)
{
    const char *p = vcp_group(caps);
    if (!p)
        return false;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == ' ') {
            p++;
        } else if (*p == ')') {
            depth--;
            p++;
        } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
            char *end;
            int code = (int)strtol(p, &end, 16);
            p = end;
            const char *vals = NULL;
            size_t vlen = 0;
            if (*p == '(') {                                /* the values this feature accepts */
                int d = 1;
                vals = ++p;
                while (*p && d > 0) {
                    d += *p == '(' ? 1 : *p == ')' ? -1 : 0;
                    p++;
                }
                vlen = (size_t)(p - vals) - 1;
            }
            if (fn(code, vals, vlen, arg))
                return true;
        } else {
            p++;
        }
    }
    return false;
}

struct caps_find {
    int code;
    int *values;
    int max, n;
};

static bool caps_find_cb(int code, const char *vals, size_t vlen, void *arg)
{
    struct caps_find *f = arg;
    if (code != f->code)
        return false;
    const char *p = vals, *end = vals ? vals + vlen : NULL;
    while (p && p < end && f->n < f->max) {
        if (*p == ' ' || *p == '(' || *p == ')') {          /* (sub-values of a value are not ours) */
            p++;
            continue;
        }
        char *e;
        long v = strtol(p, &e, 16);
        if (e == p)
            break;
        if (f->values)
            f->values[f->n] = (int)v;
        f->n++;
        p = e;
    }
    return true;
}

bool lm_ddc_caps_has(const char *caps, uint8_t code)
{
    struct caps_find f = { code, NULL, 0, 0 };
    return caps && vcp_walk(caps, caps_find_cb, &f);
}

int lm_ddc_caps_values(const char *caps, uint8_t code, int *values, int max)
{
    struct caps_find f = { code, values, max, 0 };
    if (caps)
        vcp_walk(caps, caps_find_cb, &f);
    return f.n;
}

int lm_ddc_open(int bus)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ioctl(fd, I2C_SLAVE, DDC_ADDR) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void lm_ddc_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

bool lm_ddc_get(int fd, uint8_t code, struct lm_ddc_vcp *out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t req[5], rep[16];
        int n = lm_ddc_build_get(code, req);
        if (write(fd, req, (size_t)n) == n) {
            sleep_ms(50);                                   /* the monitor needs a moment to prepare its answer */
            ssize_t got = read(fd, rep, sizeof(rep));
            if (got > 0) {
                enum lm_ddc_result r = lm_ddc_parse_get(rep, (size_t)got, code, out);
                if (r == LM_DDC_OK)
                    return true;
                if (r == LM_DDC_UNSUPPORTED)
                    return false;
            }
        }
        sleep_ms(60);
    }
    return false;
}

bool lm_ddc_capabilities(int fd, char *out, size_t n)
{
    size_t have = 0;
    for (int chunk = 0; chunk < 64; chunk++) {              /* 32 bytes a time; 64 chunks are far more than any monitor has */
        uint8_t req[6], rep[48];
        char data[40];
        int got = -1;
        for (int attempt = 0; attempt < 3 && got < 0; attempt++) {
            int len = lm_ddc_build_caps((int)have, req);
            if (write(fd, req, (size_t)len) == len) {
                sleep_ms(50);
                ssize_t r = read(fd, rep, sizeof(rep));
                if (r > 0)
                    got = lm_ddc_parse_caps(rep, (size_t)r, (int)have, data, (int)sizeof(data));
            }
            if (got < 0)
                sleep_ms(60);
        }
        if (got < 0)
            return false;
        if (got == 0)
            break;                                          /* the empty chunk ends the string */
        if (have + (size_t)got + 1 > n)
            return false;
        memcpy(out + have, data, (size_t)got);
        have += (size_t)got;
        sleep_ms(30);
    }
    out[have] = 0;
    return have > 0;
}

bool lm_ddc_set(int fd, uint8_t code, int value)
{
    uint8_t req[7];
    int n = lm_ddc_build_set(code, value, req);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (write(fd, req, (size_t)n) == n) {
            sleep_ms(50);                                   /* no further command before the monitor has applied it */
            return true;
        }
        sleep_ms(60);
    }
    return false;
}

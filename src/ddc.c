#include "ddc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
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

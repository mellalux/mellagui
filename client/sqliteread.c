#include "sqliteread.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_BUDGET 20000                                               /* pages one scan may read */
#define MAX_DEPTH 24
#define MAX_PAYLOAD (4u << 20)                                          /* a row bigger than this is cut */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A varint: its length, 0 when it runs past `end`. */
static int varint(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < 9; i++) {
        if (p + i >= end)
            return 0;
        if (i == 8) {
            *out = v << 8 | p[i];
            return 9;
        }
        v = v << 7 | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) {
            *out = v;
            return i + 1;
        }
    }
    return 0;
}

int sq_open(struct sq *q, const char *path)
{
    memset(q, 0, sizeof(*q));
    q->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (q->fd < 0)
        return -1;
    uint8_t h[100];
    if (pread(q->fd, h, sizeof(h), 0) != (ssize_t)sizeof(h) || memcmp(h, "SQLite format 3\0", 16)) {
        close(q->fd);
        return -1;
    }
    uint32_t ps = be16(h + 16);
    if (ps == 1) ps = 65536;
    if (ps < 512 || (ps & (ps - 1))) {
        close(q->fd);
        return -1;
    }
    q->psize = ps;
    q->usable = ps - h[20];
    if (q->usable < 480) {
        close(q->fd);
        return -1;
    }
    off_t sz = lseek(q->fd, 0, SEEK_END);
    q->npages = sz > 0 ? (uint32_t)(sz / ps) : 0;
    if (be32(h + 56) != 1 && be32(h + 56) != 0) {                       /* 2 / 3: UTF-16 */
        close(q->fd);
        return -2;
    }
    return 0;
}

void sq_close(struct sq *q)
{
    if (q->fd >= 0)
        close(q->fd);
    q->fd = -1;
}

struct scan {
    struct sq *q;
    sq_row_cb cb;
    void *user;
    int rows, max, budget;
    bool stop;
};

static bool read_page(struct scan *s, uint32_t no, uint8_t *buf)
{
    if (no < 1 || no > s->q->npages || --s->budget < 0)
        return false;
    return pread(s->q->fd, buf, s->q->psize, (off_t)(no - 1) * s->q->psize) == (ssize_t)s->q->psize;
}

/* A leaf cell: the payload (local part plus the overflow pages), handed to the callback. */
static void leaf_cell(struct scan *s, const uint8_t *page, uint32_t off)
{
    const uint8_t *end = page + s->q->psize, *c = page + off;
    uint64_t plen, rowid;
    int a = varint(c, end, &plen);
    if (!a) return;
    int b = varint(c + a, end, &rowid);
    if (!b) return;
    c += a + b;
    uint32_t U = s->q->usable, X = U - 35, M = ((U - 12) * 32 / 255) - 23;
    uint64_t P = plen > MAX_PAYLOAD ? MAX_PAYLOAD : plen, local;
    if (plen <= X) {
        local = plen;
    } else {
        uint64_t K = M + (plen - M) % (U - 4);
        local = K <= X ? K : M;
    }
    if (c + local > end)
        return;
    uint8_t *rec = malloc(P ? P : 1);
    if (!rec) return;
    uint64_t got = local < P ? local : P;
    memcpy(rec, c, got);
    if (plen > local && c + local + 4 <= end) {                         /* the rest is in a chain of overflow pages */
        uint32_t next = be32(c + local);
        uint8_t *op = malloc(s->q->psize);
        while (op && next && got < P) {
            if (!read_page(s, next, op)) break;
            uint64_t take = U - 4 < P - got ? U - 4 : P - got;
            memcpy(rec + got, op + 4, take);
            got += take;
            next = be32(op);
        }
        free(op);
    }
    if (got == P || plen <= local) {
        if (!s->cb(s->user, (int64_t)rowid, rec, (size_t)got))
            s->stop = true;
        s->rows++;
    }
    free(rec);
}

static void walk(struct scan *s, uint32_t no, int depth)
{
    if (depth > MAX_DEPTH || s->stop || s->rows >= s->max)
        return;
    uint8_t *page = malloc(s->q->psize);
    if (!page) return;
    if (read_page(s, no, page)) {
        uint32_t h = no == 1 ? 100 : 0;
        int type = page[h];
        uint32_t n = be16(page + h + 3), ptrs = h + (type == 5 ? 12 : 8);
        if ((type == 5 || type == 13) && ptrs + 2 * n <= s->q->psize) {
            for (uint32_t i = 0; i < n && !s->stop && s->rows < s->max; i++) {
                uint32_t off = be16(page + ptrs + 2 * i);
                if (off < ptrs || off + 4 > s->q->psize) continue;
                if (type == 5)
                    walk(s, be32(page + off), depth + 1);
                else
                    leaf_cell(s, page, off);
            }
            if (type == 5 && !s->stop && s->rows < s->max)
                walk(s, be32(page + h + 8), depth + 1);
        }
    }
    free(page);
}

int sq_scan(struct sq *q, uint32_t root, int max_rows, sq_row_cb cb, void *user)
{
    struct scan s = { q, cb, user, 0, max_rows, PAGE_BUDGET, false };
    walk(&s, root, 0);
    return s.rows;
}

int sq_record(const uint8_t *rec, size_t len, struct sq_val *v, int max)
{
    const uint8_t *end = rec + len;
    uint64_t hl;
    int a = varint(rec, end, &hl);
    if (!a || hl > len || hl < (uint64_t)a)
        return -1;
    const uint8_t *t = rec + a, *he = rec + hl, *d = he;
    int n = 0;
    while (t < he) {
        uint64_t st;
        int b = varint(t, he, &st);
        if (!b) return -1;
        t += b;
        struct sq_val x = { SQ_NULL, 0, 0, NULL, 0 };
        size_t sz = 0;
        switch (st) {
        case 0: break;
        case 1: sz = 1; break;
        case 2: sz = 2; break;
        case 3: sz = 3; break;
        case 4: sz = 4; break;
        case 5: sz = 6; break;
        case 6: sz = 8; break;
        case 7: sz = 8; break;
        case 8: x.type = SQ_INT; x.i = 0; break;
        case 9: x.type = SQ_INT; x.i = 1; break;
        case 10: case 11: return -1;
        default: sz = (size_t)(st - (st & 1 ? 13 : 12)) / 2; break;
        }
        if (d + sz > end)
            return -1;
        if (st >= 1 && st <= 6) {
            int64_t i = (int8_t)d[0];                                   /* big endian, sign extended */
            for (size_t k = 1; k < sz; k++)
                i = (int64_t)((uint64_t)i << 8 | d[k]);
            x.type = SQ_INT;
            x.i = i;
        } else if (st == 7) {
            uint64_t u = 0;
            for (int k = 0; k < 8; k++) u = u << 8 | d[k];
            memcpy(&x.f, &u, 8);
            x.type = SQ_FLOAT;
        } else if (st >= 12) {
            x.type = st & 1 ? SQ_TEXT : SQ_BLOB;
            x.p = d;
            x.n = sz;
        }
        d += sz;
        if (n < max)
            v[n] = x;
        n++;
    }
    return n;
}

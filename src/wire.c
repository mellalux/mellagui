#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define IN_INIT_CAP   4096u
#define IN_MAX_CAP    65536u  /* 16 maximum-size messages */
#define OUT_INIT_CAP  4096u

_Static_assert((LM_WIRE_MAX_MSG % 4) == 0, "max message size must be word aligned");
_Static_assert(LM_WIRE_MAX_MSG <= 0xffff, "size field is 16 bit");
_Static_assert((IN_MAX_CAP & (IN_MAX_CAP - 1)) == 0, "ring capacity must be a power of two");
_Static_assert(IN_MAX_CAP >= 2 * LM_WIRE_MAX_MSG, "input ring must hold two messages");

/* ---- ring buffer ------------------------------------------------------------------ */

static size_t round_pow2(size_t v)
{
    size_t p = 4096;
    while (p < v) {
        if (p > ((size_t)1 << (sizeof(size_t) * 8 - 2)))
            return 0;
        p <<= 1;
    }
    return p;
}

/* Copy `n` bytes starting `off` bytes after head into dst. Caller guarantees off+n <= len. */
static void ring_peek(const struct lm_ring *r, size_t off, void *dst, size_t n)
{
    size_t start = (r->head + off) & (r->cap - 1);
    size_t first = r->cap - start;
    if (first > n)
        first = n;
    memcpy(dst, r->data + start, first);
    if (n > first)
        memcpy((uint8_t *)dst + first, r->data, n - first);
}

static void ring_consume(struct lm_ring *r, size_t n)
{
    if (n > r->len)
        n = r->len;
    r->len -= n;
    r->head = r->len == 0 ? 0 : (r->head + n) & (r->cap - 1);
}

/* Grow to at least `want` bytes of capacity (power of two), linearising the data. */
static int ring_reserve(struct lm_ring *r, size_t want)
{
    if (r->cap >= want && r->cap != 0)
        return 0;
    size_t ncap = round_pow2(want);
    if (ncap == 0)
        return -1;
    uint8_t *n = malloc(ncap);
    if (!n)
        return -1;
    if (r->len > 0)
        ring_peek(r, 0, n, r->len);
    free(r->data);
    r->data = n;
    r->cap = ncap;
    r->head = 0;
    return 0;
}

static void ring_append(struct lm_ring *r, const void *src, size_t n)
{
    size_t tail = (r->head + r->len) & (r->cap - 1);
    size_t first = r->cap - tail;
    if (first > n)
        first = n;
    memcpy(r->data + tail, src, first);
    if (n > first)
        memcpy(r->data, (const uint8_t *)src + first, n - first);
    r->len += n;
}

/* ---- init / finish ------------------------------------------------------------------ */

void lm_wire_init(struct lm_wire *w, int fd, size_t max_out)
{
    memset(w, 0, sizeof(*w));
    w->fd = fd;
    w->max_out = max_out ? max_out : LM_WIRE_DEFAULT_MAX_OUT;
}

void lm_wire_finish(struct lm_wire *w)
{
    for (int i = 0; i < w->n_in_fds; i++)
        close(w->in_fds[i]);
    for (int i = 0; i < w->n_out_fds; i++)
        close(w->out_fds[i]);
    w->n_in_fds = 0;
    w->n_out_fds = 0;
    free(w->in.data);
    free(w->out.data);
    memset(&w->in, 0, sizeof(w->in));
    memset(&w->out, 0, sizeof(w->out));
}

/* ---- receive --------------------------------------------------------------------------- */

ssize_t lm_wire_recv(struct lm_wire *w)
{
    struct lm_ring *r = &w->in;

    if (w->n_in_fds >= LM_WIRE_MAX_FDS) {
        errno = ENOBUFS;
        return -1;
    }
    if (r->cap == 0 && ring_reserve(r, IN_INIT_CAP) < 0) {
        errno = ENOMEM;
        return -1;
    }
    /* Keep at least one maximum-size message worth of room when possible. */
    if (r->cap - r->len < LM_WIRE_MAX_MSG && r->cap < IN_MAX_CAP) {
        if (ring_reserve(r, r->cap * 2) < 0) {
            errno = ENOMEM;
            return -1;
        }
    }
    size_t space = r->cap - r->len;
    if (space == 0) {
        errno = ENOBUFS;
        return -1;
    }

    size_t tail = (r->head + r->len) & (r->cap - 1);
    struct iovec iov[2];
    int niov = 1;
    iov[0].iov_base = r->data + tail;
    iov[0].iov_len = r->cap - tail < space ? r->cap - tail : space;
    if (iov[0].iov_len < space) {
        iov[1].iov_base = r->data;
        iov[1].iov_len = space - iov[0].iov_len;
        niov = 2;
    }

    int room = LM_WIRE_MAX_FDS - w->n_in_fds;
    union {
        char buf[CMSG_SPACE(sizeof(int) * LM_WIRE_MAX_FDS)];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (size_t)niov;
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)room);

    ssize_t n = recvmsg(w->fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0)
        return -1;

    bool trunc = (msg.msg_flags & MSG_CTRUNC) != 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        size_t nfds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const unsigned char *p = CMSG_DATA(c);
        for (size_t i = 0; i < nfds; i++) {
            int fd;
            memcpy(&fd, p + i * sizeof(int), sizeof(int));
            if (w->n_in_fds < LM_WIRE_MAX_FDS) {
                w->in_fds[w->n_in_fds++] = fd;
            } else {
                close(fd);
                trunc = true;
            }
        }
    }
    if (trunc) {
        /* The kernel dropped (and closed) fds we had no room for: unrecoverable. */
        errno = ENOBUFS;
        return -1;
    }
    if (n > 0) {
        r->len += (size_t)n;
        return n;
    }
    return 0;
}

/* ---- flush -------------------------------------------------------------------------------- */

size_t lm_wire_out_pending(const struct lm_wire *w)
{
    return w->out.len;
}

int lm_wire_flush(struct lm_wire *w)
{
    struct lm_ring *r = &w->out;

    while (r->len > 0) {
        struct iovec iov[2];
        int niov = 1;
        size_t first = r->cap - r->head;
        if (first > r->len)
            first = r->len;
        iov[0].iov_base = r->data + r->head;
        iov[0].iov_len = first;
        if (first < r->len) {
            iov[1].iov_base = r->data;
            iov[1].iov_len = r->len - first;
            niov = 2;
        }

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = (size_t)niov;

        union {
            char buf[CMSG_SPACE(sizeof(int) * LM_WIRE_MAX_FDS)];
            struct cmsghdr align;
        } cmsg_buf;
        int nfds = w->n_out_fds;
        if (nfds > 0) {
            memset(&cmsg_buf, 0, sizeof(cmsg_buf));
            msg.msg_control = cmsg_buf.buf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
            struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
            memcpy(CMSG_DATA(c), w->out_fds, sizeof(int) * (size_t)nfds);
        }

        ssize_t n = sendmsg(w->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;
            return -1;
        }
        if (nfds > 0) {
            for (int i = 0; i < nfds; i++)
                close(w->out_fds[i]);
            w->n_out_fds = 0;
        }
        ring_consume(r, (size_t)n);
    }
    return 0;
}

/* ---- framing ------------------------------------------------------------------------------- */

int lm_wire_next(struct lm_wire *w, struct lm_msg_hdr *hdr)
{
    struct lm_ring *r = &w->in;
    if (r->len < 8)
        return 0;
    uint32_t words[2];
    ring_peek(r, 0, words, sizeof(words));
    uint32_t size = words[1] >> 16;
    if (size < 8 || (size & 3) != 0 || size > LM_WIRE_MAX_MSG)
        return LM_WIRE_E_SIZE;
    if (r->len < size)
        return 0;
    ring_peek(r, 0, w->msg, size);
    hdr->id = words[0];
    hdr->opcode = words[1] & 0xffffu;
    hdr->size = size;
    return 1;
}

void lm_wire_discard(struct lm_wire *w, const struct lm_msg_hdr *hdr)
{
    ring_consume(&w->in, hdr->size);
}

static inline bool sig_skip(char c)
{
    return c >= '0' && c <= '9';
}

static inline bool sig_letter_valid(char c)
{
    switch (c) {
    case 'i': case 'u': case 'f': case 's': case 'o': case 'n': case 'a': case 'h':
        return true;
    default:
        return false;
    }
}

int lm_wire_sig_nargs(const char *sig)
{
    int n = 0;
    bool nullable = false;
    if (!sig)
        return -1;
    for (const char *p = sig; *p; p++) {
        if (sig_skip(*p))
            continue;
        if (*p == '?') {
            if (nullable)
                return -1;
            nullable = true;
            continue;
        }
        if (!sig_letter_valid(*p))
            return -1;
        if (nullable && *p != 's' && *p != 'o' && *p != 'n' && *p != 'a')
            return -1;
        nullable = false;
        if (++n > LM_WIRE_MAX_ARGS)
            return -1;
    }
    if (nullable)
        return -1;
    return n;
}

static inline uint32_t pad4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

int lm_wire_decode(struct lm_wire *w, const struct lm_msg_hdr *hdr,
                   const char *sig, union lm_arg *args, int max_args)
{
    if (hdr->size < 8 || (hdr->size & 3) != 0 || hdr->size > LM_WIRE_MAX_MSG ||
        hdr->size > w->in.len)
        return LM_WIRE_E_SIZE;

    const uint8_t *m = w->msg;
    const uint32_t size = hdr->size;
    uint32_t pos = 8;
    int nargs = 0;
    int err = 0;
    int popped[LM_WIRE_MAX_ARGS];
    int npopped = 0;
    bool nullable = false;

    for (const char *p = sig; *p && !err; p++) {
        char c = *p;
        if (sig_skip(c))
            continue;
        if (c == '?') {
            nullable = true;
            continue;
        }
        if (nargs >= max_args || nargs >= LM_WIRE_MAX_ARGS) {
            err = LM_WIRE_E_ARGS;
            break;
        }
        union lm_arg *a = &args[nargs];
        memset(a, 0, sizeof(*a));
        switch (c) {
        case 'i':
        case 'u':
        case 'f':
        case 'o':
        case 'n': {
            uint32_t v;
            if (size - pos < 4) {
                err = LM_WIRE_E_TRUNCATED;
                break;
            }
            memcpy(&v, m + pos, 4);
            pos += 4;
            if (c == 'o' && v == 0 && !nullable) {
                err = LM_WIRE_E_NULL;
                break;
            }
            if (c == 'i' || c == 'f')
                a->i = (int32_t)v;
            else
                a->u = v;
            break;
        }
        case 's': {
            uint32_t len;
            if (size - pos < 4) {
                err = LM_WIRE_E_TRUNCATED;
                break;
            }
            memcpy(&len, m + pos, 4);
            pos += 4;
            if (len == 0) {
                if (!nullable)
                    err = LM_WIRE_E_NULL;
                else
                    a->s = NULL;
                break;
            }
            if (len > size - pos || pad4(len) > size - pos) {
                err = LM_WIRE_E_STRING;
                break;
            }
            const char *s = (const char *)(m + pos);
            if (s[len - 1] != '\0' || memchr(s, '\0', len) != s + len - 1) {
                err = LM_WIRE_E_STRING;
                break;
            }
            a->s = s;
            pos += pad4(len);
            break;
        }
        case 'a': {
            uint32_t len;
            if (size - pos < 4) {
                err = LM_WIRE_E_TRUNCATED;
                break;
            }
            memcpy(&len, m + pos, 4);
            pos += 4;
            if (len > size - pos || pad4(len) > size - pos) {
                err = LM_WIRE_E_ARRAY;
                break;
            }
            a->a.size = len;
            a->a.data = m + pos;
            pos += pad4(len);
            break;
        }
        case 'h': {
            if (w->n_in_fds <= 0) {
                err = LM_WIRE_E_FD;
                break;
            }
            int fd = w->in_fds[0];
            memmove(w->in_fds, w->in_fds + 1, sizeof(int) * (size_t)(w->n_in_fds - 1));
            w->n_in_fds--;
            popped[npopped++] = fd;
            a->h = fd;
            break;
        }
        default:
            err = LM_WIRE_E_ARGS;
            break;
        }
        nullable = false;
        if (!err)
            nargs++;
    }
    if (!err && pos != size)
        err = LM_WIRE_E_EXTRA;

    ring_consume(&w->in, size);
    if (err) {
        for (int i = 0; i < npopped; i++)
            close(popped[i]);
        return err;
    }
    return nargs;
}

/* ---- send ------------------------------------------------------------------------------------ */

int lm_wire_send(struct lm_wire *w, uint32_t id, uint32_t opcode,
                 const char *sig, const union lm_arg *args)
{
    uint32_t buf[LM_WIRE_MAX_MSG / 4];
    uint8_t *b = (uint8_t *)buf;
    size_t pos = 8;
    int fds[LM_WIRE_MAX_ARGS];
    int nfds = 0;
    int err = 0;
    int idx = 0;
    bool nullable = false;

    if (opcode > 0xffffu)
        return LM_WIRE_E_ARGS;

    for (const char *p = sig; *p && !err; p++) {
        char c = *p;
        if (sig_skip(c))
            continue;
        if (c == '?') {
            nullable = true;
            continue;
        }
        if (idx >= LM_WIRE_MAX_ARGS || !args) {
            err = LM_WIRE_E_ARGS;
            break;
        }
        const union lm_arg *a = &args[idx++];
        switch (c) {
        case 'i':
        case 'u':
        case 'f':
        case 'o':
        case 'n': {
            uint32_t v;
            if (c == 'i' || c == 'f')
                v = (uint32_t)a->i;
            else
                v = a->u;
            if (pos + 4 > LM_WIRE_MAX_MSG) {
                err = LM_WIRE_E_SIZE;
                break;
            }
            memcpy(b + pos, &v, 4);
            pos += 4;
            break;
        }
        case 's': {
            uint32_t len = 0;
            if (!a->s) {
                if (!nullable) {
                    err = LM_WIRE_E_NULL;
                    break;
                }
            } else {
                size_t sl = strnlen(a->s, LM_WIRE_MAX_MSG);
                if (sl >= LM_WIRE_MAX_MSG) {
                    err = LM_WIRE_E_SIZE;
                    break;
                }
                len = (uint32_t)sl + 1;
            }
            if (pos + 4 + pad4(len) > LM_WIRE_MAX_MSG) {
                err = LM_WIRE_E_SIZE;
                break;
            }
            memcpy(b + pos, &len, 4);
            pos += 4;
            if (len) {
                memcpy(b + pos, a->s, len);
                memset(b + pos + len, 0, pad4(len) - len);
                pos += pad4(len);
            }
            break;
        }
        case 'a': {
            uint32_t len = a->a.size;
            if (len > LM_WIRE_MAX_MSG || pos + 4 + pad4(len) > LM_WIRE_MAX_MSG) {
                err = LM_WIRE_E_SIZE;
                break;
            }
            if (len && !a->a.data) {
                err = LM_WIRE_E_ARRAY;
                break;
            }
            memcpy(b + pos, &len, 4);
            pos += 4;
            if (len) {
                memcpy(b + pos, a->a.data, len);
                memset(b + pos + len, 0, pad4(len) - len);
                pos += pad4(len);
            }
            break;
        }
        case 'h': {
            if (a->h < 0) {
                err = LM_WIRE_E_FD;
                break;
            }
            int d = fcntl(a->h, F_DUPFD_CLOEXEC, 3);
            if (d < 0) {
                err = LM_WIRE_E_NOMEM;
                break;
            }
            fds[nfds++] = d;
            break;
        }
        default:
            err = LM_WIRE_E_ARGS;
            break;
        }
        nullable = false;
    }

    if (!err) {
        uint32_t hdr[2] = { id, (uint32_t)(pos << 16) | opcode };
        memcpy(b, hdr, sizeof(hdr));
        struct lm_ring *r = &w->out;

        if (r->len + pos > w->max_out || w->n_out_fds + nfds > LM_WIRE_MAX_FDS) {
            /* Try to drain before declaring the peer too slow. */
            lm_wire_flush(w);
            if (r->len + pos > w->max_out || w->n_out_fds + nfds > LM_WIRE_MAX_FDS) {
                w->overflowed = true;
                err = LM_WIRE_E_OVERFLOW;
            }
        }
        if (!err && ring_reserve(r, r->len + pos > OUT_INIT_CAP ? r->len + pos : OUT_INIT_CAP) < 0)
            err = LM_WIRE_E_NOMEM;
        if (!err) {
            ring_append(r, b, pos);
            for (int i = 0; i < nfds; i++)
                w->out_fds[w->n_out_fds++] = fds[i];
            return 0;
        }
    }
    for (int i = 0; i < nfds; i++)
        close(fds[i]);
    return err;
}

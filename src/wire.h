#ifndef LUMEN_WIRE_H
#define LUMEN_WIRE_H

/*
 * Wayland wire protocol: connection buffers, framing, argument
 * marshalling and SCM_RIGHTS fd passing. Owner: wire.c.
 *
 * Usable by the compositor (server side) AND by clients (lumen-demo,
 * lumen-term): it knows nothing about interfaces or objects, only about
 * "signature strings".
 *
 * Wire format (host endianness): word0 = object id, word1 = (size << 16) |
 * opcode, size in bytes including the 8-byte header; body padded to 4 bytes.
 *   int/uint/fixed/object/new_id : 1 word
 *   string : length word (including NUL) + bytes + NUL + padding
 *   array  : size word + bytes + padding
 *   fd     : no payload; fd travels in SCM_RIGHTS ancillary data
 *
 * Signature letters (same as libwayland, one per argument):
 *   i int32   u uint32   f fixed (24.8)   s string   o object id
 *   n new_id  a array    h fd
 * A '?' immediately before s/o/n... marks the argument nullable. Digits are
 * ignored (reserved for "since version" in upstream signatures; we keep
 * `since` in struct lm_message instead).
 * NOTE: unlike libwayland a new_id whose interface is not fixed (only
 * wl_registry.bind) is written out explicitly as "usun" (name, interface
 * string, version, new id) in the signature, so the wire layer never needs
 * special cases.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LM_WIRE_MAX_MSG   4096  /* max total size of one message in bytes */
#define LM_WIRE_MAX_ARGS  20
#define LM_WIRE_MAX_FDS   28    /* max fds queued in either direction */
#define LM_WIRE_DEFAULT_MAX_OUT (1024 * 1024)

/* ---- fixed point ---------------------------------------------------------- */

typedef int32_t lm_fixed_t; /* signed 24.8 */

static inline lm_fixed_t lm_fixed_from_double(double d)
{
    if (d > 8388607.0)
        d = 8388607.0;
    if (d < -8388608.0)
        d = -8388608.0;
    return (lm_fixed_t)(d >= 0 ? d * 256.0 + 0.5 : d * 256.0 - 0.5);
}
static inline double lm_fixed_to_double(lm_fixed_t f) { return (double)f / 256.0; }

/* ---- arguments ------------------------------------------------------------ */

struct lm_resource; /* server.h */

struct lm_array {
    uint32_t size;       /* bytes */
    const void *data;
};

/*
 * One decoded/encodable argument. Which member is used follows the
 * signature letter: i->i, u->u, f->f, s->s, o->o, n->n, a->a, h->h.
 *
 * Decoding (lm_wire_decode): `s` and `a.data` point into the connection's
 * scratch message buffer and are valid only until the next lm_wire_next()
 * on that connection. `h` is a fresh fd owned by the caller of decode (the
 * server layer closes it after the request handler returns).
 *
 * Server dispatch (server.c) additionally rewrites every decoded 'o' argument
 * in place to `r` (resolved resource pointer, NULL for a nullable 0) before
 * invoking a handler; handlers never see raw object ids for 'o'.
 *
 * Encoding (lm_wire_send): `s` may be NULL only for a nullable string;
 * `h` is dup()'d by the wire layer, the caller keeps ownership of its fd.
 */
union lm_arg {
    int32_t i;
    uint32_t u;
    lm_fixed_t f;
    const char *s;
    uint32_t o;
    uint32_t n;
    struct lm_array a;
    int h;
    struct lm_resource *r;
};

/* ---- connection ----------------------------------------------------------- */

struct lm_ring {
    uint8_t *data;   /* capacity is a power of two (or 0 before first use) */
    size_t cap;
    size_t head;     /* read offset */
    size_t len;      /* bytes stored */
};

/* Fields are private to wire.c; they are exposed so the struct can be embedded. */
struct lm_wire {
    int fd;                          /* the socket; NOT owned (never closed by finish) */
    struct lm_ring in;
    struct lm_ring out;
    int in_fds[LM_WIRE_MAX_FDS];     /* received, not yet consumed by decode */
    int n_in_fds;
    int out_fds[LM_WIRE_MAX_FDS];    /* dup'd, queued for the next sendmsg */
    int n_out_fds;
    size_t max_out;                  /* output queue byte limit (backpressure) */
    bool overflowed;                 /* set when lm_wire_send hit max_out */
    uint8_t msg[LM_WIRE_MAX_MSG];    /* contiguous copy of the message being decoded */
    void *priv;
};

struct lm_msg_hdr {
    uint32_t id;
    uint32_t opcode;
    uint32_t size;       /* bytes including header */
};

/* Negative error codes returned by lm_wire_next / lm_wire_decode / lm_wire_send. */
enum lm_wire_error {
    LM_WIRE_E_SIZE = -1,       /* header size <8, unaligned or > LM_WIRE_MAX_MSG */
    LM_WIRE_E_TRUNCATED = -2,  /* body shorter than the signature needs */
    LM_WIRE_E_STRING = -3,     /* bad string length / missing NUL */
    LM_WIRE_E_ARRAY = -4,      /* array length exceeds message */
    LM_WIRE_E_NULL = -5,       /* null string/object for non-nullable arg */
    LM_WIRE_E_FD = -6,         /* signature wants an fd but none queued */
    LM_WIRE_E_ARGS = -7,       /* too many arguments or malformed signature */
    LM_WIRE_E_EXTRA = -8,      /* message has trailing bytes the signature does not use */
    LM_WIRE_E_OVERFLOW = -9,   /* output queue limit exceeded (send) */
    LM_WIRE_E_NOMEM = -10,
};

/* `max_out`: output queue limit in bytes (LM_WIRE_DEFAULT_MAX_OUT if 0). */
void lm_wire_init(struct lm_wire *w, int fd, size_t max_out);
/* Frees rings, closes still-queued fds (in and out). Does not close w->fd. */
void lm_wire_finish(struct lm_wire *w);

/*
 * Read available bytes and ancillary fds with one recvmsg(MSG_CMSG_CLOEXEC |
 * MSG_DONTWAIT). Returns bytes read (>0), 0 on EOF, -1 with errno set:
 * EAGAIN/EINTR = nothing to read now; ENOBUFS = input ring or fd queue full
 * (a peer that never lets us consume - treat as fatal); other = fatal.
 * Callers loop until -1/EAGAIN or until they have enough to dispatch.
 */
ssize_t lm_wire_recv(struct lm_wire *w);

/* Number of unsent output bytes. */
size_t lm_wire_out_pending(const struct lm_wire *w);
/*
 * Write queued output (with queued fds attached to the first sendmsg) until
 * done or EAGAIN. Returns 0 = everything sent, 1 = would block (data left;
 * wait for EPOLLOUT), -1 = fatal error (EPIPE, ...).
 */
int lm_wire_flush(struct lm_wire *w);

/*
 * Peek the next complete message header.
 * Returns 1 and fills *hdr (also copies the message into w->msg),
 * 0 if more bytes are needed, <0 (LM_WIRE_E_SIZE) if framing is invalid.
 * Does not consume; call lm_wire_decode() or lm_wire_discard() next.
 */
int lm_wire_next(struct lm_wire *w, struct lm_msg_hdr *hdr);

/*
 * Decode the message last returned by lm_wire_next according to `sig` into
 * args[0..n) and CONSUME it (also on error). `max_args` bounds the array.
 * Returns the number of arguments (>=0) or an lm_wire_error. The message must
 * be consumed exactly: trailing bytes are LM_WIRE_E_EXTRA. fds referenced by
 * the signature are popped from the input fd queue in order; on error any fds
 * already popped for this message are closed.
 */
int lm_wire_decode(struct lm_wire *w, const struct lm_msg_hdr *hdr,
                   const char *sig, union lm_arg *args, int max_args);

/* Consume the current message without decoding (closing nothing; fds stay queued). */
void lm_wire_discard(struct lm_wire *w, const struct lm_msg_hdr *hdr);

/*
 * Append one message to the output queue. Returns 0 or a negative
 * lm_wire_error (LM_WIRE_E_OVERFLOW sets w->overflowed; the connection should
 * then be dropped). Does not write to the socket; call lm_wire_flush().
 * Messages larger than LM_WIRE_MAX_MSG are rejected with LM_WIRE_E_SIZE.
 */
int lm_wire_send(struct lm_wire *w, uint32_t id, uint32_t opcode,
                 const char *sig, const union lm_arg *args);

#endif

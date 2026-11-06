/*
 * Minimal TLS client for lumen-web. There are no OpenSSL headers on the build machine and nothing may depend on
 * the library at build time, so libssl is loaded with dlopen() at run time (libssl.so.3, then libssl.so.1.1) and
 * only the dozen functions needed are declared by hand (tls.c). The peer certificate chain is verified against the
 * system CA store ($SSL_CERT_FILE / $SSL_CERT_DIR override it) and the host name is checked (SNI + SSL_set1_host).
 * If the library cannot be loaded tls_open() fails with a readable message; plain http keeps working.
 *
 * The socket stays non-blocking: tls_read/tls_write return TLS_AGAIN_READ / TLS_AGAIN_WRITE and the caller polls.
 */
#ifndef LUMEN_TLS_H
#define LUMEN_TLS_H

#include <stddef.h>
#include <sys/types.h>

struct tls;

#define TLS_AGAIN_READ  (-1)        /* poll for POLLIN and call again */
#define TLS_AGAIN_WRITE (-3)        /* poll for POLLOUT and call again */
#define TLS_ERROR       (-2)

/* True if libssl can be loaded (without trying a connection). */
int tls_available(void);

/* Handshake on the connected non-blocking socket `fd` for server name `host`, at most `timeout_ms`.
 * NULL + message in `err` on failure (certificate problems say why). The socket stays owned by the caller. */
struct tls *tls_open(int fd, const char *host, int timeout_ms, char *err, size_t errn);

/* >0 bytes, 0 = the peer closed the connection (also without close_notify), TLS_AGAIN_*, TLS_ERROR. */
ssize_t tls_read(struct tls *t, void *buf, size_t n);
ssize_t tls_write(struct tls *t, const void *buf, size_t n);
void tls_close(struct tls *t);

#endif

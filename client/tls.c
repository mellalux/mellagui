/* TLS through a run-time loaded libssl, see tls.h. */
#include "tls.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

/* constants from <openssl/ssl.h> / <openssl/tls1.h> (stable across 1.1 and 3.x) */
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6
#define SSL_VERIFY_PEER 1
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define TLS1_2_VERSION 0x0303
#define SSL_CTRL_OPTIONS 32
#define SSL_OP_IGNORE_UNEXPECTED_EOF 0x00000080UL     /* OpenSSL 3: servers that just close the socket */
#define X509_V_OK 0

static struct {
    int tried;
    void *ssl, *crypto;
    int (*OPENSSL_init_ssl)(uint64_t, const void *);
    const SSL_METHOD *(*TLS_client_method)(void);
    SSL_CTX *(*SSL_CTX_new)(const SSL_METHOD *);
    void (*SSL_CTX_free)(SSL_CTX *);
    int (*SSL_CTX_set_default_verify_paths)(SSL_CTX *);
    void (*SSL_CTX_set_verify)(SSL_CTX *, int, void *);
    long (*SSL_CTX_ctrl)(SSL_CTX *, int, long, void *);
    SSL *(*SSL_new)(SSL_CTX *);
    void (*SSL_free)(SSL *);
    int (*SSL_set_fd)(SSL *, int);
    long (*SSL_ctrl)(SSL *, int, long, void *);
    int (*SSL_set1_host)(SSL *, const char *);
    int (*SSL_connect)(SSL *);
    int (*SSL_read)(SSL *, void *, int);
    int (*SSL_write)(SSL *, const void *, int);
    int (*SSL_shutdown)(SSL *);
    int (*SSL_get_error)(const SSL *, int);
    long (*SSL_get_verify_result)(const SSL *);
    unsigned long (*ERR_get_error)(void);
    void (*ERR_error_string_n)(unsigned long, char *, size_t);
    const char *(*X509_verify_cert_error_string)(long);
    void (*ERR_clear_error)(void);
    SSL_CTX *ctx;
} L;

struct tls {
    SSL *ssl;
};

static void *sym(const char *name, int *ok)
{
    void *p = dlsym(L.ssl, name);
    if (!p && L.crypto)
        p = dlsym(L.crypto, name);
    if (!p)
        *ok = 0;
    return p;
}

static int load(void)
{
    if (L.tried)
        return L.ssl != NULL;
    L.tried = 1;
    static const char *const ssl_names[] = { "libssl.so.3", "libssl.so.1.1", "libssl.so", NULL };
    static const char *const crypto_names[] = { "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", NULL };
    for (int i = 0; ssl_names[i] && !L.ssl; i++)
        L.ssl = dlopen(ssl_names[i], RTLD_NOW | RTLD_LOCAL);
    for (int i = 0; crypto_names[i] && !L.crypto; i++)
        L.crypto = dlopen(crypto_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!L.ssl)
        return 0;
    int ok = 1;
#define S(name) *(void **)&L.name = sym(#name, &ok)
    S(OPENSSL_init_ssl); S(TLS_client_method); S(SSL_CTX_new); S(SSL_CTX_free); S(SSL_CTX_set_default_verify_paths);
    S(SSL_CTX_set_verify); S(SSL_CTX_ctrl); S(SSL_new); S(SSL_free); S(SSL_set_fd); S(SSL_ctrl); S(SSL_set1_host);
    S(SSL_connect); S(SSL_read); S(SSL_write); S(SSL_shutdown); S(SSL_get_error); S(SSL_get_verify_result);
    S(ERR_get_error); S(ERR_error_string_n); S(X509_verify_cert_error_string); S(ERR_clear_error);
#undef S
    if (!ok) {
        dlclose(L.ssl);
        L.ssl = NULL;
        return 0;
    }
    L.OPENSSL_init_ssl(0, NULL);
    L.ctx = L.SSL_CTX_new(L.TLS_client_method());
    if (!L.ctx) {
        L.ssl = NULL;
        return 0;
    }
    L.SSL_CTX_ctrl(L.ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);
    L.SSL_CTX_ctrl(L.ctx, SSL_CTRL_OPTIONS, (long)SSL_OP_IGNORE_UNEXPECTED_EOF, NULL);
    L.SSL_CTX_set_default_verify_paths(L.ctx);          /* honours $SSL_CERT_FILE / $SSL_CERT_DIR */
    L.SSL_CTX_set_verify(L.ctx, SSL_VERIFY_PEER, NULL);
    return 1;
}

int tls_available(void)
{
    return load();
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fail_msg(struct tls *t, int rc, char *err, size_t errn)
{
    long vr = L.SSL_get_verify_result(t->ssl);
    if (vr != X509_V_OK) {
        snprintf(err, errn, "certificate not trusted: %s", L.X509_verify_cert_error_string(vr));
        return;
    }
    unsigned long e = L.ERR_get_error();
    if (e) {
        char buf[160];
        L.ERR_error_string_n(e, buf, sizeof(buf));
        snprintf(err, errn, "TLS handshake failed: %s", buf);
    } else {
        snprintf(err, errn, "TLS handshake failed (error %d)", L.SSL_get_error(t->ssl, rc));
    }
}

struct tls *tls_open(int fd, const char *host, int timeout_ms, char *err, size_t errn)
{
    if (!load()) {
        snprintf(err, errn, "https needs OpenSSL (libssl.so.3), which is not installed");
        return NULL;
    }
    L.ERR_clear_error();
    struct tls *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->ssl = L.SSL_new(L.ctx);
    if (!t->ssl) {
        free(t);
        snprintf(err, errn, "SSL_new failed");
        return NULL;
    }
    L.SSL_set_fd(t->ssl, fd);
    L.SSL_ctrl(t->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)host);
    L.SSL_set1_host(t->ssl, host);                      /* the certificate must be valid for this name */
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        int rc = L.SSL_connect(t->ssl);
        if (rc == 1)
            return t;
        int e = L.SSL_get_error(t->ssl, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            fail_msg(t, rc, err, errn);
            break;
        }
        struct pollfd pf = { .fd = fd, .events = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT };
        long long left = deadline - now_ms();
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0) {
            snprintf(err, errn, "TLS handshake timed out");
            break;
        }
    }
    L.SSL_free(t->ssl);
    free(t);
    return NULL;
}

ssize_t tls_read(struct tls *t, void *buf, size_t n)
{
    if (n > 1 << 20)
        n = 1 << 20;
    L.ERR_clear_error();
    int r = L.SSL_read(t->ssl, buf, (int)n);
    if (r > 0)
        return r;
    switch (L.SSL_get_error(t->ssl, r)) {
    case SSL_ERROR_WANT_READ: return TLS_AGAIN_READ;
    case SSL_ERROR_WANT_WRITE: return TLS_AGAIN_WRITE;
    case SSL_ERROR_ZERO_RETURN: return 0;
    case SSL_ERROR_SYSCALL: return r == 0 ? 0 : TLS_ERROR;
    default: return TLS_ERROR;
    }
}

ssize_t tls_write(struct tls *t, const void *buf, size_t n)
{
    if (n > 1 << 20)
        n = 1 << 20;
    L.ERR_clear_error();
    int r = L.SSL_write(t->ssl, buf, (int)n);
    if (r > 0)
        return r;
    switch (L.SSL_get_error(t->ssl, r)) {
    case SSL_ERROR_WANT_READ: return TLS_AGAIN_READ;
    case SSL_ERROR_WANT_WRITE: return TLS_AGAIN_WRITE;
    default: return TLS_ERROR;
    }
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    L.SSL_shutdown(t->ssl);
    L.SSL_free(t->ssl);
    free(t);
}

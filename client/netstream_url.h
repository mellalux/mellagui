#ifndef MELLAGUI_NETSTREAM_URL_H
#define MELLAGUI_NETSTREAM_URL_H

/* What the one who shows a streamed file (netstream.h) needs to know about its address. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* "http://127.0.0.1:PORT/TOKEN/f/NAME": the port and the token. */
static inline bool netstream_parse(const char *s, int *port, char token[33])
{
    int used = 0;
    return sscanf(s, "http://127.0.0.1:%d/%32[0-9a-f]/f/%n", port, token, &used) == 2 && used > 0 && s[used] && strlen(token) == 32 && *port > 0 && *port < 65536;
}

/* Is it an address netstream_start made? */
static inline bool netstream_is_url(const char *s)
{
    int port;
    char token[33];
    return netstream_parse(s, &port, token);
}

/* The file's name out of such an address (percent codes undone). */
static inline void netstream_name(const char *url, char *out, size_t n)
{
    const char *p = strrchr(url, '/');
    size_t k = 0;
    for (p = p ? p + 1 : url; *p && k + 1 < n; p++) {
        unsigned v;
        if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) {
            out[k++] = (char)v;
            p += 2;
        } else {
            out[k++] = *p;
        }
    }
    out[k] = 0;
}

/* A connection that keeps the helper of `url` running; close() it when the file is not shown any more. -1 when there is none. */
static inline int netstream_hold(const char *url)
{
    int port;
    char token[33], req[96];
    if (!netstream_parse(url, &port, token))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int n = snprintf(req, sizeof(req), "GET /%s/h HTTP/1.1\r\n\r\n", token);
    if (fd >= 0 && (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || send(fd, req, (size_t)n, MSG_NOSIGNAL) != n)) {
        close(fd);
        fd = -1;
    }
    return fd;
}

#endif

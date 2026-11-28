#include "netstream.h"
#include "netstream_url.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sftp.h"

#define CHUNK (256 * 1024)
#define IDLE_MS 60000
#define MAX_CONNS 16
#define START_MS 30000

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the one who asks --------------------------------------------------------------------------------------------------------- */

static bool write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = write(fd, c, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

bool netstream_start(const struct smb_conn *c, const char *share, const char *path, char *url, size_t urlsz, char *err, size_t errsz)
{
    /* what to open: NUL ended fields */
    char in[2 * PATH_MAX], port[16];
    size_t n = 0;
    snprintf(port, sizeof(port), "%d", c->port);
    const char *field[] = { c->sftp ? "sftp" : "smb", c->host, port, c->user, c->pass, c->legacy ? "1" : "0", share, path };
    for (size_t i = 0; i < sizeof(field) / sizeof(field[0]); i++) {
        size_t l = strlen(field[i]) + 1;
        if (n + l > sizeof(in)) {
            snprintf(err, errsz, "The path is too long");
            return false;
        }
        memcpy(in + n, field[i], l);
        n += l;
    }
    char self[PATH_MAX];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    int to[2], from[2];
    if (sl <= 0 || pipe2(to, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "The helper cannot be started");
        return false;
    }
    self[sl] = 0;
    if (pipe2(from, O_CLOEXEC) < 0) {
        close(to[0]);
        close(to[1]);
        snprintf(err, errsz, "The helper cannot be started");
        return false;
    }
    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() != 0)                    /* nobody has to wait for the helper */
            _exit(0);
        setsid();
        dup2(to[0], 0);
        dup2(from[1], 1);
        execl(self, "mellagui-files", "--stream", (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    if (pid > 0)
        waitpid(pid, NULL, 0);              /* (fails with ECHILD when SIGCHLD is ignored: no matter) */
    bool sent = pid > 0 && write_all(to[1], in, n);
    close(to[1]);
    memset(in, 0, sizeof(in));
    char line[PATH_MAX + 128];
    size_t len = 0;
    int64_t deadline = now_ms() + START_MS;
    while (sent && len < sizeof(line) - 1) {
        struct pollfd p = { .fd = from[0], .events = POLLIN };
        int64_t left = deadline - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) == 0)
            break;
        ssize_t k = read(from[0], line + len, sizeof(line) - 1 - len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        len += (size_t)k;
        if (memchr(line, '\n', len))
            break;
    }
    close(from[0]);                         /* (a helper that has not answered finds nobody there and ends) */
    signal(SIGPIPE, old);
    line[len] = 0;
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "ok ", 3) && netstream_is_url(line + 3) && strlen(line + 3) < urlsz) {
        snprintf(url, urlsz, "%s", line + 3);
        return true;
    }
    snprintf(err, errsz, "%s", !strncmp(line, "error ", 6) ? line + 6 : "The server does not answer");
    return false;
}

/* ---- the file on the server --------------------------------------------------------------------------------------------------- */

static struct {
    bool sftp;
    char host[128], user[64], pass[128], share[256], path[PATH_MAX];
    int port;
    bool legacy;
    int64_t size;
} F;

/* SMB, through the library's simple calls (they keep one connection of their own). */
static struct {
    void *lib;
    int (*init)(void (*auth)(const char *, const char *, char *, int, char *, int, char *, int), int debug);
    void *(*set_context)(void *);
    int (*protocols)(void *, const char *, const char *);
    int (*open)(const char *, int, mode_t);
    off_t (*lseek)(int, off_t, int);
    ssize_t (*read)(int, void *, size_t);
    int (*close)(int);
    int fd;
} S = { .fd = -1 };

static void auth(const char *srv, const char *shr, char *wg, int wglen, char *un, int unlen, char *pw, int pwlen)
{
    (void)srv, (void)shr, (void)wg, (void)wglen;
    snprintf(un, (size_t)unlen, "%s", F.user[0] ? F.user : "guest");
    snprintf(pw, (size_t)pwlen, "%s", F.user[0] ? F.pass : "");
}

static void encode(const char *s, char *out, size_t cap, bool slashes)
{
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '~' || (slashes && c == '/'))
            out[n++] = (char)c;
        else
            n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

static bool smb_load(char *err, size_t errsz)
{
    if (S.lib)
        return true;
    const char *name = getenv("MELLAGUI_SMBLIB");
    void *lib = dlopen(name && *name ? name : "libsmbclient.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        snprintf(err, errsz, "libsmbclient is not installed");
        return false;
    }
    *(void **)&S.init = dlsym(lib, "smbc_init");
    *(void **)&S.set_context = dlsym(lib, "smbc_set_context");
    *(void **)&S.protocols = dlsym(lib, "smbc_setOptionProtocols");
    *(void **)&S.open = dlsym(lib, "smbc_open");
    *(void **)&S.lseek = dlsym(lib, "smbc_lseek");
    *(void **)&S.read = dlsym(lib, "smbc_read");
    *(void **)&S.close = dlsym(lib, "smbc_close");
    if (!S.init || !S.open || !S.lseek || !S.read || !S.close || S.init(auth, 0) < 0) {
        snprintf(err, errsz, "libsmbclient cannot be used");
        dlclose(lib);
        return false;
    }
    S.lib = lib;
    if (F.legacy && S.set_context && S.protocols)
        S.protocols(S.set_context(NULL), "NT1", NULL);          /* (set_context(NULL) answers the context in use) */
    return true;
}

static void smb_end(void)
{
    if (S.fd >= 0)
        S.close(S.fd);
    S.fd = -1;
}

static bool smb_begin(char *err, size_t errsz)
{
    if (!smb_load(err, errsz))
        return false;
    char host[400], share[800], path[3 * PATH_MAX], url[3 * PATH_MAX + 1300];
    encode(F.host, host, sizeof(host), false);
    encode(F.share, share, sizeof(share), false);
    encode(F.path, path, sizeof(path), true);
    if (F.port > 0)
        snprintf(url, sizeof(url), "smb://%s:%d/%s/%s", host, F.port, share, path);
    else
        snprintf(url, sizeof(url), "smb://%s/%s/%s", host, share, path);
    S.fd = S.open(url, O_RDONLY, 0);
    if (S.fd < 0) {
        snprintf(err, errsz, "%s", errno == EACCES || errno == EPERM ? "Access denied" : errno == ENOENT ? "Not found" : strerror(errno));
        return false;
    }
    off_t end = S.lseek(S.fd, 0, SEEK_END);
    if (end < 0) {
        snprintf(err, errsz, "%s", strerror(errno));
        smb_end();
        return false;
    }
    F.size = (int64_t)end;
    return true;
}

static ssize_t smb_pread(uint8_t *buf, size_t n, int64_t off)
{
    if (S.fd < 0 || S.lseek(S.fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t k = S.read(S.fd, buf + done, n - done);
        if (k < 0)
            return -1;
        if (!k)
            break;
        done += (size_t)k;
    }
    return (ssize_t)done;
}

/* SSH: `ssh -s ... sftp`, the SFTP protocol version 3 on its standard input and output. */
enum { FX_INIT = 1, FX_VERSION = 2, FX_OPEN = 3, FX_CLOSE = 4, FX_READ = 5, FX_FSTAT = 8, FX_STATUS = 101, FX_HANDLE = 102, FX_DATA = 103, FX_ATTRS = 105 };

#define PIECE 32768
#define PIECES (CHUNK / PIECE)

static struct {
    pid_t pid;
    int in, out;
    uint32_t id;
    uint8_t handle[256];
    uint32_t hlen;
    uint8_t msg[PIECE + 1024];
    uint32_t mlen;
    char said[400];                     /* what ssh wrote on its error output */
    int errfd;
} P = { .in = -1, .out = -1, .errfd = -1 };

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool sftp_send(uint8_t type, const uint8_t *body, size_t n)
{
    uint8_t h[5];
    put32(h, (uint32_t)n + 1);
    h[4] = type;
    return write_all(P.in, h, 5) && write_all(P.in, body, n);
}

static bool read_exact(int fd, uint8_t *p, size_t n)
{
    while (n) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int r = poll(&pf, 1, 20000);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* The next message: its type, the body in P.msg; -1 when there is none. */
static int sftp_next(void)
{
    uint8_t h[5];
    if (!read_exact(P.out, h, 5))
        return -1;
    uint32_t n = get32(h);
    if (n < 1 || n - 1 > sizeof(P.msg) || !read_exact(P.out, P.msg, n - 1))
        return -1;
    P.mlen = n - 1;
    return h[4];
}

static void sftp_end(void)
{
    if (P.in >= 0)
        close(P.in);
    if (P.out >= 0)
        close(P.out);
    if (P.errfd >= 0)
        close(P.errfd);
    P.in = P.out = P.errfd = -1;
    if (P.pid > 0) {
        kill(P.pid, SIGTERM);
        waitpid(P.pid, NULL, 0);
    }
    P.pid = 0;
}

static bool sftp_spawn(void)
{
    char self[PATH_MAX], askpass[PATH_MAX + 16], pass[160], port[16], target[200];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    int in[2], out[2], er[2];
    if (sl <= 0 || pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0 || pipe2(er, O_CLOEXEC) < 0)
        return false;
    self[sl] = 0;
    const char *prog = getenv("MELLAGUI_SSH");
    if (!prog || !*prog)
        prog = "ssh";
    const char *argv[32];
    int n = 0;
#define ARG(x) argv[n++] = (x)
    ARG(prog);
    ARG("-x");
    ARG("-a");
    ARG("-o"); ARG("ClearAllForwardings=yes");
    ARG("-o"); ARG("StrictHostKeyChecking=accept-new");
    ARG("-o"); ARG("ConnectTimeout=12");
    ARG("-o"); ARG("ServerAliveInterval=15");
    ARG("-o"); ARG(F.pass[0] ? "NumberOfPasswordPrompts=1" : "BatchMode=yes");
    if (F.port > 0 && F.port != 22) {
        snprintf(port, sizeof(port), "%d", F.port);
        ARG("-p"); ARG(port);
    }
    if (F.user[0])
        snprintf(target, sizeof(target), "%s@%s", F.user, F.host);
    else
        snprintf(target, sizeof(target), "%s", F.host);
    ARG("-s");
    ARG("--");                          /* (what follows is never an option) */
    ARG(target);
    ARG("sftp");
    argv[n] = NULL;
#undef ARG
    P.pid = fork();
    if (P.pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(er[1], 2);
        signal(SIGPIPE, SIG_DFL);
        setenv("LC_ALL", "C", 1);
        if (F.pass[0]) {
            snprintf(askpass, sizeof(askpass), "%.*s", PATH_MAX - 1, self);
            snprintf(pass, sizeof(pass), "%s", F.pass);
            setenv("SSH_ASKPASS", askpass, 1);
            setenv("SSH_ASKPASS_REQUIRE", "force", 1);
            setenv("MELLAGUI_ASKPASS", "1", 1);
            setenv("MELLAGUI_SFTP_PASS", pass, 1);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(er[1]);
    P.in = in[1];
    P.out = out[0];
    P.errfd = er[0];
    fcntl(P.errfd, F_SETFL, O_NONBLOCK);
    return P.pid > 0;
}

static bool sftp_begin(char *err, size_t errsz)
{
    if (!sftp_target_ok(F.user, F.host)) {
        snprintf(err, errsz, "That is not a usable server or user name");
        return false;
    }
    if (!sftp_spawn()) {
        snprintf(err, errsz, "ssh cannot be started");
        return false;
    }
    uint8_t b[PATH_MAX + 32];
    put32(b, 3);
    if (!sftp_send(FX_INIT, b, 4) || sftp_next() != FX_VERSION)
        goto no_server;
    size_t l = strlen(F.path) + 1;                          /* "/" + the path below the root */
    if (l + 16 > sizeof(b))
        goto no_server;
    put32(b, ++P.id);
    put32(b + 4, (uint32_t)l);
    b[8] = '/';
    memcpy(b + 9, F.path, l - 1);
    put32(b + 8 + l, 1);                                    /* to be read */
    put32(b + 12 + l, 0);                                   /* no attributes */
    if (!sftp_send(FX_OPEN, b, l + 16))
        goto no_server;
    int t = sftp_next();
    if (t == FX_STATUS && P.mlen >= 8) {
        uint32_t code = get32(P.msg + 4);
        snprintf(err, errsz, "%s", code == 2 ? "Not found" : code == 3 ? "Access denied" : "The server does not open the file");
        sftp_end();
        return false;
    }
    if (t != FX_HANDLE || P.mlen < 8 || (P.hlen = get32(P.msg + 4)) > sizeof(P.handle) || P.hlen > P.mlen - 8)
        goto no_server;
    memcpy(P.handle, P.msg + 8, P.hlen);
    put32(b, ++P.id);
    put32(b + 4, P.hlen);
    memcpy(b + 8, P.handle, P.hlen);
    if (!sftp_send(FX_FSTAT, b, 8 + P.hlen) || sftp_next() != FX_ATTRS || P.mlen < 16 || !(get32(P.msg + 4) & 1))
        goto no_server;
    F.size = (int64_t)((uint64_t)get32(P.msg + 8) << 32 | get32(P.msg + 12));
    return true;
no_server:;
    ssize_t k = read(P.errfd, P.said, sizeof(P.said) - 1);
    if (k <= 0) {                                           /* (ssh may still be writing it) */
        struct pollfd p = { .fd = P.errfd, .events = POLLIN };
        if (poll(&p, 1, 1000) > 0)
            k = read(P.errfd, P.said, sizeof(P.said) - 1);
    }
    P.said[k > 0 ? k : 0] = 0;
    bool login = false;
    if (P.said[0])
        sftp_explain(P.said, err, errsz, &login);
    else
        snprintf(err, errsz, "The server does not answer");
    sftp_end();
    return false;
}

/* Several reads are asked for at once: one after another would wait for the network every time. */
static ssize_t sftp_pread(uint8_t *buf, size_t n, int64_t off)
{
    if (P.in < 0)
        return -1;
    uint32_t first = P.id + 1, asked = 0;
    uint32_t got[PIECES] = { 0 };
    for (size_t at = 0; at < n && asked < PIECES; at += PIECE, asked++) {
        uint8_t b[300];
        uint64_t o = (uint64_t)off + at;
        put32(b, ++P.id);
        put32(b + 4, P.hlen);
        memcpy(b + 8, P.handle, P.hlen);
        put32(b + 8 + P.hlen, (uint32_t)(o >> 32));
        put32(b + 12 + P.hlen, (uint32_t)o);
        put32(b + 16 + P.hlen, (uint32_t)(n - at < PIECE ? n - at : PIECE));
        if (!sftp_send(FX_READ, b, 20 + P.hlen))
            return -1;
    }
    for (uint32_t i = 0; i < asked; i++) {
        int t = sftp_next();
        if (t < 0 || P.mlen < 4)
            return -1;
        uint32_t k = get32(P.msg) - first;
        if (k >= asked)
            return -1;
        if (t == FX_DATA && P.mlen >= 8) {
            uint32_t len = get32(P.msg + 4);
            size_t room = n - (size_t)k * PIECE < PIECE ? n - (size_t)k * PIECE : PIECE;
            if (len > P.mlen - 8 || len > room)
                return -1;
            memcpy(buf + (size_t)k * PIECE, P.msg + 8, len);
            got[k] = len;
        } else if (!(t == FX_STATUS && P.mlen >= 8 && get32(P.msg + 4) == 1)) {      /* 1 = the end of the file */
            return -1;
        }
    }
    size_t done = 0;                                        /* as far as there is no gap */
    for (uint32_t k = 0; k < asked; k++) {
        done += got[k];
        if (got[k] < PIECE)
            break;
    }
    return (ssize_t)done;
}

static bool src_begin(char *err, size_t errsz) { return F.sftp ? sftp_begin(err, errsz) : smb_begin(err, errsz); }
static void src_end(void) { if (F.sftp) sftp_end(); else smb_end(); }

static pthread_mutex_t src_lock = PTHREAD_MUTEX_INITIALIZER;

/* Bytes of the file; a connection that was lost is made once more. */
static ssize_t src_pread(uint8_t *buf, size_t n, int64_t off)
{
    pthread_mutex_lock(&src_lock);
    ssize_t k = F.sftp ? sftp_pread(buf, n, off) : smb_pread(buf, n, off);
    if (k <= 0) {
        char err[200];
        int64_t size = F.size;
        src_end();
        if (src_begin(err, sizeof(err)) && F.size == size)
            k = F.sftp ? sftp_pread(buf, n, off) : smb_pread(buf, n, off);
        F.size = size;
    }
    pthread_mutex_unlock(&src_lock);
    return k;
}

/* ---- serving it ---------------------------------------------------------------------------------------------------------------- */

static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_conns;
static int64_t g_quiet_since;
static char g_token[33];
static int g_port;

/* Is the other end of the connection a program of ours? The kernel's table of connections says whose socket it is. */
static bool same_user(const struct sockaddr_in *peer)
{
    FILE *f = fopen("/proc/net/tcp", "r");
    if (!f)
        return false;
    char want[64], line[256];
    snprintf(want, sizeof(want), "%08X:%04X %08X:%04X", peer->sin_addr.s_addr, ntohs(peer->sin_port), htonl(INADDR_LOOPBACK), (unsigned)g_port);
    bool ok = false;
    while (!ok && fgets(line, sizeof(line), f)) {
        char *p = strstr(line, want);
        unsigned uid;
        if (p && sscanf(p + strlen(want), " %*x %*x:%*x %*x:%*x %*x %u", &uid) == 1)
            ok = uid == (unsigned)getuid();
    }
    fclose(f);
    return ok;
}

static bool send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = send(fd, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

static void reply(int fd, const char *status)
{
    char h[160];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    send_all(fd, h, (size_t)n);
}

static void serve(int fd)
{
    char req[8192];
    size_t len = 0;
    int64_t deadline = now_ms() + 10000;
    while (len < sizeof(req) - 1) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int64_t left = deadline - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) == 0)
            return;
        ssize_t k = recv(fd, req + len, sizeof(req) - 1 - len, 0);
        if (k <= 0)
            return;
        len += (size_t)k;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[len] = 0;
    bool head = !strncmp(req, "HEAD /", 6);
    const char *p = head ? req + 6 : !strncmp(req, "GET /", 5) ? req + 5 : NULL;
    if (!p || strncmp(p, g_token, 32) || p[32] != '/') {
        reply(fd, "404 Not Found");
        return;
    }
    if (p[33] == 'h' && p[34] == ' ') {                     /* kept open: "I am still showing this" */
        char junk[256];
        while (recv(fd, junk, sizeof(junk), 0) > 0) {}
        return;
    }
    if (strncmp(p + 33, "f/", 2)) {
        reply(fd, "404 Not Found");
        return;
    }
    int64_t size = F.size, from = 0, to = size - 1;
    bool ranged = false;
    const char *r = strcasestr(req, "\r\nRange:");
    if (r) {
        r += 8;
        while (*r == ' ')
            r++;
        long long a = -1, b = -1;
        if (!strncmp(r, "bytes=-", 7) && sscanf(r + 7, "%lld", &b) == 1 && b > 0) {
            from = b < size ? size - b : 0;
            ranged = true;
        } else if (sscanf(r, "bytes=%lld-%lld", &a, &b) >= 1 && a >= 0) {
            from = a;
            if (b >= a && b < size)
                to = b;
            ranged = true;
        }
        if (ranged && from >= size) {
            char h[200];
            int n = snprintf(h, sizeof(h), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", (long long)size);
            send_all(fd, h, (size_t)n);
            return;
        }
    }
    char h[320], range[96] = "";
    if (ranged)
        snprintf(range, sizeof(range), "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)from, (long long)to, (long long)size);
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n%sContent-Length: %lld\r\nConnection: close\r\n\r\n",
                     ranged ? "206 Partial Content" : "200 OK", range, (long long)(to - from + 1));
    if (!send_all(fd, h, (size_t)n) || head)
        return;
    uint8_t *buf = malloc(CHUNK);
    size_t step = 64 * 1024;                                /* small at first: a seek that only looks costs little */
    while (buf && from <= to) {
        size_t want = (uint64_t)(to - from + 1) < step ? (size_t)(to - from + 1) : step;
        ssize_t k = src_pread(buf, want, from);
        if (k <= 0 || !send_all(fd, buf, (size_t)k))        /* (a player that waits keeps us here, without the server's lock) */
            break;
        from += k;
        step = CHUNK;
    }
    free(buf);
}

static void *conn_thread(void *ud)
{
    int fd = (int)(intptr_t)ud;
    serve(fd);
    close(fd);
    pthread_mutex_lock(&count_lock);
    if (--g_conns == 0)
        g_quiet_since = now_ms();
    pthread_mutex_unlock(&count_lock);
    return NULL;
}

static int say_error(const char *why)
{
    char line[400];
    int n = snprintf(line, sizeof(line), "error %.300s\n", why);
    write_all(1, line, (size_t)n);
    return 1;
}

int netstream_serve_main(void)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    char in[2 * PATH_MAX];
    size_t len = 0;
    ssize_t k;
    while (len < sizeof(in) - 1 && ((k = read(0, in + len, sizeof(in) - 1 - len)) > 0 || (k < 0 && errno == EINTR)))
        len += k > 0 ? (size_t)k : 0;
    in[len] = 0;
    const char *field[8];
    size_t at = 0;
    for (int i = 0; i < 8; i++) {
        if (at >= len)
            return say_error("Nothing to open");
        field[i] = in + at;
        at += strlen(field[i]) + 1;
    }
    F.sftp = !strcmp(field[0], "sftp");
    snprintf(F.host, sizeof(F.host), "%s", field[1]);
    F.port = atoi(field[2]);
    snprintf(F.user, sizeof(F.user), "%s", field[3]);
    snprintf(F.pass, sizeof(F.pass), "%s", field[4]);
    F.legacy = field[5][0] == '1';
    snprintf(F.share, sizeof(F.share), "%s", field[6]);
    snprintf(F.path, sizeof(F.path), "%s", field[7]);
    memset(in, 0, sizeof(in));
    prctl(PR_SET_DUMPABLE, 0);                              /* (it holds a password) */
    char err[200];
    if (!src_begin(err, sizeof(err)))
        return say_error(err);

    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof(sa);
    uint8_t rnd[16];
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 16) < 0 || getsockname(ls, (struct sockaddr *)&sa, &sl) < 0 ||
        getrandom(rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd)) {
        src_end();
        return say_error("No local connection can be offered");
    }
    g_port = ntohs(sa.sin_port);
    for (int i = 0; i < 16; i++)
        snprintf(g_token + 2 * i, 3, "%02x", rnd[i]);
    const char *base = strrchr(F.path, '/');
    base = base ? base + 1 : F.path;
    char name[3 * 256 + 1], line[sizeof(name) + 128];
    encode(base, name, sizeof(name), false);
    int n = snprintf(line, sizeof(line), "ok http://127.0.0.1:%d/%s/f/%s\n", g_port, g_token, name);
    if (!write_all(1, line, (size_t)n)) {
        src_end();
        return 1;
    }
    int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0);
    dup2(nul, 1);
    if (chdir("/") < 0) {}
    const char *e = getenv("MELLAGUI_STREAM_IDLE_MS");
    int64_t idle = e && atoi(e) > 0 ? atoi(e) : IDLE_MS;
    g_quiet_since = now_ms();
    for (;;) {
        struct pollfd p = { .fd = ls, .events = POLLIN };
        int r = poll(&p, 1, 200);
        pthread_mutex_lock(&count_lock);
        int conns = g_conns;
        bool done = !conns && now_ms() - g_quiet_since >= idle;
        pthread_mutex_unlock(&count_lock);
        if (done)
            break;
        if (r <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int fd = accept4(ls, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        pthread_t th;
        pthread_attr_t at_;
        pthread_attr_init(&at_);
        pthread_attr_setdetachstate(&at_, PTHREAD_CREATE_DETACHED);
        pthread_mutex_lock(&count_lock);
        bool take = conns < MAX_CONNS && same_user(&peer);
        if (take) {
            g_conns++;
            if (pthread_create(&th, &at_, conn_thread, (void *)(intptr_t)fd) != 0) {
                g_conns--;
                take = false;
            }
        }
        pthread_mutex_unlock(&count_lock);
        pthread_attr_destroy(&at_);
        if (!take)
            close(fd);
    }
    pthread_mutex_lock(&src_lock);
    src_end();
    return 0;
}

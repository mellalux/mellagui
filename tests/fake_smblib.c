/* A stand-in for libsmbclient ($MELLAGUI_SMBLIB), the simple calls that playing a file uses: smb://host[:port]/share/path is the file
 * $FAKE_SMB_ROOT/share/path. $FAKE_SMB_USER / $FAKE_SMB_PASS: the login the "server" wants. Built by the tests with gcc -shared. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

typedef void (*auth_fn)(const char *, const char *, char *, int, char *, int, char *, int);
static auth_fn g_auth;

int smbc_init(auth_fn fn, int debug)
{
    (void)debug;
    g_auth = fn;
    return 0;
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

int smbc_open(const char *url, int flags, mode_t mode)
{
    (void)flags, (void)mode;
    const char *want_user = getenv("FAKE_SMB_USER"), *want_pass = getenv("FAKE_SMB_PASS"), *root = getenv("FAKE_SMB_ROOT");
    if (want_user) {
        char wg[64] = "", un[64] = "", pw[128] = "";
        g_auth("host", "share", wg, sizeof(wg), un, sizeof(un), pw, sizeof(pw));
        if (strcmp(un, want_user) || strcmp(pw, want_pass ? want_pass : "")) {
            errno = EACCES;
            return -1;
        }
    }
    const char *p = strncmp(url, "smb://", 6) ? NULL : strchr(url + 6, '/');
    if (!p || !root) {
        errno = EINVAL;
        return -1;
    }
    char path[4096];
    size_t n = (size_t)snprintf(path, sizeof(path), "%s", root);
    for (; *p && n + 1 < sizeof(path); p++) {
        if (*p == '%' && p[1] && p[2]) {
            path[n++] = (char)(hex(p[1]) * 16 + hex(p[2]));
            p += 2;
        } else {
            path[n++] = *p;
        }
    }
    path[n] = 0;
    return open(path, O_RDONLY);
}

off_t smbc_lseek(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
ssize_t smbc_read(int fd, void *buf, size_t n) { return read(fd, buf, n > 61440 ? 61440 : n); }      /* (short reads, as over a network) */
int smbc_close(int fd) { return close(fd); }

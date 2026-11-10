#include "smb.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 15000                /* a listing / a small operation; a transfer has no limit (its poll can cancel) */
#define OUT_MAX (4u << 20)

static __thread pid_t g_child;

pid_t smb_child_pid(void)
{
    return g_child;
}

/* ---- the program ---------------------------------------------------------------------------------------------------------- */

static const char *program(void)
{
    const char *e = getenv("MELLAGUI_SMBCLIENT");
    return e && *e ? e : "smbclient";
}

bool smb_available(void)
{
    const char *p = program();
    if (strchr(p, '/'))
        return access(p, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        return false;
    char dir[512];
    for (const char *s = path; *s;) {
        size_t l = strcspn(s, ":");
        char full[1024];
        snprintf(dir, sizeof(dir), "%.*s", (int)(l < sizeof(dir) ? l : sizeof(dir) - 1), s);
        snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", p);
        if (access(full, X_OK) == 0)
            return true;
        s += l + (s[l] ? 1 : 0);
    }
    return false;
}

bool smb_name_ok(const char *name)
{
    if (!name || !*name)
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20 || *p == '"' || *p == ';' || *p == '\\' || *p == 0x7f)
            return false;
    return true;
}

/* The user and password as smbclient's -A file; the path is written to `path`. */
static bool write_auth(const struct smb_conn *c, char *path, size_t n)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    snprintf(path, n, "%s/mellagui-smb-%d-%ld.auth", dir && *dir ? dir : "/tmp", (int)getpid(), (long)time(NULL) % 100000);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        return false;
    }
    fprintf(f, "username = %s\npassword = %s\n", c->user, c->pass);
    fclose(f);
    return true;
}

/* Runs smbclient with `argv` (NULL terminated, argv[0] is filled in here); the output goes to *out (malloc'd, NUL terminated), what it wrote on
 * stderr is appended to it. Returns the exit status (0 = fine), or -1 when it could not run / was given up / timed out (err says why). */
static int run(char **argv, int timeout_ms, char **out, char *err, size_t errsz, smb_poll_fn poll_fn, void *pd, const char *progress_file)
{
    (void)progress_file;
    *out = NULL;
    int po[2], pe[2], px[2];                                       /* px: tells the parent that exec failed */
    if (pipe(po) < 0 || pipe(pe) < 0 || pipe2(px, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "cannot make a pipe");
        return -1;
    }
    argv[0] = (char *)program();
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "cannot start %s", program());
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(po[1], 1);
        dup2(pe[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); close(px[0]);
        setenv("LC_ALL", "C", 1);                                  /* the output is parsed: no translations */
        execvp(argv[0], argv);
        int e = errno;
        if (write(px[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
    close(po[1]);
    close(pe[1]);
    close(px[1]);
    g_child = pid;
    int exec_errno = 0;
    bool no_program = read(px[0], &exec_errno, sizeof(exec_errno)) == (ssize_t)sizeof(exec_errno);
    close(px[0]);
    fcntl(po[0], F_SETFL, O_NONBLOCK);
    fcntl(pe[0], F_SETFL, O_NONBLOCK);
    char *buf = malloc(8192);
    size_t len = 0, cap = 8192;
    char ebuf[512];
    size_t elen = 0;
    bool po_open = true, pe_open = true, gave_up = false;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int status = 0;
    bool exited = false;
    while ((po_open || pe_open) && !gave_up) {
        struct pollfd pf[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
        poll(pf, 2, 50);
        char tmp[4096];
        ssize_t k;
        while (po_open && (k = read(po[0], tmp, sizeof(tmp))) > 0) {
            if (len + (size_t)k + 1 > cap && cap < OUT_MAX) {
                char *nb = realloc(buf, cap * 2);
                if (!nb)
                    break;
                buf = nb;
                cap *= 2;
            }
            if (len + (size_t)k + 1 <= cap) {
                memcpy(buf + len, tmp, (size_t)k);
                len += (size_t)k;
            }
        }
        if (po_open && k == 0)
            po_open = false;
        while (pe_open && (k = read(pe[0], tmp, sizeof(tmp))) > 0)
            if (elen + (size_t)k < sizeof(ebuf)) {
                memcpy(ebuf + elen, tmp, (size_t)k);
                elen += (size_t)k;
            }
        if (pe_open && k == 0)
            pe_open = false;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        if (timeout_ms > 0 && ms > timeout_ms) {
            snprintf(err, errsz, "The server does not answer");
            gave_up = true;
        } else if (poll_fn && !poll_fn(pd)) {
            snprintf(err, errsz, "Cancelled");
            gave_up = true;
        }
    }
    if (gave_up) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    close(po[0]);
    close(pe[0]);
    g_child = 0;
    if (waitpid(pid, &status, 0) > 0)
        exited = true;
    else if (errno == ECHILD)                                      /* the caller ignores SIGCHLD: the status is gone, the output has to tell */
        status = 0, exited = true;
    buf[len] = 0;
    if (elen) {
        ebuf[elen] = 0;
        char *nb = realloc(buf, len + elen + 2);
        if (nb) {
            buf = nb;
            buf[len] = '\n';
            memcpy(buf + len + 1, ebuf, elen + 1);
        }
    }
    *out = buf;
    if (gave_up)
        return -1;
    if (no_program) {
        snprintf(err, errsz, "smbclient is not installed (package smbclient / samba-client)");
        return -1;
    }
    return exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Builds the common arguments and runs `-c cmd` (or the listing of the server when share is NULL). */
static int call(const struct smb_conn *c, const char *share, const char *dir, const char *cmd, int timeout_ms, char **out, char *err, size_t errsz,
                smb_poll_fn poll_fn, void *pd)
{
    char *argv[24];
    int n = 1;
    char target[256], auth[512] = "", opt[64], dpath[1024];
    if (share && *share)
        snprintf(target, sizeof(target), "//%s/%s", c->host, share);
    else
        snprintf(target, sizeof(target), "//%s", c->host);
    if (share && *share) {
        argv[n++] = target;
    } else {
        argv[n++] = "-L";
        argv[n++] = target;
    }
    bool have_auth = false;
    if (c->user[0]) {
        if (!write_auth(c, auth, sizeof(auth))) {
            snprintf(err, errsz, "Cannot write the credentials");
            return -1;
        }
        argv[n++] = "-A";
        argv[n++] = auth;
        have_auth = true;
    } else {
        argv[n++] = "-N";                                          /* no password: a guest */
    }
    if (c->legacy) {
        snprintf(opt, sizeof(opt), "--option=client min protocol=NT1");
        argv[n++] = opt;
    }
    if (dir && *dir) {
        snprintf(dpath, sizeof(dpath), "%s", dir);
        argv[n++] = "-D";
        argv[n++] = dpath;
    }
    if (cmd) {
        argv[n++] = "-c";
        argv[n++] = (char *)cmd;
    }
    argv[n] = NULL;
    int rc = run(argv, timeout_ms, out, err, errsz, poll_fn, pd, NULL);
    if (have_auth)
        unlink(auth);
    return rc;
}

/* Interpret a finished call: 0 when it worked, else -1 with the reason. smbclient exits 0 even when a command failed, so the output counts, too. */
static int judge(int rc, const char *out, char *err, size_t errsz)
{
    if (rc < 0)
        return -1;                                                  /* err is set already */
    bool bad = rc != 0 || (out && (strstr(out, "NT_STATUS_") || strstr(out, "tree connect failed") || strstr(out, "Connection to ") ||
                                   strstr(out, "protocol negotiation failed") || strstr(out, "session setup failed")));
    if (!bad)
        return 0;
    smb_explain(out ? out : "", err, errsz);
    return -1;
}

/* ---- parsing ------------------------------------------------------------------------------------------------------------- */

void smb_explain(const char *o, char *out, size_t n)
{
    static const struct { const char *needle, *text; } t[] = {
        { "NT_STATUS_LOGON_FAILURE", "Wrong user name or password" },
        { "NT_STATUS_ACCESS_DENIED", "Access denied" },
        { "NT_STATUS_BAD_NETWORK_NAME", "No such share" },
        { "NT_STATUS_OBJECT_NAME_NOT_FOUND", "Not found" },
        { "NT_STATUS_OBJECT_PATH_NOT_FOUND", "Not found" },
        { "NT_STATUS_OBJECT_NAME_COLLISION", "It exists already" },
        { "NT_STATUS_DIRECTORY_NOT_EMPTY", "The folder is not empty" },
        { "NT_STATUS_DISK_FULL", "The disk is full" },
        { "NT_STATUS_CONNECTION_REFUSED", "The server refuses the connection" },
        { "NT_STATUS_HOST_UNREACHABLE", "Cannot reach the server" },
        { "NT_STATUS_IO_TIMEOUT", "The server does not answer" },
        { "NT_STATUS_UNSUCCESSFUL", "The server could not be reached" },
        { "NT_STATUS_ACCOUNT_DISABLED", "The account is disabled" },
        { "NT_STATUS_ACCOUNT_LOCKED_OUT", "The account is locked" },
        { "protocol negotiation failed", "The server only speaks the old SMB1 protocol (switch 'legacy' on for this connection)" },
        { "NT_STATUS_INVALID_NETWORK_RESPONSE", "The server only speaks the old SMB1 protocol (switch 'legacy' on for this connection)" },
        { "Connection to ", "Cannot reach the server" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strstr(o, t[i].needle)) {
            snprintf(out, n, "%s", t[i].text);
            return;
        }
    const char *s = strstr(o, "NT_STATUS_");                       /* something else: the status itself */
    if (s) {
        size_t l = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
        snprintf(out, n, "%.*s", (int)l, s);
        return;
    }
    while (*o == '\n' || *o == ' ')
        o++;
    size_t l = strcspn(o, "\n");
    snprintf(out, n, "%.*s", (int)(l < 200 ? l : 200), *o ? o : "smbclient failed");
}

bool smb_parse_ls_line(const char *line, struct smb_item *it)
{
    /* "  name with spaces                    D        0  Mon Nov  3 10:22:11 2025": from the right: year, time, day, month, weekday, size, attributes */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
        len--;
    if (len < 12 || line[0] != ' ' || line[1] != ' ')
        return false;
    const char *tok[7];
    size_t tl[7];
    size_t end = len;
    for (int i = 0; i < 7; i++) {
        while (end > 0 && line[end - 1] == ' ')
            end--;
        size_t st = end;
        while (st > 0 && line[st - 1] != ' ')
            st--;
        if (st == end)
            return false;
        tok[i] = line + st;
        tl[i] = end - st;
        end = st;
    }
    while (end > 0 && line[end - 1] == ' ')
        end--;
    size_t start = 0;
    while (start < end && line[start] == ' ')
        start++;
    if (end <= start || end - start >= sizeof(it->name))
        return false;
    /* tok[0] year, [1] time, [2] day, [3] month, [4] weekday, [5] size, [6] attributes */
    char attr[16], size[24], date[64];
    if (tl[6] >= sizeof(attr) || tl[5] >= sizeof(size))
        return false;
    snprintf(attr, sizeof(attr), "%.*s", (int)tl[6], tok[6]);
    snprintf(size, sizeof(size), "%.*s", (int)tl[5], tok[5]);
    for (const char *p = attr; *p; p++)
        if (!strchr("DAHSRNdahsrn", *p))
            return false;
    for (const char *p = size; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    snprintf(date, sizeof(date), "%.*s %.*s %.*s %.*s %.*s", (int)tl[4], tok[4], (int)tl[3], tok[3], (int)tl[2], tok[2], (int)tl[1], tok[1],
             (int)tl[0], tok[0]);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(date, "%a %b %d %H:%M:%S %Y", &tm))
        return false;
    tm.tm_isdst = -1;
    snprintf(it->name, sizeof(it->name), "%.*s", (int)(end - start), line + start);
    if (!strcmp(it->name, ".") || !strcmp(it->name, ".."))
        return false;
    it->dir = strchr(attr, 'D') || strchr(attr, 'd');
    it->size = atoll(size);
    it->mtime = mktime(&tm);
    return true;
}

bool smb_parse_share_line(const char *line, char *name, size_t n)
{
    /* "\tSharename       Type      Comment": entries start with a tab, then name, then the type */
    if (line[0] != '\t')
        return false;
    const char *p = line;
    while (*p == '\t' || *p == ' ')
        p++;
    const char *e = p;
    while (*e && *e != '\t' && !(e[0] == ' ' && (e[1] == ' ' || e[1] == '\t')))        /* the name runs to the columns' gap */
        e++;
    size_t l = (size_t)(e - p);
    if (!l || l >= n)
        return false;
    const char *t = e;
    while (*t == ' ' || *t == '\t')
        t++;
    if (strncmp(t, "Disk", 4) != 0 || (t[4] && t[4] != ' ' && t[4] != '\t' && t[4] != '\n' && t[4] != '\r'))
        return false;
    snprintf(name, n, "%.*s", (int)l, p);
    if (name[l - 1] == '$')
        return false;
    return true;
}

/* ---- the operations ------------------------------------------------------------------------------------------------------- */

static bool names_ok(const char *path);

static int cmp_share(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int smb_shares(const struct smb_conn *c, char (**out)[64], int *n, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char *o;
    int rc = call(c, NULL, NULL, NULL, TIMEOUT_MS, &o, err, errsz, poll, pd);
    int j = judge(rc, o, err, errsz);
    if (j < 0) {
        free(o);
        return -1;
    }
    char (*list)[64] = calloc(64, sizeof(*list));
    int cnt = 0;
    for (char *s = o; list && *s && cnt < 64;) {
        size_t l = strcspn(s, "\n");
        char line[512], name[64];
        snprintf(line, sizeof(line), "%.*s", (int)(l < sizeof(line) - 1 ? l : sizeof(line) - 1), s);
        if (smb_parse_share_line(line, name, sizeof(name)))
            snprintf(list[cnt++], sizeof(list[0]), "%s", name);
        s += l + (s[l] ? 1 : 0);
    }
    free(o);
    if (cnt > 1)
        qsort(list, (size_t)cnt, sizeof(list[0]), cmp_share);
    *out = list;
    *n = cnt;
    return list ? 0 : -1;
}

int smb_ls(const struct smb_conn *c, const char *share, const char *dir, struct smb_item **out, int *n, char *err, size_t errsz, smb_poll_fn poll,
           void *pd)
{
    if (dir && *dir && !names_ok(dir)) {
        snprintf(err, errsz, "A folder name with a quote, ';' or a back slash cannot be opened on a network drive");
        return -1;
    }
    char *o;
    int rc = call(c, share, dir, "ls", TIMEOUT_MS, &o, err, errsz, poll, pd);
    if (judge(rc, o, err, errsz) < 0) {
        free(o);
        return -1;
    }
    int cap = 64, cnt = 0;
    struct smb_item *list = malloc((size_t)cap * sizeof(*list));
    for (char *s = o; list && *s;) {
        size_t l = strcspn(s, "\n");
        char line[512];
        snprintf(line, sizeof(line), "%.*s", (int)(l < sizeof(line) - 1 ? l : sizeof(line) - 1), s);
        struct smb_item it;
        if (smb_parse_ls_line(line, &it)) {
            if (cnt == cap) {
                struct smb_item *t = realloc(list, (size_t)cap * 2 * sizeof(*list));
                if (!t)
                    break;
                list = t;
                cap *= 2;
            }
            list[cnt++] = it;
        }
        s += l + (s[l] ? 1 : 0);
    }
    free(o);
    *out = list;
    *n = cnt;
    return list ? 0 : -1;
}

/* smbclient's command string: the quoted paths, with forward slashes (it takes both). */
static bool quoted(char *dst, size_t n, const char *a, const char *b)
{
    int w = b ? snprintf(dst, n, "\"%s\" \"%s\"", a, b) : snprintf(dst, n, "\"%s\"", a);
    return w > 0 && (size_t)w < n && smb_name_ok(a) && (!b || smb_name_ok(b)) ;
}

static bool names_ok(const char *path)
{
    for (const char *s = path; *s;) {                               /* every part of a path ("a/b/c") must be a good name */
        size_t l = strcspn(s, "/");
        char part[256];
        if (l >= sizeof(part))
            return false;
        snprintf(part, sizeof(part), "%.*s", (int)l, s);
        if (l && !smb_name_ok(part))
            return false;
        s += l + (s[l] ? 1 : 0);
    }
    return true;
}

static int simple(const struct smb_conn *c, const char *share, const char *verb, const char *a, const char *b, int timeout, char *err, size_t errsz,
                  smb_poll_fn poll, void *pd)
{
    char cmd[1200], q[1100];
    if (!names_ok(a) || (b && !names_ok(b)) || !quoted(q, sizeof(q), a, b)) {
        snprintf(err, errsz, "A name with a quote, ';' or a back slash cannot be used on a network drive");
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%s %s", verb, q);
    char *o;
    int rc = call(c, share, NULL, cmd, timeout, &o, err, errsz, poll, pd);
    int j = judge(rc, o, err, errsz);
    free(o);
    return j;
}

int smb_get(const struct smb_conn *c, const char *share, const char *remote, const char *local, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    return simple(c, share, "get", remote, local, 0, err, errsz, poll, pd);
}

int smb_put(const struct smb_conn *c, const char *share, const char *local, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    return simple(c, share, "put", local, remote, 0, err, errsz, poll, pd);
}

int smb_del(const struct smb_conn *c, const char *share, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    return simple(c, share, dir ? "deltree" : "del", remote, NULL, TIMEOUT_MS * 4, err, errsz, poll, pd);
}

int smb_mkdir(const struct smb_conn *c, const char *share, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    return simple(c, share, "mkdir", remote, NULL, TIMEOUT_MS, err, errsz, poll, pd);
}

int smb_rename(const struct smb_conn *c, const char *share, const char *from, const char *to, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    return simple(c, share, "rename", from, to, TIMEOUT_MS, err, errsz, poll, pd);
}

/* ---- the default gateway -------------------------------------------------------------------------------------------------- */

void smb_default_gateway(char *out, size_t n)
{
    out[0] = 0;
    const char *e = getenv("MELLAGUI_SMB_GATEWAY");                  /* tests: an address, or "none" */
    if (e) {
        if (strcmp(e, "none") != 0)
            snprintf(out, n, "%s", e);
        return;
    }
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return;
    char line[256];
    if (!fgets(line, sizeof(line), f)) {                           /* the header */
        fclose(f);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        unsigned dest, gw, flags;
        if (sscanf(line, "%31s %x %x %x", iface, &dest, &gw, &flags) == 4 && dest == 0 && (flags & 2) && gw) {          /* the default route, up, with a gateway */
            snprintf(out, n, "%u.%u.%u.%u", gw & 255, (gw >> 8) & 255, (gw >> 16) & 255, (gw >> 24) & 255);          /* (little endian in the file) */
            break;
        }
    }
    fclose(f);
}

/* ---- selftest -------------------------------------------------------------------------------------------------------------- */

int smb_selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "smb selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    struct smb_item it;
    CHECK(smb_parse_ls_line("  Documents                           D        0  Mon Nov  3 10:22:11 2025", &it) && it.dir && !strcmp(it.name, "Documents") && it.size == 0);
    CHECK(smb_parse_ls_line("  file with  spaces.txt               N     1234  Tue Nov 10 12:00:00 2026", &it) && !it.dir && !strcmp(it.name, "file with  spaces.txt") &&
          it.size == 1234);
    CHECK(smb_parse_ls_line("  big.iso                             A 4700372992  Sat Jan  4 23:59:59 2025", &it) && it.size == 4700372992LL);
    CHECK(smb_parse_ls_line("  hidden                             DH        0  Sat Jan  4 23:59:59 2025", &it) && it.dir);
    CHECK(!smb_parse_ls_line("  .                                   D        0  Mon Nov  3 10:22:11 2025", &it));
    CHECK(!smb_parse_ls_line("  ..                                  D        0  Mon Nov  3 10:22:11 2025", &it));
    CHECK(!smb_parse_ls_line("\t\t974405520 blocks of size 1024. 12345 blocks available", &it));
    CHECK(!smb_parse_ls_line("", &it));
    CHECK(!smb_parse_ls_line("  x D 0 not a date", &it));
    char name[64];
    CHECK(smb_parse_share_line("\t1TB             Disk      1TB's 1TB in CT1000BX 500SSD1", name, sizeof(name)) && !strcmp(name, "1TB"));
    CHECK(smb_parse_share_line("\tMy Share        Disk      comment", name, sizeof(name)) && !strcmp(name, "My Share"));
    CHECK(!smb_parse_share_line("\tIPC$            IPC       IPC Service (RT-BE58U)", name, sizeof(name)));
    CHECK(!smb_parse_share_line("\tC$              Disk      Default share", name, sizeof(name)));
    CHECK(!smb_parse_share_line("\tSharename       Type      Comment", name, sizeof(name)));
    CHECK(!smb_parse_share_line("Anonymous login successful", name, sizeof(name)));
    char why[200];
    smb_explain("tree connect failed: NT_STATUS_BAD_NETWORK_NAME", why, sizeof(why));
    CHECK(!strcmp(why, "No such share"));
    smb_explain("session setup failed: NT_STATUS_LOGON_FAILURE", why, sizeof(why));
    CHECK(!strcmp(why, "Wrong user name or password"));
    smb_explain("Connection to 10.0.0.9 failed (Error NT_STATUS_HOST_UNREACHABLE)", why, sizeof(why));
    CHECK(!strcmp(why, "Cannot reach the server"));
    smb_explain("NT_STATUS_SOMETHING_NEW happened", why, sizeof(why));
    CHECK(!strcmp(why, "NT_STATUS_SOMETHING_NEW"));
    CHECK(smb_name_ok("a b.txt") && !smb_name_ok("a\"b") && !smb_name_ok("a;b") && !smb_name_ok("a\\b") && !smb_name_ok("") && !smb_name_ok("a\nb"));
    return bad ? 1 : 0;
}

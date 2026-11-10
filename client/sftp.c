#include "sftp.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 20000

static const char *program(void)
{
    const char *e = getenv("MELLAGUI_SFTP");
    return e && *e ? e : "sftp";
}

static bool in_path(const char *prog)
{
    if (strchr(prog, '/'))
        return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    for (const char *s = path ? path : ""; *s;) {
        size_t l = strcspn(s, ":");
        char full[1024];
        snprintf(full, sizeof(full), "%.*s/%s", (int)(l < 800 ? l : 800), s, prog);
        if (access(full, X_OK) == 0)
            return true;
        s += l + (s[l] ? 1 : 0);
    }
    return false;
}

bool sftp_available(void)
{
    return in_path(program()) && (getenv("MELLAGUI_SFTP") || in_path("ssh"));
}

bool sftp_name_ok(const char *path)
{
    if (!path)
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        if (*p < 0x20 || *p == '"' || *p == '\\' || *p == 0x7f)
            return false;
    return true;
}

int sftp_askpass_main(void)
{
    const char *p = getenv("MELLAGUI_SFTP_PASS");
    printf("%s\n", p ? p : "");
    return 0;
}

/* ---- parsing -------------------------------------------------------------------------------------------------------------- */

void sftp_explain(const char *o, char *out, size_t n, bool *need_login)
{
    if (need_login)
        *need_login = false;
    static const struct { const char *needle, *text; } t[] = {
        { "REMOTE HOST IDENTIFICATION HAS CHANGED", "The server's key has changed: check ~/.ssh/known_hosts (somebody may be listening in)" },
        { "Host key verification failed", "The server's key is not trusted: check ~/.ssh/known_hosts" },
        { "Could not resolve hostname", "Cannot find that server name" },
        { "Connection refused", "The server refuses the connection (is SSH running, is the port right?)" },
        { "Connection timed out", "The server does not answer" },
        { "Operation timed out", "The server does not answer" },
        { "No route to host", "No route to the server" },
        { "Network is unreachable", "The network cannot reach the server" },
        { "Connection closed", "The server closed the connection" },
        { "Connection reset", "The connection was reset" },
        { "No such file or directory", "Not found" },
        { "not found", "Not found" },
        { "Permission denied", "Access denied" },
        { "Failure", "The server could not do that" },
    };
    if (strstr(o, "Permission denied (")) {                             /* "Permission denied (publickey,password)." is the login, not a file */
        if (need_login)
            *need_login = true;
        snprintf(out, n, "Login refused: wrong user, password or key");
        return;
    }
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strstr(o, t[i].needle)) {
            snprintf(out, n, "%s", t[i].text);
            return;
        }
    while (*o == '\n' || *o == ' ')
        o++;
    const char *l = o;                                                  /* the first line that is no echo of our command */
    while (*l && !strncmp(l, "sftp> ", 6))
        l += strcspn(l, "\n") + (l[strcspn(l, "\n")] ? 1 : 0);
    size_t len = strcspn(l, "\n");
    snprintf(out, n, "%.*s", (int)(len < 200 ? len : 200), *l ? l : "sftp failed");
}

bool sftp_parse_ls_line(const char *line, struct smb_item *it)
{
    /* "drwxr-xr-x    2 root     root         4096 Nov 10 12:00 name with blanks": ten characters of mode, then eight more blank separated fields */
    if (strlen(line) < 20 || !strchr("-dlcbps", line[0]) || (line[10] != ' ' && line[10] != '.' && line[10] != '+' && line[10] != '@'))
        return false;
    for (int i = 1; i < 10; i++)
        if (!strchr("rwxsStT-", line[i]))
            return false;
    const char *p = line + 10;
    while (*p && *p != ' ')                                             /* "." or "+" after the mode */
        p++;
    char f[8][64];
    for (int i = 0; i < 7; i++) {
        while (*p == ' ')
            p++;
        if (!*p)
            return false;
        size_t l = strcspn(p, " ");
        if (i == 6)
            break;                                                      /* the 7th field is the time / year, then the name follows */
        snprintf(f[i], sizeof(f[i]), "%.*s", (int)(l < 63 ? l : 63), p);
        p += l;
    }
    /* f: 0 links, 1 user, 2 group, 3 size, 4 month, 5 day, 6 (time or year is p) */
    size_t tl = strcspn(p, " ");
    char tm[16];
    snprintf(tm, sizeof(tm), "%.*s", (int)(tl < 15 ? tl : 15), p);
    p += tl;
    while (*p == ' ')
        p++;
    if (!*p)
        return false;
    for (const char *q = f[3]; *q; q++)
        if (*q < '0' || *q > '9')
            return false;
    static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int m = -1;
    for (int i = 0; i < 12; i++)
        if (!strcmp(f[4], mon[i]))
            m = i;
    if (m < 0)
        return false;
    struct tm t;
    memset(&t, 0, sizeof(t));
    time_t now = time(NULL);
    struct tm nt;
    localtime_r(&now, &nt);
    t.tm_mon = m;
    t.tm_mday = atoi(f[5]);
    t.tm_isdst = -1;
    if (strchr(tm, ':')) {
        t.tm_hour = atoi(tm);
        t.tm_min = atoi(strchr(tm, ':') + 1);
        t.tm_year = nt.tm_year;
        if (mktime(&t) > now + 86400) {                                 /* "Dec 31 23:00" in January is last year's */
            t.tm_year--;
        }
    } else {
        t.tm_year = atoi(tm) - 1900;
    }
    char name[256];
    snprintf(name, sizeof(name), "%s", p);
    name[strcspn(name, "\r\n")] = 0;
    char *arrow = strstr(name, " -> ");                                 /* a link: its own name */
    if (arrow)
        *arrow = 0;
    char *slash = strrchr(name, '/');                                   /* (some servers put the whole path in the long name) */
    if (slash)
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    snprintf(it->name, sizeof(it->name), "%s", name);
    it->dir = line[0] == 'd' || line[0] == 'l';
    it->size = atoll(f[3]);
    it->mtime = mktime(&t);
    return true;
}

/* ---- running sftp ------------------------------------------------------------------------------------------------------------- */

static int ask_exe(char *out, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0)
        return -1;
    out[k] = 0;
    return 0;
}

/* Runs the batch `commands` (one per line) against the server; the text of the answer goes to *out. */
static int batch(const struct smb_conn *c, const char *commands, int timeout_ms, char **out, char *err, size_t errsz, smb_poll_fn poll_fn, void *pd)
{
    char *argv[24];
    int n = 1;
    char port[16], target[200];
    argv[n++] = "-b";
    argv[n++] = "-";
    argv[n++] = "-o";
    argv[n++] = "StrictHostKeyChecking=accept-new";
    argv[n++] = "-o";
    argv[n++] = "ConnectTimeout=12";
    argv[n++] = "-o";
    argv[n++] = "ServerAliveInterval=15";
    if (c->pass[0]) {
        argv[n++] = "-o";
        argv[n++] = "NumberOfPasswordPrompts=1";
    } else {
        argv[n++] = "-o";
        argv[n++] = "BatchMode=yes";
    }
    if (c->port > 0 && c->port != 22) {
        snprintf(port, sizeof(port), "%d", c->port);
        argv[n++] = "-P";
        argv[n++] = port;
    }
    if (c->user[0])
        snprintf(target, sizeof(target), "%s@%s", c->user, c->host);
    else
        snprintf(target, sizeof(target), "%s", c->host);
    argv[n++] = target;
    argv[n] = NULL;
    const char *env[5];
    char self[PATH_MAX], pass[160], askpass[PATH_MAX + 16];
    int ne = 0;
    if (c->pass[0] && ask_exe(self, sizeof(self)) == 0) {
        snprintf(askpass, sizeof(askpass), "SSH_ASKPASS=%.*s", PATH_MAX - 1, self);
        snprintf(pass, sizeof(pass), "MELLAGUI_SFTP_PASS=%s", c->pass);
        env[ne++] = askpass;
        env[ne++] = "SSH_ASKPASS_REQUIRE=force";
        env[ne++] = "MELLAGUI_ASKPASS=1";
        env[ne++] = pass;
    }
    env[ne] = NULL;
    return smb_run_program(program(), argv, commands, env, timeout_ms, out, err, errsz, poll_fn, pd, "sftp is not installed (package openssh-client)");
}

static int judge(int rc, const char *out, char *err, size_t errsz, bool *need_login)
{
    if (need_login)
        *need_login = false;
    if (rc < 0)
        return -1;                                                      /* err is set already */
    if (rc == 0)
        return 0;
    sftp_explain(out ? out : "", err, errsz, need_login);
    return -1;
}

static bool quote(char *dst, size_t n, const char *path)
{
    int w = snprintf(dst, n, "\"%s\"", path);
    return w > 0 && (size_t)w < n && sftp_name_ok(path);
}

static bool bad_name(char *err, size_t errsz)
{
    snprintf(err, errsz, "A name with a quote or a back slash cannot be used on an SSH server");
    return false;
}

int sftp_pwd(const struct smb_conn *c, char *out, size_t n, char *err, size_t errsz, bool *need_login, smb_poll_fn poll, void *pd)
{
    char *o;
    int rc = batch(c, "pwd\n", TIMEOUT_MS, &o, err, errsz, poll, pd);
    if (judge(rc, o, err, errsz, need_login) < 0) {
        free(o);
        return -1;
    }
    const char *k = strstr(o, "Remote working directory: ");
    if (!k) {
        free(o);
        snprintf(err, errsz, "The server did not say where its home folder is");
        return -1;
    }
    k += 26;
    snprintf(out, n, "%.*s", (int)strcspn(k, "\r\n"), k);
    free(o);
    return 0;
}

int sftp_ls(const struct smb_conn *c, const char *dir, struct smb_item **out, int *n, char *err, size_t errsz, bool *need_login, smb_poll_fn poll, void *pd)
{
    char q[PATH_MAX + 4], cmd[PATH_MAX + 32];
    if (!quote(q, sizeof(q), dir && *dir ? dir : "/")) {
        bad_name(err, errsz);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "ls -la %s\n", q);
    char *o;
    int rc = batch(c, cmd, TIMEOUT_MS, &o, err, errsz, poll, pd);
    if (judge(rc, o, err, errsz, need_login) < 0) {
        free(o);
        return -1;
    }
    int cap = 64, cnt = 0;
    struct smb_item *list = malloc((size_t)cap * sizeof(*list));
    for (char *s = o; list && *s;) {
        size_t l = strcspn(s, "\n");
        char line[600];
        snprintf(line, sizeof(line), "%.*s", (int)(l < sizeof(line) - 1 ? l : sizeof(line) - 1), s);
        struct smb_item it;
        if (sftp_parse_ls_line(line, &it)) {
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

static int simple(const struct smb_conn *c, const char *cmd, int timeout, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char *o;
    int rc = batch(c, cmd, timeout, &o, err, errsz, poll, pd);
    int j = judge(rc, o, err, errsz, NULL);
    free(o);
    return j;
}

int sftp_get(const struct smb_conn *c, const char *remote, const char *local, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char a[PATH_MAX + 4], b[PATH_MAX + 4], cmd[2 * PATH_MAX + 32];
    if (!quote(a, sizeof(a), remote) || !quote(b, sizeof(b), local)) {
        bad_name(err, errsz);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "get %s%s %s\n", dir ? "-r " : "", a, b);
    return simple(c, cmd, 0, err, errsz, poll, pd);
}

int sftp_put(const struct smb_conn *c, const char *local, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char a[PATH_MAX + 4], b[PATH_MAX + 4], cmd[2 * PATH_MAX + 32];
    if (!quote(a, sizeof(a), local) || !quote(b, sizeof(b), remote)) {
        bad_name(err, errsz);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "put %s%s %s\n", dir ? "-r " : "", a, b);
    return simple(c, cmd, 0, err, errsz, poll, pd);
}

int sftp_mkdir(const struct smb_conn *c, const char *remote, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char a[PATH_MAX + 4], cmd[PATH_MAX + 32];
    if (!quote(a, sizeof(a), remote)) {
        bad_name(err, errsz);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "mkdir %s\n", a);
    return simple(c, cmd, TIMEOUT_MS, err, errsz, poll, pd);
}

int sftp_rename(const struct smb_conn *c, const char *from, const char *to, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    char a[PATH_MAX + 4], b[PATH_MAX + 4], cmd[2 * PATH_MAX + 32];
    if (!quote(a, sizeof(a), from) || !quote(b, sizeof(b), to)) {
        bad_name(err, errsz);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "rename %s %s\n", a, b);
    return simple(c, cmd, TIMEOUT_MS, err, errsz, poll, pd);
}

/* Collects the commands that delete `path` and everything below it (children first) into *cmds. */
static int del_tree(const struct smb_conn *c, const char *path, bool dir, int depth, char **cmds, size_t *len, size_t *cap, char *err, size_t errsz,
                    smb_poll_fn poll, void *pd)
{
    char q[PATH_MAX + 4];
    if (depth > 64 || !quote(q, sizeof(q), path)) {
        snprintf(err, errsz, depth > 64 ? "The folder is nested too deeply" : "A name with a quote or a back slash cannot be used on an SSH server");
        return -1;
    }
    if (dir) {
        struct smb_item *it = NULL;
        int n = 0;
        if (sftp_ls(c, path, &it, &n, err, errsz, NULL, poll, pd) < 0)
            return -1;
        for (int i = 0; i < n; i++) {
            char sub[PATH_MAX];
            snprintf(sub, sizeof(sub), "%s%s%s", path, strcmp(path, "/") ? "/" : "", it[i].name);
            if (it[i].dir && strcmp(sub, path) != 0) {                  /* (a link counts as a folder in the listing: tried as one, then as a file) */
                struct smb_item *probe = NULL;
                int pn = 0;
                char perr[100];
                if (sftp_ls(c, sub, &probe, &pn, perr, sizeof(perr), NULL, poll, pd) == 0) {
                    free(probe);
                    if (del_tree(c, sub, true, depth + 1, cmds, len, cap, err, errsz, poll, pd) < 0) {
                        free(it);
                        return -1;
                    }
                    continue;
                }
            }
            if (del_tree(c, sub, false, depth + 1, cmds, len, cap, err, errsz, poll, pd) < 0) {
                free(it);
                return -1;
            }
        }
        free(it);
    }
    size_t need = *len + strlen(q) + 16;
    if (need > *cap) {
        *cap = need * 2;
        char *nb = realloc(*cmds, *cap);
        if (!nb)
            return -1;
        *cmds = nb;
    }
    *len += (size_t)snprintf(*cmds + *len, *cap - *len, "%s %s\n", dir ? "rmdir" : "rm", q);
    return 0;
}

int sftp_del(const struct smb_conn *c, const char *remote, bool dir, char *err, size_t errsz, smb_poll_fn poll, void *pd)
{
    size_t len = 0, cap = 1024;
    char *cmds = malloc(cap);
    if (!cmds)
        return -1;
    cmds[0] = 0;
    if (del_tree(c, remote, dir, 0, &cmds, &len, &cap, err, errsz, poll, pd) < 0) {
        free(cmds);
        return -1;
    }
    int rc = simple(c, cmds, TIMEOUT_MS * 6, err, errsz, poll, pd);
    free(cmds);
    return rc;
}

/* ---- selftest -------------------------------------------------------------------------------------------------------------- */

int sftp_selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "sftp selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    struct smb_item it;
    CHECK(sftp_parse_ls_line("drwxr-xr-x    2 root     root         4096 Nov 10 12:00 etc", &it) && it.dir && !strcmp(it.name, "etc") && it.size == 4096);
    CHECK(sftp_parse_ls_line("-rw-r--r--    1 mella    mella          12 Jan  4  2024 my file  with  blanks.txt", &it) && !it.dir &&
          !strcmp(it.name, "my file  with  blanks.txt") && it.size == 12);
    CHECK(sftp_parse_ls_line("lrwxrwxrwx    1 root     root            7 Mar  1 09:15 lib -> usr/lib", &it) && it.dir && !strcmp(it.name, "lib"));
    CHECK(sftp_parse_ls_line("-rwxr-xr-x.   1 root     root      1048576 Dec 31 23:59 big", &it) && !it.dir && it.size == 1048576);
    CHECK(sftp_parse_ls_line("-rw-rw-r--    ? mella    mella           3 Nov 10 16:31 /tmp/some dir/with space.txt", &it) && !it.dir &&
          !strcmp(it.name, "with space.txt") && it.size == 3);
    CHECK(!sftp_parse_ls_line("drwxrwxr-x    ? mella    mella        4096 Nov 10 16:31 /tmp/some dir/.", &it));
    CHECK(!sftp_parse_ls_line("drwxrwxr-x    ? mella    mella        4096 Nov 10 16:31 /tmp/some dir/..", &it));
    CHECK(!sftp_parse_ls_line("drwxr-xr-x    2 root     root         4096 Nov 10 12:00 .", &it));
    CHECK(!sftp_parse_ls_line("drwxr-xr-x    2 root     root         4096 Nov 10 12:00 ..", &it));
    CHECK(!sftp_parse_ls_line("sftp> ls -la \"/\"", &it));
    CHECK(!sftp_parse_ls_line("Connected to host.", &it));
    CHECK(!sftp_parse_ls_line("", &it));
    char why[200];
    bool nl;
    sftp_explain("user@host: Permission denied (publickey,password).", why, sizeof(why), &nl);
    CHECK(nl && strstr(why, "Login refused"));
    sftp_explain("remote open(\"/root/x\"): Permission denied", why, sizeof(why), &nl);
    CHECK(!nl && !strcmp(why, "Access denied"));
    sftp_explain("ssh: connect to host nas port 22: Connection refused", why, sizeof(why), &nl);
    CHECK(strstr(why, "refuses"));
    sftp_explain("ssh: Could not resolve hostname nope: Name or service not known", why, sizeof(why), &nl);
    CHECK(strstr(why, "Cannot find"));
    sftp_explain("@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@", why, sizeof(why), &nl);
    CHECK(strstr(why, "key has changed"));
    CHECK(sftp_name_ok("/a b/c.txt") && !sftp_name_ok("/a\"b") && !sftp_name_ok("/a\\b") && !sftp_name_ok("/a\nb"));
    return bad ? 1 : 0;
}

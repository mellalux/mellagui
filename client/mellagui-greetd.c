/* mellagui-greetd - the login daemon (docs/GREETER.md). It runs as root (a systemd service on a text console), starts the login screen as an
 * unprivileged user, checks the password through PAM and starts the chosen session as the user who logged in.
 *
 *   greetd (root) --spawns--> worker: PAM session of the greeter user --> `mellagui --kiosk --exec mellagui-greeter`   (the login screen)
 *        |  unix socket $GREETD_SOCK (a line protocol, below)                                  ^
 *        +--- login USER --forks--> worker: PAM conversation with the greeter, then the session as the user
 *
 * Why a worker for each login: pam_open_session / pam_close_session must be called by the process that stays alive as long as the session
 * does, and the PAM conversation is blocking. The daemon itself never holds a PAM handle and never sees a password.
 *
 * The protocol (one line each way, UTF-8, at most 1000 bytes, no line break inside):
 *   greeter -> daemon   users | sessions | last | login USER | power suspend|reboot|poweroff | quit
 *   daemon -> greeter   users:    "user NAME<TAB>DISPLAY<TAB>UID" ... "end"       sessions: "session ID<TAB>NAME<TAB>COMMAND" ... "end"
 *                       last:     "last USER<TAB>SESSION"  or "last -"            power:    "ok" or "err TEXT"
 *   after `login USER` the worker talks:
 *                       "prompt secret|visible TEXT"  (greeter answers "answer TEXT")   "info TEXT"   "error TEXT"
 *                       "ok"  authenticated  (greeter says "start SESSIONID" or "cancel")     "fail TEXT"  (back to the start)
 *                       "started"  the session is going
 *
 * Test mode: MELLAGUI_GREETD_FAKE=FILE, lines "user password [Display name]" (password * = none): no PAM, no setuid, the session runs as the
 * daemon's own user. The tests use it (tests/test_mellagui.py TestGreeter).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/vt.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN 1000
#define MAX_USERS 64
#define MAX_SESSIONS 32
#define EXIT_AUTH_ENDED 10                       /* a login worker that ended without a session: the greeter may try again */

/* ---- options -------------------------------------------------------------------------------------------------------------------- */

static struct {
    const char *greeter_cmd;                     /* "none" = do not start one (the tests start their own) */
    const char *session_cmd;                     /* the built-in session "mellagui" */
    const char *greeter_user;
    const char *socket;
    const char *state_dir;
    const char *pam_service;                     /* the service for a login */
    const char *greeter_pam;                     /* the service of the greeter's own session (pam_permit + pam_systemd) */
    const char *fake;                            /* MELLAGUI_GREETD_FAKE */
    const char *sessions_dir;
    int vt;
    bool once;
    bool fake_mode;
} opt = {
    .greeter_cmd = "mellagui --backend drm --kiosk --config none --exec mellagui-greeter",
    .session_cmd = "mellagui",
    .greeter_user = "mellagui-greeter",
    .socket = "/run/mellagui-greetd.sock",
    .state_dir = "/var/cache/mellagui-greetd",
    .pam_service = "mellagui",
    .greeter_pam = "mellagui-greeter",
    .vt = 1,
};

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mellagui-greetd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- lines -------------------------------------------------------------------------------------------------------------------- */

struct conn {
    int fd;
    char buf[LINE_MAX_LEN + 2];
    size_t n;
};

static bool send_line(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool send_line(int fd, const char *fmt, ...)
{
    char line[LINE_MAX_LEN + 8];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, LINE_MAX_LEN, fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if (n > LINE_MAX_LEN - 1)
        n = LINE_MAX_LEN - 1;
    for (int i = 0; i < n; i++)
        if (line[i] == '\n' || line[i] == '\r')
            line[i] = ' ';
    line[n++] = '\n';
    for (int off = 0; off < n;) {
        ssize_t w = send(fd, line + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += (int)w;
    }
    return true;
}

/* One line from the connection into out (without the line end). 1 = got one, 0 = nothing yet / timeout, -1 = closed, error or a line that is too long. */
static int read_line(struct conn *c, char *out, size_t cap, int timeout_ms)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->n);
        if (nl) {
            size_t len = (size_t)(nl - c->buf);
            size_t cp = len < cap - 1 ? len : cap - 1;
            memcpy(out, c->buf, cp);
            out[cp] = 0;
            if (cp && out[cp - 1] == '\r')
                out[cp - 1] = 0;
            memmove(c->buf, nl + 1, c->n - len - 1);
            c->n -= len + 1;
            return 1;
        }
        if (c->n >= LINE_MAX_LEN)
            return -1;
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r < 0 ? -1 : 0;
        ssize_t k = recv(c->fd, c->buf + c->n, LINE_MAX_LEN + 1 - c->n, 0);
        if (k <= 0)
            return -1;
        c->n += (size_t)k;
    }
}

/* ---- users and sessions ---------------------------------------------------------------------------------------------------------- */

struct user {
    char name[64];
    char display[64];
    uid_t uid;
};

struct sess {
    char id[64];
    char name[64];
    char cmd[256];
};

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 60)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-' || s[i] == '.') || (i == 0 && s[i] == '-'))
            return false;
    return true;
}

static int fake_users(struct user *u, int max)
{
    FILE *f = fopen(opt.fake, "r");
    if (!f)
        return 0;
    int n = 0;
    char line[300];
    while (n < max && fgets(line, sizeof(line), f)) {
        char name[64], pw[128], disp[64] = "";
        if (sscanf(line, "%63s %127s %63[^\n]", name, pw, disp) < 2 || name[0] == '#')
            continue;
        snprintf(u[n].name, sizeof(u[n].name), "%s", name);
        snprintf(u[n].display, sizeof(u[n].display), "%s", disp[0] ? disp : name);
        u[n].uid = 1000 + (uid_t)n;
        n++;
    }
    fclose(f);
    return n;
}

static bool real_shell_ok(const char *sh)
{
    if (!sh || !*sh)
        return false;
    const char *b = strrchr(sh, '/');
    b = b ? b + 1 : sh;
    return strcmp(b, "nologin") && strcmp(b, "false") && strcmp(b, "sync") && strcmp(b, "halt") && strcmp(b, "shutdown");
}

static int list_users(struct user *u, int max)
{
    if (opt.fake_mode)
        return fake_users(u, max);
    int n = 0;
    setpwent();
    struct passwd *pw;
    while (n < max && (pw = getpwent())) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000 || !real_shell_ok(pw->pw_shell) || !valid_name(pw->pw_name))
            continue;
        snprintf(u[n].name, sizeof(u[n].name), "%s", pw->pw_name);
        char disp[64] = "";
        if (pw->pw_gecos) {
            snprintf(disp, sizeof(disp), "%s", pw->pw_gecos);
            char *comma = strchr(disp, ',');
            if (comma)
                *comma = 0;
        }
        snprintf(u[n].display, sizeof(u[n].display), "%s", disp[0] ? disp : pw->pw_name);
        u[n].uid = pw->pw_uid;
        n++;
    }
    endpwent();
    return n;
}

static void desktop_value(const char *line, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    if (!strncmp(line, key, kl) && line[kl] == '=') {
        snprintf(out, cap, "%s", line + kl + 1);
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = 0;
    }
}

static int list_sessions(struct sess *s, int max)
{
    int n = 0;
    snprintf(s[n].id, sizeof(s[n].id), "mellagui");
    snprintf(s[n].name, sizeof(s[n].name), "MellaGUI");
    snprintf(s[n].cmd, sizeof(s[n].cmd), "%s", opt.session_cmd);
    n++;
    const char *dirs[] = { opt.sessions_dir, "/usr/share/wayland-sessions", "/usr/local/share/wayland-sessions" };
    for (int d = 0; d < 3; d++) {
        if (!dirs[d] || (d > 0 && opt.sessions_dir))
            continue;
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        struct dirent *e;
        while (n < max && (e = readdir(dir))) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || l - 8 >= sizeof(s[0].id))
                continue;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            FILE *f = fopen(path, "r");
            if (!f)
                continue;
            struct sess cur = { { 0 }, { 0 }, { 0 } };
            snprintf(cur.id, sizeof(cur.id), "%.*s", (int)(l - 8), e->d_name);
            char line[400];
            bool hidden = false, in_entry = false;
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '[')
                    in_entry = !strncmp(line, "[Desktop Entry]", 15);
                if (!in_entry)
                    continue;
                char v[300] = "";
                desktop_value(line, "Name", cur.name, sizeof(cur.name));
                desktop_value(line, "Exec", cur.cmd, sizeof(cur.cmd));
                desktop_value(line, "NoDisplay", v, sizeof(v));
                if (!strcmp(v, "true")) hidden = true;
                desktop_value(line, "Hidden", v, sizeof(v));
                if (!strcmp(v, "true")) hidden = true;
            }
            fclose(f);
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (!strcmp(s[i].id, cur.id))
                    dup = true;
            if (!hidden && !dup && cur.cmd[0]) {
                if (!cur.name[0])
                    snprintf(cur.name, sizeof(cur.name), "%s", cur.id);
                s[n++] = cur;
            }
        }
        closedir(dir);
    }
    return n;
}

/* A copy that PAM's own getpwnam calls cannot change under us (the libc keeps one static buffer). */
static struct passwd pw_copy(const struct passwd *src)
{
    struct passwd d = *src;
    d.pw_name = strdup(src->pw_name ? src->pw_name : "");
    d.pw_dir = strdup(src->pw_dir ? src->pw_dir : "/");
    d.pw_shell = strdup(src->pw_shell && *src->pw_shell ? src->pw_shell : "/bin/sh");
    d.pw_passwd = NULL;
    d.pw_gecos = NULL;
    return d;
}

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/last", opt.state_dir);
}

static void last_read(char *user, size_t un, char *sess, size_t sn)
{
    user[0] = sess[0] = 0;
    char path[PATH_MAX];
    state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[200];
    if (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (tab) {
            *tab = 0;
            snprintf(sess, sn, "%.63s", tab + 1);
        }
        snprintf(user, un, "%.63s", line);
    }
    fclose(f);
}

static void last_write(const char *user, const char *sess)
{
    mkdir(opt.state_dir, 0755);
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    state_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%s\t%s\n", user, sess);
    fclose(f);
    rename(tmp, path);
}

/* ---- PAM (libpam.so.0 through dlopen: there are no headers on the dev machine and none are needed) ------------------------------------ */

struct pam_message { int msg_style; const char *msg; };
struct pam_response { char *resp; int resp_retcode; };
struct pam_conv { int (*conv)(int, const struct pam_message **, struct pam_response **, void *); void *appdata_ptr; };
typedef struct pam_handle pam_handle_t;
enum { PAM_SUCCESS = 0, PAM_PROMPT_ECHO_OFF = 1, PAM_PROMPT_ECHO_ON = 2, PAM_ERROR_MSG = 3, PAM_TEXT_INFO = 4, PAM_CONV_ERR = 19 };
enum { PAM_TTY = 3, PAM_ESTABLISH_CRED = 0x2, PAM_DELETE_CRED = 0x4, PAM_SILENT = 0x8000 };

static struct {
    void *lib;
    int (*start)(const char *, const char *, const struct pam_conv *, pam_handle_t **);
    int (*end)(pam_handle_t *, int);
    int (*authenticate)(pam_handle_t *, int);
    int (*acct_mgmt)(pam_handle_t *, int);
    int (*setcred)(pam_handle_t *, int);
    int (*open_session)(pam_handle_t *, int);
    int (*close_session)(pam_handle_t *, int);
    char **(*getenvlist)(pam_handle_t *);
    int (*putenv)(pam_handle_t *, const char *);
    int (*set_item)(pam_handle_t *, int, const void *);
    const char *(*strerror)(pam_handle_t *, int);
} pam;

static bool pam_load(void)
{
    if (pam.lib)
        return true;
    pam.lib = dlopen("libpam.so.0", RTLD_NOW);
    if (!pam.lib) {
        logf_("cannot load libpam.so.0: %s", dlerror());
        return false;
    }
#define SYM(field, name) do { *(void **)&pam.field = dlsym(pam.lib, name); if (!pam.field) { logf_("libpam has no %s", name); pam.lib = NULL; return false; } } while (0)
    SYM(start, "pam_start"); SYM(end, "pam_end"); SYM(authenticate, "pam_authenticate"); SYM(acct_mgmt, "pam_acct_mgmt");
    SYM(setcred, "pam_setcred"); SYM(open_session, "pam_open_session"); SYM(close_session, "pam_close_session");
    SYM(getenvlist, "pam_getenvlist"); SYM(putenv, "pam_putenv"); SYM(set_item, "pam_set_item"); SYM(strerror, "pam_strerror");
#undef SYM
    return true;
}

/* The conversation goes to the greeter on `conv_conn` (a login worker only). */
static struct conn *conv_conn;

static int pam_conversation(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    (void)data;
    if (n <= 0 || n > 16 || !conv_conn)
        return PAM_CONV_ERR;
    struct pam_response *r = calloc((size_t)n, sizeof(*r));
    if (!r)
        return PAM_CONV_ERR;
    for (int i = 0; i < n; i++) {
        const char *text = msg[i]->msg ? msg[i]->msg : "";
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON: {
            char line[LINE_MAX_LEN + 8];
            if (!send_line(conv_conn->fd, "prompt %s %s", msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ? "secret" : "visible", text))
                goto fail;
            if (read_line(conv_conn, line, sizeof(line), 120000) != 1 || strncmp(line, "answer ", 7) != 0) {
                if (!strncmp(line, "answer", 6) && line[6] == 0)      /* an empty answer */
                    r[i].resp = strdup("");
                else
                    goto fail;
            } else {
                r[i].resp = strdup(line + 7);
            }
            memset(line, 0, sizeof(line));
            if (!r[i].resp)
                goto fail;
            break;
        }
        case PAM_ERROR_MSG:
            send_line(conv_conn->fd, "error %s", text);
            break;
        default:
            send_line(conv_conn->fd, "info %s", text);
            break;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
fail:
    for (int i = 0; i < n; i++)
        if (r[i].resp) {
            memset(r[i].resp, 0, strlen(r[i].resp));
            free(r[i].resp);
        }
    free(r);
    return PAM_CONV_ERR;
}

/* ---- a session as a user ------------------------------------------------------------------------------------------------------------- */

/* The environment of a session: PAM's (pam_systemd gives XDG_SESSION_ID and XDG_RUNTIME_DIR) and ours. */
static char **session_env(pam_handle_t *h, const struct passwd *pw, const char *klass, const char *extra)
{
    char **env = calloc(64, sizeof(char *));
    int n = 0;
    char buf[PATH_MAX + 32];
#define ADD(...) do { if (n < 60) { snprintf(buf, sizeof(buf), __VA_ARGS__); env[n++] = strdup(buf); } } while (0)
    ADD("HOME=%s", pw->pw_dir);
    ADD("USER=%s", pw->pw_name);
    ADD("LOGNAME=%s", pw->pw_name);
    ADD("SHELL=%s", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh");
    ADD("PATH=%s", getenv("PATH") && *getenv("PATH") ? getenv("PATH") : "/usr/local/bin:/usr/bin:/bin");
    ADD("XDG_SEAT=seat0");
    ADD("XDG_VTNR=%d", opt.vt);
    ADD("XDG_SESSION_TYPE=wayland");
    ADD("XDG_SESSION_CLASS=%s", klass);
    if (getenv("LANG"))
        ADD("LANG=%s", getenv("LANG"));
    if (extra)
        ADD("%s", extra);
    if (h) {
        char **pe = pam.getenvlist(h);
        for (int i = 0; pe && pe[i] && n < 60; i++)
            env[n++] = pe[i];                                    /* (ownership passes to env; they are never freed: the child execs) */
        free(pe);
    }
    return env;
#undef ADD
}

/* Runs `cmd` as the user in a new process group; the caller waits. In fake mode (or without root) the user stays the one we are. */
static pid_t spawn_as(const struct passwd *pw, char **env, const char *cmd, bool new_session)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    if (new_session)
        setsid();                                            /* (the greeter's command stays in its worker's group: one kill reaches both) */
    if (!opt.fake_mode && geteuid() == 0) {
        if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            logf_("cannot become %s: %s", pw->pw_name, strerror(errno));
            _exit(126);
        }
    }
    if (pw->pw_dir && chdir(pw->pw_dir) < 0)
        if (chdir("/") != 0)
            _exit(126);
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    extern char **environ;
    environ = env;
    if (opt.fake_mode)
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    else
        execl("/bin/sh", "sh", "-l", "-c", cmd, (char *)NULL);                /* (-l: the user's profile, like a text login) */
    _exit(127);
}

static int wait_for(pid_t pid)
{
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void set_tty_item(pam_handle_t *h)
{
    char tty[16];
    snprintf(tty, sizeof(tty), "tty%d", opt.vt);
    pam.set_item(h, PAM_TTY, tty);
}

/* ---- the greeter's own session ---------------------------------------------------------------------------------------------------- */

/* Runs in a forked process: a PAM session for the greeter user (no password) and the greeter's command in it. Exit status = the command's. */
static int greeter_worker(void)
{
    struct passwd *pw = opt.fake_mode ? getpwuid(getuid()) : getpwnam(opt.greeter_user);
    if (!pw) {
        logf_("no user %s: create it (see docs/GREETER.md)", opt.greeter_user);
        return 127;
    }
    struct passwd pwc = pw_copy(pw);
    pam_handle_t *h = NULL;
    if (!opt.fake_mode) {
        if (!pam_load())
            return 127;
        struct pam_conv conv = { pam_conversation, NULL };
        conv_conn = NULL;
        if (pam.start(opt.greeter_pam, pwc.pw_name, &conv, &h) != PAM_SUCCESS)
            return 127;
        set_tty_item(h);
        if (pam.setcred(h, PAM_ESTABLISH_CRED) != PAM_SUCCESS || pam.open_session(h, 0) != PAM_SUCCESS) {
            logf_("PAM refused the greeter's session (service %s)", opt.greeter_pam);
            pam.end(h, 1);
            return 127;
        }
    }
    char sock[PATH_MAX + 16];
    snprintf(sock, sizeof(sock), "GREETD_SOCK=%s", opt.socket);
    char **env = session_env(h, &pwc, "greeter", sock);
    pid_t pid = spawn_as(&pwc, env, opt.greeter_cmd, false);
    int rc = pid > 0 ? wait_for(pid) : 127;
    if (h) {
        pam.close_session(h, 0);
        pam.setcred(h, PAM_DELETE_CRED);
        pam.end(h, PAM_SUCCESS);
    }
    return rc;
}

/* ---- a login: the conversation, then the session ----------------------------------------------------------------------------------- */

static bool fake_check(const char *user, const char *password)
{
    FILE *f = fopen(opt.fake, "r");
    if (!f)
        return false;
    char line[300];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        char name[64], pw[128];
        if (sscanf(line, "%63s %127s", name, pw) == 2 && !strcmp(name, user)) {
            ok = !strcmp(pw, "*") || !strcmp(pw, password);
            break;
        }
    }
    fclose(f);
    return ok;
}

/* Returns the exit status of the worker: 0 after a session that ended, EXIT_AUTH_ENDED when there was none. */
static int login_worker(struct conn *c, int to_daemon, int from_daemon, const char *user)
{
    char line[LINE_MAX_LEN + 8];
    struct passwd *pwp = NULL;
    struct passwd pwc;
    if (opt.fake_mode) {
        pwp = getpwuid(getuid());
    } else if (valid_name(user)) {
        pwp = getpwnam(user);
    }
    if (!pwp || (!opt.fake_mode && (pwp->pw_uid == 0 || !real_shell_ok(pwp->pw_shell)))) {
        send_line(c->fd, "fail Login incorrect");
        return EXIT_AUTH_ENDED;
    }
    pwc = pw_copy(pwp);
    pam_handle_t *h = NULL;
    bool authed = false;
    conv_conn = c;
    if (opt.fake_mode) {
        send_line(c->fd, "prompt secret Password: ");
        if (read_line(c, line, sizeof(line), 120000) == 1 && !strncmp(line, "answer", 6))
            authed = fake_check(user, line[6] == ' ' ? line + 7 : "");
        memset(line, 0, sizeof(line));
    } else {
        if (!pam_load()) {
            send_line(c->fd, "fail The login service is not available");
            return EXIT_AUTH_ENDED;
        }
        struct pam_conv conv = { pam_conversation, NULL };
        if (pam.start(opt.pam_service, pwc.pw_name, &conv, &h) != PAM_SUCCESS) {
            send_line(c->fd, "fail The login service is not available");
            return EXIT_AUTH_ENDED;
        }
        set_tty_item(h);
        int rc = pam.authenticate(h, 0);
        if (rc == PAM_SUCCESS)
            rc = pam.acct_mgmt(h, 0);
        authed = rc == PAM_SUCCESS;
        if (!authed) {
            send_line(c->fd, "fail %s", pam.strerror(h, rc));
            pam.end(h, rc);
            return EXIT_AUTH_ENDED;
        }
    }
    if (!authed) {
        send_line(c->fd, "fail Login incorrect");
        return EXIT_AUTH_ENDED;
    }
    send_line(c->fd, "ok");
    if (read_line(c, line, sizeof(line), 120000) != 1 || strncmp(line, "start ", 6) != 0) {       /* "cancel", a hang-up, anything else */
        if (h)
            pam.end(h, PAM_SUCCESS);
        return EXIT_AUTH_ENDED;
    }
    struct sess sessions[MAX_SESSIONS];
    int ns = list_sessions(sessions, MAX_SESSIONS), pick = -1;
    for (int i = 0; i < ns; i++)
        if (!strcmp(sessions[i].id, line + 6))
            pick = i;
    if (pick < 0) {
        send_line(c->fd, "fail No such session");
        if (h)
            pam.end(h, PAM_SUCCESS);
        return EXIT_AUTH_ENDED;
    }
    /* The greeter must go (it holds the screen) before the session starts: tell the daemon and wait for its word. */
    char go = 0;
    if (write(to_daemon, "S", 1) != 1 || read(from_daemon, &go, 1) != 1 || go != 'G') {
        if (h)
            pam.end(h, PAM_SUCCESS);
        return EXIT_AUTH_ENDED;
    }
    if (h) {
        pam.setcred(h, PAM_ESTABLISH_CRED);
        if (pam.open_session(h, 0) != PAM_SUCCESS) {
            send_line(c->fd, "fail PAM could not open the session");
            pam.end(h, PAM_SUCCESS);
            return EXIT_AUTH_ENDED;
        }
    }
    last_write(user, sessions[pick].id);
    char **env = session_env(h, &pwc, "user", NULL);
    pid_t pid = spawn_as(&pwc, env, sessions[pick].cmd, true);
    send_line(c->fd, pid > 0 ? "started" : "fail Cannot start the session");
    close(c->fd);
    int rc = pid > 0 ? wait_for(pid) : 127;
    logf_("the session of %s ended with status %d", user, rc);
    if (h) {
        pam.close_session(h, 0);
        pam.setcred(h, PAM_DELETE_CRED);
        pam.end(h, PAM_SUCCESS);
    }
    return 0;
}

/* ---- the daemon ----------------------------------------------------------------------------------------------------------------- */

static int sigpipe_fd[2] = { -1, -1 };
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    if (sig == SIGTERM || sig == SIGINT)
        g_stop = 1;
    int e = errno;
    (void)!write(sigpipe_fd[1], "x", 1);
    errno = e;
}

static int listen_socket(void)
{
    unlink(opt.socket);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(opt.socket) >= sizeof(sa.sun_path)) {
        logf_("bad socket path %s", opt.socket);
        return -1;
    }
    strcpy(sa.sun_path, opt.socket);
    mode_t old = umask(0177);
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (rc < 0 || listen(fd, 4) < 0) {
        logf_("cannot listen on %s: %s", opt.socket, strerror(errno));
        return -1;
    }
    if (!opt.fake_mode && geteuid() == 0) {
        struct passwd *pw = getpwnam(opt.greeter_user);
        if (pw && chown(opt.socket, pw->pw_uid, pw->pw_gid) < 0)
            logf_("cannot give %s to %s: %s", opt.socket, opt.greeter_user, strerror(errno));
    }
    return fd;
}

static void activate_vt(void)
{
    if (opt.fake_mode || opt.vt <= 0)
        return;
    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return;
    if (ioctl(fd, VT_ACTIVATE, opt.vt) == 0)
        ioctl(fd, VT_WAITACTIVE, opt.vt);
    close(fd);
}

static void run_power(struct conn *c, const char *what)
{
    if (strcmp(what, "suspend") && strcmp(what, "reboot") && strcmp(what, "poweroff")) {
        send_line(c->fd, "err unknown action");
        return;
    }
    const char *prog = getenv("MELLAGUI_GREETD_POWER");                       /* (the tests) */
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        if (prog && *prog)
            execl(prog, prog, what, (char *)NULL);
        else
            execl("/usr/bin/systemctl", "systemctl", what, (char *)NULL);
        _exit(127);
    }
    send_line(c->fd, pid > 0 ? "ok" : "err cannot run it");
}

static void usage(void)
{
    fputs("usage: mellagui-greetd [--greeter-cmd CMD|none] [--session-cmd CMD] [--greeter-user USER] [--socket PATH] [--state-dir DIR]\n"
          "                      [--pam-service NAME] [--greeter-pam NAME] [--vt N] [--sessions-dir DIR] [--once]\n"
          "  test mode: MELLAGUI_GREETD_FAKE=FILE (lines: user password [Display name]), no PAM, no root\n", stderr);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--once")) opt.once = true;
        else if (!strcmp(a, "--greeter-cmd") && more) opt.greeter_cmd = argv[++i];
        else if (!strcmp(a, "--session-cmd") && more) opt.session_cmd = argv[++i];
        else if (!strcmp(a, "--greeter-user") && more) opt.greeter_user = argv[++i];
        else if (!strcmp(a, "--socket") && more) opt.socket = argv[++i];
        else if (!strcmp(a, "--state-dir") && more) opt.state_dir = argv[++i];
        else if (!strcmp(a, "--pam-service") && more) opt.pam_service = argv[++i];
        else if (!strcmp(a, "--greeter-pam") && more) opt.greeter_pam = argv[++i];
        else if (!strcmp(a, "--sessions-dir") && more) opt.sessions_dir = argv[++i];
        else if (!strcmp(a, "--vt") && more) opt.vt = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    opt.fake = getenv("MELLAGUI_GREETD_FAKE");
    opt.fake_mode = opt.fake && *opt.fake;
    if (!opt.fake_mode && geteuid() != 0) {
        logf_("must run as root (or set MELLAGUI_GREETD_FAKE for a test without PAM)");
        return 1;
    }
    if (pipe2(sigpipe_fd, O_CLOEXEC | O_NONBLOCK) < 0)
        return 1;
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int lfd = listen_socket();
    if (lfd < 0)
        return 1;
    activate_vt();

    struct conn conn = { .fd = -1 };
    pid_t gpid = 0, wpid = 0;
    int ctl[2] = { -1, -1 }, go[2] = { -1, -1 };                /* login worker -> daemon, daemon -> login worker */
    bool session_active = false;
    time_t crash_at[6] = { 0 };
    int crashes = 0;
    bool want_greeter = strcmp(opt.greeter_cmd, "none") != 0;
    int exit_code = 0;

    for (;;) {
        if (g_stop)
            break;
        if (want_greeter && !gpid && !session_active) {
            time_t now = time(NULL);
            if (crashes >= 5 && now - crash_at[0] < 20) {         /* a greeter that dies at once, again and again: wait a bit */
                struct timespec ts = { 5, 0 };
                nanosleep(&ts, NULL);
                crashes = 0;
            }
            gpid = fork();
            if (gpid == 0) {
                close(lfd);
                setpgid(0, 0);
                _exit(greeter_worker());
            }
            if (gpid < 0)
                gpid = 0;
            else
                logf_("the login screen runs (pid %d)", (int)gpid);
        }
        struct pollfd p[4];
        int np = 0;
        p[np++] = (struct pollfd){ .fd = sigpipe_fd[0], .events = POLLIN };
        p[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        int ci = -1, wi = -1;
        if (conn.fd >= 0 && !wpid) { ci = np; p[np++] = (struct pollfd){ .fd = conn.fd, .events = POLLIN }; }
        if (wpid && ctl[0] >= 0) { wi = np; p[np++] = (struct pollfd){ .fd = ctl[0], .events = POLLIN }; }
        int r = poll(p, (nfds_t)np, 1000);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0 && (p[0].revents & POLLIN)) {
            char tmp[64];
            while (read(sigpipe_fd[0], tmp, sizeof(tmp)) > 0)
                ;
        }
        /* children */
        int st;
        pid_t dead;
        while ((dead = waitpid(-1, &st, WNOHANG)) > 0) {
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            if (dead == gpid) {
                gpid = 0;
                if (!session_active) {
                    logf_("the login screen ended with status %d", code);
                    if (code != 0) {
                        crash_at[crashes % 6] = time(NULL);
                        if (crashes == 0) crash_at[0] = crash_at[crashes % 6];
                        crashes++;
                    }
                    if (conn.fd >= 0 && !wpid) { close(conn.fd); conn.fd = -1; conn.n = 0; }
                }
            } else if (dead == wpid) {
                wpid = 0;
                if (ctl[0] >= 0) { close(ctl[0]); ctl[0] = -1; }
                if (go[1] >= 0) { close(go[1]); go[1] = -1; }
                if (code == EXIT_AUTH_ENDED && !session_active) {
                    /* no session: the greeter's connection is ours again */
                } else {
                    session_active = false;                         /* the session ended: the greeter comes back */
                    if (conn.fd >= 0) { close(conn.fd); conn.fd = -1; conn.n = 0; }
                    if (opt.once) { exit_code = 0; g_stop = 1; }
                }
            }
        }
        if (g_stop)
            break;
        if (r <= 0)
            continue;
        if (p[1].revents & POLLIN) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) {
                struct ucred cred;
                socklen_t cl = sizeof(cred);
                bool ok = true;
                if (!opt.fake_mode && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0) {
                    struct passwd *gp = getpwnam(opt.greeter_user);
                    ok = gp && cred.uid == gp->pw_uid;
                }
                if (!ok || wpid) {
                    close(fd);
                } else {
                    if (conn.fd >= 0)
                        close(conn.fd);
                    conn.fd = fd;
                    conn.n = 0;
                }
            }
        }
        if (wi >= 0 && (p[wi].revents & POLLIN)) {                   /* the login worker has authenticated and wants the screen */
            char b;
            if (read(ctl[0], &b, 1) == 1 && b == 'S') {
                if (gpid > 0) {
                    kill(-gpid, SIGTERM);
                    for (int i = 0; i < 50 && gpid > 0; i++) {       /* up to 5 s, then no more manners */
                        struct timespec ts = { 0, 100 * 1000 * 1000 };
                        nanosleep(&ts, NULL);
                        pid_t g = waitpid(gpid, &st, WNOHANG);
                        if (g == gpid)
                            gpid = 0;
                    }
                    if (gpid > 0) {
                        kill(-gpid, SIGKILL);
                        waitpid(gpid, &st, 0);
                        gpid = 0;
                    }
                }
                session_active = true;
                (void)!write(go[1], "G", 1);
            }
        }
        if (ci >= 0 && (p[ci].revents & (POLLIN | POLLHUP))) {
            char line[LINE_MAX_LEN + 8];
            for (;;) {                                               /* (a greeter may send several requests at once) */
            int rl = read_line(&conn, line, sizeof(line), 0);
            if (rl < 0) {
                close(conn.fd);
                conn.fd = -1;
                conn.n = 0;
                break;
            }
            if (rl == 0)
                break;
            {
                if (!strcmp(line, "users")) {
                    struct user u[MAX_USERS];
                    int n = list_users(u, MAX_USERS);
                    for (int i = 0; i < n; i++)
                        send_line(conn.fd, "user %s\t%s\t%u", u[i].name, u[i].display, (unsigned)u[i].uid);
                    send_line(conn.fd, "end");
                } else if (!strcmp(line, "sessions")) {
                    struct sess s[MAX_SESSIONS];
                    int n = list_sessions(s, MAX_SESSIONS);
                    for (int i = 0; i < n; i++)
                        send_line(conn.fd, "session %s\t%s\t%s", s[i].id, s[i].name, s[i].cmd);
                    send_line(conn.fd, "end");
                } else if (!strcmp(line, "last")) {
                    char lu[64], ls[64];
                    last_read(lu, sizeof(lu), ls, sizeof(ls));
                    if (lu[0])
                        send_line(conn.fd, "last %s\t%s", lu, ls);
                    else
                        send_line(conn.fd, "last -");
                } else if (!strncmp(line, "login ", 6)) {
                    if (pipe2(ctl, O_CLOEXEC) < 0 || pipe2(go, O_CLOEXEC) < 0) {
                        send_line(conn.fd, "fail Out of resources");
                    } else {
                        wpid = fork();
                        if (wpid == 0) {
                            close(lfd);
                            close(ctl[0]);
                            close(go[1]);
                            _exit(login_worker(&conn, ctl[1], go[0], line + 6));
                        }
                        close(ctl[1]); ctl[1] = -1;
                        close(go[0]); go[0] = -1;
                        conn.n = 0;                                 /* (what is left in the buffer belongs to the worker now) */
                        if (wpid < 0) {
                            wpid = 0;
                            send_line(conn.fd, "fail Out of resources");
                        }
                    }
                } else if (!strncmp(line, "power ", 6)) {
                    run_power(&conn, line + 6);
                } else if (!strcmp(line, "quit")) {
                    close(conn.fd);
                    conn.fd = -1;
                    conn.n = 0;
                } else {
                    send_line(conn.fd, "err unknown request");
                }
            }
            if (wpid || conn.fd < 0)
                break;
            }
        }
    }
    if (gpid > 0)
        kill(-gpid, SIGTERM);
    if (wpid > 0)
        kill(wpid, SIGTERM);
    close(lfd);
    unlink(opt.socket);
    return exit_code;
}

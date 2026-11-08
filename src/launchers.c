/*
 * Launchers: the app menu and the Super+... program commands. See launchers.h for the contract.
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "launchers.h"
#include "log.h"
#include "server.h"

#define NAME_MAX_LEN 64
#define CMD_MAX 1024

/* The default menu is a mellagui.conf text like any other. VS Code: ~/VSCode/code (tarball), else the Electron binary
 * inside the snap; /snap/bin/code is deliberately not used: its launcher appends --ozone-platform=x11 after our flags
 * and there is no X11. Firefox: native Wayland, WebRender on the GPU (MellaGUI offers zwp_linux_dmabuf_v1; without it
 * Firefox falls back by itself); the snap is confined: it may only connect to sockets named wayland-N, which is what
 * MellaGUI picks by default. */
#define CODE_FLAGS "--ozone-platform=wayland --disable-gpu --no-sandbox --password-store=basic"
#define FF_ENV "env MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland MOZ_WEBRENDER=1"
#define DEFAULT_MENU \
    "[menu]\n" \
    "Terminal   = @terminal\n" \
    "Calculator = mellagui-calc\n" \
    "Images     = @view\n" \
    "Editor     = mellagui-edit\n" \
    "Files      = mellagui-files\n" \
    "Code       = ~/VSCode/code " CODE_FLAGS " || /snap/code/current/usr/share/code/code " CODE_FLAGS "\n" \
    "Firefox    = " FF_ENV " /snap/bin/firefox || " FF_ENV " /usr/bin/firefox\n" \
    "Settings   = @settings\n"

static const char default_conf[] =
    "# mellagui.conf - what MellaGUI does when this file is missing. Copy what you want to change.\n"
    "[commands]\n"
    "terminal = mellagui-term\n"
    "run      = mellagui-run\n"
    "settings = mellagui-settings\n"
    "view     = mellagui-view\n"
    "\n"
    DEFAULT_MENU;

const char *lm_launch_default_conf(void) { return default_conf; }

/* ---- collected input ---------------------------------------------------------------------------------------------- */

enum { C_TERMINAL, C_RUN, C_SETTINGS, C_VIEW, C_COUNT };
static const char *const cmd_names[C_COUNT] = { "terminal", "run", "settings", "view" };

static char cmd_raw[C_COUNT][CMD_MAX] = { "mellagui-term", "mellagui-run", "mellagui-settings", "mellagui-view" };
static char cmd_res[C_COUNT][CMD_MAX];
static const char *const cmd_defaults[C_COUNT] = { "mellagui-term", "mellagui-run", "mellagui-settings", "mellagui-view" };
static bool cmd_cli[C_COUNT];                   /* set on the command line: the file may not change it, also on a reload */

/* Menu entries come in two layers, so a reload can throw the file's away and keep the command line's:
 * conf_* = [menu] of mellagui.conf, cli_* = --app. menu_* is the merged, resolved list that config.apps points into. */
struct menu_layer { char name[LM_MAX_APPS][NAME_MAX_LEN]; char cmd[LM_MAX_APPS][CMD_MAX]; int n; };
static struct menu_layer conf_menu, cli_menu;
static char menu_name[LM_MAX_APPS][NAME_MAX_LEN];
static char menu_raw[LM_MAX_APPS][CMD_MAX];
static char menu_res[LM_MAX_APPS][CMD_MAX];
static int menu_n;

static bool layer_add(struct menu_layer *l, const char *name, const char *cmd)
{
    if (l->n >= LM_MAX_APPS || !*name || !*cmd || strlen(name) >= NAME_MAX_LEN || strlen(cmd) >= CMD_MAX)
        return false;
    strcpy(l->name[l->n], name);
    strcpy(l->cmd[l->n], cmd);
    l->n++;
    return true;
}

bool lm_launch_add(struct lm_config *cfg, const char *name, const char *cmd)
{
    (void)cfg;
    return layer_add(&cli_menu, name, cmd);
}

void lm_launch_reset(void)
{
    for (int i = 0; i < C_COUNT; i++)
        if (!cmd_cli[i])
            strcpy(cmd_raw[i], cmd_defaults[i]);
    conf_menu.n = 0;
}

bool lm_launch_set_command(const char *which, const char *cmd)
{
    for (int i = 0; i < C_COUNT; i++) {
        if (!strcmp(which, cmd_names[i]) && *cmd && strlen(cmd) < CMD_MAX) {
            strcpy(cmd_raw[i], cmd);
            cmd_cli[i] = true;
            return true;
        }
    }
    return false;
}

bool lm_launch_conf(void *ud, const char *section, const char *key, const char *value)
{
    if (!strcmp(section, "commands")) {
        for (int i = 0; i < C_COUNT; i++) {
            if (!strcmp(key, cmd_names[i])) {
                if (!*value || strlen(value) >= CMD_MAX)
                    return false;
                if (!cmd_cli[i])
                    strcpy(cmd_raw[i], value);
                return true;
            }
        }
        return false;
    }
    if (!strcmp(section, "menu")) {
        (void)ud;
        if (conf_menu.n >= LM_MAX_APPS)
            lm_warn("menu: at most %d entries, '%s' ignored", LM_MAX_APPS, key);
        return layer_add(&conf_menu, key, value) || conf_menu.n >= LM_MAX_APPS;
    }
    return false;
}

/* ---- resolving a command -------------------------------------------------------------------------------------------- */

/* Directory of this binary ("" when unknown): own programs live next to it. */
static const char *self_dir(void)
{
    static char dir[PATH_MAX];
    static bool done;
    if (!done) {
        done = true;
        ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
        if (n > 0) {
            dir[n] = 0;
            char *slash = strrchr(dir, '/');
            if (slash)
                *slash = 0;
            else
                dir[0] = 0;
        }
    }
    return dir;
}

static bool in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char full[PATH_MAX];
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        if (len && snprintf(full, sizeof(full), "%.*s/%s", (int)len, path, prog) < (int)sizeof(full) && access(full, X_OK) == 0)
            return true;
        path += len + (colon ? 1 : 0);
    }
    return false;
}

static const char *token_cmd(const char *name, size_t len)
{
    for (int i = 0; i < C_COUNT; i++)
        if (strlen(cmd_names[i]) == len && !strncmp(cmd_names[i], name, len))
            return cmd_res[i][0] ? cmd_res[i] : cmd_raw[i];
    return NULL;
}

/* Resolve one alternative into `out`. Returns true when its program exists; *own is set for MellaGUI's own programs. */
static bool resolve_one(const char *alt, bool tokens, char *out, size_t n, bool *own)
{
    while (isspace((unsigned char)*alt))
        alt++;
    *own = false;
    if (tokens && *alt == '@') {
        const char *name = alt + 1, *e = name;
        while (*e && !isspace((unsigned char)*e))
            e++;
        const char *cmd = token_cmd(name, (size_t)(e - name));
        if (!cmd)
            return false;
        *own = true;
        return snprintf(out, n, "%s%s", cmd, e) < (int)n;
    }

    /* find the program word: skip `env` and VAR=value words */
    const char *ws = alt, *we = alt;
    char prog[PATH_MAX];
    for (;;) {
        while (isspace((unsigned char)*ws))
            ws++;
        if (!*ws)
            return false;
        char quote = (*ws == '\'' || *ws == '"') ? *ws : 0;
        const char *b = quote ? ws + 1 : ws, *e = b;
        while (*e && (quote ? *e != quote : !isspace((unsigned char)*e)))
            e++;
        we = (quote && *e) ? e + 1 : e;
        size_t len = (size_t)(e - b);
        if (len >= sizeof(prog))
            return false;
        memcpy(prog, b, len);
        prog[len] = 0;
        bool assign = !quote && (isalpha((unsigned char)prog[0]) || prog[0] == '_');
        if (assign) {
            const char *q = prog;
            while (isalnum((unsigned char)*q) || *q == '_')
                q++;
            assign = *q == '=';
        }
        if (!quote && (!strcmp(prog, "env") || assign)) {
            ws = we;
            continue;
        }
        break;
    }

    char path[PATH_MAX];
    bool found, replaced = false;
    if (!strncmp(prog, "~/", 2)) {
        const char *home = getenv("HOME");
        if (!home || !*home || strchr(home, '\'') || snprintf(path, sizeof(path), "%s%s", home, prog + 1) >= (int)sizeof(path))
            return false;
        found = access(path, X_OK) == 0;
        replaced = true;
    } else if (strchr(prog, '/')) {
        found = access(prog, X_OK) == 0;
    } else if (!strncmp(prog, "mellagui-", 9)) {
        *own = true;
        found = true;
        const char *dir = self_dir();
        if (*dir && !strchr(dir, '\'') && snprintf(path, sizeof(path), "%s/%s", dir, prog) < (int)sizeof(path) &&
            access(path, X_OK) == 0)
            replaced = true;
    } else {
        found = in_path(prog);
    }

    if (!replaced)
        return snprintf(out, n, "%s", alt) < (int)n && found;
    return snprintf(out, n, "%.*s'%s'%s", (int)(ws - alt), alt, path, we) < (int)n && found;
}

/* The next top-level " || " separated alternative of *pp (outside quotes) copied to buf; false at the end. */
static bool next_alt(const char **pp, char *buf, size_t n)
{
    const char *s = *pp;
    if (!s)
        return false;
    char quote = 0;
    const char *i = s;
    for (; *i; i++) {
        if (quote) {
            if (*i == quote)
                quote = 0;
        } else if (*i == '\'' || *i == '"') {
            quote = *i;
        } else if (i[0] == '|' && i[1] == '|' && i > s && isspace((unsigned char)i[-1]) && isspace((unsigned char)i[2])) {
            break;
        }
    }
    size_t len = (size_t)(i - s);
    if (len >= n)
        len = n - 1;
    memcpy(buf, s, len);
    while (len && isspace((unsigned char)buf[len - 1]))
        len--;
    buf[len] = 0;
    *pp = *i ? i + 2 : NULL;
    return true;
}

/* Try every alternative of `spec`; the first whose program exists wins. On failure `out` holds the last tried alternative. */
static bool resolve_spec(const char *spec, bool tokens, char *out, size_t n, bool *own)
{
    char alt[CMD_MAX];
    const char *p = spec;
    out[0] = 0;
    *own = false;
    while (next_alt(&p, alt, sizeof(alt))) {
        if (resolve_one(alt, tokens, out, n, own))
            return true;
    }
    return false;
}

void lm_launch_expand(const char *spec, char *out, size_t n)
{
    bool own;
    if (!resolve_spec(spec, true, out, n, &own) && !own)
        snprintf(out, n, "%s", spec);
}

void lm_launch_resolve(struct lm_config *cfg)
{
    bool own;
    for (int i = 0; i < C_COUNT; i++) {
        if (!resolve_spec(cmd_raw[i], false, cmd_res[i], sizeof(cmd_res[i]), &own))
            strcpy(cmd_res[i], cmd_raw[i]);                               /* not found: the shell will say so */
    }
    cfg->terminal_cmd = cmd_res[C_TERMINAL];
    cfg->run_cmd = cmd_res[C_RUN];
    cfg->settings_cmd = cmd_res[C_SETTINGS];
    cfg->view_cmd = cmd_res[C_VIEW];

    if (conf_menu.n == 0 && cli_menu.n == 0)
        lm_conf_parse(DEFAULT_MENU, "<built-in menu>", lm_launch_conf, cfg);
    menu_n = 0;                                 /* the file's entries first, then --app */
    for (int i = 0; i < conf_menu.n; i++, menu_n++) {
        strcpy(menu_name[menu_n], conf_menu.name[i]);
        strcpy(menu_raw[menu_n], conf_menu.cmd[i]);
    }
    for (int i = 0; i < cli_menu.n && menu_n < LM_MAX_APPS; i++, menu_n++) {
        strcpy(menu_name[menu_n], cli_menu.name[i]);
        strcpy(menu_raw[menu_n], cli_menu.cmd[i]);
    }

    cfg->napps = 0;
    for (int i = 0; i < menu_n; i++) {
        if (!resolve_spec(menu_raw[i], true, menu_res[cfg->napps], sizeof(menu_res[0]), &own) && !own) {
            lm_info("menu: '%s' left out, its program was not found", menu_name[i]);
            continue;
        }
        if (cfg->napps != i) {
            memcpy(menu_name[cfg->napps], menu_name[i], sizeof(menu_name[0]));
            memcpy(menu_raw[cfg->napps], menu_raw[i], sizeof(menu_raw[0]));
        }
        cfg->apps[cfg->napps] = (struct lm_app_entry){ menu_name[cfg->napps], menu_res[cfg->napps] };
        cfg->napps++;
    }
    menu_n = cfg->napps;
}

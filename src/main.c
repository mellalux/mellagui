/*
 * MellaGUI compositor entry point: argument parsing, module wiring, shutdown.
 */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "keymap.h"
#include "log.h"
#include "loop.h"
#include "output.h"
#include "render.h"
#include "seat.h"
#include "settings.h"
#include "lock.h"
#include "server.h"
#include "shm.h"
#include "surface.h"
#include "wm.h"
#include "dmabuf.h"
#include "mixer.h"
#include "sysinfo.h"
#include "xdg.h"

static const char *usage_text =
    "Usage: mellagui [options]\n"
    "  --backend drm|headless   display backend (default: drm)\n"
    "  --size WxH               headless output size (default 1280x720)\n"
    "  --socket NAME            Wayland socket name (default: first free wayland-N)\n"
    "  --exec CMD               shell command to start once the compositor is up\n"
    "  --terminal CMD           command for Super+Enter and the panel launcher (default mellagui-term)\n"
    "  --app NAME=CMD           add an entry to the MellaGUI menu (repeatable; replaces the default Terminal/Calculator/Code)\n"
    "  --run CMD                command for Super+R and the panel's Run button (default mellagui-run)\n"
    "  --drm-device PATH        DRM device (default: auto-detect /dev/dri/card*)\n"
    "  --mode WxH[@Hz]|preferred  drm: video mode. Default: the monitor's preferred one, except when its shape differs from the\n"
    "                           panel's (e.g. 21:9 panel, 16:9 modes only): then a mode of the panel's shape is generated\n"
    "                           (CVT reduced blanking). WxH forces a size (generated if the monitor does not list it),\n"
    "                           'preferred' turns the correction off\n"
    "  --control PATH           headless control socket (testing)\n"
    "  --log-level LEVEL        error|warn|info|debug|trace\n"
    "  --log-file PATH          additionally append the log to PATH\n"
    "  --settings FILE          where the settings panel saves (and the start reads) the user settings; \"none\" = nowhere\n"
    "                           (default drm: $XDG_CONFIG_HOME/mellagui/settings.conf, headless: none); options below win\n"
    "  --clipboard-file FILE    where the clipboard history is kept between runs (private file, newest entry first); \"none\" = nowhere\n"
    "                           (default drm: $XDG_CONFIG_HOME/mellagui/clipboard-history, headless: none)\n"
    "  --gadgets-file FILE      where the places and sizes of the desktop gadgets are kept between runs; \"none\" = nowhere\n"
    "                           (default drm: $XDG_CONFIG_HOME/mellagui/gadgets.conf, headless: none)\n"
    "  --settings-access any|own  who may read and change the settings through the private mellagui_settings_v1 interface: \"own\" =\n"
    "                           only MellaGUI's own programs (mellagui-settings ... next to the compositor binary), \"any\" = every client\n"
    "                           of the session (default: own on drm, any on headless, which the tests need)\n"
    "  --lock-file FILE         where the screen lock keeps its salted password / PIN hash (mode 0600); \"none\" = in memory only\n"
    "                           (default drm: $XDG_CONFIG_HOME/mellagui/lock.conf, headless: none)\n"
    "  --screenshot-dir DIR     where Print (screen) and Alt+Print (window) save PNGs; Ctrl+Print only copies; every shot also goes to the\n"
    "                           clipboard as image/png; \"none\" = clipboard only (default drm: ~/Pictures, headless: none)\n"
    "  --layouts LIST           keyboard layouts to switch between, e.g. us,ee,de (default: us,ee,de,fi; first is active;\n"
    "                           Super+Space / the panel indicator switch; with more than \"us\" right Alt is AltGr)\n"
    "  --sticky                 start with sticky keys on (also toggled by pressing Shift five times or the panel button)\n"
    "  --no-gadget              start with the desktop gadget (clipboard history) hidden; Super+G toggles it\n"
    "  --mixer                  start with the volume mixer gadget shown (Super+V toggles it; needs pactl, wpctl or amixer)\n"
    "  --no-dmabuf              do not offer zwp_linux_dmabuf_v1 (GPU-rendered client buffers); clients then use wl_shm\n"
    "  --sysinfo                start with the system monitor gadget (CPU, memory, temperature) shown; Super+S toggles it\n"
    "  --no-sound               no beeps for sticky keys (latch, lock, unlock, on/off; needs aplay otherwise)\n"
    "  --no-grab                do not EVIOCGRAB keyboards\n"
    "  --timeout SECONDS        quit automatically after SECONDS (safe first run on a real VT)\n"
    "  -h, --help               this text\n"
    "Environment: MELLAGUI_DUMP=path  (headless) frame dump target for SIGUSR1\n";

static struct lm_server *g_srv;
static struct lm_backend *g_backend;

/* The program used to be called "lumen": move its config directory (settings, clipboard history, gadgets) to the new
 * name once, so nobody loses their settings. Does nothing if the new directory exists or the old one does not. */
static void migrate_old_config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[PATH_MAX - 16], from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else if (home && *home)
        snprintf(base, sizeof(base), "%s/.config", home);
    else
        return;
    snprintf(from, sizeof(from), "%s/lumen", base);
    snprintf(to, sizeof(to), "%s/mellagui", base);
    if (stat(from, &st) != 0 || !S_ISDIR(st.st_mode) || lstat(to, &st) == 0)
        return;
    if (rename(from, to) == 0)
        lm_info("config: moved %s to %s", from, to);
}

static void on_output_added(void *data, struct lm_output *out)
{
    struct lm_server *srv = data;
    if (srv->outputs)
        lm_output_mgr_add(srv->outputs, out);
    if (srv->render)
        lm_render_output_added(srv->render, out);
    if (srv->wm)
        lm_wm_output_added(srv->wm, out);
    lm_info("output %s: %dx%d@%.2f at %d,%d", out->name, out->width, out->height,
            out->refresh_mhz / 1000.0, out->x, out->y);
}

static void on_output_changed(void *data, struct lm_output *out)
{
    struct lm_server *srv = data;
    if (srv->outputs)
        lm_output_mgr_changed(srv->outputs, out);
    if (srv->render)
        lm_render_output_changed(srv->render, out);
    if (srv->wm)
        lm_wm_output_changed(srv->wm, out);
}

static void on_mixer_changed(void *data)
{
    struct lm_server *srv = data;
    if (srv->wm)
        lm_wm_mixer_changed(srv->wm);
}

static void on_sysinfo_changed(void *data)
{
    struct lm_server *srv = data;
    if (srv->wm)
        lm_wm_sysinfo_changed(srv->wm);
}

static void on_output_removed(void *data, struct lm_output *out)
{
    struct lm_server *srv = data;
    if (srv->wm)
        lm_wm_output_removed(srv->wm, out);
    if (srv->render)
        lm_render_output_removed(srv->render, out);
    if (srv->outputs)
        lm_output_mgr_remove(srv->outputs, out);
}

static void on_output_ready(void *data, struct lm_output *out, uint32_t time_ms)
{
    struct lm_server *srv = data;
    if (srv->render)
        lm_render_output_ready(srv->render, out, time_ms);
}

static void on_session_active(void *data, bool active)
{
    struct lm_server *srv = data;
    lm_info("session %s", active ? "active" : "inactive");
    if (srv->input)
        lm_input_set_active(srv->input, active);
    if (active && srv->render)
        lm_render_damage_all(srv->render);
}

static void sig_quit(int signo, void *data)
{
    (void)signo;
    lm_info("signal %d: shutting down", signo);
    lm_server_quit(data);
}

static void sig_dump(int signo, void *data)
{
    struct lm_server *srv = data;
    (void)signo;
    if (srv->config.dump_path && g_backend && !strcmp(srv->config.backend, "headless")) {
        if (srv->render)
            lm_render_flush(srv->render);
        if (lm_backend_headless_dump(g_backend, srv->config.dump_path) < 0)
            lm_warn("dump to %s failed: %s", srv->config.dump_path, strerror(errno));
    }
}

/* Default for a bundled program: `name` next to this binary when it exists, else whatever is in PATH.
 * Each call site passes its own buffer. */
static const char *default_sibling(const char *name, char *cmd, size_t cmdsz)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(exe) - (size_t)(slash + 1 - exe), "%s", name);
            if (access(exe, X_OK) == 0 && !strchr(exe, '\'')) {
                snprintf(cmd, cmdsz, "'%s'", exe);
                return cmd;
            }
        }
    }
    return name;
}

static void quit_timer(void *data)
{
    lm_info("--timeout reached, shutting down");
    lm_server_quit(data);
}

static bool ensure_runtime_dir(void)
{
    const char *d = getenv("XDG_RUNTIME_DIR");
    if (d && *d)
        return true;
    char path[64];
    snprintf(path, sizeof(path), "/run/user/%u", (unsigned)getuid());
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid()) {
        setenv("XDG_RUNTIME_DIR", path, 1);
        lm_info("XDG_RUNTIME_DIR not set, using %s", path);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    struct lm_config cfg;
    lm_config_defaults(&cfg);
    cfg.dump_path = getenv("MELLAGUI_DUMP");
    const char *lvl = getenv("MELLAGUI_LOG");
    if (lvl) {
        int l = lm_log_level_from_string(lvl);
        if (l >= 0) cfg.log_level = l;
    }

    enum { OPT_NO_GRAB = 256, OPT_SETTINGS, OPT_TIMEOUT, OPT_RUN, OPT_APP, OPT_LAYOUTS, OPT_STICKY, OPT_NO_SOUND, OPT_NO_GADGET, OPT_MODE, OPT_MIXER, OPT_SYSINFO, OPT_NO_DMABUF, OPT_CLIPBOARD_FILE, OPT_GADGETS_FILE, OPT_LOCK_FILE, OPT_SETTINGS_ACCESS, OPT_SCREENSHOT_DIR };
    const char *layouts_arg = "us,ee,de,fi";
    int timeout_s = 0;
    bool terminal_set = false, run_set = false;
    static const struct option opts[] = {
        { "backend", required_argument, 0, 'b' },
        { "size", required_argument, 0, 's' },
        { "socket", required_argument, 0, 'S' },
        { "exec", required_argument, 0, 'e' },
        { "terminal", required_argument, 0, 't' },
        { "run", required_argument, 0, OPT_RUN },
        { "app", required_argument, 0, OPT_APP },
        { "drm-device", required_argument, 0, 'd' },
        { "mode", required_argument, 0, OPT_MODE },
        { "control", required_argument, 0, 'c' },
        { "log-level", required_argument, 0, 'l' },
        { "log-file", required_argument, 0, 'f' },
        { "settings", required_argument, 0, OPT_SETTINGS },
        { "clipboard-file", required_argument, 0, OPT_CLIPBOARD_FILE },
        { "gadgets-file", required_argument, 0, OPT_GADGETS_FILE },
        { "lock-file", required_argument, 0, OPT_LOCK_FILE },
        { "screenshot-dir", required_argument, 0, OPT_SCREENSHOT_DIR },
        { "settings-access", required_argument, 0, OPT_SETTINGS_ACCESS },
        { "layouts", required_argument, 0, OPT_LAYOUTS },
        { "sticky", no_argument, 0, OPT_STICKY },
        { "no-gadget", no_argument, 0, OPT_NO_GADGET },
        { "mixer", no_argument, 0, OPT_MIXER },
        { "sysinfo", no_argument, 0, OPT_SYSINFO },
        { "no-dmabuf", no_argument, 0, OPT_NO_DMABUF },
        { "no-sound", no_argument, 0, OPT_NO_SOUND },
        { "no-grab", no_argument, 0, OPT_NO_GRAB },
        { "timeout", required_argument, 0, OPT_TIMEOUT },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    /* The settings file is read first so that the command line can override it: peek at --backend / --settings. */
    const char *settings_arg = NULL;
    bool headless_arg = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--settings") && i + 1 < argc)
            settings_arg = argv[i + 1];
        else if (!strncmp(argv[i], "--settings=", 11))
            settings_arg = argv[i] + 11;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            headless_arg = !strcmp(argv[i + 1], "headless");
        else if (!strncmp(argv[i], "--backend=", 10))
            headless_arg = !strcmp(argv[i] + 10, "headless");
    }
    static char settings_buf[PATH_MAX], layouts_buf[64], clip_buf[PATH_MAX], gadgets_buf[PATH_MAX], lock_buf[PATH_MAX], shot_buf[PATH_MAX];
    if (!headless_arg)
        migrate_old_config_dir();
    if (!settings_arg && !headless_arg) {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(settings_buf, sizeof(settings_buf), "%s/mellagui/settings.conf", xdg);
        else if (home && *home)
            snprintf(settings_buf, sizeof(settings_buf), "%s/.config/mellagui/settings.conf", home);
        if (settings_buf[0])
            settings_arg = settings_buf;
    }
    if (!headless_arg) {                                  /* the history lives beside the settings (--clipboard-file wins) */
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(clip_buf, sizeof(clip_buf), "%s/mellagui/clipboard-history", xdg);
        else if (home && *home)
            snprintf(clip_buf, sizeof(clip_buf), "%s/.config/mellagui/clipboard-history", home);
        if (clip_buf[0])
            cfg.clip_path = clip_buf;
        if (xdg && *xdg)
            snprintf(gadgets_buf, sizeof(gadgets_buf), "%s/mellagui/gadgets.conf", xdg);
        else if (home && *home)
            snprintf(gadgets_buf, sizeof(gadgets_buf), "%s/.config/mellagui/gadgets.conf", home);
        if (gadgets_buf[0])
            cfg.gadgets_path = gadgets_buf;
        if (xdg && *xdg)
            snprintf(lock_buf, sizeof(lock_buf), "%s/mellagui/lock.conf", xdg);
        else if (home && *home)
            snprintf(lock_buf, sizeof(lock_buf), "%s/.config/mellagui/lock.conf", home);
        if (lock_buf[0])
            cfg.lock_path = lock_buf;
        if (home && *home) {                          /* --screenshot-dir (parsed below) wins */
            snprintf(shot_buf, sizeof(shot_buf), "%s/Pictures", home);
            cfg.screenshot_dir = shot_buf;
        }
    }
    if (settings_arg && *settings_arg && strcmp(settings_arg, "none")) {
        cfg.settings_path = settings_arg;
        if (lm_settings_load(&cfg, settings_arg, layouts_buf, sizeof(layouts_buf)) > 0 && layouts_buf[0])
            layouts_arg = layouts_buf;
    }

    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            if (strcmp(optarg, "drm") && strcmp(optarg, "headless")) {
                fprintf(stderr, "mellagui: unknown backend '%s'\n", optarg);
                return 2;
            }
            cfg.backend = optarg;
            break;
        case 's': {
            int w, h;
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
                fprintf(stderr, "mellagui: bad --size '%s' (expected WxH)\n", optarg);
                return 2;
            }
            cfg.headless_width = w;
            cfg.headless_height = h;
            break;
        }
        case 'S': cfg.socket_name = optarg; break;
        case 'e': cfg.exec_cmd = optarg; break;
        case 't': cfg.terminal_cmd = optarg; terminal_set = true; break;
        case OPT_APP: {
            char *eq = strchr(optarg, '=');
            if (!eq || eq == optarg || !eq[1] || cfg.napps >= LM_MAX_APPS) {
                fprintf(stderr, "mellagui: bad --app '%s' (expected NAME=CMD, at most %d)\n", optarg, LM_MAX_APPS);
                return 2;
            }
            *eq = 0;
            cfg.apps[cfg.napps++] = (struct lm_app_entry){ optarg, eq + 1 };
            break;
        }
        case OPT_RUN: cfg.run_cmd = optarg; run_set = true; break;
        case 'd': cfg.drm_device = optarg; break;
        case OPT_MODE: {
            int w, h, hz = 60;
            if (!strcmp(optarg, "preferred")) {
                cfg.drm_mode_w = -1;
                break;
            }
            if (sscanf(optarg, "%dx%d@%d", &w, &h, &hz) < 2 || w < 320 || h < 200 || w > 8192 || h > 4320 || hz < 24 || hz > 240) {
                fprintf(stderr, "mellagui: bad --mode '%s' (expected WxH or WxH@Hz)\n", optarg);
                return 2;
            }
            cfg.drm_mode_w = w;
            cfg.drm_mode_h = h;
            cfg.drm_mode_hz = hz;
            break;
        }
        case 'c': cfg.control_path = optarg; break;
        case 'l': {
            int l = lm_log_level_from_string(optarg);
            if (l < 0) {
                fprintf(stderr, "mellagui: bad --log-level '%s'\n", optarg);
                return 2;
            }
            cfg.log_level = l;
            break;
        }
        case 'f': cfg.log_file = optarg; break;
        case OPT_SETTINGS: break;           /* handled above */
        case OPT_CLIPBOARD_FILE: cfg.clip_path = strcmp(optarg, "none") ? optarg : NULL; break;
        case OPT_GADGETS_FILE: cfg.gadgets_path = strcmp(optarg, "none") ? optarg : NULL; break;
        case OPT_SETTINGS_ACCESS:
            if (!strcmp(optarg, "any")) cfg.settings_access = 1;
            else if (!strcmp(optarg, "own")) cfg.settings_access = 0;
            else { fprintf(stderr, "mellagui: --settings-access wants any or own, not '%s'\n", optarg); return 2; }
            break;
        case OPT_LOCK_FILE: cfg.lock_path = strcmp(optarg, "none") ? optarg : NULL; break;
        case OPT_SCREENSHOT_DIR: cfg.screenshot_dir = strcmp(optarg, "none") ? optarg : NULL; break;
        case OPT_LAYOUTS: layouts_arg = optarg; break;
        case OPT_STICKY: cfg.sticky_keys = true; break;
        case OPT_NO_GADGET: cfg.gadget = false; break;
        case OPT_MIXER: cfg.mixer = true; break;
        case OPT_SYSINFO: cfg.sysinfo = true; break;
        case OPT_NO_DMABUF: cfg.dmabuf = false; break;
        case OPT_NO_SOUND: cfg.sound = false; break;
        case OPT_NO_GRAB: cfg.grab_input = false; break;
        case OPT_TIMEOUT:
            timeout_s = atoi(optarg);
            if (timeout_s <= 0) {
                fprintf(stderr, "mellagui: bad --timeout '%s'\n", optarg);
                return 2;
            }
            break;
        case 'h': fputs(usage_text, stdout); return 0;
        default: fputs(usage_text, stderr); return 2;
        }
    }

    {
        char err[160];
        cfg.kbd_nlayouts = lm_layout_parse(layouts_arg, cfg.kbd_layouts, err, sizeof(err));
        if (cfg.kbd_nlayouts == 0) {
            fprintf(stderr, "mellagui: --layouts: %s\n", err);
            return 2;
        }
    }

    static char term_buf[PATH_MAX + 4], run_buf[PATH_MAX + 4];
    if (!terminal_set)
        cfg.terminal_cmd = default_sibling("mellagui-term", term_buf, sizeof(term_buf));
    if (!run_set)
        cfg.run_cmd = default_sibling("mellagui-run", run_buf, sizeof(run_buf));
    static char settings_cmd_buf[PATH_MAX + 4];
    cfg.settings_cmd = default_sibling("mellagui-settings", settings_cmd_buf, sizeof(settings_cmd_buf));
    static char calc_buf[PATH_MAX + 4], view_buf[PATH_MAX + 4], code_buf[PATH_MAX + 128], ff_buf[PATH_MAX + 160];
    cfg.view_cmd = default_sibling("mellagui-view", view_buf, sizeof(view_buf));
    if (cfg.napps == 0) {
        /* default menu: Terminal, Calculator, VS Code and Firefox (when found) */
        cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Terminal", cfg.terminal_cmd };
        cfg.apps[cfg.napps++] = (struct lm_app_entry){
            "Calculator", default_sibling("mellagui-calc", calc_buf, sizeof(calc_buf)) };
        cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Images", cfg.view_cmd };
        /* VS Code: ~/VSCode/code (tarball), else the Electron binary inside the snap. /snap/bin/code is
         * deliberately not used: its launcher appends --ozone-platform=x11 after our flags, and there is no X11. */
        const char *home = getenv("HOME");
        char tarball[PATH_MAX];
        const char *cands[2] = { NULL, "/snap/code/current/usr/share/code/code" };
        if (home && snprintf(tarball, sizeof(tarball), "%s/VSCode/code", home) < (int)sizeof(tarball))
            cands[0] = tarball;
        for (int i = 0; i < 2; i++) {
            if (!cands[i] || access(cands[i], X_OK) != 0 || strchr(cands[i], '\''))
                continue;
            snprintf(code_buf, sizeof(code_buf),
                     "'%s' --ozone-platform=wayland --disable-gpu --no-sandbox --password-store=basic", cands[i]);
            cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Code", code_buf };
            break;
        }
        /* Firefox: the snap's launcher or a distro binary. Native Wayland, WebRender on the GPU (MellaGUI offers zwp_linux_dmabuf_v1; without it Firefox falls back by itself).
         * (The snap is confined: it may only connect to sockets named wayland-N, which is what MellaGUI picks by default.) */
        const char *ffs[2] = { "/snap/bin/firefox", "/usr/bin/firefox" };
        for (int i = 0; i < 2; i++) {
            if (access(ffs[i], X_OK) != 0)
                continue;
            snprintf(ff_buf, sizeof(ff_buf),
                     "env MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland MOZ_WEBRENDER=1 %s", ffs[i]);
            cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Firefox", ff_buf };
            break;
        }
        cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Settings", cfg.settings_cmd };   /* last: keeps the others' places */
    }
    lm_log_set_level((enum lm_log_level)cfg.log_level);
    if (cfg.log_file && lm_log_set_file(cfg.log_file) < 0)
        fprintf(stderr, "mellagui: cannot open log file %s: %s\n", cfg.log_file, strerror(errno));

    if (!ensure_runtime_dir()) {
        fprintf(stderr, "mellagui: XDG_RUNTIME_DIR is not set and /run/user/%u does not exist.\n"
                        "Log in through a normal login session (logind) or set XDG_RUNTIME_DIR to a private directory (mode 0700).\n",
                (unsigned)getuid());
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    int rc = 1;
    struct lm_loop *loop = lm_loop_create();
    if (!loop) {
        lm_error("cannot create event loop");
        return 1;
    }
    struct lm_server *srv = lm_server_create(loop, &cfg);
    if (!srv) {
        lm_error("cannot create server");
        lm_loop_destroy(loop);
        return 1;
    }
    g_srv = srv;

    lm_loop_add_signal(loop, SIGINT, sig_quit, srv);
    lm_loop_add_signal(loop, SIGTERM, sig_quit, srv);
    lm_loop_add_signal(loop, SIGHUP, sig_quit, srv);
    lm_loop_add_signal(loop, SIGUSR1, sig_dump, srv);

    bool headless = !strcmp(cfg.backend, "headless");
    srv->backend = headless ? lm_backend_headless_create(srv, cfg.headless_width, cfg.headless_height)
                            : lm_backend_drm_create(srv);
    if (!srv->backend) {
        lm_error("cannot create %s backend", cfg.backend);
        goto out;
    }
    g_backend = srv->backend;
    srv->backend->listener = (struct lm_backend_listener){
        .data = srv,
        .output_added = on_output_added,
        .output_changed = on_output_changed,
        .output_removed = on_output_removed,
        .output_ready = on_output_ready,
        .session_active = on_session_active,
    };

    if (!(srv->render = lm_render_create(srv)) ||
        !(srv->compositor = lm_compositor_create(srv)) ||
        !(srv->shm = lm_shm_create(srv)) ||
        !(srv->outputs = lm_output_mgr_create(srv)) ||
        !(srv->seat = lm_seat_create(srv)) ||
        !(srv->xdg = lm_xdg_create(srv)) ||
        !(srv->wm = lm_wm_create(srv)) ||
        !(srv->input = lm_input_create(srv)) ||
        !(srv->settings = lm_settings_create(srv))) {
        lm_error("module initialisation failed");
        goto out;
    }
    lm_input_set_handler(srv->input, lm_seat_handle_input_cb, srv->seat);
    srv->lock = lm_lock_create(srv);            /* with start=1 in lock.conf the screen is locked from the first frame */
    srv->dmabuf = lm_dmabuf_create(srv, cfg.dmabuf);
    srv->mixer = lm_mixer_create(loop, on_mixer_changed, srv);
    if (srv->mixer)
        lm_mixer_set_active(srv->mixer, lm_wm_gadget_visible_id(srv->wm, LM_GADGET_MIXER));
    srv->sysinfo = lm_sysinfo_create(loop, on_sysinfo_changed, srv);
    if (srv->sysinfo)
        lm_sysinfo_set_active(srv->sysinfo, lm_wm_gadget_visible_id(srv->wm, LM_GADGET_SYSTEM));
    if (!headless && lm_input_count_keyboards(srv->input) == 0) {
        /* Without a keyboard the user could not even switch VT: refuse to take over the screen. */
        lm_error("no keyboard could be opened, refusing to start (the screen would be unusable). "
                 "Fix the permissions on /dev/input/event*: sudo usermod -aG input $USER, then log in again.");
        fprintf(stderr, "mellagui: no keyboard available. Add yourself to group 'input' "
                        "(sudo usermod -aG input $USER) and log in again.\n");
        goto out;
    }

    if (lm_server_listen(srv) < 0)
        goto out;
    if (lm_backend_init(srv->backend) < 0) {
        lm_error("backend initialisation failed");
        goto out;
    }
    if (timeout_s > 0) {
        struct lm_timer *tm = lm_loop_add_timer(loop, quit_timer, srv);
        if (tm) {
            lm_timer_arm_ms(tm, (uint32_t)timeout_s * 1000u, 0);
            lm_info("will quit automatically after %d s", timeout_s);
        }
    }
    lm_info("MellaGUI ready on WAYLAND_DISPLAY=%s (backend %s)", lm_server_socket_name(srv), cfg.backend);
    if (cfg.exec_cmd)
        lm_server_spawn(srv, cfg.exec_cmd);

    rc = lm_loop_run(loop);

out:
    /* Reverse creation order. */
    if (srv->dmabuf) lm_dmabuf_destroy(srv->dmabuf);
    srv->dmabuf = NULL;
    if (srv->mixer) lm_mixer_destroy(srv->mixer);
    srv->mixer = NULL;
    if (srv->sysinfo) lm_sysinfo_destroy(srv->sysinfo);
    srv->sysinfo = NULL;
    if (srv->lock) lm_lock_destroy(srv->lock);
    srv->lock = NULL;
    if (srv->settings) lm_settings_destroy(srv->settings);
    srv->settings = NULL;
    if (srv->input) lm_input_destroy(srv->input);
    srv->input = NULL;
    if (srv->wm) lm_wm_destroy(srv->wm);
    srv->wm = NULL;
    if (srv->xdg) lm_xdg_destroy(srv->xdg);
    srv->xdg = NULL;
    if (srv->seat) lm_seat_destroy(srv->seat);
    srv->seat = NULL;
    if (srv->outputs) lm_output_mgr_destroy(srv->outputs);
    srv->outputs = NULL;
    if (srv->shm) lm_shm_destroy(srv->shm);
    srv->shm = NULL;
    if (srv->compositor) lm_compositor_destroy(srv->compositor);
    srv->compositor = NULL;
    if (srv->render) lm_render_destroy(srv->render);
    srv->render = NULL;
    if (srv->backend) lm_backend_destroy(srv->backend);
    srv->backend = NULL;
    g_backend = NULL;
    lm_server_destroy(srv);
    lm_loop_destroy(loop);
    lm_log_close();
    (void)g_srv;
    return rc;
}

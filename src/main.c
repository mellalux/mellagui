/*
 * Lumen compositor entry point: argument parsing, module wiring, shutdown.
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
#include "seatdev.h"
#include "settings.h"
#include "server.h"
#include "shm.h"
#include "surface.h"
#include "wm.h"
#include "xdg.h"

static const char *usage_text =
    "Usage: lumen [options]\n"
    "  --backend drm|headless   display backend (default: drm)\n"
    "  --size WxH               headless output size (default 1280x720)\n"
    "  --socket NAME            Wayland socket name (default: first free wayland-N)\n"
    "  --exec CMD               shell command to start once the compositor is up\n"
    "  --terminal CMD           command for Super+Enter and the panel launcher (default lumen-term)\n"
    "  --app NAME=CMD           add an entry to the Lumen menu (repeatable; replaces the default Terminal/Calculator/Code)\n"
    "  --run CMD                command for Super+R and the panel's Run button (default lumen-run)\n"
    "  --drm-device PATH        DRM device (default: auto-detect /dev/dri/card*)\n"
    "  --control PATH           headless control socket (testing)\n"
    "  --log-level LEVEL        error|warn|info|debug|trace\n"
    "  --log-file PATH          additionally append the log to PATH\n"
    "  --settings FILE          where the settings panel saves (and the start reads) the user settings; \"none\" = nowhere\n"
    "                           (default drm: $XDG_CONFIG_HOME/lumen/settings.conf, headless: none); options below win\n"
    "  --layouts LIST           keyboard layouts to switch between, e.g. us,ee,de (default: us,ee,de,fi; first is active;\n"
    "                           Super+Space / the panel indicator switch; with more than \"us\" right Alt is AltGr)\n"
    "  --sticky                 start with sticky keys on (also toggled by pressing Shift five times or the panel button)\n"
    "  --no-gadget              start with the desktop gadget (clipboard history) hidden; Super+G toggles it\n"
    "  --no-sound               no beeps for sticky keys (latch, lock, unlock, on/off; needs aplay otherwise)\n"
    "  --no-grab                do not EVIOCGRAB keyboards\n"
    "  --timeout SECONDS        quit automatically after SECONDS (safe first run on a real VT)\n"
    "  -h, --help               this text\n"
    "Environment: LUMEN_DUMP=path  (headless) frame dump target for SIGUSR1\n";

static struct lm_server *g_srv;
static struct lm_backend *g_backend;

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
    cfg.dump_path = getenv("LUMEN_DUMP");
    const char *lvl = getenv("LUMEN_LOG");
    if (lvl) {
        int l = lm_log_level_from_string(lvl);
        if (l >= 0) cfg.log_level = l;
    }

    enum { OPT_NO_GRAB = 256, OPT_SETTINGS, OPT_TIMEOUT, OPT_RUN, OPT_APP, OPT_LAYOUTS, OPT_STICKY, OPT_NO_SOUND, OPT_NO_GADGET };
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
        { "control", required_argument, 0, 'c' },
        { "log-level", required_argument, 0, 'l' },
        { "log-file", required_argument, 0, 'f' },
        { "settings", required_argument, 0, OPT_SETTINGS },
        { "layouts", required_argument, 0, OPT_LAYOUTS },
        { "sticky", no_argument, 0, OPT_STICKY },
        { "no-gadget", no_argument, 0, OPT_NO_GADGET },
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
    static char settings_buf[PATH_MAX], layouts_buf[64];
    if (!settings_arg && !headless_arg) {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(settings_buf, sizeof(settings_buf), "%s/lumen/settings.conf", xdg);
        else if (home && *home)
            snprintf(settings_buf, sizeof(settings_buf), "%s/.config/lumen/settings.conf", home);
        if (settings_buf[0])
            settings_arg = settings_buf;
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
                fprintf(stderr, "lumen: unknown backend '%s'\n", optarg);
                return 2;
            }
            cfg.backend = optarg;
            break;
        case 's': {
            int w, h;
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
                fprintf(stderr, "lumen: bad --size '%s' (expected WxH)\n", optarg);
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
                fprintf(stderr, "lumen: bad --app '%s' (expected NAME=CMD, at most %d)\n", optarg, LM_MAX_APPS);
                return 2;
            }
            *eq = 0;
            cfg.apps[cfg.napps++] = (struct lm_app_entry){ optarg, eq + 1 };
            break;
        }
        case OPT_RUN: cfg.run_cmd = optarg; run_set = true; break;
        case 'd': cfg.drm_device = optarg; break;
        case 'c': cfg.control_path = optarg; break;
        case 'l': {
            int l = lm_log_level_from_string(optarg);
            if (l < 0) {
                fprintf(stderr, "lumen: bad --log-level '%s'\n", optarg);
                return 2;
            }
            cfg.log_level = l;
            break;
        }
        case 'f': cfg.log_file = optarg; break;
        case OPT_SETTINGS: break;           /* handled above */
        case OPT_LAYOUTS: layouts_arg = optarg; break;
        case OPT_STICKY: cfg.sticky_keys = true; break;
        case OPT_NO_GADGET: cfg.gadget = false; break;
        case OPT_NO_SOUND: cfg.sound = false; break;
        case OPT_NO_GRAB: cfg.grab_input = false; break;
        case OPT_TIMEOUT:
            timeout_s = atoi(optarg);
            if (timeout_s <= 0) {
                fprintf(stderr, "lumen: bad --timeout '%s'\n", optarg);
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
            fprintf(stderr, "lumen: --layouts: %s\n", err);
            return 2;
        }
    }

    static char term_buf[PATH_MAX + 4], run_buf[PATH_MAX + 4];
    if (!terminal_set)
        cfg.terminal_cmd = default_sibling("lumen-term", term_buf, sizeof(term_buf));
    if (!run_set)
        cfg.run_cmd = default_sibling("lumen-run", run_buf, sizeof(run_buf));
    static char settings_cmd_buf[PATH_MAX + 4];
    cfg.settings_cmd = default_sibling("lumen-settings", settings_cmd_buf, sizeof(settings_cmd_buf));
    static char calc_buf[PATH_MAX + 4], web_buf[PATH_MAX + 4], code_buf[PATH_MAX + 128];
    if (cfg.napps == 0) {
        /* default menu: Terminal, Calculator, Browser and VS Code (when found) */
        cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Terminal", cfg.terminal_cmd };
        cfg.apps[cfg.napps++] = (struct lm_app_entry){
            "Calculator", default_sibling("lumen-calc", calc_buf, sizeof(calc_buf)) };
        cfg.apps[cfg.napps++] = (struct lm_app_entry){
            "Browser", default_sibling("lumen-web", web_buf, sizeof(web_buf)) };
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
        cfg.apps[cfg.napps++] = (struct lm_app_entry){ "Settings", cfg.settings_cmd };   /* last: keeps the others' places */
    }
    lm_log_set_level((enum lm_log_level)cfg.log_level);
    if (cfg.log_file && lm_log_set_file(cfg.log_file) < 0)
        fprintf(stderr, "lumen: cannot open log file %s: %s\n", cfg.log_file, strerror(errno));

    if (!ensure_runtime_dir()) {
        fprintf(stderr, "lumen: XDG_RUNTIME_DIR is not set and /run/user/%u does not exist.\n"
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

    srv->seatdev = lm_seatdev_create(loop);
    if (!srv->seatdev)
        goto out;

    bool headless = !strcmp(cfg.backend, "headless");
    srv->backend = headless ? lm_backend_headless_create(srv, cfg.headless_width, cfg.headless_height)
                            : lm_backend_drm_create(srv, cfg.drm_device);
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
    if (!headless && lm_input_count_keyboards(srv->input) == 0) {
        /* Without a keyboard the user could not even switch VT: refuse to take over the screen. */
        lm_error("no keyboard could be opened, refusing to start (the screen would be unusable). "
                 "Fix the permissions on /dev/input/event*: sudo usermod -aG input $USER, then log in again.");
        fprintf(stderr, "lumen: no keyboard available. Add yourself to group 'input' "
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
    lm_info("Lumen ready on WAYLAND_DISPLAY=%s (backend %s)", lm_server_socket_name(srv), cfg.backend);
    if (cfg.exec_cmd)
        lm_server_spawn(srv, cfg.exec_cmd);

    rc = lm_loop_run(loop);

out:
    /* Reverse creation order. */
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
    if (srv->seatdev) lm_seatdev_destroy(srv->seatdev);
    srv->seatdev = NULL;
    lm_server_destroy(srv);
    lm_loop_destroy(loop);
    lm_log_close();
    (void)g_srv;
    return rc;
}

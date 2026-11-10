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
#include "conf.h"
#include "dock.h"
#include "confdefault.h"
#include "keys.h"
#include "launchers.h"
#include "metrics.h"
#include "theme.h"
#include "input.h"
#include "keymap.h"
#include "log.h"
#include "loop.h"
#include "output.h"
#include "render.h"
#include "seat.h"
#include "settings.h"
#include "capture.h"
#include "tray.h"
#include "menu.h"
#include "idleinhibit.h"
#include "lock.h"
#include "saver.h"
#include "logind.h"
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
    "  --terminal CMD           command for Super+Enter (default mellagui-term)\n"
    "  --config FILE|none       hand-written configuration: dock, commands ... (default drm: $XDG_CONFIG_HOME/mellagui/mellagui.conf,\n"
    "                           headless: none); options win. --print-config shows the built-in defaults as such a file\n"
    "  --print-config           print the built-in mellagui.conf and exit\n"
    "  --run CMD                command for Super+R and the panel's Run button (default mellagui-run)\n"
    "  --drm-device PATH        DRM device (default: auto-detect /dev/dri/card*)\n"
    "  --mode WxH[@Hz]|preferred  drm: video mode. Default: the monitor's preferred one, except when its shape differs from the\n"
    "                           panel's (e.g. 21:9 panel, 16:9 modes only): then a mode of the panel's shape is generated\n"
    "                           (CVT reduced blanking). WxH forces a size (generated if the monitor does not list it),\n"
    "                           'preferred' turns the correction off\n"
    "  --output WxH             headless: one more output (repeatable, up to 3), to the right of the first one (testing)\n"
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
    "  --renderer software|auto|gl  how the picture is composed: gl (OpenGL ES through EGL: the GPU composites the windows, GPU clients'\n"
    "                           dmabufs are used in place, the display is scanned out through GBM; fails when GL is not usable), software\n"
    "                           (the CPU does everything) or auto (gl when it works, else software; default on the display, headless\n"
    "                           defaults to software). Env MELLAGUI_RENDERER\n"
    "  --logind off|auto|on     get the GPU and input devices from systemd-logind (no `video` / `input` group needed, VT switching\n"
    "                           done by the session manager); auto (default) falls back to opening the device files, on = required\n"
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

/* mellagui.conf: every section belongs to one module, this only hands the lines over. */
static bool conf_line(void *ud, const char *section, const char *key, const char *value)
{
    if (!strcmp(section, "commands"))
        return lm_launch_conf(ud, section, key, value);
    if (!strcmp(section, "menu"))
        return true;                            /* the MellaGUI menu is gone (the dock replaced it): old files keep working, the section is ignored */
    if (lm_dock_section(section))
        return lm_dock_conf(ud, section, key, value);
    if (!strcmp(section, "theme"))
        return lm_theme_conf(ud, section, key, value);
    if (lm_metrics_section(section))
        return lm_metrics_conf(ud, section, key, value);
    if (!strcmp(section, "keys"))
        return lm_keys_conf(&((struct lm_config *)ud)->keys, section, key, value);
    return false;
}

static char g_conf_path[PATH_MAX];      /* the mellagui.conf that was read at start ("" = none) */

/* Read mellagui.conf again: theme, sizes, shortcuts, dock and commands start from the built-in values and the file is applied on top,
 * the command line (--terminal, --run, --app) stays. Programs that are already running keep the colours they started with.
 * Returns the number of rejected lines, -1 when there is no (readable) file; nothing changes then. */
static int reload_config(struct lm_server *srv, bool quiet)
{
    if (!g_conf_path[0] || access(g_conf_path, R_OK) != 0) {
        lm_warn("reload: no readable configuration file");
        srv->reload_result = -1;
        srv->reloaded = true;
        lm_settings_changed(srv->settings);
        return -1;
    }
    lm_theme_reset();
    lm_metrics_reset();
    lm_keys_defaults(&srv->config.keys);
    lm_launch_reset();
    lm_dock_reset();
    int bad = lm_conf_load(g_conf_path, conf_line, &srv->config);
    lm_metrics_fixup();
    lm_launch_resolve(&srv->config);
    lm_dock_resolve();
    lm_info("reload: %s read again, %d line(s) ignored", g_conf_path, bad < 0 ? 0 : bad);
    srv->reload_result = bad < 0 ? 0 : bad;
    srv->reloaded = true;
    lm_settings_changed(srv->settings);
    if (srv->wm) {
        lm_wm_config_changed(srv->wm);
        if (quiet && bad <= 0)
            return bad < 0 ? 0 : bad;
        char body[96];
        if (bad > 0)
            snprintf(body, sizeof(body), "%d line(s) ignored, see the log", bad);
        else
            snprintf(body, sizeof(body), "mellagui.conf read again");
        lm_wm_notify(srv->wm, "Configuration", body, 3000);
    }
    return bad < 0 ? 0 : bad;
}

static void sig_reload(int signo, void *data)
{
    (void)signo;
    reload_config(data, false);
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

    enum { OPT_NO_GRAB = 256, OPT_SETTINGS, OPT_TIMEOUT, OPT_RUN, OPT_LAYOUTS, OPT_STICKY, OPT_NO_SOUND, OPT_NO_GADGET, OPT_MODE, OPT_MIXER, OPT_SYSINFO, OPT_NO_DMABUF, OPT_CLIPBOARD_FILE, OPT_GADGETS_FILE, OPT_LOCK_FILE, OPT_SETTINGS_ACCESS, OPT_SCREENSHOT_DIR, OPT_LOGIND, OPT_RENDERER, OPT_OUTPUT, OPT_CONFIG, OPT_PRINT_CONFIG };
    const char *layouts_arg = "us,ee,de,fi";
    int timeout_s = 0;
    static const struct option opts[] = {
        { "backend", required_argument, 0, 'b' },
        { "size", required_argument, 0, 's' },
        { "socket", required_argument, 0, 'S' },
        { "exec", required_argument, 0, 'e' },
        { "terminal", required_argument, 0, 't' },
        { "run", required_argument, 0, OPT_RUN },
        { "drm-device", required_argument, 0, 'd' },
        { "mode", required_argument, 0, OPT_MODE },
        { "control", required_argument, 0, 'c' },
        { "log-level", required_argument, 0, 'l' },
        { "log-file", required_argument, 0, 'f' },
        { "settings", required_argument, 0, OPT_SETTINGS },
        { "config", required_argument, 0, OPT_CONFIG },
        { "print-config", no_argument, 0, OPT_PRINT_CONFIG },
        { "clipboard-file", required_argument, 0, OPT_CLIPBOARD_FILE },
        { "gadgets-file", required_argument, 0, OPT_GADGETS_FILE },
        { "lock-file", required_argument, 0, OPT_LOCK_FILE },
        { "screenshot-dir", required_argument, 0, OPT_SCREENSHOT_DIR },
        { "logind", required_argument, 0, OPT_LOGIND },
        { "renderer", required_argument, 0, OPT_RENDERER },
        { "output", required_argument, 0, OPT_OUTPUT },
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
    const char *settings_arg = NULL, *config_arg = NULL;
    bool headless_arg = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config_arg = argv[i + 1];
        else if (!strncmp(argv[i], "--config=", 9))
            config_arg = argv[i] + 9;
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc)
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
    static char conf_buf[PATH_MAX];
    if (!config_arg && !headless_arg) {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(conf_buf, sizeof(conf_buf), "%s/mellagui/mellagui.conf", xdg);
        else if (home && *home)
            snprintf(conf_buf, sizeof(conf_buf), "%s/.config/mellagui/mellagui.conf", home);
        if (conf_buf[0])
            config_arg = conf_buf;
    }
    /* the programs we start read [theme] from the same file; "none" (headless) keeps them off the user's file too */
    setenv("MELLAGUI_CONFIG", config_arg && *config_arg ? config_arg : "none", 1);
    if (config_arg && *config_arg && strcmp(config_arg, "none")) {
        /* mellagui.conf: the hand-written configuration (dock, commands ...); the command line still wins */
        snprintf(g_conf_path, sizeof(g_conf_path), "%s", config_arg);
        cfg.conf_path = g_conf_path;
        int bad = lm_conf_load(config_arg, conf_line, &cfg);
        if (bad < 0 && config_arg != conf_buf)
            fprintf(stderr, "mellagui: cannot read --config %s: %s\n", config_arg, strerror(errno));
        else if (bad > 0)
            fprintf(stderr, "mellagui: %s: %d line(s) ignored\n", config_arg, bad);
        lm_metrics_fixup();
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
        case 't': lm_launch_set_command("terminal", optarg); break;
        case OPT_CONFIG: break;                     /* handled before the loop (read first, so these options win) */
        case OPT_PRINT_CONFIG: fputs(lm_config_default_text(), stdout); return 0;
        case OPT_RUN: lm_launch_set_command("run", optarg); break;
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
        case OPT_OUTPUT: {
            int w, h;
            if (sscanf(optarg, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384 || cfg.headless_nextra >= 3) {
                fprintf(stderr, "mellagui: bad --output '%s' (expected WxH, at most 3)\n", optarg);
                return 2;
            }
            cfg.headless_extra[cfg.headless_nextra][0] = w;
            cfg.headless_extra[cfg.headless_nextra++][1] = h;
            break;
        }
        case OPT_RENDERER:
            if (!strcmp(optarg, "software")) cfg.renderer = 0;
            else if (!strcmp(optarg, "auto")) cfg.renderer = 1;
            else if (!strcmp(optarg, "gl")) cfg.renderer = 2;
            else { fprintf(stderr, "mellagui: --renderer wants software, auto or gl, not '%s'\n", optarg); return 2; }
            break;
        case OPT_LOGIND:
            if (!strcmp(optarg, "off")) cfg.logind = 0;
            else if (!strcmp(optarg, "auto")) cfg.logind = 1;
            else if (!strcmp(optarg, "on")) cfg.logind = 2;
            else { fprintf(stderr, "mellagui: --logind wants off, auto or on, not '%s'\n", optarg); return 2; }
            break;
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

    lm_launch_resolve(&cfg);                  /* terminal / run / settings / view commands */
    lm_dock_resolve();                        /* the dock's buttons (after the commands are known: @terminal ...) */
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
    lm_loop_add_signal(loop, SIGUSR2, sig_reload, srv);
    srv->reload_config = reload_config;

    bool headless = !strcmp(cfg.backend, "headless");
    if (!headless && cfg.logind > 0) {
        char why[200];
        srv->logind = lm_logind_open(loop, why, sizeof(why));
        if (!srv->logind) {
            if (cfg.logind == 2) {
                lm_error("logind is required (--logind on) but not usable: %s", why);
                goto out;
            }
            lm_info("logind not used: %s; opening the device files directly (needs groups video and input)", why);
        }
    }
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
        !(srv->settings = lm_settings_create(srv)) ||
        !(srv->capture = lm_capture_create(srv)) ||
        !(srv->idle_inhibit = lm_idle_inhibit_create(srv)) ||
        !(srv->menu = lm_menu_create(srv)) ||
        !(srv->tray = lm_tray_create(srv))) {
        lm_error("module initialisation failed");
        goto out;
    }
    lm_render_set_wallpaper(srv->render, cfg.wallpaper, cfg.wallpaper_mode);            /* from the settings file */
    lm_input_set_handler(srv->input, lm_seat_handle_input_cb, srv->seat);
    srv->lock = lm_lock_create(srv);            /* with start=1 in lock.conf the screen is locked from the first frame */
    srv->saver = lm_saver_create(srv);
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
    srv->dmabuf = lm_dmabuf_create(srv, cfg.dmabuf);        /* after the backend: the GL context says which layouts the GPU can sample */
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
    if (srv->wm && (!headless || getenv("MELLAGUI_CALENDAR_PRELOAD")))      /* the calendar waits hidden for a click on the clock (tests start it by that click) */
        lm_wm_calendar_start(srv->wm);

    rc = lm_loop_run(loop);

out:
    /* Reverse creation order. */
    if (srv->dmabuf) lm_dmabuf_destroy(srv->dmabuf);
    srv->dmabuf = NULL;
    if (srv->mixer) lm_mixer_destroy(srv->mixer);
    srv->mixer = NULL;
    if (srv->sysinfo) lm_sysinfo_destroy(srv->sysinfo);
    srv->sysinfo = NULL;
    if (srv->saver) lm_saver_destroy(srv->saver);
    srv->saver = NULL;
    if (srv->lock) lm_lock_destroy(srv->lock);
    srv->lock = NULL;
    if (srv->idle_inhibit) lm_idle_inhibit_destroy(srv->idle_inhibit);
    srv->idle_inhibit = NULL;
    if (srv->menu) lm_menu_destroy(srv->menu);
    srv->menu = NULL;
    if (srv->tray) lm_tray_destroy(srv->tray);
    srv->tray = NULL;
    if (srv->capture) lm_capture_destroy(srv->capture);
    srv->capture = NULL;
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
    lm_logind_close(srv->logind);               /* after everything that holds devices: hands them back */
    srv->logind = NULL;
    g_backend = NULL;
    lm_server_destroy(srv);
    lm_loop_destroy(loop);
    lm_log_close();
    (void)g_srv;
    return rc;
}

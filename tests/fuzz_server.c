/*
 * fuzz_server: coverage-guided, structure-aware fuzzer for the whole compositor, in one process.
 *
 * Why: every Wayland client is untrusted input, and the request handlers (server.c, surface.c, shm.c, xdg.c, seat.c, dmabuf.c,
 * settings.c ...) are hand-written C. There is no clang / libFuzzer / AFL on the dev machine, so this is a small one of our own:
 *
 *   - the compositor is built into this binary (everything in src/ except main.c) with ASan + UBSan and
 *     -fsanitize-coverage=trace-pc; tests/fuzz_cov.c turns that into an edge bitmap,
 *   - an input is just bytes; a generator turns it into a *valid-looking* conversation (it knows every interface, request and
 *     argument type from protocol.c, tracks the objects it created, learns configure serials from the events it gets back) with
 *     deliberately nasty values mixed in: huge sizes, negative numbers, dead / foreign object ids, memfds of odd sizes that are
 *     truncated afterwards (SIGBUS), garbage frames,
 *   - between requests it also drives the compositor side: keys, pointer, touch, the lock screen, settings keys, a render flush,
 *   - inputs that reach new edges join the corpus and are mutated (bit flips, splices, insertions ...).
 *
 *   build/fuzz-server [-t SECONDS] [-s SEED] [-n MAX_EXECS] [-c CORPUS_DIR] [-o CRASH_FILE]
 *   build/fuzz-server -r FILE            replay one input (a crash file) and exit
 *
 * The input being run is written to the crash file (default build/fuzz/current.bin) BEFORE each run, so when ASan / UBSan
 * aborts the process the file is the reproducer. Findings go to tests/test_mellagui.py as regression tests once fixed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include "backend.h"
#include "dmabuf.h"
#include "fuzz_cov.h"
#include "input.h"
#include "lock.h"
#include "log.h"
#include "loop.h"
#include "output.h"
#include "protocol.h"
#include "render.h"
#include "seat.h"
#include "server.h"
#include "settings.h"
#include "shm.h"
#include "surface.h"
#include "wire.h"
#include "wm.h"
#include "xdg.h"

const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_leaks=0:abort_on_error=1:handle_segv=1:allocator_may_return_null=1";
}

/* ---- the compositor, set up like main.c does ---------------------------------------------------------------------- */

static struct lm_loop *loop;
static struct lm_server *srv;
static char sockpath[256];

static void on_output_added(void *d, struct lm_output *o)
{
    struct lm_server *s = d;
    if (s->outputs) lm_output_mgr_add(s->outputs, o);
    if (s->render) lm_render_output_added(s->render, o);
    if (s->wm) lm_wm_output_added(s->wm, o);
}
static void on_output_changed(void *d, struct lm_output *o)
{
    struct lm_server *s = d;
    if (s->outputs) lm_output_mgr_changed(s->outputs, o);
    if (s->render) lm_render_output_changed(s->render, o);
    if (s->wm) lm_wm_output_changed(s->wm, o);
}
static void on_output_removed(void *d, struct lm_output *o)
{
    struct lm_server *s = d;
    if (s->wm) lm_wm_output_removed(s->wm, o);
    if (s->render) lm_render_output_removed(s->render, o);
    if (s->outputs) lm_output_mgr_remove(s->outputs, o);
}
static void on_output_ready(void *d, struct lm_output *o, uint32_t t)
{
    struct lm_server *s = d;
    if (s->render) lm_render_output_ready(s->render, o, t);
}
static void on_session_active(void *d, bool a)
{
    (void)d;
    (void)a;
}

static void setup(void)
{
    char dir[] = "/tmp/mellagui-fuzz-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(2);
    }
    setenv("XDG_RUNTIME_DIR", dir, 1);
    setenv("PATH", "/nonexistent", 1);                      /* nothing the compositor spawns may find a program */
    setenv("MELLAGUI_NO_GADGET_PROCS", "1", 1);             /* the gadget switches must not start programs that connect to the server */
    setenv("MELLAGUI_LOCK_ITER", "1000", 1);
    setenv("MELLAGUI_LOCK_UNIT_MS", "60000", 1);
    signal(SIGPIPE, SIG_IGN);
    lm_log_set_level(LM_LOG_ERROR);

    struct lm_config cfg;
    lm_config_defaults(&cfg);
    cfg.backend = "headless";
    cfg.headless_width = 640;
    cfg.headless_height = 400;
    cfg.socket_name = "fuzz";
    cfg.terminal_cmd = cfg.run_cmd = cfg.settings_cmd = ":";
    cfg.kbd_nlayouts = 2;
    cfg.kbd_layouts[0] = 0;
    cfg.kbd_layouts[1] = 1;
    cfg.sound = false;
    cfg.grab_input = false;

    loop = lm_loop_create();
    srv = lm_server_create(loop, &cfg);
    srv->backend = lm_backend_headless_create(srv, cfg.headless_width, cfg.headless_height);
    srv->backend->listener = (struct lm_backend_listener){ .data = srv, .output_added = on_output_added,
                                                           .output_changed = on_output_changed, .output_removed = on_output_removed,
                                                           .output_ready = on_output_ready, .session_active = on_session_active };
    if (!(srv->render = lm_render_create(srv)) || !(srv->compositor = lm_compositor_create(srv)) ||
        !(srv->shm = lm_shm_create(srv)) || !(srv->outputs = lm_output_mgr_create(srv)) || !(srv->seat = lm_seat_create(srv)) ||
        !(srv->xdg = lm_xdg_create(srv)) || !(srv->wm = lm_wm_create(srv)) || !(srv->input = lm_input_create(srv)) ||
        !(srv->settings = lm_settings_create(srv))) {
        fprintf(stderr, "fuzz-server: module initialisation failed\n");
        exit(2);
    }
    lm_input_set_handler(srv->input, lm_seat_handle_input_cb, srv->seat);
    srv->lock = lm_lock_create(srv);
    srv->dmabuf = lm_dmabuf_create(srv, true);
    if (lm_server_listen(srv) < 0 || lm_backend_init(srv->backend) < 0) {
        fprintf(stderr, "fuzz-server: cannot listen / init the backend\n");
        exit(2);
    }
    snprintf(sockpath, sizeof(sockpath), "%s/%s", dir, lm_server_socket_name(srv));
    lm_lock_apply(srv->lock, "", "pin", "1357", "0", "0", "0", "1");
}

static void pump(int rounds)
{
    for (int i = 0; i < rounds; i++)
        lm_loop_dispatch(loop, 0);
}

/* ---- the byte stream that drives everything ---------------------------------------------------------------------------- */

struct rd {
    const uint8_t *p;
    size_t n, i;
};

static uint8_t rd8(struct rd *r) { return r->i < r->n ? r->p[r->i++] : 0; }
static uint32_t rd32(struct rd *r)
{
    uint32_t v = 0;
    for (int k = 0; k < 4; k++)
        v |= (uint32_t)rd8(r) << (8 * k);
    return v;
}
static bool rd_more(const struct rd *r) { return r->i < r->n; }

/* ---- the fuzzing client ------------------------------------------------------------------------------------------------- */

#define MAX_OBJ 160
#define MAX_FDS 24
#define SERIALS 8

struct obj {
    uint32_t id;
    const struct lm_interface *iface;
};

struct cli {
    int fd;
    struct lm_wire w;
    struct obj objs[MAX_OBJ];
    int nobj;
    uint32_t next_id;
    uint32_t serials[SERIALS];
    int nserial;
    uint32_t last_serial;               /* the newest one: what a client that behaves answers a configure with */
    int memfds[MAX_FDS];                /* kept so that they can be truncated while the server maps them */
    int nmemfd;
    bool dead;
};

static void obj_add(struct cli *c, uint32_t id, const struct lm_interface *iface)
{
    if (c->nobj < MAX_OBJ)
        c->objs[c->nobj++] = (struct obj){ id, iface };
    else
        c->objs[id % MAX_OBJ] = (struct obj){ id, iface };      /* replace an old one: the object table churns */
}

static void cli_connect(struct cli *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, sockpath, sizeof(sa.sun_path) - 1);
    if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("fuzz client connect");
        exit(2);
    }
    lm_wire_init(&c->w, c->fd, 8 * 1024 * 1024);
    c->next_id = 3;
    obj_add(c, 1, &lm_wl_display_interface);
    obj_add(c, 2, &lm_wl_registry_interface);
    union lm_arg a[1] = { { .n = 2 } };
    lm_wire_send(&c->w, 1, 1, "n", a);                          /* wl_display.get_registry */
    lm_wire_flush(&c->w);
    pump(3);
}

static void cli_close(struct cli *c)
{
    for (int i = 0; i < c->nmemfd; i++)
        close(c->memfds[i]);
    lm_wire_finish(&c->w);
    close(c->fd);
}

/* Read what the compositor sent: learn serials (configure / ping / enter ...) and server-made objects, close received fds. */
static void cli_drain(struct cli *c)
{
    for (int round = 0; round < 8; round++) {
        ssize_t n = lm_wire_recv(&c->w);
        if (n <= 0) {
            if (n == 0)
                c->dead = true;
            break;
        }
        struct lm_msg_hdr h;
        int got;
        while ((got = lm_wire_next(&c->w, &h)) > 0) {
            const struct lm_interface *iface = NULL;
            for (int i = 0; i < c->nobj; i++)
                if (c->objs[i].id == h.id)
                    iface = c->objs[i].iface;
            const struct lm_message *m = iface && (int)h.opcode < iface->n_events ? &iface->events[h.opcode] : NULL;
            if (!m) {
                lm_wire_discard(&c->w, &h);
                continue;
            }
            union lm_arg args[LM_WIRE_MAX_ARGS];
            int k = lm_wire_decode(&c->w, &h, m->signature, args, LM_WIRE_MAX_ARGS);
            if (k < 0)
                continue;
            int ai = 0;
            for (const char *s = m->signature; *s; s++) {
                if (*s == '?' || (*s >= '0' && *s <= '9'))
                    continue;
                if (*s == 'u' && ai == 0) {
                    c->last_serial = args[ai].u;
                    if (c->nserial < SERIALS)
                        c->serials[c->nserial++] = args[ai].u;
                    else
                        c->serials[args[ai].u % SERIALS] = args[ai].u;
                }
                if (*s == 'n' && m->types && m->types[ai])
                    obj_add(c, args[ai].n, m->types[ai]);
                if (*s == 'h' && args[ai].h >= 0)
                    close(args[ai].h);
                ai++;
            }
        }
        if (got < 0) {
            c->dead = true;
            break;
        }
    }
}

static const uint32_t INTERESTING[] = {
    0, 1, 2, 3, 4, 7, 8, 15, 16, 32, 64, 100, 127, 128, 255, 256, 320, 640, 1000, 4095, 4096, 4097, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x3fffffff, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu, 0x34325241u /* AR24 */, 0x34325258u /* XR24 */,
};
#define N_INTERESTING ((int)(sizeof(INTERESTING) / sizeof(INTERESTING[0])))

static uint32_t gen_uint(struct cli *c, struct rd *r)
{
    uint8_t sel = rd8(r);
    switch (sel % 6) {
    case 0: case 1: return INTERESTING[rd8(r) % N_INTERESTING];
    case 2: return rd8(r);
    case 3: return rd32(r);
    case 4: return c->nserial ? c->serials[rd8(r) % (uint8_t)c->nserial] : rd8(r);          /* a serial the server really sent */
    default: return (uint32_t)(rd8(r) % 8) + 1;
    }
}

static int make_memfd(struct cli *c, struct rd *r)
{
    static const size_t sizes[] = { 0, 1, 15, 4096, 4097, 65536, 1u << 20, 4u << 20, 1u << 24 };
    int fd = memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t sz = sizes[rd8(r) % (sizeof(sizes) / sizeof(sizes[0]))];
    if (sz && ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return -1;
    }
    if (sz) {                                                                                  /* some content: the server reads it */
        uint8_t pat = rd8(r);
        size_t len = sz < 65536 ? sz : 65536;
        uint8_t *p = malloc(len);
        if (p) {
            memset(p, pat, len);
            if (pwrite(fd, p, len, 0) < 0) {}
            free(p);
        }
    }
    if (c->nmemfd < MAX_FDS)
        c->memfds[c->nmemfd++] = dup(fd);
    return fd;
}

static int gen_fd(struct cli *c, struct rd *r)
{
    switch (rd8(r) % 5) {
    case 0: { int p[2]; return pipe2(p, O_CLOEXEC) == 0 ? (close(p[1]), p[0]) : -1; }
    case 1: return open("/dev/null", O_RDWR | O_CLOEXEC);
    case 2: return open("/dev/zero", O_RDONLY | O_CLOEXEC);
    default: return make_memfd(c, r);
    }
}

static struct obj *pick_obj(struct cli *c, struct rd *r, const struct lm_interface *want)
{
    if (c->nobj == 0)
        return NULL;
    int start = rd8(r) % c->nobj;
    if (!want)
        return &c->objs[start];
    for (int k = 0; k < c->nobj; k++) {
        struct obj *o = &c->objs[(start + k) % c->nobj];
        if (o->iface == want)
            return o;
    }
    return NULL;
}

static const char *STRINGS[] = {
    "", "a", "title", "mellagui.app", "text/plain", "text/plain;charset=utf-8", "UTF8_STRING", "\xff\xfe\xfd", "%s%n%x",
    "0", "1", "us,ee", "1024x768@60", "auto", "preferred", "layout", "lock-now", "\t\t\t\t\t", "../../etc/passwd",
};

static void gen_string(struct rd *r, char *out, size_t cap)
{
    uint8_t sel = rd8(r) % 4;
    if (sel == 0) {
        snprintf(out, cap, "%s", STRINGS[rd8(r) % (sizeof(STRINGS) / sizeof(STRINGS[0]))]);
    } else if (sel == 1) {
        size_t n = rd8(r) % 24;
        for (size_t i = 0; i < n && i + 1 < cap; i++)
            out[i] = (char)(rd8(r) | 1);
        out[n < cap ? n : cap - 1] = 0;
    } else if (sel == 2) {
        size_t n = 2000 + rd8(r) * 8;                          /* long, but still one message */
        if (n >= cap)
            n = cap - 1;
        memset(out, 'x' + (rd8(r) & 3), n);
        out[n] = 0;
    } else {
        size_t n = rd8(r) % 12;
        for (size_t i = 0; i < n; i++)
            out[i] = (char)('a' + rd8(r) % 26);
        out[n] = 0;
    }
}

/* One request on a tracked object, arguments by the signature of the protocol table. */
static void send_request(struct cli *c, struct rd *r)
{
    struct obj *o = pick_obj(c, r, NULL);
    uint32_t id = o ? o->id : 1;
    const struct lm_interface *iface = o ? o->iface : &lm_wl_display_interface;
    uint8_t special = rd8(r);
    if (special % 16 == 0)                                                                       /* a guess at a server-made id */
        id = LM_ID_SERVER_BASE + (uint32_t)(rd8(r) % 6), iface = &lm_wl_data_offer_interface;
    else if (special % 16 == 1)                                                                  /* an id nobody has */
        id = 3 + (uint32_t)(rd8(r) % 200);

    uint32_t opcode = (uint32_t)(rd8(r) % (iface->n_requests + 1));
    if ((int)opcode >= iface->n_requests) {                                                      /* an opcode that does not exist */
        union lm_arg none[1];
        lm_wire_send(&c->w, id, opcode, "", none);
        return;
    }
    const struct lm_message *m = &iface->requests[opcode];
    union lm_arg args[LM_WIRE_MAX_ARGS];
    char strs[LM_WIRE_MAX_ARGS][4200];
    int fds[LM_WIRE_MAX_ARGS], nfds = 0;
    int ai = 0;
    uint32_t created_id = 0;
    const struct lm_interface *created_iface = NULL;
    uint32_t bind_ver = 0;
    bool is_bind = iface == &lm_wl_registry_interface && opcode == 0;
    struct lm_global *bound = NULL;
    for (const char *s = m->signature; *s; s++) {
        if (*s == '?' || (*s >= '0' && *s <= '9'))
            continue;
        if (ai >= LM_WIRE_MAX_ARGS)
            return;
        switch (*s) {
        case 'i': args[ai].i = (int32_t)gen_uint(c, r); break;
        case 'u':
            args[ai].u = gen_uint(c, r);
            if (is_bind && ai == 0 && !lm_list_empty(&srv->globals)) {                            /* a real global of the compositor */
                int want = rd8(r) % lm_list_length(&srv->globals), k = 0;
                struct lm_global *g;
                lm_list_for_each(g, &srv->globals, link)
                    if (k++ == want)
                        bound = g;
                if (bound && rd8(r) % 8)
                    args[ai].u = bound->name;
            }
            if (is_bind && ai == 2) {
                bind_ver = bound && rd8(r) % 8 ? 1 + rd8(r) % bound->version : gen_uint(c, r);
                args[ai].u = bind_ver;
            }
            break;
        case 'f': args[ai].f = (lm_fixed_t)rd32(r); break;
        case 's':
            if (is_bind && bound && rd8(r) % 8) {
                snprintf(strs[ai], sizeof(strs[ai]), "%s", bound->iface->name);
            } else {
                gen_string(r, strs[ai], sizeof(strs[ai]));
            }
            args[ai].s = strs[ai];
            break;
        case 'a': {
            static uint8_t blob[300];
            size_t n = rd8(r) % 64;
            for (size_t i = 0; i < n; i++)
                blob[i] = rd8(r);
            args[ai].a = (struct lm_array){ (uint32_t)n, blob };
            break;
        }
        case 'h': {
            int fd = gen_fd(c, r);
            args[ai].h = fd;
            if (fd >= 0)
                fds[nfds++] = fd;
            break;
        }
        case 'o': {
            const struct lm_interface *want = m->types ? m->types[ai] : NULL;
            struct obj *t = rd8(r) % 12 ? pick_obj(c, r, want) : NULL;
            args[ai].o = t ? t->id : (rd8(r) % 4 == 0 ? 0 : gen_uint(c, r));                     /* sometimes: nothing, or nonsense */
            break;
        }
        case 'n':
            created_id = rd8(r) % 24 ? c->next_id++ : (rd8(r) % 2 ? gen_uint(c, r) : (uint32_t)(1 + rd8(r) % 8));   /* usually fresh, sometimes reused */
            created_iface = m->types && m->types[ai] ? m->types[ai] : (bound ? bound->iface : NULL);
            args[ai].n = created_id;
            break;
        default: return;
        }
        ai++;
    }
    if (lm_wire_send(&c->w, id, opcode, m->signature, args) == 0 && created_id && created_iface)
        obj_add(c, created_id, created_iface);
    for (int i = 0; i < nfds; i++)
        close(fds[i]);
    (void)bind_ver;
}

/* Raw garbage: a bad header, or random bytes. */
static void send_garbage(struct cli *c, struct rd *r)
{
    uint8_t buf[256];
    uint8_t kind = rd8(r) % 4;
    size_t n;
    if (kind == 0) {                                                                             /* a header that lies about its size */
        uint32_t h[2] = { c->nobj ? c->objs[rd8(r) % c->nobj].id : 1, (rd32(r) & 0xffff0000u) | (rd8(r) % 16) };
        memcpy(buf, h, 8);
        n = 8 + rd8(r) % 24;
        for (size_t i = 8; i < n; i++)
            buf[i] = rd8(r);
    } else if (kind == 1) {                                                                      /* a header with the right size, wrong body */
        uint32_t words = 2 + rd8(r) % 12;
        uint32_t h[2] = { c->nobj ? c->objs[rd8(r) % c->nobj].id : 1, (words * 4u << 16) | (rd8(r) % 14) };
        memcpy(buf, h, 8);
        n = words * 4;
        for (size_t i = 8; i < n; i++)
            buf[i] = rd8(r);
    } else {
        n = 1 + rd8(r);
        for (size_t i = 0; i < n; i++)
            buf[i] = rd8(r);
    }
    lm_wire_flush(&c->w);
    if (send(c->fd, buf, n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {}
}

/* ---- macros: the valid conversations that random requests would never get right ------------------------------------------------ */

#define SEND(c, id, op, sig, ...) do { union lm_arg a_[] = { __VA_ARGS__ }; lm_wire_send(&(c)->w, (id), (op), (sig), a_); } while (0)

static uint32_t new_id(struct cli *c, const struct lm_interface *iface)
{
    uint32_t id = c->next_id++;
    obj_add(c, id, iface);
    return id;
}

static uint32_t first_of(struct cli *c, const struct lm_interface *iface)
{
    for (int i = 0; i < c->nobj; i++)
        if (c->objs[i].iface == iface)
            return c->objs[i].id;
    return 0;
}

static void settle(struct cli *c)
{
    lm_wire_flush(&c->w);
    pump(2);
    cli_drain(c);
}

/* Bind every global the compositor advertises (a real client's first step), then the seat's devices. */
static void macro_setup(struct cli *c, struct rd *r)
{
    struct lm_global *g;
    lm_list_for_each(g, &srv->globals, link) {
        if (first_of(c, g->iface) && rd8(r) % 4)                                                 /* mostly one of each, sometimes more */
            continue;
        uint32_t ver = g->version;
        if (rd8(r) % 4 == 0)
            ver = 1 + rd8(r) % g->version;                                                       /* an older version */
        uint32_t id = new_id(c, g->iface);
        SEND(c, 2, 0, "usun", { .u = g->name }, { .s = g->iface->name }, { .u = ver }, { .n = id });
    }
    uint32_t seat = first_of(c, &lm_wl_seat_interface);
    if (seat) {
        if (rd8(r) % 4) { uint32_t p = new_id(c, &lm_wl_pointer_interface);  SEND(c, seat, LM_WL_SEAT_REQ_GET_POINTER, "n", { .n = p }); }
        if (rd8(r) % 4) { uint32_t k = new_id(c, &lm_wl_keyboard_interface); SEND(c, seat, LM_WL_SEAT_REQ_GET_KEYBOARD, "n", { .n = k }); }
        if (rd8(r) % 2) { uint32_t t = new_id(c, &lm_wl_touch_interface);    SEND(c, seat, LM_WL_SEAT_REQ_GET_TOUCH, "n", { .n = t }); }
        uint32_t ddm = first_of(c, &lm_wl_data_device_manager_interface);
        if (ddm) {
            uint32_t dd = new_id(c, &lm_wl_data_device_interface);
            SEND(c, ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE, "no", { .n = dd }, { .o = seat });
        }
    }
    settle(c);
}

/* A shm buffer of w x h on surface `s`, attached and committed. */
static void attach_buffer(struct cli *c, struct rd *r, uint32_t s, int w, int h)
{
    uint32_t shm = first_of(c, &lm_wl_shm_interface);
    if (!shm)
        return;
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = memfd_create("fuzz-buf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0) close(fd);
        return;
    }
    uint8_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px != MAP_FAILED) {
        memset(px, rd8(r), size);
        munmap(px, size);
    }
    if (c->nmemfd < MAX_FDS)
        c->memfds[c->nmemfd++] = dup(fd);
    uint32_t pool = new_id(c, &lm_wl_shm_pool_interface);
    SEND(c, shm, LM_WL_SHM_REQ_CREATE_POOL, "nhi", { .n = pool }, { .h = fd }, { .i = (int32_t)size });
    close(fd);
    uint32_t buf = new_id(c, &lm_wl_buffer_interface);
    SEND(c, pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, "niiiiu", { .n = buf }, { .i = 0 }, { .i = w }, { .i = h }, { .i = stride },
         { .u = rd8(r) % 2 });
    SEND(c, s, LM_WL_SURFACE_REQ_ATTACH, "?oii", { .o = buf }, { .i = 0 }, { .i = 0 });
    SEND(c, s, LM_WL_SURFACE_REQ_DAMAGE, "iiii", { .i = 0 }, { .i = 0 }, { .i = w }, { .i = h });
    uint32_t cb = new_id(c, &lm_wl_callback_interface);
    SEND(c, s, LM_WL_SURFACE_REQ_FRAME, "n", { .n = cb });
    SEND(c, s, LM_WL_SURFACE_REQ_COMMIT, "");
}

/* surface + xdg_surface + xdg_toplevel, the first commit, the configure answered, a buffer: a window on the screen. */
static void macro_window(struct cli *c, struct rd *r)
{
    uint32_t comp = first_of(c, &lm_wl_compositor_interface), base = first_of(c, &lm_xdg_wm_base_interface);
    if (!comp || !base)
        return;
    uint32_t s = new_id(c, &lm_wl_surface_interface);
    SEND(c, comp, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, "n", { .n = s });
    uint32_t x = new_id(c, &lm_xdg_surface_interface);
    SEND(c, base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, "no", { .n = x }, { .o = s });
    uint32_t t = new_id(c, &lm_xdg_toplevel_interface);
    SEND(c, x, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, "n", { .n = t });
    uint32_t dm = first_of(c, &lm_zxdg_decoration_manager_v1_interface);
    if (dm && rd8(r) % 2) {
        uint32_t d = new_id(c, &lm_zxdg_toplevel_decoration_v1_interface);
        SEND(c, dm, LM_ZXDG_DECORATION_MANAGER_V1_REQ_GET_TOPLEVEL_DECORATION, "no", { .n = d }, { .o = t });
    }
    if (rd8(r) % 2) {
        char title[64];
        gen_string(r, title, sizeof(title));
        SEND(c, t, LM_XDG_TOPLEVEL_REQ_SET_TITLE, "s", { .s = title });
    }
    SEND(c, s, LM_WL_SURFACE_REQ_COMMIT, "");                                                    /* no buffer yet: asks for the configure */
    settle(c);
    SEND(c, x, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, "u", { .u = rd8(r) % 8 ? c->last_serial : gen_uint(c, r) });
    int w = 1 + (int)(rd8(r) % 3 == 0 ? rd8(r) : rd8(r) % 200), h = 1 + (int)(rd8(r) % 3 == 0 ? rd8(r) : rd8(r) % 150);
    attach_buffer(c, r, s, w, h);
    settle(c);
}

static void macro_popup(struct cli *c, struct rd *r)
{
    uint32_t comp = first_of(c, &lm_wl_compositor_interface), base = first_of(c, &lm_xdg_wm_base_interface);
    uint32_t parent = first_of(c, &lm_xdg_surface_interface);
    if (!comp || !base)
        return;
    uint32_t pos = new_id(c, &lm_xdg_positioner_interface);
    SEND(c, base, LM_XDG_WM_BASE_REQ_CREATE_POSITIONER, "n", { .n = pos });
    SEND(c, pos, LM_XDG_POSITIONER_REQ_SET_SIZE, "ii", { .i = 1 + rd8(r) % 100 }, { .i = 1 + rd8(r) % 100 });
    SEND(c, pos, LM_XDG_POSITIONER_REQ_SET_ANCHOR_RECT, "iiii", { .i = (int8_t)rd8(r) }, { .i = (int8_t)rd8(r) },
         { .i = 1 + rd8(r) % 50 }, { .i = 1 + rd8(r) % 50 });
    uint32_t s = new_id(c, &lm_wl_surface_interface);
    SEND(c, comp, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, "n", { .n = s });
    uint32_t x = new_id(c, &lm_xdg_surface_interface);
    SEND(c, base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, "no", { .n = x }, { .o = s });
    uint32_t p = new_id(c, &lm_xdg_popup_interface);
    SEND(c, x, LM_XDG_SURFACE_REQ_GET_POPUP, "n?oo", { .n = p }, { .o = rd8(r) % 8 ? parent : 0 }, { .o = pos });
    uint32_t seat = first_of(c, &lm_wl_seat_interface);
    if (seat && rd8(r) % 2)
        SEND(c, p, 1, "ou", { .o = seat }, { .u = c->last_serial });                              /* xdg_popup.grab */
    SEND(c, s, LM_WL_SURFACE_REQ_COMMIT, "");
    settle(c);
    SEND(c, x, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, "u", { .u = c->last_serial });
    attach_buffer(c, r, s, 1 + rd8(r) % 120, 1 + rd8(r) % 90);
    settle(c);
}

static void macro_subsurface(struct cli *c, struct rd *r)
{
    uint32_t comp = first_of(c, &lm_wl_compositor_interface), sub = first_of(c, &lm_wl_subcompositor_interface);
    if (!comp || !sub)
        return;
    struct obj *parent = pick_obj(c, r, &lm_wl_surface_interface);
    if (!parent)
        return;
    uint32_t s = new_id(c, &lm_wl_surface_interface);
    SEND(c, comp, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, "n", { .n = s });
    uint32_t ss = new_id(c, &lm_wl_subsurface_interface);
    SEND(c, sub, LM_WL_SUBCOMPOSITOR_REQ_GET_SUBSURFACE, "noo", { .n = ss }, { .o = s }, { .o = parent->id });
    SEND(c, ss, 1, "ii", { .i = (int8_t)rd8(r) }, { .i = (int8_t)rd8(r) });                       /* set_position */
    attach_buffer(c, r, s, 1 + rd8(r) % 80, 1 + rd8(r) % 60);
    SEND(c, parent->id, LM_WL_SURFACE_REQ_COMMIT, "");
    settle(c);
}

static void macro_selection(struct cli *c, struct rd *r)
{
    uint32_t ddm = first_of(c, &lm_wl_data_device_manager_interface), dd = first_of(c, &lm_wl_data_device_interface);
    if (!ddm || !dd)
        return;
    uint32_t src = new_id(c, &lm_wl_data_source_interface);
    SEND(c, ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE, "n", { .n = src });
    char mime[64];
    gen_string(r, mime, sizeof(mime));
    SEND(c, src, LM_WL_DATA_SOURCE_REQ_OFFER, "s", { .s = mime });
    SEND(c, dd, LM_WL_DATA_DEVICE_REQ_SET_SELECTION, "?ou", { .o = src }, { .u = c->last_serial });
    settle(c);
}

/* ---- the compositor side ----------------------------------------------------------------------------------------------------- */

static const char *SETTING_KEYS[] = {
    "layouts", "layout", "sticky", "repeat-delay", "repeat-rate", "pointer-speed", "natural-scroll", "sound", "gadget", "mixer",
    "sysinfo", "resolution", "audio-out", "resolution-confirm", "lock-set", "lock-now", "file", "nonsense",
};

static void drive_compositor(struct cli *c, struct rd *r)
{
    struct lm_input *in = srv->input;
    uint8_t op = rd8(r) % 16;
    struct lm_input_event ev = { 0 };
    ev.time_ms = lm_now_msec32();
    switch (op) {
    case 0: case 1: case 2: {                                                                   /* a key, often with a modifier before */
        uint32_t mod[] = { KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_CAPSLOCK };
        uint32_t code = rd8(r) % 3 == 0 ? (uint32_t)(rd8(r) % 120) : (uint32_t)(KEY_A + rd8(r) % 40);
        bool use_mod = rd8(r) % 2;
        uint32_t m = mod[rd8(r) % 6];
        ev.type = LM_INPUT_KEY;
        if (use_mod) { ev.u.key.code = m; ev.u.key.pressed = true; lm_input_inject(in, &ev); }
        ev.u.key.code = code; ev.u.key.pressed = true; lm_input_inject(in, &ev);
        ev.u.key.pressed = false; lm_input_inject(in, &ev);
        if (use_mod) { ev.u.key.code = m; ev.u.key.pressed = false; lm_input_inject(in, &ev); }
        break;
    }
    case 3: case 4:
        ev.type = LM_INPUT_POINTER_MOTION_ABS;
        ev.u.motion_abs.x = rd8(r) / 255.0;
        ev.u.motion_abs.y = rd8(r) / 255.0;
        lm_input_inject(in, &ev);
        break;
    case 5:
        ev.type = LM_INPUT_POINTER_MOTION;
        ev.u.motion.dx = (int8_t)rd8(r);
        ev.u.motion.dy = (int8_t)rd8(r);
        lm_input_inject(in, &ev);
        break;
    case 6: case 7:
        ev.type = LM_INPUT_POINTER_BUTTON;
        ev.u.button.code = rd8(r) % 3 == 0 ? (uint32_t)BTN_RIGHT : (uint32_t)BTN_LEFT;
        ev.u.button.pressed = true;
        lm_input_inject(in, &ev);
        ev.u.button.pressed = rd8(r) % 4 == 0;                                                  /* sometimes the release is missing */
        lm_input_inject(in, &ev);
        break;
    case 8:
        ev.type = LM_INPUT_POINTER_AXIS;
        ev.u.axis.orientation = rd8(r) % 3;
        ev.u.axis.value = (int8_t)rd8(r);
        ev.u.axis.discrete = (int8_t)rd8(r) % 5;
        lm_input_inject(in, &ev);
        break;
    case 9:
        ev.type = rd8(r) % 3 == 0 ? LM_INPUT_TOUCH_UP : rd8(r) % 2 ? LM_INPUT_TOUCH_DOWN : LM_INPUT_TOUCH_MOTION;
        ev.u.touch.id = rd8(r) % 4;
        ev.u.touch.x = rd8(r) / 255.0;
        ev.u.touch.y = rd8(r) / 255.0;
        lm_input_inject(in, &ev);
        break;
    case 10: {                                                                                   /* a setting */
        char val[2000];
        gen_string(r, val, sizeof(val));
        if (rd8(r) % 4 == 0)                                                                     /* settings take long values too */
            memset(val, 'v', sizeof(val) - 1), val[sizeof(val) - 1 - rd8(r) % 1500] = 0;
        if (rd8(r) % 3 == 0)
            snprintf(val, sizeof(val), "%s\t%s\t%s\t%u\t%u\t%u\t%u", rd8(r) % 2 ? "1357" : "x", rd8(r) % 2 ? "pin" : "keep",
                     rd8(r) % 2 ? "2468" : "", rd32(r) % 5, rd32(r) % 3, rd32(r) % 5, rd32(r) % 3);
        lm_settings_set(srv->settings, SETTING_KEYS[rd8(r) % (sizeof(SETTING_KEYS) / sizeof(SETTING_KEYS[0]))], val);
        break;
    }
    case 11:                                                                                     /* the lock screen: lock, type the PIN */
        lm_lock_now(srv->lock);
        if (rd8(r) % 2) {
            ev.type = LM_INPUT_KEY;
            static const uint32_t pin[] = { KEY_1, KEY_3, KEY_5, KEY_7, KEY_ENTER };
            for (int i = 0; i < 5; i++) {
                ev.u.key.code = pin[i]; ev.u.key.pressed = true; lm_input_inject(in, &ev);
                ev.u.key.pressed = false; lm_input_inject(in, &ev);
            }
        }
        break;
    case 12: lm_render_flush(srv->render); break;
    case 13:                                                                                     /* truncate a mapped memfd: SIGBUS path */
        if (c->nmemfd) {
            int fd = c->memfds[rd8(r) % c->nmemfd];
            if (ftruncate(fd, (off_t)(rd8(r) % 3 ? 0 : 4096 * (rd8(r) % 4))) < 0) {}
        }
        break;
    case 14: pump(2); break;
    default: break;
    }
}

/* ---- one run ---------------------------------------------------------------------------------------------------------------------- */

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

static long stat_windows, stat_runs_with_window;

/* How deep did this connection get? Counts the windows on screen. */
static void note_depth(void)
{
    char desc[4096];
    lm_wm_describe(srv->wm, desc, sizeof(desc));
    for (const char *p = desc; (p = strstr(p, "\nwindow ")); p++)
        stat_windows++;
    stat_runs_with_window += strstr(desc, "\nwindow ") != NULL;
}

static void run_input(const uint8_t *data, size_t len)
{
    struct rd r = { data, len, 0 };
    struct cli c;
    cli_connect(&c);
    int steps = 0, reconnects = 0;
    macro_setup(&c, &r);
    while (rd_more(&r) && steps++ < 400) {
        if (c.dead) {                                                                            /* a protocol error: connect again */
            if (reconnects++ >= 8)
                break;
            note_depth();
            cli_close(&c);
            for (int i = 0; i < 20 && !lm_list_empty(&srv->clients); i++)
                pump(2);
            cli_connect(&c);
            macro_setup(&c, &r);
        }
        uint8_t kind = rd8(&r) % 44;
        if (kind < 20 || kind >= 40) {
            send_request(&c, &r);
        } else if (kind < 22) {
            send_garbage(&c, &r);
        } else if (kind < 28) {
            drive_compositor(&c, &r);
        } else if (kind < 34) {
            macro_window(&c, &r);
        } else if (kind == 34) {
            macro_popup(&c, &r);
        } else if (kind == 35 || kind == 36) {
            macro_subsurface(&c, &r);
        } else if (kind == 37) {
            macro_selection(&c, &r);
        } else {
            macro_setup(&c, &r);
        }
        if (steps % 3 == 0 || (kind >= 20 && kind < 40)) {
            lm_wire_flush(&c.w);
            pump(2);
            cli_drain(&c);
        }
    }
    lm_wire_flush(&c.w);
    pump(3);
    cli_drain(&c);
    note_depth();
    cli_close(&c);
    for (int i = 0; i < 20 && !lm_list_empty(&srv->clients); i++)                                  /* the server notices the hang-up */
        pump(2);
    if (!lm_list_empty(&srv->clients)) {
        fprintf(stderr, "fuzz-server: a client is still connected after it hung up\n");
        abort();
    }
    /* reset the session so that runs do not depend on each other */
    if (lm_lock_active(srv->lock)) {
        struct lm_input_event ev = { .type = LM_INPUT_KEY };
        static const uint32_t pin[] = { KEY_ESC, KEY_1, KEY_3, KEY_5, KEY_7, KEY_ENTER };
        for (int round = 0; round < 8 && lm_lock_active(srv->lock); round++) {
            for (int i = 0; i < 6; i++) {
                ev.u.key.code = pin[i]; ev.u.key.pressed = true; lm_input_inject(srv->input, &ev);
                ev.u.key.pressed = false; lm_input_inject(srv->input, &ev);
            }
            pump(1);
        }
    }
    if (!lm_lock_configured(srv->lock))
        lm_lock_apply(srv->lock, "", "pin", "1357", "0", "0", "0", "1");
    pump(2);
}

/* ---- corpus and mutation --------------------------------------------------------------------------------------------------------- */

struct entry {
    uint8_t *data;
    size_t len;
};

static struct entry *corpus;
static size_t ncorpus, capcorpus;
static uint64_t rng_state = 88172645463325252ull;

static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void corpus_add(const uint8_t *d, size_t n, const char *dir)
{
    if (ncorpus == capcorpus) {
        capcorpus = capcorpus ? capcorpus * 2 : 256;
        corpus = realloc(corpus, capcorpus * sizeof(*corpus));
    }
    corpus[ncorpus].data = malloc(n ? n : 1);
    memcpy(corpus[ncorpus].data, d, n);
    corpus[ncorpus].len = n;
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%06zu.bin", dir, ncorpus);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(d, 1, n, f);
            fclose(f);
        }
    }
    ncorpus++;
}

#define MAX_LEN 4096

static size_t mutate(uint8_t *out, const struct entry *base)
{
    size_t n = base->len < MAX_LEN ? base->len : MAX_LEN;
    memcpy(out, base->data, n);
    int rounds = 1 + (int)(rnd() % 4);
    for (int k = 0; k < rounds; k++) {
        switch (rnd() % 8) {
        case 0: if (n) out[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) out[rnd() % n] = (uint8_t)rnd(); break;
        case 2: if (n) { size_t p = rnd() % n; out[p] = (uint8_t)(out[p] + (int)(rnd() % 9) - 4); } break;
        case 3:                                                                                  /* insert random bytes */
            if (n + 16 < MAX_LEN) {
                size_t p = n ? rnd() % n : 0, cnt = 1 + rnd() % 12;
                memmove(out + p + cnt, out + p, n - p);
                for (size_t i = 0; i < cnt; i++)
                    out[p + i] = (uint8_t)rnd();
                n += cnt;
            }
            break;
        case 4:                                                                                  /* delete a chunk */
            if (n > 4) {
                size_t p = rnd() % n, cnt = 1 + rnd() % 12;
                if (p + cnt > n) cnt = n - p;
                memmove(out + p, out + p + cnt, n - p - cnt);
                n -= cnt;
            }
            break;
        case 5:                                                                                  /* splice a piece of another entry */
            if (ncorpus > 1) {
                const struct entry *o = &corpus[rnd() % ncorpus];
                if (o->len) {
                    size_t from = rnd() % o->len, cnt = 1 + rnd() % (o->len - from);
                    if (cnt > 64) cnt = 64;
                    size_t p = n ? rnd() % n : 0;
                    if (p + cnt < MAX_LEN) {
                        if (p + cnt > n) n = p + cnt;
                        memcpy(out + p, o->data + from, cnt);
                    }
                }
            }
            break;
        case 6:                                                                                  /* duplicate a chunk */
            if (n > 8 && n + 64 < MAX_LEN) {
                size_t p = rnd() % n, cnt = 1 + rnd() % 32;
                if (p + cnt > n) cnt = n - p;
                memmove(out + p + cnt, out + p, n - p);
                n += cnt;
            }
            break;
        default:                                                                                 /* a fresh random tail */
            if (n + 32 < MAX_LEN) {
                size_t cnt = 1 + rnd() % 24;
                for (size_t i = 0; i < cnt; i++)
                    out[n + i] = (uint8_t)rnd();
                n += cnt;
            }
        }
    }
    return n;
}

static uint8_t virgin[LM_FUZZ_COV_SIZE];

static bool new_coverage(void)
{
    bool fresh = false;
    for (size_t i = 0; i < LM_FUZZ_COV_SIZE; i++)
        if (lm_fuzz_cov[i] && !virgin[i]) {
            virgin[i] = 1;
            fresh = true;
        }
    return fresh;
}

static size_t covered(void)
{
    size_t n = 0;
    for (size_t i = 0; i < LM_FUZZ_COV_SIZE; i++)
        n += virgin[i];
    return n;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? (size_t)n : 1);
    *len = fread(d, 1, n > 0 ? (size_t)n : 0, f);
    fclose(f);
    return d;
}

static void load_corpus_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t n;
        uint8_t *buf = read_file(path, &n);
        if (buf) {
            corpus_add(buf, n, NULL);
            free(buf);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    double budget = 10;
    long max_execs = -1;
    const char *corpus_dir = NULL, *crash = "build/fuzz/current.bin", *replay = NULL;
    uint64_t seed = (uint64_t)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) budget = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_execs = atol(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) corpus_dir = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) crash = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) replay = argv[++i];
        else { fprintf(stderr, "usage: fuzz-server [-t SECONDS] [-s SEED] [-n EXECS] [-c CORPUS_DIR] [-o CRASH_FILE] | -r FILE\n"); return 2; }
    }
    rng_state ^= seed * 0x9e3779b97f4a7c15ull;
    if (!rng_state) rng_state = 1;
    setup();

    if (replay) {
        size_t n;
        uint8_t *d = read_file(replay, &n);
        if (!d) { perror(replay); return 2; }
        run_input(d, n);
        printf("fuzz-server: %s ran without a sanitizer report\n", replay);
        return 0;
    }

    int crash_fd = open(crash, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (crash_fd < 0) {
        fprintf(stderr, "fuzz-server: cannot write %s (mkdir -p build/fuzz?): %s\n", crash, strerror(errno));
        return 2;
    }
    if (corpus_dir) {
        mkdir(corpus_dir, 0755);
        load_corpus_dir(corpus_dir);
    }
    uint8_t buf[MAX_LEN + 128];
    int fds_before = -1;
    double start = now_s(), last = start;
    long execs = 0;
    printf("fuzz-server: seed %llu, %zu corpus entries, %g s\n", (unsigned long long)seed, ncorpus, budget);

    for (;; execs++) {
        size_t n;
        if (ncorpus < 8 || rnd() % 10 == 0) {                                                    /* fresh random input */
            n = 16 + rnd() % 400;
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)rnd();
        } else {
            n = mutate(buf, &corpus[rnd() % ncorpus]);
        }
        if (pwrite(crash_fd, buf, n, 0) < 0 || ftruncate(crash_fd, (off_t)n) < 0) {}
        memset(lm_fuzz_cov, 0, sizeof(lm_fuzz_cov));
        run_input(buf, n);
        if (new_coverage())
            corpus_add(buf, n, corpus_dir);
        int fds = count_fds();
        if (fds_before < 0)
            fds_before = fds;
        else if (fds > fds_before + 4) {
            fprintf(stderr, "fuzz-server: file descriptors leak: %d open now, %d at the start (input in %s)\n", fds, fds_before, crash);
            return 1;
        }
        double t = now_s();
        if (t - last >= 5) {
            printf("  %ld execs (%.0f/s), %zu edges, corpus %zu, %d fds, %ld runs ended with a window (%ld windows)\n", execs,
                   (double)execs / (t - start), covered(), ncorpus, fds, stat_runs_with_window, stat_windows);
            fflush(stdout);
            last = t;
        }
        if (t - start >= budget || (max_execs >= 0 && execs + 1 >= max_execs))
            break;
    }
    printf("fuzz-server: %ld execs, %zu edges, corpus %zu: no sanitizer report\n", execs + 1, covered(), ncorpus);
    return 0;
}

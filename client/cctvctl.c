#include "cctvctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

#define QUEUE 32
#define IDLE_MS 60000                   /* a session with nothing to do is closed after this */
#define PRESET_START 65535
#define PRESET_STOP (-1)

enum { OP_MOVE, OP_STOP, OP_GOTO, OP_SAVE, OP_NIGHT, OP_LAMP, OP_QUERY };

struct op {
    int kind, n;
    char text[24];
};

struct cctv_ctl {
    pthread_t thread;
    pthread_mutex_t lock;
    int wake[2];
    char name[48], host[128], user[64], pass[96];
    int port;
    /* under the lock */
    struct op q[QUEUE];
    int head, count;
    bool quit;
    struct cctv_view view;
    /* the thread's own */
    struct dv_session s;
    bool refused;
    char moving[24];                    /* the direction the camera was told to go, "" = it stands */
    int64_t move_ms, used_ms, alive_ms;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(struct cctv_ctl *c, const char *msg)
{
    pthread_mutex_lock(&c->lock);
    snprintf(c->view.msg, sizeof(c->view.msg), "%s", msg);
    c->view.said++;
    c->view.serial++;
    pthread_mutex_unlock(&c->lock);
}

/* A session that can take a command: the one there is, else a new one. */
static bool connected(struct cctv_ctl *c)
{
    if (c->s.ctl >= 0)
        return true;
    if (c->refused)
        return false;
    char err[160] = "";
    /* (no wake pipe: a new command must not cut a login short, and a stop on the way out must still get through) */
    int r = dv_login(&c->s, c->host, c->port, c->user, c->pass, -1, err, sizeof(err));
    if (r > 0)
        c->refused = true;
    if (r != 0) {
        say(c, err);
        return false;
    }
    c->alive_ms = now_ms() + c->s.alive_s * 500;
    return true;
}

/* Runs one command, once more with a new session when the old one turns out to be gone. 0, the camera's code, or -1. */
static int command(struct cctv_ctl *c, const char *cmd, int preset)
{
    int r = -1;
    for (int attempt = 0; attempt < 2 && r < 0; attempt++) {
        if (!connected(c))
            return -1;
        r = dv_ptz(&c->s, cmd, preset, CCTV_SPEED, -1);
        if (r < 0)
            dv_close(&c->s);
    }
    return r;
}

static void stop_now(struct cctv_ctl *c)
{
    if (!c->moving[0])
        return;
    int r = -1;
    for (int attempt = 0; attempt < 3 && r != 0 && !c->refused; attempt++) {
        r = command(c, c->moving, PRESET_STOP);
        if (r != 0)
            poll(NULL, 0, 200);
    }
    if (r != 0)
        say(c, "The camera could not be told to stop moving");
    c->moving[0] = 0;
}

/* The settings, read anew. False when the connection failed. */
static bool query(struct cctv_ctl *c)
{
    int night = -1;
    char lamp[24] = "";
    char *reply = NULL;
    if (!connected(c))
        return false;
    int r = dv_config_get(&c->s, "Camera.Param", &reply, -1);
    if (r == 0) {
        struct json *j = json_parse(reply), *p = j ? json_get(j, "Camera.Param") : NULL;
        if (p && p->type == JSON_ARR)
            p = p->child;
        const char *v = json_str(p, "DayNightColor", NULL);
        if (v)
            night = (int)strtol(v, NULL, 16);
        json_free(j);
    }
    free(reply);
    reply = NULL;
    if (r >= 0)
        r = dv_config_get(&c->s, "Camera.WhiteLight", &reply, -1);
    if (r == 0) {
        struct json *j = json_parse(reply);
        snprintf(lamp, sizeof(lamp), "%s", json_str(j ? json_get(j, "Camera.WhiteLight") : NULL, "WorkMode", ""));
        json_free(j);
    }
    free(reply);
    if (r < 0) {
        dv_close(&c->s);
        say(c, "The camera did not answer");
        return false;
    }
    pthread_mutex_lock(&c->lock);
    c->view.known = true;
    c->view.night = night;
    snprintf(c->view.lamp, sizeof(c->view.lamp), "%s", lamp);
    c->view.serial++;
    pthread_mutex_unlock(&c->lock);
    return true;
}

/* Reads the setting, changes one value in the camera's own text and writes it back. */
static void change(struct cctv_ctl *c, const char *name, const char *key, const char *to)
{
    char *reply = NULL;
    if (!connected(c))
        return;
    int r = dv_config_get(&c->s, name, &reply, -1);
    if (r == 0) {
        char *value = dv_config_change(reply, name, key, to);
        r = value ? dv_config_set(&c->s, name, value, -1) : 1;
        free(value);
    }
    free(reply);
    if (r < 0)
        dv_close(&c->s);
    if (r == 603)
        say(c, "Saved; the camera wants to be restarted for it");
    else if (r != 0)
        say(c, "The camera did not take the setting");
    query(c);
}

static void do_op(struct cctv_ctl *c, const struct op *o)
{
    char text[32];
    int r;
    switch (o->kind) {
    case OP_MOVE:
        stop_now(c);
        snprintf(c->moving, sizeof(c->moving), "%s", o->text);
        c->move_ms = now_ms();
        r = command(c, o->text, PRESET_START);
        if (r > 0) {
            say(c, "The camera does not move that way");
            c->moving[0] = 0;
        } else if (r < 0) {
            stop_now(c);                    /* (it may have heard the command all the same) */
        }
        break;
    case OP_STOP:
        stop_now(c);
        break;
    case OP_GOTO:
    case OP_SAVE:
        stop_now(c);
        r = command(c, o->kind == OP_SAVE ? "SetPreset" : "GotoPreset", o->n);
        snprintf(text, sizeof(text), "%d", o->n);
        if (r == 0 && o->kind == OP_SAVE && o->n >= 1 && o->n <= CCTV_POSITIONS) {
            pthread_mutex_lock(&c->lock);
            c->view.stored[o->n]++;
            pthread_mutex_unlock(&c->lock);
        }
        if (r == 0) {
            char m[80];
            snprintf(m, sizeof(m), o->kind == OP_SAVE ? "This view is now position %s" : "Going to position %s", text);
            say(c, m);
        } else if (r > 0) {
            say(c, "The camera did not take the position");
        }
        break;
    case OP_NIGHT:
        snprintf(text, sizeof(text), "0x%08X", o->n);
        change(c, "Camera.Param", "DayNightColor", text);
        break;
    case OP_LAMP:
        change(c, "Camera.WhiteLight", "WorkMode", o->text);
        break;
    default:
        query(c);
    }
    c->used_ms = now_ms();
}

static void *ctl_thread(void *ud)
{
    struct cctv_ctl *c = ud;
    for (;;) {
        char b[64];
        while (read(c->wake[0], b, sizeof(b)) > 0) {}
        struct op o;
        pthread_mutex_lock(&c->lock);
        bool quit = c->quit, have = c->count > 0;
        if (have) {
            o = c->q[c->head];
            c->head = (c->head + 1) % QUEUE;
            c->count--;
        }
        pthread_mutex_unlock(&c->lock);
        if (quit)
            break;
        if (have) {
            do_op(c, &o);
            continue;
        }
        int64_t now = now_ms();
        if (c->moving[0] && now - c->move_ms >= CCTV_MOVE_MAX_MS)
            stop_now(c);
        if (c->s.ctl >= 0 && !c->moving[0] && now - c->used_ms >= IDLE_MS)
            dv_close(&c->s);
        if (c->s.ctl >= 0 && now >= c->alive_ms) {
            c->alive_ms = now + c->s.alive_s * 500;
            if (dv_keepalive(&c->s) < 0)        /* (its answer is passed over by the next command) */
                dv_close(&c->s);
        }
        struct pollfd p = { .fd = c->wake[0], .events = POLLIN };
        poll(&p, 1, c->moving[0] ? 100 : c->s.ctl >= 0 ? 500 : -1);
    }
    stop_now(c);
    dv_close(&c->s);
    return NULL;
}

struct cctv_ctl *cctv_ctl_start(const struct cctv_target *t)
{
    struct cctv_ctl *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    snprintf(c->name, sizeof(c->name), "%s", t->name);
    snprintf(c->host, sizeof(c->host), "%s", t->host);
    snprintf(c->user, sizeof(c->user), "%s", t->user);
    snprintf(c->pass, sizeof(c->pass), "%s", t->password);
    c->port = t->port;
    c->view.night = -1;
    dv_init(&c->s);
    if (pipe2(c->wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        free(c);
        return NULL;
    }
    pthread_mutex_init(&c->lock, NULL);
    if (pthread_create(&c->thread, NULL, ctl_thread, c) != 0) {
        close(c->wake[0]);
        close(c->wake[1]);
        free(c);
        return NULL;
    }
    return c;
}

static void poke(struct cctv_ctl *c)
{
    if (write(c->wake[1], "x", 1) < 0) {}
}

static void push(struct cctv_ctl *c, int kind, int n, const char *text)
{
    struct op o = { .kind = kind, .n = n };
    snprintf(o.text, sizeof(o.text), "%s", text ? text : "");
    pthread_mutex_lock(&c->lock);
    if (c->count == QUEUE && kind == OP_STOP)
        c->count = 0;                   /* a stop always gets in: what waited is not worth more */
    if (c->count < QUEUE)
        c->q[(c->head + c->count++) % QUEUE] = o;
    pthread_mutex_unlock(&c->lock);
    poke(c);
}

void cctv_ctl_move(struct cctv_ctl *c, const char *direction) { push(c, direction ? OP_MOVE : OP_STOP, 0, direction); }
void cctv_ctl_position(struct cctv_ctl *c, int n, bool save) { push(c, save ? OP_SAVE : OP_GOTO, n, NULL); }
void cctv_ctl_query(struct cctv_ctl *c) { push(c, OP_QUERY, 0, NULL); }

void cctv_ctl_night(struct cctv_ctl *c, int night, const char *lamp)
{
    if (night >= 0)
        push(c, OP_NIGHT, night, NULL);
    if (lamp)
        push(c, OP_LAMP, 0, lamp);
}

void cctv_ctl_view(struct cctv_ctl *c, struct cctv_view *out)
{
    pthread_mutex_lock(&c->lock);
    *out = c->view;
    pthread_mutex_unlock(&c->lock);
}

void cctv_ctl_quit(struct cctv_ctl *c)
{
    pthread_mutex_lock(&c->lock);
    c->quit = true;
    pthread_mutex_unlock(&c->lock);
    poke(c);
}

void cctv_ctl_free(struct cctv_ctl *c)
{
    if (!c)
        return;
    cctv_ctl_quit(c);
    pthread_join(c->thread, NULL);
    close(c->wake[0]);
    close(c->wake[1]);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

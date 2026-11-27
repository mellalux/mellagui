#include "smartscene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"

static bool scene_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/smart-scenes.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/smart-scenes.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

/* One line of the file into the list: false when it says nothing that is understood. */
static bool parse_line(const char *line, struct scene *s, int *n, int max, bool *in_scene)
{
    char id[12], desk[8], dev[40], code[40], value[24];
    int used = 0;
    if (sscanf(line, "scene %11s %7s %n", id, desk, &used) == 2 && used > 0) {
        *in_scene = false;
        if (*n >= max || (strcmp(desk, "desk") && strcmp(desk, "-")) || scene_find(s, *n, id) >= 0)
            return false;
        struct scene *c = &s[*n];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "%s", id);
        snprintf(c->name, sizeof(c->name), "%s", line + used);
        c->name[strcspn(c->name, "\r\n")] = 0;
        c->desk = !strcmp(desk, "desk");
        if (!c->name[0])
            return false;
        (*n)++;
        *in_scene = true;
        return true;
    }
    if (*in_scene && sscanf(line, "do %39s %39s %23s", dev, code, value) == 3)
        return scene_act_set(&s[*n - 1], dev, code, value);
    return false;
}

int scene_load(struct scene *s, int max)
{
    char path[1024], line[256];
    int n = 0;
    bool in_scene = false;
    if (!scene_path(path, sizeof(path)))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (line[0] != '#')
            parse_line(line, s, &n, max, &in_scene);
    fclose(f);
    return n;
}

bool scene_save(const struct scene *s, int n)
{
    char path[1024], tmp[1100];
    if (!scene_path(path, sizeof(path)))
        return false;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0700);
        *p = '/';
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    fprintf(f, "# mellagui-smart: its scenes (scene ID desk|- NAME, then what it tells the devices). Written by the program itself.\n");
    for (int i = 0; i < n; i++) {
        fprintf(f, "scene %s %s %s\n", s[i].id, s[i].desk ? "desk" : "-", s[i].name);
        for (int k = 0; k < s[i].n; k++)
            fprintf(f, "do %s %s %s\n", s[i].act[k].dev, s[i].act[k].code, s[i].act[k].value);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

int scene_find(const struct scene *s, int n, const char *which)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].id, which))
            return i;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(s[i].name, which))
            return i;
    return -1;
}

void scene_new_id(const struct scene *s, int n, char *id, size_t size)
{
    int top = 0;
    for (int i = 0; i < n; i++)
        if (s[i].id[0] == 's' && atoi(s[i].id + 1) > top)
            top = atoi(s[i].id + 1);
    snprintf(id, size, "s%d", top + 1);
}

int scene_act_find(const struct scene *s, const char *dev, const char *code)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->act[i].dev, dev) && !strcmp(s->act[i].code, code))
            return i;
    return -1;
}

bool scene_act_set(struct scene *s, const char *dev, const char *code, const char *value)
{
    int i = scene_act_find(s, dev, code);
    if (i < 0) {
        if (s->n >= SCENE_ACTS)
            return false;
        i = s->n++;
    }
    snprintf(s->act[i].dev, sizeof(s->act[i].dev), "%s", dev);
    snprintf(s->act[i].code, sizeof(s->act[i].code), "%s", code);
    snprintf(s->act[i].value, sizeof(s->act[i].value), "%s", value);
    return true;
}

void scene_act_remove(struct scene *s, const char *dev, const char *code)
{
    int i = scene_act_find(s, dev, code);
    if (i < 0)
        return;
    memmove(&s->act[i], &s->act[i + 1], sizeof(s->act[0]) * (size_t)(s->n - i - 1));
    s->n--;
}

void scene_value_json(const struct tuya_dp *dp, const char *value, char *out, size_t n)
{
    char *end;
    strtod(value, &end);
    bool word = dp && (dp->type == TUYA_ENUM || dp->type == TUYA_STR);
    if (!word && (!strcmp(value, "true") || !strcmp(value, "false") || (value[0] && !*end)))
        snprintf(out, n, "%s", value);
    else
        json_quote(value, out, n);
}

static void add(char *out, size_t n, const char *a, const char *b)
{
    size_t o = strlen(out);
    if (o + 1 < n)
        snprintf(out + o, n - o, ", %s%s", a, b);
}

int scene_run(const struct scene *s, struct tuya_dev *dev, int ndev, scene_send_fn send, void *ud, char *summary, size_t n)
{
    int done = 0, failed = 0;
    char trouble[200] = "";
    bool told[SCENE_ACTS] = { false };                          /* an offline device is named once */
    for (int i = 0; i < s->n; i++) {
        const struct scene_act *c = &s->act[i];
        struct tuya_dev *d = NULL;
        for (int k = 0; k < ndev && !d; k++)
            if (!strcmp(dev[k].id, c->dev))
                d = &dev[k];
        if (!d) {
            failed++;
            add(trouble, sizeof(trouble), "a device is gone from the account", "");
            continue;
        }
        if (!d->online) {
            failed++;
            bool named = false;
            for (int k = 0; k < i; k++)
                named = named || (told[k] && !strcmp(s->act[k].dev, c->dev));
            if (!named) {
                told[i] = true;
                add(trouble, sizeof(trouble), d->name, " is offline");
            }
            continue;
        }
        char json[100], err[200] = "", why[240];
        scene_value_json(tuya_find_dp(d, c->code), c->value, json, sizeof(json));
        if (send(ud, c->dev, c->code, json, err, sizeof(err))) {
            done++;
        } else {
            failed++;
            snprintf(why, sizeof(why), ": %.120s", err[0] ? err : "did not go out");
            add(trouble, sizeof(trouble), d->name, why);
        }
    }
    snprintf(summary, n, "%s: %d done%s", s->name, done, trouble);
    return failed;
}

static bool cloud_send(void *ud, const char *dev, const char *code, const char *json, char *err, size_t errsz)
{
    return tuya_command(ud, dev, code, json, err, errsz);
}

int scene_run_cloud(const struct scene *s, char *summary, size_t n)
{
    static struct tuya_session sess;
    static struct tuya_dev dev[TUYA_MAX_DEV];
    int ndev = 0;
    char err[200] = "";
    if (!tuya_session_load(&sess)) {
        snprintf(summary, n, "%s: not logged in", s->name);
        return -1;
    }
    if (!tuya_load_devices(&sess, dev, &ndev, err, sizeof(err))) {
        snprintf(summary, n, "%s: %.150s", s->name, err);
        return -1;
    }
    return scene_run(s, dev, ndev, cloud_send, &sess, summary, n);
}

/* ---- the self test ------------------------------------------------------------------------------------------------------------------ */

static int g_sent;
static char g_log[200];

static bool test_send(void *ud, const char *dev, const char *code, const char *json, char *err, size_t errsz)
{
    (void)ud;
    size_t o = strlen(g_log);
    snprintf(g_log + o, sizeof(g_log) - o, "%s %s %s;", dev, code, json);
    if (!strcmp(code, "broken")) {
        snprintf(err, errsz, "no");
        return false;
    }
    g_sent++;
    return true;
}

int scene_selftest(void)
{
    int bad = 0;
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "smartscene selftest: %s\n", what); bad++; } } while (0)
    static struct scene s[3];
    int n = 0;
    bool in_scene = false;
    CHECK(parse_line("scene s1 desk Good night\n", s, &n, 3, &in_scene) && n == 1 && s[0].desk && !strcmp(s[0].name, "Good night"), "a scene line");
    CHECK(parse_line("do d1 switch_1 false", s, &n, 3, &in_scene) && parse_line("do d1 switch_1 true", s, &n, 3, &in_scene) && s[0].n == 1 &&
          !strcmp(s[0].act[0].value, "true"), "an action is kept once, the last one counts");
    CHECK(!parse_line("scene s1 - again", s, &n, 3, &in_scene) && !parse_line("do d2 switch_1 true", s, &n, 3, &in_scene) && s[0].n == 1,
          "an id twice is not taken, nor what follows it");
    CHECK(!parse_line("scene s2 maybe x", s, &n, 3, &in_scene) && !parse_line("scene s2 - \n", s, &n, 3, &in_scene), "bad scene lines are accepted");
    CHECK(parse_line("scene s7 - Evening", s, &n, 3, &in_scene) && n == 2 && !s[1].desk, "a second scene");
    CHECK(scene_find(s, n, "s7") == 1 && scene_find(s, n, "good NIGHT") == 0 && scene_find(s, n, "nothing") < 0, "finding a scene");
    char id[12];
    scene_new_id(s, n, id, sizeof(id));
    CHECK(!strcmp(id, "s8"), "a new id");
    scene_act_set(&s[0], "d1", "control", "close");
    scene_act_set(&s[0], "d2", "switch_1", "true");
    scene_act_set(&s[0], "d2", "broken", "1");
    scene_act_set(&s[0], "d9", "switch_1", "true");
    static struct tuya_dev dev[2];
    memset(dev, 0, sizeof(dev));
    snprintf(dev[0].id, sizeof(dev[0].id), "d1");
    snprintf(dev[0].name, sizeof(dev[0].name), "Blind");
    dev[0].online = true;
    snprintf(dev[1].id, sizeof(dev[1].id), "d2");
    snprintf(dev[1].name, sizeof(dev[1].name), "Tree");
    char sum[200];
    int failed = scene_run(&s[0], dev, 2, test_send, NULL, sum, sizeof(sum));
    CHECK(failed == 3 && g_sent == 2 && !strcmp(g_log, "d1 switch_1 true;d1 control \"close\";"), "the actions go out in order, not to an offline device");
    CHECK(!strcmp(sum, "Good night: 2 done, Tree is offline, a device is gone from the account"), "the summary");
    scene_act_remove(&s[0], "d1", "switch_1");
    CHECK(s[0].n == 4 && scene_act_find(&s[0], "d1", "switch_1") < 0 && scene_act_find(&s[0], "d1", "control") == 0, "removing an action");
#undef CHECK
    return bad;
}

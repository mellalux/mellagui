#ifndef MELLAGUI_SMARTSCENE_H
#define MELLAGUI_SMARTSCENE_H

/*
 * smartscene - mellagui-smart's own scenes: a name and a list of things to tell the devices, run with one click (the window, the desktop card,
 * `mellagui-smart --scene NAME`, the timetable). They live in $XDG_CONFIG_HOME/mellagui/smart-scenes.conf:
 *
 *     scene s1 desk Good night                 (the id, desk = a button on the desktop card or - = none, the name)
 *     do DEVICE_ID control close               (what it tells a device: code and value, in the order they are sent)
 *     do DEVICE_ID switch_1 false
 *
 * The id never changes (the timetable and the card refer to it), the name may. A switch's value may be `toggle`: the other way from what the device
 * says when the scene runs.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tuya.h"

#define SCENE_MAX 16
#define SCENE_ACTS 64
#define SCENE_NAME 48

struct scene_act { char dev[40], code[40], value[24]; };

struct scene {
    char id[12], name[SCENE_NAME];
    bool desk;                          /* shown on the desktop card */
    int n;
    struct scene_act act[SCENE_ACTS];
};

int scene_load(struct scene *s, int max);
bool scene_save(const struct scene *s, int n);
/* By id, else by name whatever the case: the index, -1 = none. */
int scene_find(const struct scene *s, int n, const char *which);
/* An id that no scene of the list has ("s3"). */
void scene_new_id(const struct scene *s, int n, char *id, size_t size);
/* The action about (dev, code), -1 = the scene leaves that alone. */
int scene_act_find(const struct scene *s, const char *dev, const char *code);
/* Sets or replaces that action; false when the scene is full. */
bool scene_act_set(struct scene *s, const char *dev, const char *code, const char *value);
void scene_act_remove(struct scene *s, const char *dev, const char *code);
/* The value as JSON (true, 30, "open"); `dp` (may be NULL) says when a word that looks like a number is a word. */
void scene_value_json(const struct tuya_dp *dp, const char *value, char *out, size_t n);

/* Tells one device one thing; false with `err` when it did not go out. */
typedef bool (*scene_send_fn)(void *ud, const char *dev, const char *code, const char *json, char *err, size_t errsz);
/* Sends every action in order to the devices of the list that are online. `summary` says how it went ("Good night: 2 done, Lamp is offline").
 * Returns the number of actions that did not go out. */
int scene_run(const struct scene *s, struct tuya_dev *dev, int ndev, scene_send_fn send, void *ud, char *summary, size_t n);
/* The same through the cloud with the saved login, for the programs without a window. -1 = nothing at all was sent (not logged in, no
 * network: it may be tried again), else as scene_run(). */
int scene_run_cloud(const struct scene *s, char *summary, size_t n);
/* A scene that only turns switches the other way is on or off, like the switches: true for such a scene (its button shows which). */
bool scene_is_toggle(const struct scene *s);
/* ... and whether it is on now: one of its switches is (an offline device's does not count). */
bool scene_is_on(const struct scene *s, struct tuya_dev *dev, int ndev);

int scene_selftest(void);

#endif

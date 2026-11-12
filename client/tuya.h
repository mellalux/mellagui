#ifndef MELLAGUI_TUYA_H
#define MELLAGUI_TUYA_H

/*
 * tuya - the Smart Life / Tuya Smart account in the cloud, for mellagui-smart. It speaks the "device sharing" API that Tuya made for Home
 * Assistant (the open tuya-device-sharing-sdk is the reference): the login is a QR code the phone's Smart Life app scans - the password never
 * comes near this program -, then the homes, their devices with the current values, what each device can be told, and commands.
 *
 * Every call runs `curl` ($MELLAGUI_CURL names another program) with its options on stdin, so no token is ever on a command line, and waits for it:
 * call from a thread, not from the loop that paints. A request's parameters, body and answer are AES-128-GCM encrypted with a key derived from
 * the request id and the refresh token, and the headers are signed with HMAC-SHA256 (src/aes.c, src/md5.c, src/sha256.c).
 *
 * The session (tokens; no password) is kept in $XDG_CONFIG_HOME/mellagui/smart.conf, mode 0600. $MELLAGUI_TUYA_ENDPOINT replaces the login server
 * (tests: tests/fake_tuya.py).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUYA_MAX_DEV 48
#define TUYA_MAX_DP 40
#define TUYA_MAX_RANGE 12

enum tuya_type { TUYA_BOOL, TUYA_INT, TUYA_ENUM, TUYA_STR, TUYA_OTHER };

/* One "data point" of a device: something it reports (a status) and maybe something it can be told (a function). */
struct tuya_dp {
    char code[40];
    enum tuya_type type;
    bool func;                          /* it can be set */
    bool has;                           /* a value is known */
    bool b;                             /* TUYA_BOOL */
    double num;                         /* TUYA_INT (the raw number: the shown value is num / 10^scale) */
    char str[64];                       /* TUYA_ENUM, TUYA_STR, TUYA_OTHER (cut) */
    double min, max, step;              /* TUYA_INT, when the device says (max > min) */
    int scale;
    char unit[12];
    char range[TUYA_MAX_RANGE][24];     /* TUYA_ENUM */
    int nrange;
    int lan_id;                         /* its number in the local protocol, 0 = not known */
    bool lan_ok;                        /* ... and the value goes over the local network as it is (no conversion) */
};

struct tuya_dev {
    char id[40], name[80], category[16], product[80], home[64];
    char local_key[40], ip[48];
    bool online;
    bool specs;                         /* the functions were asked for (also when the answer was an error) */
    bool map;                           /* ... and the numbers of the local protocol */
    struct tuya_dp dp[TUYA_MAX_DP];
    int ndp;
};

struct tuya_session {
    char user_code[48], endpoint[160], terminal_id[80], uid[80], username[80];
    char access[300], refresh[300];
    int64_t expire_ms;                  /* when the access token ends (ms since 1970) */
    bool relogin;                       /* the server refused the tokens: the user has to log in again */
    int64_t file_stamp;                 /* the session file as this copy knows it (another process may renew the tokens) */
};

/* The saved session: false when there is none. */
bool tuya_session_load(struct tuya_session *s);
bool tuya_session_save(const struct tuya_session *s);
/* Forgets the tokens (the file goes) but keeps the user code. */
void tuya_session_forget(struct tuya_session *s);
static inline bool tuya_logged_in(const struct tuya_session *s) { return s->access[0] && s->refresh[0] && s->endpoint[0]; }

/* Asks for a login code: `token` is what the QR code carries after TUYA_QR_PREFIX. */
#define TUYA_QR_PREFIX "tuyaSmart--qrLogin?token="
bool tuya_qr_start(const char *user_code, char *token, size_t n, char *err, size_t errsz);
/* Was the code scanned? 1 = yes (the session is filled in and saved), 0 = not yet, -1 = the request itself failed (err). */
int tuya_qr_poll(const char *user_code, const char *token, struct tuya_session *s, char *err, size_t errsz);

/* The devices of every home with their current values, merged into dev[0..*n) by id: a device that is there keeps what is known about its data
 * points, one that is gone leaves the list. */
bool tuya_load_devices(struct tuya_session *s, struct tuya_dev *dev, int *n, char *err, size_t errsz);
/* What the device can be told and the ranges of its values (sets d->specs whatever the answer). */
bool tuya_load_specs(struct tuya_session *s, struct tuya_dev *d, char *err, size_t errsz);
/* The numbers the device's data points have in the local protocol (sets d->map whatever the answer). */
bool tuya_load_local_map(struct tuya_session *s, struct tuya_dev *d, char *err, size_t errsz);
/* Takes the values a device said itself in the local network (tuyalan_get: {"dps":{"1":true}} by number) into its data points; only those whose
 * value is the same on both sides (lan_ok). Returns how many it took. */
int tuya_apply_local(struct tuya_dev *d, const char *json);
/* Every local number of the device with the value null, {"1":null,"9":null} (what tuyalan_get may ask with); empty when none is known. */
void tuya_local_question(const struct tuya_dev *d, char *out, size_t n);
/* Tells a device a value: `value_json` is JSON (true, 50, "open"). */
bool tuya_command(struct tuya_session *s, const char *dev_id, const char *code, const char *value_json, char *err, size_t errsz);

/* A scene of the app ("Tap-to-Run"): made there, started from here. */
#define TUYA_MAX_SCENE 24
struct tuya_scene {
    char id[48], home[40], name[80];
    bool enabled;
};
bool tuya_load_scenes(struct tuya_session *s, struct tuya_scene *scene, int *n, char *err, size_t errsz);
bool tuya_trigger_scene(struct tuya_session *s, const char *home, const char *id, char *err, size_t errsz);

struct tuya_dp *tuya_find_dp(struct tuya_dev *d, const char *code);
/* The value as text for people: "on" / "off", the number with its scale and unit, the text. */
void tuya_dp_text(const struct tuya_dp *dp, char *out, size_t n);

/* The parts that need no network (the program's --selftest). */
int tuya_selftest(void);

#endif

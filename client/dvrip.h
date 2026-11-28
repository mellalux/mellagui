#ifndef MELLAGUI_DVRIP_H
#define MELLAGUI_DVRIP_H

/*
 * DVRIP ("Sofia"), the protocol of XiongMai network cameras (the ones the iCSee app talks to), TCP port 34567, as much of it as live viewing needs:
 * login, keep alive, claim and start of a video stream, and taking the stream apart into frames. Written from the publicly known behaviour of
 * these devices; nothing goes to any cloud.
 *
 * A message is a 20 byte header (0xff, version, 2 reserved, session u32, sequence u32, 2 bytes, message id u16, length u32; little endian) and a body,
 * JSON for commands. The video comes on a SECOND connection that claims the stream; its message bodies (id 1412) are one long byte stream of frames,
 * cut anywhere, each with a small header that begins 00 00 01 Fx.
 *
 * Everything that waits does so with a time limit and gives up at once when `wake_fd` (a pipe of the caller's, -1 = none) becomes readable.
 * Nothing here is shared between sessions, so each camera may have its own thread.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DV_PORT 34567
#define DV_MSG_STREAM 1412
#define DV_MSG_PLAY 1426                    /* a piece of a recording of the card; one with nothing in it ends the file */
#define DV_MSG_REPLAY 1422                  /* the same for a recording that is being played (dv_replay_start) */
#define DV_MSG_ALARM 1504
#define DV_MAX_BODY (4u << 20)              /* a longer message or frame is not believed */

/* The 8 characters a camera wants in place of the password (made from its MD5). */
void dv_hash(const char *password, char out[9]);

/* ---- taking the stream apart ---------------------------------------------------------------------------------------- */

enum { DV_VIDEO_KEY, DV_VIDEO, DV_AUDIO, DV_INFO };
enum { DV_CODEC_NONE = 0, DV_CODEC_H264 = 2, DV_CODEC_H265 = 3 };
#define DV_SOUND_ALAW 14

struct dv_frame {
    int kind;
    int codec, w, h;                        /* of a key frame (DV_CODEC_*, pixels); of sound: codec = its kind (DV_SOUND_ALAW); 0 otherwise */
    uint32_t time;                          /* of a key frame: the camera's clock then (dv_seconds), 0 otherwise */
    const uint8_t *data;                    /* video: the elementary stream (Annex B), sound: the samples; valid during the callback only */
    size_t len;
};

struct dv_demux {
    uint8_t *buf;
    size_t len, cap;
    uint64_t skipped;                       /* bytes that belonged to no frame */
};

typedef void (*dv_frame_fn)(void *ud, const struct dv_frame *f);
/* Add bytes of the stream; `fn` is called for every frame that is complete now. False only when memory runs out. */
bool dv_demux_feed(struct dv_demux *d, const void *data, size_t len, dv_frame_fn fn, void *ud);
void dv_demux_free(struct dv_demux *d);

/* ---- messages -------------------------------------------------------------------------------------------------------- */

/* The messages of one connection, collected from whatever pieces the socket gives. */
struct dv_packets {
    uint8_t *buf;
    size_t len, cap;
};

typedef void (*dv_packet_fn)(void *ud, int msg, const uint8_t *body, size_t len);
/* Add received bytes; false when they are no DVRIP messages (or memory runs out): the connection is of no use then. */
bool dv_packets_feed(struct dv_packets *p, const void *data, size_t len, dv_packet_fn fn, void *ud);
void dv_packets_free(struct dv_packets *p);

/* ---- a session ------------------------------------------------------------------------------------------------------- */

struct dv_session {
    int ctl, data;                          /* -1 = not open */
    uint32_t id, seq;
    char idtext[16];                        /* the session id as the camera wrote it */
    int alive_s;                            /* send dv_keepalive() more often than this */
    char host[128];
    int port;
};

void dv_init(struct dv_session *s);
/* Connect and log in. 0 = in; a positive number = the camera answered and refused (its code: do NOT try again by yourself, cameras lock accounts);
 * -1 = no connection or no sensible answer. `err` says it in words. */
int dv_login(struct dv_session *s, const char *host, int port, const char *user, const char *password, int wake_fd, char *err, size_t errsz);
/* Claim the main or the sub stream on a second connection and start it; its bytes then arrive on s->data. 0 or -1. */
int dv_start(struct dv_session *s, bool sub, int wake_fd, char *err, size_t errsz);
/* 0, or -1 when the connection is gone. The camera's answer is left in s->ctl (read it and throw it away). */
int dv_keepalive(struct dv_session *s);
void dv_close(struct dv_session *s);

/* ---- commands (on a session that watches nothing: they read the camera's answer) -------------------------------------------- */

/* These return 0, the camera's code when it says no, or -1 when the connection is of no use any more. */

/* Move the camera: `command` is "DirectionLeft", "DirectionRightUp" ... with preset 65535 it STARTS and goes on until the same call with preset -1
 * stops it; "SetPreset" / "GotoPreset" with a position's number remember and go back to a view. step = the speed, 1..8. */
int dv_ptz(struct dv_session *s, const char *command, int preset, int step, int wake_fd);
/* A setting as the camera writes it ("Camera.Param"): the whole answer's text, to be freed by the caller. */
int dv_config_get(struct dv_session *s, const char *name, char **reply, int wake_fd);
/* Writes a setting back: `value` is its JSON. */
int dv_config_set(struct dv_session *s, const char *name, const char *value, int wake_fd);

/* In the text of an answer of dv_config_get: a new copy of the member `name`'s JSON with the string member `key` inside it changed to `to` (everything
 * else as the camera wrote it). NULL when there is no such member or key. */
char *dv_config_change(const char *reply, const char *name, const char *key, const char *to);

/* ---- alarms ------------------------------------------------------------------------------------------------------------ */

/* Ask the camera to tell about its alarms: they then come on s->ctl as messages DV_MSG_ALARM. Returns like dv_ptz. */
int dv_alarm_subscribe(struct dv_session *s, int wake_fd);
/* The text of an alarm message: the event's name ("VideoMotion", "HumanDetect" ...) and whether it begins (true) or ends. False when it is no event. */
bool dv_alarm_parse(const char *body, size_t len, char *event, size_t cap, bool *begins);

/* ---- the recordings on the camera's card -------------------------------------------------------------------------------- */

struct dv_file {
    char name[200];                     /* the camera's own path */
    char begin[20], end[20];            /* "2026-11-27 22:28:23", the camera's clock */
    uint32_t kb;                        /* its size in units of 1024 bytes */
};

/* The video files of the time from `begin` to `end` (that format), oldest first: at most `max` of them into *files (to be freed). Returns like dv_ptz;
 * *n is the number. */
int dv_files(struct dv_session *s, const char *begin, const char *end, struct dv_file **files, int *n, int max, int wake_fd);
/* Ask for a file: its bytes (a stream of frames like a live one) then come on s->data as messages DV_MSG_PLAY. 0 or -1 (`err`). */
int dv_play_start(struct dv_session *s, const struct dv_file *f, int wake_fd, char *err, size_t errsz);
void dv_play_stop(struct dv_session *s, const struct dv_file *f);
/* Ask for a file to be played, from f->begin on (a later time than the file's own begins at the next key frame after it): messages DV_MSG_REPLAY on
 * s->data, several times faster than the pictures were taken, so the reader sets the pace by reading; an earlier replay of the session is to be
 * stopped first. 0 or -1 (`err`). */
int dv_replay_start(struct dv_session *s, const struct dv_file *f, int wake_fd, char *err, size_t errsz);
void dv_replay_stop(struct dv_session *s, const struct dv_file *f, int wake_fd);

/* A time of the camera's clock as seconds that can be subtracted (no time zone): of a key frame's field, and of "2026-11-27 22:28:23" (0 = not one). */
long long dv_seconds(uint32_t packed);
long long dv_seconds_text(const char *text);

#endif

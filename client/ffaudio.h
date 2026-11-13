#ifndef MELLAGUI_FFAUDIO_H
#define MELLAGUI_FFAUDIO_H

/*
 * Sound for mellagui-view: `ffmpeg -ss START -i FILE -vn -f s16le -ar 48000 -ac 2 -` feeds a player that talks to the sound server
 * (pw-cat for PipeWire, else paplay for PulseAudio, else aplay for ALSA; $MELLAGUI_AUDIO_PLAYER names one, "none" switches the sound off).
 * Both programs run in their own process group, die with the viewer (PR_SET_PDEATHSIG) and are stopped / continued as a pair, so a pause
 * costs nothing and a resume has no start-up delay. Nothing depends on it: without ffmpeg or a player the video plays in silence.
 *
 * The clock of the picture is the wall clock; the sound runs from its own device clock, so a very long film may drift by a few tenths of a
 * second (a seek brings them together again).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ffaudio;
struct lm_image;

/* ---- sound files ---------------------------------------------------------------------------------------------------------------- */

struct ffaudio_info {
    uint64_t duration_ms;           /* 0 = unknown */
    char codec[24];                 /* "mp3", "flac", "opus" ... */
    int rate;                       /* Hz, 0 = unknown */
    char channels[16];              /* "stereo", "mono", "5.1" */
    int kbps;                       /* 0 = unknown */
    char title[96], artist[96], album[96];
    bool cover;                     /* a picture is stored in the file (an "attached pic") */
};

/* Is it a file name of a sound file (mp3 flac ogg oga opus wav m4a aac wma aif aiff mka weba)? */
bool ffaudio_name_is_audio(const char *name);
/* `ffmpeg -i FILE`: false (message in err) when there is no ffmpeg or the file has no sound track. Quick (it only reads the header). */
bool ffaudio_probe(const char *path, struct ffaudio_info *info, char *err, size_t errsz);
/* The picture stored in the file (album art) decoded into img (XRGB, free it with lm_img_free); false when there is none. */
bool ffaudio_cover(const char *path, struct lm_image *img);

/* How many players this machine has (0 = no sound possible) and the name of number i. */
int ffaudio_players(void);
const char *ffaudio_player_name(int i);

/* Starts the sound of `path` at `start_ms` with player number `player`. NULL with a message in err when it cannot be started. */
struct ffaudio *ffaudio_start(const char *path, uint64_t start_ms, int player, char *err, size_t errsz);
/* 1 while it runs (also while stopped), 0 when it has ended by itself (the file is over, or the player gave up); *quick is set when that
 * happened within two seconds of the start (a player that cannot reach a sound server). */
int ffaudio_alive(struct ffaudio *a, bool *quick);
void ffaudio_stop(struct ffaudio *a);           /* pause: the pair is stopped (SIGSTOP) */
void ffaudio_cont(struct ffaudio *a);           /* ... and goes on */
bool ffaudio_is_stopped(const struct ffaudio *a);
void ffaudio_close(struct ffaudio *a);          /* kills and reaps both */

#endif

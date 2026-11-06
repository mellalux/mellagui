/*
 * mixer.h - the volume mixer behind the desktop gadget.
 *
 * Backends (nothing is linked, everything goes through short-lived child processes started asynchronously by the event
 * loop, see audio.h): PulseAudio / PipeWire's pulse server via `pactl` (master volume and mute of the default sink, the
 * list of outputs, one slider per playing application), else PipeWire's own `wpctl` (master volume and mute of the default
 * output), else ALSA's `Master` control via `amixer`. The state is polled
 * while the gadget is visible and refreshed after every change; slider drags are coalesced so that at most one command
 * is in flight.
 *
 * The text parsers and the layout/hit-test functions have no dependencies (unit-tested in tests/keymap_test.c); the
 * renderer paints with lm_mixer_layout() and the window manager hit-tests with lm_mixer_hit(), so both always agree.
 */
#ifndef LM_MIXER_H
#define LM_MIXER_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

#define LM_MIXER_MAX_SINKS 8
#define LM_MIXER_MAX_STREAMS 6          /* applications listed (more are ignored) */

struct lm_mixer_sink {
    char name[96];
    char desc[64];
    int volume;                         /* percent of the first channel, 0..150 */
    bool mute;
};

struct lm_mixer_stream {
    unsigned id;                        /* sink-input index */
    char name[48];                      /* application name */
    int volume;
    bool mute;
};

enum lm_mixer_backend { LM_MIXER_NONE, LM_MIXER_PULSE, LM_MIXER_WPCTL, LM_MIXER_ALSA };

struct lm_mixer_state {
    enum lm_mixer_backend backend;
    bool ready;                         /* a refresh finished at least once */
    int volume;                         /* master (the default sink / ALSA Master), percent */
    bool mute;
    char output[64];                    /* description of the default sink ("ALSA Master" for ALSA) */
    int nsinks;                         /* PulseAudio: all outputs; the default one is sinks[cur] */
    int cur;
    struct lm_mixer_sink sinks[LM_MIXER_MAX_SINKS];
    int nstreams;
    struct lm_mixer_stream streams[LM_MIXER_MAX_STREAMS];
};

/* ---- pure helpers ---------------------------------------------------------------------------------------------- */

/* `pactl list sinks` -> sinks (name, description, volume, mute); returns the count. */
int lm_mixer_parse_sinks(const char *text, struct lm_mixer_sink *out, int max);
/* `pactl list sink-inputs` -> playing applications; returns the count. */
int lm_mixer_parse_streams(const char *text, struct lm_mixer_stream *out, int max);
/* `wpctl get-volume @DEFAULT_AUDIO_SINK@` -> "Volume: 0.64" (+ " [MUTED]"): percent and mute. False when there is no volume. */
bool lm_mixer_parse_wpctl(const char *text, int *volume, bool *mute);
/* `amixer sget Master` -> "[75%]" and "[on]"/"[off]". False when no volume is found. */
bool lm_mixer_parse_amixer(const char *text, int *volume, bool *mute);
/* Position -> percent along a slider of `w` pixels starting at x0 (clamped to 0..100). */
int lm_mixer_slider_value(int x0, int w, double x);

struct lm_mixer_layout {
    struct lm_rect master_mute, master_slider, output;     /* `output` is empty without PulseAudio (nothing to choose) */
    int nstreams;                                          /* rows that fit */
    struct { struct lm_rect label, mute, slider; } stream[LM_MIXER_MAX_STREAMS];
    struct lm_rect hint;                                   /* a line of small text at the bottom (empty when no room) */
};

/* Layout of the card `card` whose title bar is `title_h` high; `want` streams are wanted, those that fit are returned. */
void lm_mixer_layout(const struct lm_rect *card, int title_h, int want, bool with_output, struct lm_mixer_layout *out);

enum lm_mixer_part { LM_MIXER_NOTHING, LM_MIXER_MASTER_MUTE, LM_MIXER_MASTER_SLIDER, LM_MIXER_OUTPUT, LM_MIXER_STREAM_MUTE,
                     LM_MIXER_STREAM_SLIDER };
/* What is at (x, y); `index` is the stream row for the STREAM parts. */
enum lm_mixer_part lm_mixer_hit(const struct lm_mixer_layout *l, int x, int y, int *index);

/* ---- the live mixer -------------------------------------------------------------------------------------------- */

struct lm_loop;
struct lm_mixer;

/* `changed(data)` is called whenever the state (or the gadget's look) changed: repaint. */
struct lm_mixer *lm_mixer_create(struct lm_loop *loop, void (*changed)(void *), void *data);
void lm_mixer_destroy(struct lm_mixer *m);
/* The gadget is shown / hidden: poll while shown (a first refresh starts at once). */
void lm_mixer_set_active(struct lm_mixer *m, bool active);
const struct lm_mixer_state *lm_mixer_state(const struct lm_mixer *m);

void lm_mixer_set_master(struct lm_mixer *m, int percent);
void lm_mixer_toggle_master_mute(struct lm_mixer *m);
void lm_mixer_set_stream(struct lm_mixer *m, int index, int percent);
void lm_mixer_toggle_stream_mute(struct lm_mixer *m, int index);
void lm_mixer_next_output(struct lm_mixer *m);          /* PulseAudio: make the next sink the default one */

#endif

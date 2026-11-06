/*
 * audio.h - sound outputs for the settings panel. Lumen has no audio server of its own:
 *   * ALSA devices come from /proc/asound (env LUMEN_ASOUND_DIR overrides, for tests); the chosen one is where Lumen's own
 *     feedback beeps (sound.h) are played, HDMI/DisplayPort outputs are labelled as such;
 *   * the system output (PulseAudio / PipeWire's pulse server) is listed and changed with `pactl` (nothing is listed when
 *     pactl is missing or no server runs): the default sink decides where applications like Firefox play.
 * Lists are text "name|description;name|description;..." (those two characters never occur inside a name or description).
 */
#ifndef LUMEN_AUDIO_H
#define LUMEN_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

struct lm_loop;

/* Playback devices: "default|System default;plughw:CARD=PCH,DEV=3|HDA Intel PCH - HDMI 0 (HDMI);...". */
void lm_audio_list_alsa(char *out, size_t n);

/* A device/sink name that is safe to hand to aplay -D / pactl: 1..63 chars of [A-Za-z0-9_:=,.+@-]. */
bool lm_audio_name_ok(const char *name);

/*
 * PulseAudio/PipeWire output sinks via `pactl` (waits at most ~1 s, never longer): list as above, current default sink
 * name in `current`. Both empty when there is no sound server. Returns the number of sinks.
 */
int lm_audio_list_sinks(char *list, size_t n, char *current, size_t cn);
/* `pactl set-default-sink name` (and moves running streams there). True when the default now is `name`. */
bool lm_audio_set_sink(const char *name);

/*
 * Asynchronous child process for the mixer gadget: argv[0] is searched in PATH, LC_ALL=C, stdin/stderr are /dev/null. The
 * loop collects stdout; `fn(data, output)` is called once, when the program closes its output (output = everything it
 * printed, up to 16 KiB) or after `timeout_ms` (the child is killed; output is then ""). `fn` may be NULL (fire and
 * forget; the server's SIGCHLD watch reaps the child). The returned handle is only valid until `fn` ran.
 */
struct lm_proc;
typedef void (*lm_proc_fn)(void *data, const char *output);
struct lm_proc *lm_proc_run(struct lm_loop *loop, char *const argv[], int timeout_ms, lm_proc_fn fn, void *data);
/* Forget a running process without calling its callback (the child is killed). */
void lm_proc_cancel(struct lm_proc *p);

#endif

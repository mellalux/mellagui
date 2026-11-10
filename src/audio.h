/*
 * audio.h - sound outputs for the settings panel. MellaGUI has no audio server of its own:
 *   * ALSA devices come from /proc/asound (env MELLAGUI_ASOUND_DIR overrides, for tests); the chosen one is where MellaGUI's own
 *     feedback beeps (sound.h) are played, HDMI/DisplayPort outputs are labelled as such;
 *   * the system output (PulseAudio / PipeWire's pulse server) is listed and changed with `pactl` (nothing is listed when
 *     pactl is missing or no server runs): the default sink decides where applications like Firefox play.
 * Lists are text "name|description;name|description;..." (those two characters never occur inside a name or description).
 */
#ifndef MELLAGUI_AUDIO_H
#define MELLAGUI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

struct lm_loop;

/* Playback devices: "default|System default;plughw:CARD=PCH,DEV=3|HDA Intel PCH - HDMI 0 (HDMI);...". */
void lm_audio_list_alsa(char *out, size_t n);

/* A device/sink name that is safe to hand to aplay -D / pactl: 1..63 chars of [A-Za-z0-9_:=,.+@-]. */
bool lm_audio_name_ok(const char *name);

/*
 * Sound server outputs (waits at most ~1 s per program, never longer): list as above, current default in `current`. Both
 * empty when there is no sound server. Returns the number of sinks. `pactl` is tried first (names are sink names); without
 * it PipeWire's `wpctl status` (the "names" are numeric object ids, set with `wpctl set-default`).
 */
int lm_audio_list_sinks(char *list, size_t n, char *current, size_t cn);
/* Makes `name` (as listed) the default output. True when the default now is `name`. */
bool lm_audio_set_sink(const char *name);
/* Why the last lm_audio_list_sinks() found nothing: "ok" (it found some), "missing-tools" (neither pactl nor wpctl in PATH),
 * "no-server" (a tool exists but no sound server answers), "" before the first call. */
const char *lm_audio_sink_status(void);
/* `wpctl status` -> list/current (exposed for the unit tests). Returns the number of sinks. */
int lm_audio_parse_wpctl_sinks(const char *text, char *list, size_t n, char *current, size_t cn);

/*
 * PipeWire card profiles (through `pw-cli`): a sound card exposes one output at a time (analog, HDMI, S/PDIF ...) and
 * switching the profile is how HDMI audio is chosen. The list has one entry per output type of each card, as
 * "cardid:profileindex|Card name - Profile description;..." (e.g. "43:6|Built-in Audio - Digital Stereo (HDMI) Output");
 * `current` is the entry whose output type is active now. Outputs that are not available (an unplugged HDMI) and
 * surround variants are left out. Empty without pw-cli or a running PipeWire. Returns the number of entries.
 */
int lm_audio_list_profiles(char *list, size_t n, char *current, size_t cn);
/* Switches the card to the profile of an entry of that list ("43:6"). True when PipeWire reports it active afterwards. */
bool lm_audio_set_profile(const char *entry);

/* The lasting name of a profile entry ("43:6" -> "Built-in Audio|output:hdmi-stereo"): the card's description and the
 * profile's name survive a restart of PipeWire, the ids do not. False when it cannot be found. */
bool lm_audio_profile_key(const char *entry, char *out, size_t n);
/* Switches the card to a profile saved by lm_audio_profile_key(). 0 = it is active now, 1 = nothing to do / the profile is gone
 * (give up), -1 = the sound server or the card is not there yet (try again later). */
int lm_audio_apply_profile_key(const char *key);

struct lm_audio_card { int id; char desc[64]; };
struct lm_audio_profile { int index; char name[64]; char desc[96]; bool available; };
/* `pw-cli ls Device`: the Audio/Device objects (id, device.description). */
int lm_audio_parse_cards(const char *text, struct lm_audio_card *out, int max);
/* `pw-cli enum-params <id> EnumProfile`: every profile. */
int lm_audio_parse_profiles(const char *text, struct lm_audio_profile *out, int max);
/* `pw-cli enum-params <id> Profile`: index of the active profile, -1 when unknown. */
int lm_audio_parse_current_profile(const char *text);
/* The profiles worth showing: indexes into `in` (one per output type, see above); `*current_pick` = position in `picked` of the
 * active output type (-1 none). Returns the count. */
int lm_audio_pick_profiles(const struct lm_audio_profile *in, int n, int current_index, int *picked, int max, int *current_pick);

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

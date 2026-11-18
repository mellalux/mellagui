/*
 * mellagui-blocks - a world of blocks for MellaGUI (a Minecraft-style game; the name leaves the trademark out on purpose).
 *
 *   mellagui-blocks [--seed N] [--flat] [--world-size N] [--size WxH] [--render WxH] [--threads N] [--time H] [--no-clock]
 *                   [--yaw DEG] [--pitch DEG] [--fov DEG] [--save | --no-save]
 *   mellagui-blocks --selftest
 *
 * The world (client/blocks.c) is made from a seed: hills, a sea, tunnels, trees. You walk in it, break blocks and put them back; it is a creative
 * game (no hurt, no mobs). The picture (client/blocksview.c) is a ray for every pixel in software, so the window draws it at a smaller size and
 * scales it up: the size follows the time a frame takes (`--render WxH` fixes it).
 *
 * Click in the window to play: the pointer is locked to it (the compositor's zwp_pointer_constraints, wlwin_lock_pointer) and its motion turns
 * the head. Esc lets it go again (and pauses); so does Alt+Tab, a dialog of the shell or any other change of the keyboard focus, and the game
 * waits. Without a pointer lock (a compositor that has none) the arrow keys look round.
 * Keys (the PLACE of a key counts, not its letter): W A S D walk, Space jumps (up while flying, in water), Shift sneaks (down while flying) and does not walk
 * off an edge, Ctrl runs, F flies, the arrow keys look, 1 .. 9 or the wheel choose the block in the hand, the left button breaks (hold it to go on), the
 * right one puts the block in the hand against the face you aim at, the middle one takes the block you aim at into the hand, [ and ] move the clock by an hour,
 * T stops and starts it, F3 shows the numbers, F1 the keys, Ctrl+S saves, Ctrl+N a new world, Ctrl+Q quits (it saves).
 *
 * The world is kept as the edits to what its seed makes (`bl_save`) in $MELLAGUI_BLOCKS_DIR (default $XDG_DATA_HOME/mellagui/blocks) as world.bl, with
 * where you stand, the hand and the settings in player.conf. It is saved a minute after a change, when you pause, and when the window closes. `--seed` and
 * `--flat` make a new world that is NOT saved (so a look at a seed never replaces yours) unless `--save` is given.
 *
 * $MELLAGUI_BLOCKS_DUMP=FILE writes the state after every paint for the tests: where the player is, where he looks, the block aimed at, the hand, the
 * render size, the number of blocks of each kind, the boxes of the hotbar and of the buttons of a card.
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "blocks.h"
#include "blockspets.h"
#include "blocksmobs.h"
#include "blocksview.h"
#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "smartsun.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlwin.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* evdev key codes: the place of a key, not the letter it makes */
enum {
	K_ESC = 1, K_1 = 2, K_9 = 10, K_Q = 16, K_W = 17, K_T = 20, K_LBRACE = 26, K_RBRACE = 27, K_ENTER = 28, K_LCTRL = 29, K_A = 30, K_S = 31, K_D = 32,
	K_E = 18, K_F = 33, K_P = 25, K_M = 50, K_G = 34, K_X = 45, K_LSHIFT = 42, K_N = 49, K_RSHIFT = 54, K_SPACE = 57, K_F1 = 59, K_F3 = 61, K_RCTRL = 97, K_UP = 103, K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108,
};

#define SLOTS 9
#define REPEAT_MS 250                                       /* a held button breaks / places again after this */
#define AUTOSAVE_MS 60000
#define LOOK_RATE 1.7f                                      /* radians a second with the arrow keys */
#define MIN_BUDGET 40000                                    /* the least pixels the picture is made of, and the most it is asked to take (ms) */
#define SLOW_MS 27.0
#define MAX_HP 20.0f                                        /* ten hearts, two hit points each */
#define SWORD_MS 400                                        /* the least time between two blows */
#define KEEP_HP 60.0f                                       /* what the keep can take: a goblin's blow is 2 */
#define KEEP_H 3                                            /* it is a tower of three blocks */
#define FAST_MS 15.0

enum prompt { P_NONE, P_PAUSE, P_NEW, P_HELP, P_PALETTE, P_DEAD, P_LOST };

struct app {
	struct wlwin win;
	struct bl_world *world;
	struct bl_player pl;
	char dir[512];
	bool nosave, force_save;
	uint32_t saved_rev;
	int64_t last_save_ms;
	uint32_t seed_given;
	bool have_seed, flat;
	int world_size;

	bool playing;                                           /* the pointer is the player's: false while a card is up */
	bool lock_ok;                                           /* wlwin_lock_pointer was accepted */
	enum prompt prompt;
	bool first;                                             /* the card is the first one: it says how to start */
	int pc_hover, pc_down;
	bool ignore_release;                                    /* the click that started the game must not break a block */
	double px, py;
	bool has_pointer;

	bool k_fwd, k_back, k_left, k_right, k_jump, k_sneak, k_ctrl, k_lk, k_rk, k_uk, k_dk;
	bool jump_latch;                                        /* Space was pressed since the last step: a tap shorter than a frame still jumps */
	bool b_break, b_place;
	int64_t next_break, next_place;
	double wheel;
	int slot;
	int hotbar[SLOTS];
	int64_t name_until;
	float sens, fov;
	float time_h;
	int clockm;                                             /* the clock: 0 stands, 1 runs (a day is day_minutes), 2 follows the real sun (smartsun) */
	float day_minutes;
	int64_t real_ms;                                        /* when the real clock was read last */
	uint32_t settings;                                      /* mellagui_settings_v1: the compositor plays the sounds (card-sound) */
	bool sound_on;
	int64_t last_sound_ms;
	int nsounds;
	float water_acc;                                        /* time since the water was last looked at */
	struct bl_pets pets;                                    /* Tom and Filip (the Matrix room's cat and turtle) in a yard on the ground */
	bool pets_on;
	float pet_t;                                            /* seconds of their day: it runs while the game is played */
	int64_t pose_ms;
	float pet_start;
	bool have_yard;                                         /* player.conf had the yard: it stays where it was */
	float yard_x, yard_z;
	bool have_tom, have_filip;                              /* ... and where they were */
	float tom_pos[3], filip_pos[3];
	const uint8_t *glyph[MCAT_GLYPHS];                      /* the fur's letters: a spread over the Matrix rain's alphabet */
	int pal_hover;

	struct bl_mobs mobs;                                    /* the goblins: they come out at night */
	bool mobs_on;
	float hp;                                               /* the player's: MAX_HP, 0 = dead */
	float hurt_t;                                           /* seconds of the red flash that are left */
	float hit_t;                                            /* seconds the crosshair stays red after a blow that landed */
	float regen_t;                                          /* seconds since the last time he was hurt */
	int64_t next_sword;
	int kills;                                              /* goblins killed since the world started (this run) */
	bool warned;                                            /* "a goblin is coming" was said tonight */

	bool survival;                                          /* breaking takes time and gives what was broken, a block put is used up, the keep is to be defended; false = creative */
	bool conf_read, conf_mode;                              /* player.conf was there / had a `survival` line (an old file without one is a creative world) */
	int inv[BL_COUNT];                                      /* what the player has (survival) */
	bool dig_on;                                            /* a block is being broken: which, and for how long */
	int dig_x, dig_y, dig_z;
	float dig_t;
	bool have_keep;
	int keep[3];                                            /* the lowest block of the keep */
	float keep_hp;
	float keep_msg_t;                                       /* seconds until "the keep is under attack" may be said again */
	int nights;                                             /* nights survived with the keep standing */
	bool in_night;

	struct bl_hit aim;
	bool aimed;

	uint32_t *frame;
	size_t frame_cap;
	int rw, rh;
	int fix_w, fix_h;
	double budget;
	double ms_render;
	int threads;
	int64_t fps_t0;
	int fps_n, fps;
	bool debug;

	uint32_t *icon[BL_COUNT];
	int icon_s;

	char msg[160];
	int64_t msg_ms;
	long hist[BL_COUNT];
	uint32_t hist_rev;
	bool hist_ok;
};

static const char *g_dump;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(struct app *a, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(a->msg, sizeof a->msg, fmt, ap);
	va_end(ap);
	a->msg_ms = now_ms();
	a->win.redraw = true;
}

/* ================================================================ the hand and the clock ================================================================ */

static const int k_default_hotbar[SLOTS] = { BL_STONE, BL_DIRT, BL_GRASS, BL_COBBLE, BL_PLANKS, BL_LOG, BL_BRICK, BL_SAND, BL_GLASS };

/* a block that can be in the hand: any but air, bedrock and flowing water (water in the hand is a source) */
static bool hand_ok(int b)
{
	return b > BL_AIR && b < BL_COUNT && b != BL_BEDROCK && !(b >= BL_FLOW1 && b <= BL_FLOW7);
}

#define PAL_N 14
#define PAL_COLS 7
static const int k_palette[PAL_N] = { BL_GRASS, BL_DIRT, BL_STONE, BL_SAND, BL_LOG, BL_LEAVES, BL_PLANKS, BL_GLASS, BL_WATER, BL_BRICK, BL_SNOW, BL_COBBLE, BL_TORCH, BL_LAMP };

static int hand_block(const struct app *a)
{
	return a->hotbar[a->slot];
}

static void choose_slot(struct app *a, int slot)
{
	if (slot < 0)
		slot = SLOTS - 1;
	if (slot >= SLOTS)
		slot = 0;
	a->slot = slot;
	a->name_until = now_ms() + 1800;
	a->win.redraw = true;
}

/* the render size for a window of pw x ph pixels and a budget of pixels: the window's shape, at least 64 x 36, at most the window */
static void render_size(int pw, int ph, double budget, int *rw, int *rh)
{
	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;
	double b = budget;
	if (b > (double)pw * ph)
		b = (double)pw * ph;
	double aspect = (double)pw / (double)ph;
	int h = (int)floor(sqrt(b / aspect));
	int w = (int)floor((double)h * aspect);
	if (h < 36)
		h = 36;
	if (w < 64)
		w = 64;
	if (w > pw && pw >= 64)
		w = pw;
	if (h > ph && ph >= 36)
		h = ph;
	*rw = w;
	*rh = h;
}

/* the budget follows the time the last frame took: too slow -> fewer pixels, quick -> more, between MIN_BUDGET and the whole window */
static double budget_next(double budget, double ms, double max_px)
{
	if (ms > SLOW_MS)
		budget *= ms > 2.0 * SLOW_MS ? 0.75 : 0.92;
	else if (ms < FAST_MS)
		budget *= 1.06;
	if (budget < MIN_BUDGET)
		budget = MIN_BUDGET;
	if (budget > max_px)
		budget = max_px;
	return budget;
}

/* ================================================================ the files ================================================================ */

static int mkdirs(const char *path)
{
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	return mkdir(tmp, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

static void data_dir(char *out, size_t n)
{
	const char *d = getenv("MELLAGUI_BLOCKS_DIR"), *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (d && *d)
		snprintf(out, n, "%s", d);
	else if (x && *x)
		snprintf(out, n, "%s/mellagui/blocks", x);
	else
		snprintf(out, n, "%s/.local/share/mellagui/blocks", h && *h ? h : "/tmp");
}

static void world_path(const struct app *a, char *out, size_t n)
{
	snprintf(out, n, "%.500s/world.bl", a->dir);
}

static void conf_path(const struct app *a, char *out, size_t n)
{
	snprintf(out, n, "%.500s/player.conf", a->dir);
}

/* x y z yaw pitch time slot hotbar fov sens flying */
static void conf_load(struct app *a, bool with_player)
{
	char path[600];
	conf_path(a, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	a->conf_read = true;
	char line[2048];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*k == ' ')
			k++;
		for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); )
			*--e = 0;
		while (*v == ' ')
			v++;
		double d = strtod(v, NULL);
		if (!strcmp(k, "fov") && d >= 40 && d <= 120)
			a->fov = (float)(d * M_PI / 180.0);
		else if (!strcmp(k, "sensitivity") && d >= 0.0005 && d <= 0.02)
			a->sens = (float)d;
		else if (!strcmp(k, "clock") && d >= 0 && d <= 2)
			a->clockm = (int)d;
		else if (!strcmp(k, "sound") && (d == 0 || d == 1))
			a->sound_on = d != 0;
		else if (!strcmp(k, "pets") && (d == 0 || d == 1))
			a->pets_on = d != 0;
		else if (!strcmp(k, "mobs") && (d == 0 || d == 1))
			a->mobs_on = d != 0;
		else if (with_player && !strcmp(k, "survival") && (d == 0 || d == 1)) {
			a->survival = d != 0;
			a->conf_mode = true;
		} else if (with_player && !strcmp(k, "keep_hp") && d >= 0 && d <= KEEP_HP)
			a->keep_hp = (float)d;
		else if (with_player && !strncmp(k, "keep_", 5) && k[5] >= 'x' && k[5] <= 'z' && !k[6]) {
			a->keep[k[5] - 'x'] = (int)d;
			a->have_keep = true;
		} else if (with_player && !strcmp(k, "nights") && d >= 0 && d < 100000)
			a->nights = (int)d;
		else if (with_player && !strcmp(k, "kills") && d >= 0 && d < 1e6)
			a->kills = (int)d;
		else if (with_player && !strcmp(k, "inv")) {
			for (char *p = v; *p;) {
				char *end;
				long b = strtol(p, &end, 10);
				if (end == p || *end != ':')
					break;
				p = end + 1;
				long n = strtol(p, &end, 10);
				if (end == p)
					break;
				if (b > 0 && b < BL_COUNT && n >= 0 && n <= 99999)
					a->inv[b] = (int)n;
				p = end;
				while (*p == ',' || *p == ' ')
					p++;
			}
		}
		else if (with_player && !strcmp(k, "pet_t") && d >= 0 && d < 1e7)
			a->pet_t = (float)d;
		else if (with_player && !strcmp(k, "yard_x") && d > 0 && d < 5000) {
			a->yard_x = (float)d;
			a->have_yard = a->yard_z > 0;
		} else if (with_player && !strcmp(k, "yard_z") && d > 0 && d < 5000) {
			a->yard_z = (float)d;
			a->have_yard = a->yard_x > 0;
		} else if (with_player && !strncmp(k, "tom_", 4) && k[4] >= 'x' && k[4] <= 'z' && !k[5]) {
			a->tom_pos[k[4] - 'x'] = (float)d;
			a->have_tom = true;
		} else if (with_player && !strncmp(k, "filip_", 6) && k[6] >= 'x' && k[6] <= 'z' && !k[7]) {
			a->filip_pos[k[6] - 'x'] = (float)d;
			a->have_filip = true;
		}
		else if (!strcmp(k, "slot") && d >= 1 && d <= SLOTS)
			a->slot = (int)d - 1;
		else if (!strcmp(k, "hotbar")) {
			int n = 0;
			for (char *p = v; *p && n < SLOTS;) {
				char *end;
				long b = strtol(p, &end, 10);
				if (end == p)
					break;                                   /* not a number (the end of the line): stop, do not go round for ever */
				p = end;
				if (hand_ok((int)b))
					a->hotbar[n++] = (int)b;
				while (*p == ',' || *p == ' ')
					p++;
			}
		} else if (with_player) {
			if (!strcmp(k, "x")) a->pl.x = (float)d;
			else if (!strcmp(k, "y")) a->pl.y = (float)d;
			else if (!strcmp(k, "z")) a->pl.z = (float)d;
			else if (!strcmp(k, "yaw")) a->pl.yaw = (float)d;
			else if (!strcmp(k, "pitch")) a->pl.pitch = (float)d;
			else if (!strcmp(k, "time") && d >= 0 && d < 24) a->time_h = (float)d;
			else if (!strcmp(k, "flying")) a->pl.flying = d != 0;
		}
	}
	fclose(f);
}

static void conf_save(const struct app *a)
{
	char path[600], tmp[620];
	conf_path(a, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "x = %.3f\ny = %.3f\nz = %.3f\nyaw = %.4f\npitch = %.4f\ntime = %.3f\nflying = %d\nslot = %d\nclock = %d\nsound = %d\npets = %d\npet_t = %.2f\nmobs = %d\n", a->pl.x, a->pl.y, a->pl.z, a->pl.yaw,
	        a->pl.pitch, a->time_h, a->pl.flying, a->slot + 1, a->clockm, a->sound_on, a->pets_on, a->pet_t, a->mobs_on);
	if (a->pets.placed)
		fprintf(f, "yard_x = %.2f\nyard_z = %.2f\ntom_x = %.3f\ntom_y = %.3f\ntom_z = %.3f\nfilip_x = %.3f\nfilip_y = %.3f\nfilip_z = %.3f\n", a->pets.yard[0], a->pets.yard[2],
		        a->pets.tom.x, a->pets.tom.y, a->pets.tom.z, a->pets.filip.x, a->pets.filip.y, a->pets.filip.z);
	fprintf(f, "survival = %d\nnights = %d\nkills = %d\n", a->survival, a->nights, a->kills);
	if (a->have_keep)
		fprintf(f, "keep_x = %d\nkeep_y = %d\nkeep_z = %d\nkeep_hp = %.1f\n", a->keep[0], a->keep[1], a->keep[2], a->keep_hp);
	fprintf(f, "inv = ");
	{
		bool first = true;
		for (int b = 1; b < BL_COUNT; b++)
			if (a->inv[b] > 0) {
				fprintf(f, "%s%d:%d", first ? "" : ",", b, a->inv[b]);
				first = false;
			}
	}
	fprintf(f, "\nhotbar = ");
	for (int i = 0; i < SLOTS; i++)
		fprintf(f, "%s%d", i ? "," : "", a->hotbar[i]);
	fprintf(f, "\nfov = %.1f\nsensitivity = %.5f\n", a->fov * 180.0 / M_PI, a->sens);
	fclose(f);
	if (rename(tmp, path) != 0)
		unlink(tmp);
}

static bool save_all(struct app *a, bool say_it)
{
	if (a->nosave) {
		if (say_it)
			say(a, "This world is not saved (it was started with --seed or --flat)");
		return false;
	}
	if (mkdirs(a->dir) != 0) {
		say(a, "Cannot make %s", a->dir);
		return false;
	}
	char path[600];
	world_path(a, path, sizeof path);
	if (a->world->rev != a->saved_rev || a->force_save) {
		if (bl_save(a->world, path) != 0) {
			say(a, "Saving failed");
			return false;
		}
		a->saved_rev = a->world->rev;
		a->force_save = false;
	}
	conf_save(a);
	a->last_save_ms = now_ms();
	if (say_it)
		say(a, "Saved");
	return true;
}

/* The compositor plays them (write-only setting card-sound: draw place shuffle win no), so they follow Settings > Sound > Sound feedback and the chosen
 * output. The card game's sounds are borrowed: draw = a block broken, place = a block put, no = a block that could not be put. Never listened to as block sounds. */
static void sound(struct app *a, const char *name)
{
	int64_t now = now_ms();
	if (!a->sound_on || !a->settings || !a->win.c || now - a->last_sound_ms < 90)
		return;
	a->last_sound_ms = now;
	a->nsounds++;
	WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "card-sound" }, { .s = name });
	wlc_flush(a->win.c);
}

/* The hour of the model's day (6 sunrise, 18 sunset: what the renderer's sun does) for a moment of the real day, so that the real sunrise is the model's
 * 6 o'clock and the real sunset its 18: the day and the night are each stretched evenly (as in the Matrix room). Minutes since midnight in, hours out. */
static float solar_hours(double now_min, int rise, int set)
{
	if (rise < 0 || set > 1439 || set - rise < 30 || set - rise > 1410) {
		rise = 360;
		set = 1080;
	}
	double day = (double)(set - rise), h;
	if (now_min >= rise && now_min < set) {
		h = 6.0 + 12.0 * (now_min - rise) / day;
	} else {
		double since = now_min - set;
		if (since < 0)
			since += 1440.0;
		h = 18.0 + 12.0 * since / (1440.0 - day);
	}
	return (float)fmod(h, 24.0);
}

/* the real time of day (the clock of the computer; $MELLAGUI_BLOCKS_NOW = HH:MM[:SS] for the tests) and today's sunrise and sunset (smartsun: the place of
 * the smart home or the time zone's; unknown = 6 and 18) -> the model's hour */
static void real_clock_update(struct app *a, int64_t now)
{
	if (a->real_ms && now - a->real_ms < 900)
		return;
	a->real_ms = now;
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	double min = tm.tm_hour * 60.0 + tm.tm_min + tm.tm_sec / 60.0;
	const char *env = getenv("MELLAGUI_BLOCKS_NOW");
	int hh, mm, ss = 0;
	if (env && sscanf(env, "%d:%d:%d", &hh, &mm, &ss) >= 2)
		min = hh * 60.0 + mm + ss / 60.0;
	int rise, set;
	if (!sun_today(t, &rise, &set)) {
		rise = 360;
		set = 1080;
	}
	a->time_h = solar_hours(min, rise, set);
}

/* The Matrix rain's alphabet (digits, capitals, signs, accented letters, box drawing) and the 64 letters of the fur picked from it the way the screensaver does. */
static int fur_alphabet(const uint8_t **out)
{
	uint32_t set[160];
	int n = 0;
	for (uint32_t c = '0'; c <= '9'; c++)
		set[n++] = c;
	for (uint32_t c = 'A'; c <= 'Z'; c++)
		set[n++] = c;
	for (const char *q = ":.=*+-<>|\"'~^?!#$%&@"; *q; q++)
		set[n++] = (uint32_t)(unsigned char)*q;
	for (uint32_t c = 0xc0; c <= 0xff && n < 150; c++)
		if (c != 0xd7 && c != 0xf7)
			set[n++] = c;
	for (uint32_t c = 0x2550; c <= 0x256c && n < 158; c += 3)
		set[n++] = c;
	for (int i = 0; i < MCAT_GLYPHS; i++)
		out[i] = lm_font_glyph(set[(i * 7) % n]);
	return MCAT_GLYPHS;
}

/* Tom and Filip: a new day for the world's seed (0 = the script of the screensaver) and a yard near the player (or where player.conf had it) */
static void pets_setup(struct app *a, const float near[3], bool keep_yard)
{
	fur_alphabet(a->glyph);
	bl_pets_init(&a->pets, a->world->seed, a->glyph, MCAT_GLYPHS);
	a->pets.on = a->pets_on;
	if (keep_yard && a->have_yard)
		bl_pets_set_yard(&a->pets, a->world, a->yard_x, a->yard_z, NULL);
	else
		bl_pets_place(&a->pets, a->world, near);
	if (keep_yard && a->pets.placed)
		bl_pets_restore(&a->pets, a->world, a->have_tom ? a->tom_pos : NULL, a->have_filip ? a->filip_pos : NULL);
	a->pose_ms = 0;
}

/* the goblins: none yet, a fresh sequence from the world's seed, full health */
static void mobs_setup(struct app *a)
{
	bl_mobs_init(&a->mobs, a->world->seed);
	a->mobs.enabled = a->mobs_on;
	a->hp = MAX_HP;
	a->hurt_t = a->hit_t = 0.0f;
	a->regen_t = 0.0f;
	a->warned = false;
}

/* ================================================================ survival: what he has, what he makes, the keep ================================================================ */

/* A block comes into the inventory. One the hotbar does not show yet takes the place of a slot that is empty (the hand's own slot only when it is empty too). */
static void inv_add(struct app *a, int block, int n)
{
	if (block <= BL_AIR || block >= BL_COUNT || n <= 0)
		return;
	a->inv[block] += n;
	if (a->inv[block] > 99999)
		a->inv[block] = 99999;
	for (int i = 0; i < SLOTS; i++)
		if (a->hotbar[i] == block)
			return;
	for (int i = 0; i < SLOTS; i++)
		if (a->inv[a->hotbar[i]] == 0 && hand_ok(block)) {
			a->hotbar[i] = block;
			return;
		}
}

/* what a block can be made into by hand: one of it gives `n_out` of the other (X) */
struct recipe {
	int from, to, n_out;
};
static const struct recipe k_recipes[] = {
	{ BL_LOG, BL_PLANKS, 4 }, { BL_PLANKS, BL_TORCH, 2 }, { BL_SAND, BL_GLASS, 1 }, { BL_COBBLE, BL_STONE, 1 }, { BL_STONE, BL_BRICK, 1 },
};
#define N_RECIPES ((int)(sizeof k_recipes / sizeof k_recipes[0]))

static const struct recipe *recipe_for(int block)
{
	for (int i = 0; i < N_RECIPES; i++)
		if (k_recipes[i].from == block)
			return &k_recipes[i];
	return NULL;
}

/* make something of the block in the hand: false (and a message) when there is no way or he has none */
static bool craft_hand(struct app *a)
{
	int b = hand_block(a);
	const struct recipe *r = recipe_for(b);
	if (!r) {
		say(a, "A %s cannot be made into anything", bl_block_name(b));
		return false;
	}
	if (a->inv[b] < 1) {
		say(a, "You have no %s", bl_block_name(b));
		return false;
	}
	a->inv[b]--;
	inv_add(a, r->to, r->n_out);
	say(a, "Made %d %s", r->n_out, bl_block_name(r->to));
	return true;
}

/* the keep: three blocks high on open ground 4 to 9 blocks from (cx, cz), the first place found going round from straight ahead (+z) */
static bool keep_place(struct app *a, float cx, float cz)
{
	static const int rings[] = { 6, 7, 5, 8, 9, 4 };
	struct bl_world *w = a->world;
	for (size_t ri = 0; ri < sizeof rings / sizeof rings[0]; ri++)
		for (int k = 0; k < 8; k++) {
			float ang = (float)k * 0.785398f;
			int x = (int)floorf(cx + sinf(ang) * (float)rings[ri]), z = (int)floorf(cz + cosf(ang) * (float)rings[ri]);
			if (x < 3 || z < 3 || x >= w->w - 3 || z >= w->d - 3)
				continue;
			int top = bl_top(w, x, z);
			if (top < 1 || top + KEEP_H + 1 >= w->h)
				continue;
			bool ok = !bl_water(bl_get(w, x, top, z));
			for (int i = 1; i <= KEEP_H && ok; i++)
				ok = bl_get(w, x, top + i, z) == BL_AIR;
			if (!ok)
				continue;
			for (int i = 1; i <= KEEP_H; i++)
				bl_set(w, x, top + i, z, BL_KEEP);
			a->keep[0] = x;
			a->keep[1] = top + 1;
			a->keep[2] = z;
			a->have_keep = true;
			return true;
		}
	return false;
}

/* the goblins know where the keep is, and only in survival */
static void keep_sync(struct app *a)
{
	bl_mobs_set_keep(&a->mobs, a->survival && a->have_keep, (float)a->keep[0] + 0.5f, (float)a->keep[1], (float)a->keep[2] + 0.5f);
}

/* In survival there is a keep: the one the world has (its blocks are in the world), else a new one near (cx, cz) with all its hit points. */
static void ensure_keep(struct app *a, float cx, float cz)
{
	if (a->survival && !(a->have_keep && bl_get(a->world, a->keep[0], a->keep[1], a->keep[2]) == BL_KEEP)) {
		a->have_keep = false;
		if (keep_place(a, cx, cz))
			a->keep_hp = KEEP_HP;
	}
	keep_sync(a);
}

/* a new world in survival: nothing in the hands, no nights behind him, a keep near where he starts */
static void survival_setup(struct app *a, float cx, float cz)
{
	memset(a->inv, 0, sizeof a->inv);
	a->nights = 0;
	a->in_night = false;
	a->dig_on = false;
	a->dig_t = 0.0f;
	a->have_keep = false;
	a->keep_hp = KEEP_HP;
	ensure_keep(a, cx, cz);
}

/* ================================================================ the world ================================================================ */

static uint32_t fresh_seed(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint32_t)(ts.tv_nsec * 2654435761u) ^ (uint32_t)ts.tv_sec ^ ((uint32_t)getpid() << 16);
}

/* a moment of standing still, so that the player rests on what is under him (on_ground is right from the start) */
static void settle_player(struct app *a)
{
	struct bl_input none = { 0 };
	for (int i = 0; i < 5; i++)
		bl_player_step(a->world, &a->pl, &none, 0.02f);
}

static bool make_world(struct app *a, uint32_t seed, bool flat)
{
	struct bl_world *w = flat ? bl_world_flat(a->world_size, a->world_size, 64, 20) : bl_world_new(seed, a->world_size, a->world_size, 64);
	if (!w)
		return false;
	bl_world_free(a->world);
	a->world = w;
	bl_player_init(&a->pl, w);
	settle_player(a);
	a->saved_rev = w->rev;
	a->force_save = true;
	a->hist_ok = false;
	return true;
}

static void new_world(struct app *a)
{
	if (!make_world(a, fresh_seed(), false)) {
		say(a, "Cannot make a world");
		return;
	}
	a->nosave = false;
	a->time_h = 9.0f;
	a->pet_t = 0.0f;
	a->have_yard = false;
	a->have_tom = a->have_filip = false;
	mobs_setup(a);
	a->kills = 0;
	survival_setup(a, a->pl.x, a->pl.z);
	{
		float near[3] = { a->pl.x, a->pl.y, a->pl.z };
		pets_setup(a, near, false);
	}
	say(a, "A new world (seed %u)", a->world->seed);
}

/* ================================================================ the icons of the hotbar ================================================================ */

/* A block as a cube seen from a corner, S x S pixels, straight ARGB, see-through outside (made once for the size of the buffer). */
static void icon_build(uint32_t *out, int S, int block)
{
	const uint32_t *top = bl_texture(block, BL_FACE_TOP), *side = bl_texture(block, BL_FACE_SIDE);
	for (int py = 0; py < S; py++)
		for (int px = 0; px < S; px++) {
			float x = (((float)px + 0.5f) / (float)S * 2.0f - 1.0f) * 0.866f, y = ((float)py + 0.5f) / (float)S * 2.0f - 1.0f;
			const uint32_t *tex = NULL;
			float u = 0, v = 0, shade = 1.0f;
			if (block == BL_TORCH) {                                         /* not a cube: a stick with a flame */
				float fu = ((float)px + 0.5f) / (float)S, fv = ((float)py + 0.5f) / (float)S;
				uint32_t c = 0;
				if (fabsf(fu - 0.5f) < 0.075f && fv > 0.40f && fv < 0.92f)
					c = 0xff000000u | ((uint32_t)(0x7a - (int)(fabsf(fu - 0.5f) * 200)) << 16) | 0x5428u;
				else if (fv > 0.12f && fv < 0.42f && fabsf(fu - 0.5f) < (fv < 0.22f ? 0.08f + (fv - 0.12f) * 0.8f : 0.16f - (fv - 0.22f) * 0.1f))
					c = 0xff000000u | (fv < 0.27f ? 0xffe060u : fv < 0.34f ? 0xffb030u : 0xf07818u);
				out[py * S + px] = c;
				continue;
			}
			float ta = -y + x / 1.732f, tb = -y - x / 1.732f;               /* the top face: C + a * (0.866, -0.5) + b * (-0.866, -0.5) */
			if (ta >= 0 && ta <= 1 && tb >= 0 && tb <= 1) {
				tex = top;
				u = ta;
				v = tb;
				shade = 1.0f;
			} else if (x < 0) {                                              /* the left face */
				float s = (x + 0.866f) / 0.866f, t = (y + 0.5f) - 0.5f * s;
				if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
					tex = side;
					u = s;
					v = t;
					shade = 0.78f;
				}
			} else {                                                         /* the right face */
				float s = (0.866f - x) / 0.866f, t = (y + 0.5f) - 0.5f * s;
				if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
					tex = side;
					u = 1.0f - s;
					v = t;
					shade = 0.58f;
				}
			}
			uint32_t c = 0;
			if (tex) {
				int tu = LM_CLAMP((int)(u * 16.0f), 0, 15), tv = LM_CLAMP((int)(v * 16.0f), 0, 15);
				uint32_t t = tex[tv * 16 + tu];
				if (block == BL_WATER)
					t = (t & 0x00ffffffu) | (215u << 24);               /* water is see-through in the world, a pale cube in the hand */
				c = (t & 0xff000000u) | (uint32_t)((float)((t >> 16) & 255) * shade) << 16 | (uint32_t)((float)((t >> 8) & 255) * shade) << 8 |
				    (uint32_t)((float)(t & 255) * shade);
			}
			out[py * S + px] = c;
		}
}

static void icons_free(struct app *a)
{
	for (int b = 0; b < BL_COUNT; b++) {
		free(a->icon[b]);
		a->icon[b] = NULL;
	}
}

static void icons_for(struct app *a, int S)
{
	if (S == a->icon_s && a->icon[BL_STONE])
		return;
	icons_free(a);
	a->icon_s = S;
	for (int b = 1; b < BL_COUNT; b++) {
		if (b == BL_BEDROCK || (b >= BL_FLOW1 && b <= BL_FLOW7))
			continue;
		a->icon[b] = malloc((size_t)S * (size_t)S * 4);
		if (a->icon[b])
			icon_build(a->icon[b], S, b);
	}
}

/* ================================================================ the cursor and the pointer lock ================================================================ */

static void arrow_cursor(uint32_t *px, int w, int h)
{
	memset(px, 0, (size_t)w * (size_t)h * 4);
	for (int y = 0; y < 17; y++)                                             /* a plain arrow: a triangle with a tail, white with a dark rim */
		for (int x = 0; x <= y * 11 / 17 + 1 && x < 12; x++)
			if (x == 0 || x == y * 11 / 17 + 1 || y == 16)
				px[(y + 1) * w + x + 1] = 0xff101010u;
			else
				px[(y + 1) * w + x + 1] = 0xfff8f8f8u;
	for (int y = 11; y < 20; y++)
		for (int x = 4; x < 8; x++)
			px[(y + 1) * w + x + 1] = (x == 4 || x == 7 || y == 19) ? 0xff101010u : 0xfff8f8f8u;
}

static void pause_game(struct app *a, enum prompt p)
{
	a->playing = false;
	a->prompt = p;
	a->k_fwd = a->k_back = a->k_left = a->k_right = a->k_jump = a->k_sneak = a->k_ctrl = a->k_lk = a->k_rk = a->k_uk = a->k_dk = false;
	a->b_break = a->b_place = false;
	a->dig_on = false;
	a->dig_t = 0.0f;
	a->pc_hover = a->pc_down = a->pal_hover = -1;
	wlwin_lock_pointer(&a->win, false);
	wlwin_hide_cursor(&a->win, false);
	a->win.redraw = true;
}

static void resume_game(struct app *a)
{
	a->prompt = P_NONE;
	a->first = false;
	a->playing = true;
	a->lock_ok = wlwin_lock_pointer(&a->win, true);
	wlwin_hide_cursor(&a->win, true);
	if (!a->lock_ok)
		say(a, "No pointer lock here: look with the arrow keys");
	a->win.redraw = true;
}

/* ================================================================ one step of the game ================================================================ */

static void aim_update(struct app *a)
{
	float eye[3], look[3];
	bl_player_eye(&a->pl, eye);
	bl_player_look(&a->pl, look);
	a->aimed = bl_pick(a->world, eye, look, BL_REACH, &a->aim);
}

/* the sword: a goblin the player aims at, nearer than the block he aims at, takes the blow (and nothing is broken behind it) */
static bool sword_swing(struct app *a, int64_t now)
{
	if (!a->mobs_on)
		return false;
	float eye[3], look[3], t = 0.0f;
	bl_player_eye(&a->pl, eye);
	bl_player_look(&a->pl, look);
	int id = bl_mobs_pick(&a->mobs, eye, look, BL_SWORD_REACH, &t);
	if (id < 0 || (a->aimed && a->aim.t < t))
		return false;
	if (now < a->next_sword)
		return true;                                         /* too soon for another blow, and the block behind it is not touched */
	a->next_sword = now + SWORD_MS;
	bool dead = bl_mobs_hurt(&a->mobs, id, BL_SWORD_DAMAGE, look[0], look[2]);
	a->hit_t = 0.18f;
	if (dead) {
		a->kills++;
		sound(a, "win");
		say(a, "Goblin down (%d)", a->kills);
	} else {
		sound(a, "draw");
	}
	return true;
}

static void do_break(struct app *a, int64_t now)
{
	if (sword_swing(a, now))
		return;
	if (a->aimed && bl_break(a->world, &a->aim)) {
		sound(a, "draw");
		aim_update(a);
	}
}

/* survival: the button is held on a block: the sword first, else the block takes its time (`bl_hardness`) and gives what it leaves (`bl_drop`) */
static void dig_step(struct app *a, int64_t now, float dt)
{
	if (sword_swing(a, now)) {
		a->dig_on = false;
		return;
	}
	if (!a->aimed) {
		a->dig_on = false;
		return;
	}
	int b = bl_get(a->world, a->aim.x, a->aim.y, a->aim.z);
	if (!bl_breakable(b)) {
		if (b == BL_KEEP && !a->dig_on) {
			a->dig_on = true;                                       /* (said once while the button is held) */
			say(a, "The keep cannot be broken");
		}
		return;
	}
	if (!a->dig_on || a->dig_x != a->aim.x || a->dig_y != a->aim.y || a->dig_z != a->aim.z) {
		a->dig_on = true;
		a->dig_x = a->aim.x;
		a->dig_y = a->aim.y;
		a->dig_z = a->aim.z;
		a->dig_t = 0.0f;
	}
	a->dig_t += dt;
	if (a->dig_t >= bl_hardness(b)) {
		if (bl_break(a->world, &a->aim)) {
			int d = bl_drop(b);
			if (d != BL_AIR)
				inv_add(a, d, 1);
			sound(a, "draw");
			aim_update(a);
		}
		a->dig_on = false;
		a->dig_t = 0.0f;
	}
}

static void do_place(struct app *a)
{
	if (!a->aimed)
		return;
	if ((bl_pets_hit_cell(&a->pets, a->aim.px, a->aim.py, a->aim.pz) || (a->mobs_on && bl_mobs_hit_cell(&a->mobs, a->aim.px, a->aim.py, a->aim.pz))) &&
	    bl_solid(hand_block(a))) {                              /* a block cannot be put into Tom, Filip or a goblin */
		sound(a, "no");
		return;
	}
	if (a->survival && a->inv[hand_block(a)] < 1) {
		sound(a, "no");
		say(a, "You have no %s", bl_block_name(hand_block(a)));
		return;
	}
	if (bl_place(a->world, &a->aim, hand_block(a), &a->pl)) {
		if (a->survival)
			a->inv[hand_block(a)]--;
		sound(a, "place");
		aim_update(a);
	} else {
		sound(a, "no");
	}
}

static void do_pick(struct app *a)
{
	if (!a->aimed)
		return;
	int b = bl_get(a->world, a->aim.x, a->aim.y, a->aim.z);
	if (!hand_ok(b))
		return;
	for (int i = 0; i < SLOTS; i++)
		if (a->hotbar[i] == b) {
			choose_slot(a, i);
			return;
		}
	a->hotbar[a->slot] = b;
	choose_slot(a, a->slot);
}

static void look_by(struct app *a, double dyaw, double dpitch)
{
	a->pl.yaw += (float)dyaw;
	if (a->pl.yaw > (float)M_PI)
		a->pl.yaw -= 2.0f * (float)M_PI;
	if (a->pl.yaw < -(float)M_PI)
		a->pl.yaw += 2.0f * (float)M_PI;
	float p = a->pl.pitch + (float)dpitch;
	a->pl.pitch = p < -1.553f ? -1.553f : p > 1.553f ? 1.553f : p;
}

/* the keep has no hit points left: it is gone, and the game with it (a card) */
static void keep_fall(struct app *a)
{
	for (int i = 0; i < KEEP_H; i++)
		bl_set(a->world, a->keep[0], a->keep[1] + i, a->keep[2], BL_AIR);
	a->have_keep = false;
	a->keep_hp = 0.0f;
	keep_sync(a);
	pause_game(a, P_LOST);
	say(a, "The keep has fallen");
}

/* The goblins move; their blows take hit points and push the player; he mends slowly when nobody has hit him for a while; no hit points left = dead (a card). */
static void mobs_advance(struct app *a, float dt)
{
	float feet[3] = { a->pl.x, a->pl.y, a->pl.z };
	struct bl_mobs_result R;
	int before = bl_mobs_count(&a->mobs);
	bl_mobs_step(&a->mobs, a->world, feet, a->hp > 0.0f, a->time_h, dt, &R);
	if (R.keep_damage > 0.0f && a->survival && a->have_keep) {
		a->keep_hp -= R.keep_damage;
		if (a->keep_msg_t <= 0.0f) {
			say(a, "The keep is under attack!");
			sound(a, "no");
			a->keep_msg_t = 8.0f;
		}
		if (a->keep_hp <= 0.0f)
			keep_fall(a);
	}
	if (R.born && !a->warned && before == 0) {
		a->warned = true;
		say(a, "A goblin! The left button is the sword");
	}
	if (bl_mobs_count(&a->mobs) == 0 && bl_daylight(a->time_h) > 0.6f)
		a->warned = false;
	if (R.damage > 0.0f) {
		a->hp -= R.damage;
		a->hurt_t = 0.45f;
		a->regen_t = 0.0f;
		a->pl.vx += R.kbx;
		a->pl.vz += R.kbz;
		if (a->pl.on_ground && !a->pl.flying) {
			a->pl.vy = 4.5f;
			a->pl.on_ground = false;
		}
		sound(a, "no");
		if (a->hp <= 0.0f) {
			a->hp = 0.0f;
			pause_game(a, P_DEAD);
			say(a, "A goblin got you");
		}
	} else if (a->hp < MAX_HP) {
		a->regen_t += dt;
		if (a->regen_t > 6.0f)
			a->hp = fminf(MAX_HP, a->hp + dt / 3.0f);       /* a hit point every three seconds */
	}
}

static void respawn(struct app *a)
{
	bl_player_init(&a->pl, a->world);
	settle_player(a);
	a->hp = MAX_HP;
	a->hurt_t = a->hit_t = 0.0f;
	a->regen_t = 0.0f;
	bl_mobs_clear(&a->mobs);
	a->mobs.spawn_t = 8.0f;                                    /* a breather before the next one */
	say(a, "Back at the start");
}

/* survival: the keep mends by day, the nights are counted (a night is over when the sun is well up again) */
static void survival_tick(struct app *a, float dt)
{
	a->keep_msg_t = fmaxf(0.0f, a->keep_msg_t - dt);
	float day = bl_daylight(a->time_h);
	if (day < 0.3f) {
		a->in_night = true;
	} else if (day > 0.6f) {
		if (a->in_night && a->have_keep) {
			a->nights++;
			say(a, "Night %d survived", a->nights);
		}
		a->in_night = false;
		if (a->have_keep && a->keep_hp < KEEP_HP)
			a->keep_hp = fminf(KEEP_HP, a->keep_hp + 1.5f * dt);
	}
}

static void advance(struct app *a, int64_t now, float dt)
{
	if (a->playing) {
		if (a->k_lk || a->k_rk || a->k_uk || a->k_dk)
			look_by(a, ((a->k_rk ? 1 : 0) - (a->k_lk ? 1 : 0)) * LOOK_RATE * dt, ((a->k_uk ? 1 : 0) - (a->k_dk ? 1 : 0)) * LOOK_RATE * dt);
		struct bl_input in = { .fwd = (a->k_fwd ? 1.0f : 0.0f) - (a->k_back ? 1.0f : 0.0f), .strafe = (a->k_right ? 1.0f : 0.0f) - (a->k_left ? 1.0f : 0.0f),
		                       .jump = a->k_jump || a->jump_latch, .sneak = a->k_sneak, .sprint = a->k_ctrl && a->k_fwd };
		bl_player_step(a->world, &a->pl, &in, dt);
		a->jump_latch = false;
		if (a->pets_on && a->pets.placed) {
			bl_pets_step(&a->pets, a->world, dt);
			bl_pets_push_player(&a->pets, a->world, &a->pl);
		}
		if (a->mobs_on)
			mobs_advance(a, dt);
		a->hurt_t = fmaxf(0.0f, a->hurt_t - dt);
		a->hit_t = fmaxf(0.0f, a->hit_t - dt);
		if (a->playing)                                      /* (he may have died in that step) */
			aim_update(a);
		if (a->survival) {
			if (a->b_break && a->playing)
				dig_step(a, now, dt);
			survival_tick(a, dt);
		} else if (a->b_break && now >= a->next_break) {
			do_break(a, now);
			a->next_break = now + REPEAT_MS;
		}
		if (a->b_place && now >= a->next_place) {
			do_place(a);
			a->next_place = now + REPEAT_MS;
		}
		if (a->clockm == 1) {
			a->time_h += dt * 24.0f / (a->day_minutes * 60.0f);
			if (a->time_h >= 24.0f)
				a->time_h -= 24.0f;
		} else if (a->clockm == 2) {
			real_clock_update(a, now);
		}
		a->pet_t += dt;
		a->water_acc += dt;                                  /* the water flows four times a second */
		if (a->water_acc >= 0.25f) {
			a->water_acc = 0;
			bl_world_tick(a->world, 3000);
		}
	}
	if (!a->nosave && a->world->rev != a->saved_rev && now - a->last_save_ms >= AUTOSAVE_MS)
		save_all(a, false);
}

/* ================================================================ the picture ================================================================ */

struct lay {
	int slot, gap, x0, y0;                                  /* the hotbar: the size of a slot, the gap, the top left of the first */
};

static void layout(const struct app *a, struct lay *L)
{
	L->gap = 4;
	L->slot = LM_CLAMP((a->win.bw - 24) / SLOTS - L->gap, 28, 48);
	int total = SLOTS * L->slot + (SLOTS - 1) * L->gap;
	L->x0 = (a->win.bw - total) / 2;
	L->y0 = a->win.bh - L->slot - 12;
}

static void shadow_text(struct wlwin *w, int x, int y, const char *s, uint32_t rgb)
{
	wlwin_text(w, x + 1, y + 1, s, 1, 0x101010u, 0);
	wlwin_text(w, x, y, s, 1, rgb, 0);
}

static void paint_crosshair(struct wlwin *w, bool hit)
{
	int cx = w->bw / 2, cy = w->bh / 2;
	uint32_t c = hit ? 0xff4a3au : 0xf4f4f4u;
	wlwin_fill(w, cx - 10, cy - 2, 21, 4, 0x101010u);
	wlwin_fill(w, cx - 2, cy - 10, 4, 21, 0x101010u);
	wlwin_fill(w, cx - 9, cy - 1, 19, 2, c);
	wlwin_fill(w, cx - 1, cy - 9, 2, 19, c);
}

/* a blow: the picture goes red, most at its edges (a dead player's picture stays red) */
static void red_flash(struct app *a)
{
	float k = a->hp <= 0.0f ? 0.8f : a->hurt_t / 0.45f;
	if (k <= 0.0f)
		return;
	int W = a->rw, H = a->rh;
	for (int y = 0; y < H; y++) {
		float fy = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
		for (int x = 0; x < W; x++) {
			float fx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f, e = fx * fx + fy * fy;
			float f = k * (0.22f + 0.45f * (e > 1.0f ? 1.0f : e));
			uint32_t c = a->frame[y * W + x];
			int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
			r = (int)((float)r * (1.0f - f) + 230.0f * f);
			g = (int)((float)g * (1.0f - f) + 20.0f * f);
			b = (int)((float)b * (1.0f - f) + 20.0f * f);
			a->frame[y * W + x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
}

/* ten hearts over the left end of the hotbar: two hit points each, a half heart is half red */
static void paint_hearts(struct app *a, const struct lay *L)
{
	static const char *const k_heart[6] = { ".##.##.", "#######", "#######", ".#####.", "..###..", "...#..." };
	struct wlwin *w = &a->win;
	int sc = L->slot >= 36 ? 3 : 2, hw = 7 * sc, hh = 6 * sc, gap = sc + 1;
	int y0 = L->y0 - 12 - hh - 6;
	for (int i = 0; i < 10; i++) {
		int x0 = L->x0 + i * (hw + gap);
		float fill = (a->hp - (float)i * 2.0f) / 2.0f;                     /* 1 = full, 0.5 = half, 0 = empty */
		fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
		for (int r = 0; r < 6; r++)
			for (int c = 0; c < 7; c++) {
				if (k_heart[r][c] != '#')
					continue;
				uint32_t col = 0x3a1014u;
				if (fill >= 1.0f || (fill >= 0.5f && c < 4))
					col = r == 0 || (r == 1 && c < 3) ? 0xff6a6au : 0xe02a2au;
				wlwin_fill(w, x0 + c * sc, y0 + r * sc, sc, sc, col);
			}
	}
}

static void paint_hotbar(struct app *a, const struct lay *L)
{
	struct wlwin *w = &a->win;
	int sc = wlwin_scale(w);
	int isz = L->slot - 8;
	icons_for(a, isz * sc);
	int total = SLOTS * L->slot + (SLOTS - 1) * L->gap;
	wlwin_round_fill(w, L->x0 - 6, L->y0 - 6, total + 12, L->slot + 12, 8, 0x12151bu);
	for (int i = 0; i < SLOTS; i++) {
		int x = L->x0 + i * (L->slot + L->gap), y = L->y0;
		if (i == a->slot) {
			wlwin_round_fill(w, x - 3, y - 3, L->slot + 6, L->slot + 6, 6, 0xf0f0f0u);
			wlwin_round_fill(w, x - 1, y - 1, L->slot + 2, L->slot + 2, 5, 0x2b3039u);
		} else {
			wlwin_round_fill(w, x, y, L->slot, L->slot, 5, 0x2b3039u);
		}
		const uint32_t *ic = a->icon[a->hotbar[i]];
		if (ic)
			wlwin_blit_px_over(w, x + 4, y + 4, ic, isz * sc, isz * sc, isz * sc);
		char n[2] = { (char)('1' + i), 0 };
		wlwin_text(w, x + 3, y + 1, n, 1, i == a->slot ? 0xffffffu : 0xa8b0bcu, 0);
		if (a->survival) {                                                     /* how many he has: red when none */
			int have = a->inv[a->hotbar[i]];
			char cnt[16];
			snprintf(cnt, sizeof cnt, "%d", have > 999 ? 999 : have);
			shadow_text(w, x + L->slot - lm_font_text_width(cnt) - 3, y + L->slot - LM_FONT_H - 1, cnt, have > 0 ? 0xffffffu : 0xff6a5au);
		}
	}
	if (now_ms() < a->name_until) {
		const char *nm = bl_block_name(hand_block(a));
		shadow_text(w, (w->bw - lm_font_text_width(nm)) / 2, L->y0 - 30, nm, 0xffffffu);
	}
}

/* survival: the keep's hit points over the middle of the top, the nights survived, and under the crosshair how far the block is broken */
static void paint_survival(struct app *a)
{
	struct wlwin *w = &a->win;
	if (a->have_keep) {
		int bw = LM_CLAMP(w->bw / 3, 140, 260), x = (w->bw - bw) / 2, y = 8;
		float f = a->keep_hp / KEEP_HP;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		wlwin_round_fill(w, x - 2, y - 2, bw + 4, 16, 6, 0x101216u);
		wlwin_round_fill(w, x, y, bw, 12, 5, 0x3a2a2au);
		if (f > 0.0f)
			wlwin_round_fill(w, x, y, LM_MAX(8, (int)((float)bw * f)), 12, 5, f > 0.5f ? 0x4cb050u : f > 0.25f ? 0xe0a030u : 0xe04030u);
		char l[80];
		snprintf(l, sizeof l, "Keep %d   Nights %d", (int)ceilf(a->keep_hp), a->nights);
		shadow_text(w, (w->bw - lm_font_text_width(l)) / 2, y + 18, l, 0xffffffu);
	}
	if (a->dig_on) {
		float hd = bl_hardness(bl_get(a->world, a->dig_x, a->dig_y, a->dig_z)), f = hd > 0.0f ? a->dig_t / hd : 0.0f;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		int cx = w->bw / 2, cy = w->bh / 2 + 22;
		wlwin_fill(w, cx - 24, cy - 1, 48, 7, 0x101010u);
		wlwin_fill(w, cx - 23, cy, (int)(46.0f * f), 5, 0xf0e8d0u);
	}
}

static void paint_debug(struct app *a)
{
	struct wlwin *w = &a->win;
	char l[200];
	int y = 6;
	snprintf(l, sizeof l, "x %.2f  y %.2f  z %.2f", a->pl.x, a->pl.y, a->pl.z);
	shadow_text(w, 8, y, l, 0xffffffu);
	y += LM_FONT_H + 2;
	snprintf(l, sizeof l, "looking %.0f / %.0f degrees   %s%s%s", a->pl.yaw * 180.0 / M_PI, a->pl.pitch * 180.0 / M_PI, a->pl.on_ground ? "on the ground" : "in the air",
	         a->pl.in_water ? ", in water" : "", a->pl.flying ? ", flying" : "");
	shadow_text(w, 8, y, l, 0xffffffu);
	y += LM_FONT_H + 2;
	snprintf(l, sizeof l, "%d frames a second, %dx%d drawn in %.0f ms", a->fps, a->rw, a->rh, a->ms_render);
	shadow_text(w, 8, y, l, 0xffffffu);
	y += LM_FONT_H + 2;
	int h = (int)a->time_h, m = (int)((a->time_h - (float)h) * 60.0f);
	snprintf(l, sizeof l, "%02d:%02d   seed %u   %d x %d x %d", h, m, a->world->seed, a->world->w, a->world->d, a->world->h);
	shadow_text(w, 8, y, l, 0xffffffu);
	y += LM_FONT_H + 2;
	if (a->aimed)
		snprintf(l, sizeof l, "aiming at %s (%d %d %d)", bl_block_name(bl_get(a->world, a->aim.x, a->aim.y, a->aim.z)), a->aim.x, a->aim.y, a->aim.z);
	else
		snprintf(l, sizeof l, "aiming at nothing");
	shadow_text(w, 8, y, l, 0xffffffu);
}

static const char *const HELP_LINES[] = {
	"W A S D walk, Space jumps (swims up, flies up), Shift sneaks (flies down).",
	"Ctrl runs. F flies. The arrow keys look round.",
	"The left button breaks a block (hold it), the right one puts the block in",
	"your hand against the face you aim at, the middle one takes the block you aim at.",
	"1 to 9 or the wheel choose the block in the hand, E opens the palette of every block.",
	"Torches and lamps give light. Water put by hand is a source and flows.",
	"[ and ] move the clock an hour, T: the clock stands, runs, follows the real sun.",
	"P shows or hides Tom the cat and Filip the turtle (they live in a yard near where you start).",
	"At night goblins come: the left button is the sword (three blows). Ten hearts, M switches them off.",
	"Survival (G switches it and creative): blocks take time, what you break you have, a block put is used up,",
	"X makes something of the block in your hand (log: planks, planks: torches). Defend the keep, the tower near the start.",
	"F3 shows the numbers. Ctrl+S saves, Ctrl+N makes a new world, Esc pauses, Ctrl+Q quits.",
};

static int pal_cell(const struct app *a)                   /* the size of a square of the palette */
{
	struct lay L;
	layout(a, &L);
	return L.slot + 10;
}

static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
	*nb = a->prompt == P_PAUSE ? 4 : a->prompt == P_NEW ? 2 : 1;
	int ch = a->prompt == P_HELP ? 420 : a->prompt == P_PAUSE ? 200 : (a->prompt == P_DEAD || a->prompt == P_LOST) ? 150 : 130;
	int cw = a->prompt == P_HELP ? 600 : 470;
	if (a->prompt == P_PALETTE) {
		int cell = pal_cell(a);
		cw = PAL_COLS * cell + 2 * WLCARD_PAD;
		ch = 124 + ((PAL_N + PAL_COLS - 1) / PAL_COLS) * cell;
	}
	c->w = LM_MIN(cw, a->win.bw - 16);
	c->h = LM_MIN(ch, a->win.bh - 16);
	wlcard_center(&a->win, c);
}

/* the palette's squares: the top left of the first one, and which one is at a point */
static void pal_origin(const struct app *a, const struct wlcard *c, int *gx, int *gy)
{
	*gx = c->x + (c->w - PAL_COLS * pal_cell(a)) / 2;
	*gy = c->y + 40;
}

static int pal_cell_at(const struct app *a, double px, double py)
{
	if (a->prompt != P_PALETTE)
		return -1;
	struct wlcard c;
	int nb, gx, gy, cell = pal_cell(a);
	card_rect(a, &c, &nb);
	pal_origin(a, &c, &gx, &gy);
	int col = (int)floor((px - gx) / cell), row = (int)floor((py - gy) / cell);
	if (px < gx || py < gy || col < 0 || col >= PAL_COLS || row < 0)
		return -1;
	int i = row * PAL_COLS + col;
	return i < PAL_N ? i : -1;
}

static void paint_card(struct app *a)
{
	struct wlwin *w = &a->win;
	struct wlcard c;
	int nb;
	card_rect(a, &c, &nb);
	const char *title = a->prompt == P_LOST ? "The keep has fallen" : a->prompt == P_DEAD ? "You died" : a->prompt == P_NEW ? "A new world" : a->prompt == P_HELP ? "Keys" : a->prompt == P_PALETTE ? "Blocks" : a->first ? "Blocks" : "Paused";
	wlcard_frame(w, &c, c.w, c.h, title, lm_theme.accent, lm_theme.background, lm_theme.text_bright);
	const char *labels[4] = { "Cancel", "OK" };
	uint32_t cols[4] = { lm_theme.btn_grey, lm_theme.btn_blue };
	int tx = c.x + WLCARD_PAD, ty = c.y + 44, tw = c.w - 2 * WLCARD_PAD;
	if (a->prompt == P_PAUSE) {
		labels[0] = "Quit";
		labels[1] = "Save";
		labels[2] = "New world";
		labels[3] = "Play";
		cols[0] = lm_theme.btn_red;
		cols[1] = lm_theme.btn_blue;
		cols[2] = lm_theme.btn_amber;
		cols[3] = lm_theme.btn_green;
		wlwin_text(w, tx, ty, a->first ? "Click in the window to look around and play." : "Click in the window to go on.", 1, lm_theme.text, tw);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, "W A S D walk, Space jumps, Shift sneaks, Ctrl runs, F flies.", 1, lm_theme.text_dim, tw);
		wlwin_text(w, tx, ty + 2 * (LM_FONT_H + 5), "Left button breaks, right button puts a block, 1 to 9 choose it.", 1, lm_theme.text_dim, tw);
		wlwin_text(w, tx, ty + 3 * (LM_FONT_H + 5), "Esc pauses. F1 lists every key.", 1, lm_theme.text_dim, tw);
	} else if (a->prompt == P_LOST) {
		labels[0] = "New world";
		cols[0] = lm_theme.btn_amber;
		char l[120];
		wlwin_text(w, tx, ty, "The goblins pulled it down. The game is over.", 1, lm_theme.text, tw);
		snprintf(l, sizeof l, "Nights survived: %d     Goblins killed: %d", a->nights, a->kills);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, l, 1, lm_theme.text_dim, tw);
	} else if (a->prompt == P_DEAD) {
		labels[0] = "Respawn";
		cols[0] = lm_theme.btn_green;
		char l[120];
		wlwin_text(w, tx, ty, "A goblin got you. Your world is as you left it.", 1, lm_theme.text, tw);
		snprintf(l, sizeof l, "Goblins killed: %d", a->kills);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, l, 1, lm_theme.text_dim, tw);
	} else if (a->prompt == P_PALETTE) {
		labels[0] = "Close";
		cols[0] = lm_theme.btn_green;
		int gx, gy, cell = pal_cell(a), isz = cell - 18, sc = wlwin_scale(w);
		pal_origin(a, &c, &gx, &gy);
		for (int i = 0; i < PAL_N; i++) {
			int x = gx + (i % PAL_COLS) * cell, y = gy + (i / PAL_COLS) * cell;
			bool cur = hand_block(a) == k_palette[i];
			wlwin_round_fill(w, x + 1, y + 1, cell - 2, cell - 2, 6, cur ? 0xf0f0f0u : a->pal_hover == i ? 0x6a7384u : 0x3a4150u);
			wlwin_round_fill(w, x + 3, y + 3, cell - 6, cell - 6, 5, 0x20242cu);
			const uint32_t *ic = a->icon[k_palette[i]];
			if (ic && a->icon_s == isz * sc)
				wlwin_blit_px_over(w, x + (cell - isz) / 2, y + (cell - isz) / 2, ic, isz * sc, isz * sc, isz * sc);
			if (a->survival) {
				int have = a->inv[k_palette[i]];
				char cnt[16];
				snprintf(cnt, sizeof cnt, "%d", have > 999 ? 999 : have);
				shadow_text(w, x + cell - lm_font_text_width(cnt) - 5, y + cell - LM_FONT_H - 3, cnt, have > 0 ? 0xffffffu : 0xff6a5au);
			}
		}
		char l[120];
		int shown = a->pal_hover >= 0 ? k_palette[a->pal_hover] : hand_block(a);
		snprintf(l, sizeof l, "%s - slot %d  (click to put it there; 1-9: other slot)", bl_block_name(shown), a->slot + 1);
		wlwin_text(w, tx, gy + 2 * cell + 8, l, 1, lm_theme.text, tw);
	} else if (a->prompt == P_NEW) {
		wlwin_text(w, tx, ty, "Make a new world? The one you have now is replaced.", 1, lm_theme.text, tw);
		labels[1] = "New world";
		cols[1] = lm_theme.btn_amber;
	} else {
		labels[0] = "Close";
		cols[0] = lm_theme.btn_blue;
		for (size_t i = 0; i < sizeof HELP_LINES / sizeof HELP_LINES[0]; i++)
			wlwin_text(w, tx, c.y + 40 + (int)i * (LM_FONT_H + 5), HELP_LINES[i], 1, lm_theme.text, tw);
	}
	wlcard_buttons(w, &c, labels, nb, cols, a->pc_hover, a->pc_down, lm_theme.on_colour);
}

static void write_dump(struct app *a)
{
	if (!g_dump)
		return;
	if (!a->hist_ok || a->hist_rev != a->world->rev) {
		memset(a->hist, 0, sizeof a->hist);
		size_t n = (size_t)a->world->w * (size_t)a->world->d * (size_t)a->world->h;
		for (size_t i = 0; i < n; i++)
			a->hist[a->world->b[i]]++;
		a->hist_rev = a->world->rev;
		a->hist_ok = true;
	}
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.new", g_dump);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	const struct bl_world *w = a->world;
	int under = bl_get(w, (int)floorf(a->pl.x), (int)floorf(a->pl.y - 0.1f), (int)floorf(a->pl.z));
	fprintf(f, "state seed=%u kind=%d size=%dx%dx%d x=%.3f y=%.3f z=%.3f yaw=%.4f pitch=%.4f ground=%d water=%d fly=%d sneak=%d vy=%.3f playing=%d locked=%d lockok=%d "
	           "prompt=%d slot=%d hand=%s rev=%u time=%.3f clock=%d aimed=%d rw=%d rh=%d fps=%d budget=%.0f debug=%d under=%s saved=%d nosave=%d wpend=%zu light=%d sound=%d sounds=%d\n",
	        w->seed, w->kind, w->w, w->d, w->h, a->pl.x, a->pl.y, a->pl.z, a->pl.yaw, a->pl.pitch, a->pl.on_ground, a->pl.in_water, a->pl.flying, a->pl.sneaking,
	        a->pl.vy, a->playing, a->win.locked, a->lock_ok, (int)a->prompt, a->slot + 1, bl_block_name(hand_block(a)), w->rev, a->time_h, a->clockm, a->aimed,
	        a->rw, a->rh, a->fps, a->budget, a->debug, bl_block_name(under), w->rev == a->saved_rev, a->nosave, bl_water_pending(w),
	        bl_light(w, (int)floorf(a->pl.x), (int)floorf(a->pl.y + 0.5f), (int)floorf(a->pl.z)), a->sound_on, a->nsounds);
	if (a->aimed)
		fprintf(f, "aim x=%d y=%d z=%d face=%d px=%d py=%d pz=%d t=%.3f block=%s\n", a->aim.x, a->aim.y, a->aim.z, a->aim.face, a->aim.px, a->aim.py, a->aim.pz,
		        a->aim.t, bl_block_name(bl_get(w, a->aim.x, a->aim.y, a->aim.z)));
	fprintf(f, "player hp=%.2f dead=%d kills=%d hurt=%.2f hit=%.2f mobs=%d goblins=%d spawned=%d burned=%d\n", a->hp, a->prompt == P_DEAD, a->kills, a->hurt_t, a->hit_t, a->mobs_on,
	        bl_mobs_count(&a->mobs), a->mobs.spawned, a->mobs.burned);
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (a->mobs.mob[i].alive) {
			const struct bl_mob *m = &a->mobs.mob[i];
			fprintf(f, "mob %d x=%.3f y=%.3f z=%.3f hp=%.1f yaw=%.3f hurt=%.2f dist=%.2f\n", i, m->x, m->y, m->z, m->hp, m->yaw, m->hurt, hypotf(m->x - a->pl.x, m->z - a->pl.z));
		}
	fprintf(f, "game survival=%d have_keep=%d keep=%d,%d,%d keep_hp=%.1f nights=%d dig=%.2f\n", a->survival, a->have_keep, a->keep[0], a->keep[1], a->keep[2], a->keep_hp, a->nights,
	        a->dig_on ? a->dig_t : -1.0f);
	fprintf(f, "inv");
	for (int b = 1; b < BL_COUNT; b++)
		if (a->inv[b] > 0)
			fprintf(f, " %s=%d", bl_block_name(b), a->inv[b]);
	fprintf(f, "\n");
	if (!a->pets.placed)
		fprintf(f, "pets on=%d placed=0\n", a->pets_on);
	if (a->pets.placed) {
		struct bl_pets_info pi;
		bl_pets_info(&a->pets, &pi);
		fprintf(f, "pets on=%d yard=%.2f,%.2f,%.2f t=%.2f\n", a->pets_on, pi.yard[0], pi.yard[1], pi.yard[2], a->pet_t);
		fprintf(f, "tom x=%.3f y=%.3f z=%.3f yaw=%.3f stand=%.3f lie=%.3f walk=%.3f air=%.3f mode=%d\n", pi.tom[0], pi.tom[1], pi.tom[2], pi.tom_yaw, pi.tom_stand, pi.tom_lie, pi.tom_walk,
		        pi.tom_air, pi.tom_mode);
		fprintf(f, "filip x=%.3f y=%.3f z=%.3f rest=%.3f inwater=%d pond=%.2f,%.2f,%.2f\n", pi.filip[0], pi.filip[1], pi.filip[2], pi.filip_rest, pi.filip_in_water, pi.pond[0], pi.pond[1], pi.pond[2]);
	}
	fprintf(f, "hist");
	for (int b = 0; b < BL_COUNT; b++)
		fprintf(f, " %s=%ld", bl_block_name(b), a->hist[b]);
	fprintf(f, "\nhotbar");
	for (int i = 0; i < SLOTS; i++)
		fprintf(f, " %s", bl_block_name(a->hotbar[i]));
	fprintf(f, "\n");
	struct lay L;
	layout(a, &L);
	for (int i = 0; i < SLOTS; i++)
		fprintf(f, "slot %d x=%d y=%d w=%d h=%d\n", i + 1, L.x0 + i * (L.slot + L.gap), L.y0, L.slot, L.slot);
	if (a->prompt != P_NONE) {
		struct wlcard c;
		int nb;
		card_rect(a, &c, &nb);
		fprintf(f, "card x=%d y=%d w=%d h=%d\n", c.x, c.y, c.w, c.h);
		if (a->prompt == P_PALETTE) {
			int gx, gy, cell = pal_cell(a);
			pal_origin(a, &c, &gx, &gy);
			for (int i = 0; i < PAL_N; i++)
				fprintf(f, "pal %d x=%d y=%d w=%d h=%d block=%s\n", i, gx + (i % PAL_COLS) * cell, gy + (i / PAL_COLS) * cell, cell, cell, bl_block_name(k_palette[i]));
		}
		for (int k = 0; k < nb; k++) {
			int x, y, bw, bh;
			wlcard_button_rect(&c, k, nb, &x, &y, &bw, &bh);
			fprintf(f, "cbtn %d %d %d %d %d\n", k, x, y, bw, bh);
		}
	}
	fclose(f);
	rename(tmp, g_dump);
}

static void paint(struct wlwin *w)
{
	struct app *a = w->data;
	int64_t t0 = now_ms();
	/* the picture: as many pixels as the last frames allowed, the window's shape */
	if (a->fix_w > 0) {
		a->rw = a->fix_w;
		a->rh = a->fix_h;
	} else {
		render_size(w->pw, w->ph, a->budget, &a->rw, &a->rh);
	}
	size_t need = (size_t)a->rw * (size_t)a->rh;
	if (need > a->frame_cap) {
		free(a->frame);
		a->frame = malloc(need * 4);
		a->frame_cap = a->frame ? need : 0;
	}
	if (!a->frame) {
		wlwin_fill(w, 0, 0, w->bw, w->bh, 0x000000u);
		return;
	}
	float eye[3];
	bl_player_eye(&a->pl, eye);
	struct bl_camera cam = { eye[0], eye[1], eye[2], a->pl.yaw, a->pl.pitch, a->fov };
	struct bl_view v;
	bl_view_init(&v);
	v.time_h = a->time_h;
	v.dist = 96.0f;
	if (a->pets_on && a->pets.placed) {
		float dtp = a->pose_ms ? (float)(t0 - a->pose_ms) / 1000.0f : 0.0f;
		a->pose_ms = t0;
		bl_pets_pose(&a->pets, a->world, a->pet_t, dtp, eye, a->time_h);
		v.pets = &a->pets;
	}
	if (a->mobs_on && bl_mobs_count(&a->mobs) > 0) {
		bl_mobs_light(&a->mobs, a->world, a->time_h);
		v.mobs = &a->mobs;
	}
	if (a->aimed && a->playing) {
		v.target = true;
		if (a->survival && a->dig_on && a->dig_x == a->aim.x && a->dig_y == a->aim.y && a->dig_z == a->aim.z) {
			float hd = bl_hardness(bl_get(a->world, a->dig_x, a->dig_y, a->dig_z));
			v.crack = hd > 0.0f ? fminf(1.0f, a->dig_t / hd) : 0.0f;
		}
		v.tx = a->aim.x;
		v.ty = a->aim.y;
		v.tz = a->aim.z;
	}
	bl_render(a->world, &cam, &v, a->frame, a->rw, a->rh, a->rw, a->threads);
	if (a->hurt_t > 0.0f || a->hp <= 0.0f)
		red_flash(a);
	int64_t t1 = now_ms();
	a->ms_render = (double)(t1 - t0);
	if (a->fix_w <= 0 && a->prompt == P_NONE)
		a->budget = budget_next(a->budget, a->ms_render, (double)w->pw * (double)w->ph);
	wlwin_blit(w, 0, 0, w->bw, w->bh, a->frame, a->rw, a->rh, a->rw);

	struct lay L;
	layout(a, &L);
	paint_hotbar(a, &L);
	paint_hearts(a, &L);
	if (a->survival)
		paint_survival(a);
	if (a->prompt == P_NONE)
		paint_crosshair(w, a->hit_t > 0.0f);
	if (a->debug)
		paint_debug(a);
	if (a->msg[0]) {
		shadow_text(w, (w->bw - lm_font_text_width(a->msg)) / 2, a->survival ? 52 : 26, a->msg, 0xffffffu);       /* (under the keep's bar in survival) */
		if (t1 - a->msg_ms > 3000)
			a->msg[0] = 0;
	}
	if (a->prompt != P_NONE)
		paint_card(a);
	a->fps_n++;
	if (t1 - a->fps_t0 >= 1000) {
		a->fps = (int)((int64_t)a->fps_n * 1000 / (t1 - a->fps_t0));
		a->fps_n = 0;
		a->fps_t0 = t1;
	}
	write_dump(a);
}

/* ================================================================ the pointer and the keys ================================================================ */

static void card_action(struct app *a, int which)
{
	if (a->prompt == P_LOST) {                                  /* the game is over: a new world (the old one stays in its file until it is saved over) */
		new_world(a);
		resume_game(a);
		return;
	}
	if (a->prompt == P_DEAD) {
		respawn(a);
		resume_game(a);
		return;
	}
	if (a->prompt == P_PALETTE) {
		resume_game(a);
		return;
	}
	if (a->prompt == P_PAUSE) {
		if (which == 0) {
			save_all(a, false);
			a->win.running = false;
		} else if (which == 1) {
			save_all(a, true);
		} else if (which == 2) {
			a->prompt = P_NEW;
			a->pc_hover = a->pc_down = -1;
		} else {
			resume_game(a);
		}
		return;
	}
	if (a->prompt == P_NEW) {
		a->prompt = P_PAUSE;
		a->pc_hover = a->pc_down = -1;
		if (which == 1) {
			new_world(a);
			resume_game(a);
		}
		return;
	}
	a->prompt = P_PAUSE;                                    /* the keys card closes back to the pause card */
	a->pc_hover = a->pc_down = -1;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
	struct wlcard c;
	int nb;
	card_rect(a, &c, &nb);
	if (op == LM_WL_POINTER_EVT_MOTION) {
		if (wlcard_drag_motion(&a->win, &c, a->px, a->py))
			return;
		a->pc_hover = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
		a->pal_hover = pal_cell_at(a, a->px, a->py);
		a->win.redraw = true;
		return;
	}
	if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
		return;
	int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
	if (args[3].u == 1) {
		int pc = pal_cell_at(a, a->px, a->py);
		if (pc >= 0 && a->survival && a->inv[k_palette[pc]] < 1) {      /* (in survival only what he has) */
			sound(a, "no");
			return;
		}
		if (pc >= 0) {                                       /* a block of the palette goes into the slot in the hand */
			a->hotbar[a->slot] = k_palette[pc];
			a->name_until = now_ms() + 1800;
			a->win.redraw = true;
			return;
		}
		if (k >= 0)
			a->pc_down = k;
		else if (wlcard_drag_press(&a->win, &c, a->px, a->py))
			;
		else if (a->prompt == P_PAUSE) {                     /* a click anywhere else goes on playing */
			resume_game(a);
			a->ignore_release = true;
		} else if (!wlcard_has(&c, (int)a->px, (int)a->py) && a->prompt != P_DEAD && a->prompt != P_LOST) {
			card_action(a, 0);
		}
	} else {
		wlcard_drag_release(&a->win);
		if (a->pc_down >= 0 && a->pc_down == k)
			card_action(a, k);
		a->pc_down = -1;
	}
	a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
	struct app *a = w->data;
	switch (op) {
	case LM_WL_POINTER_EVT_ENTER:
		a->px = lm_fixed_to_double(args[2].f);
		a->py = lm_fixed_to_double(args[3].f);
		a->has_pointer = true;
		if (a->playing)
			wlwin_hide_cursor(w, true);
		w->redraw = true;
		return;
	case LM_WL_POINTER_EVT_LEAVE:
		a->has_pointer = false;
		a->pc_hover = -1;
		w->redraw = true;
		return;
	case LM_WL_POINTER_EVT_MOTION:
		a->px = lm_fixed_to_double(args[1].f);
		a->py = lm_fixed_to_double(args[2].f);
		a->has_pointer = true;
		break;
	default:
		break;
	}
	if (a->prompt != P_NONE) {
		card_pointer(a, op, args);
		return;
	}
	if (op == LM_WL_POINTER_EVT_BUTTON) {
		uint32_t button = args[2].u, state = args[3].u;
		int64_t now = now_ms();
		if (state == 0 && a->ignore_release && button == 272) {
			a->ignore_release = false;
			return;
		}
		if (!a->playing)
			return;
		if (button == 272) {
			a->b_break = state == 1;
			if (state == 1) {
				if (a->survival)
					dig_step(a, now, 0.0f);                    /* (the sword at once; a block takes its time) */
				else
					do_break(a, now);
				a->next_break = now + REPEAT_MS;
			} else {
				a->dig_on = false;
				a->dig_t = 0.0f;
			}
		} else if (button == 273) {
			a->b_place = state == 1;
			if (state == 1) {
				do_place(a);
				a->next_place = now + REPEAT_MS;
			}
		} else if (button == 274 && state == 1) {
			do_pick(a);
		}
		w->redraw = true;
	} else if (op == LM_WL_POINTER_EVT_AXIS && a->playing && args[1].u == 0) {
		a->wheel += lm_fixed_to_double(args[2].f);
		if (fabs(a->wheel) >= 10.0) {
			choose_slot(a, a->slot + (a->wheel > 0 ? 1 : -1));
			a->wheel = 0;
		}
	}
}

static void relative_event(struct wlwin *w, double dx, double dy)
{
	struct app *a = w->data;
	if (!a->playing)
		return;
	look_by(a, dx * a->sens, -dy * a->sens);
	w->redraw = true;
}

static void lock_event(struct wlwin *w, bool locked)
{
	struct app *a = w->data;
	if (!locked && a->playing)                              /* the focus went elsewhere: the game waits */
		pause_game(a, P_PAUSE);
	w->redraw = true;
}

static void keys_release(struct app *a)
{
	a->k_fwd = a->k_back = a->k_left = a->k_right = a->k_jump = a->k_sneak = a->k_ctrl = a->k_lk = a->k_rk = a->k_uk = a->k_dk = false;
	a->b_break = a->b_place = false;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
	struct app *a = w->data;
	if (op == LM_WL_KEYBOARD_EVT_LEAVE) {
		keys_release(a);
		if (a->playing)
			pause_game(a, P_PAUSE);
		w->redraw = true;
		return;
	}
	if (op != LM_WL_KEYBOARD_EVT_KEY)
		return;
	uint32_t code = args[2].u;
	bool down = args[3].u != 0;
	/* keys that are held: they count while the game is on, and are let go whatever happens */
	switch (code) {
	case K_W: a->k_fwd = down && a->playing; break;
	case K_S: a->k_back = down && a->playing; break;
	case K_A: a->k_left = down && a->playing; break;
	case K_D: a->k_right = down && a->playing; break;
	case K_SPACE:
		a->k_jump = down && a->playing;
		if (down && a->playing)
			a->jump_latch = true;
		break;
	case K_LSHIFT: case K_RSHIFT: a->k_sneak = down && a->playing; break;
	case K_LCTRL: case K_RCTRL: a->k_ctrl = down && a->playing; break;
	case K_LEFT: a->k_lk = down && a->playing; break;
	case K_RIGHT: a->k_rk = down && a->playing; break;
	case K_UP: a->k_uk = down && a->playing; break;
	case K_DOWN: a->k_dk = down && a->playing; break;
	default: break;
	}
	if (!down)
		return;
	bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
	w->redraw = true;
	if (ctrl) {
		if (code == K_Q)
			w->running = false;
		else if (code == K_S)
			save_all(a, true);
		else if (code == K_N) {
			if (a->playing)
				pause_game(a, P_NEW);
			else
				a->prompt = P_NEW;
		}
		return;
	}
	if (a->prompt == P_PALETTE) {
		if (code == K_ESC || code == K_E || code == K_ENTER)
			resume_game(a);
		else if (code >= K_1 && code <= K_9)
			choose_slot(a, (int)(code - K_1));
		return;
	}
	if (a->prompt == P_DEAD) {                                  /* the only way on is back to the start */
		if (code == K_ENTER || code == K_SPACE || code == K_ESC)
			card_action(a, 0);
		return;
	}
	if (a->prompt == P_LOST) {                                  /* (Esc does not start a new world by accident) */
		if (code == K_ENTER || code == K_SPACE)
			card_action(a, 0);
		return;
	}
	if (a->prompt != P_NONE) {
		if (code == K_ESC) {
			if (a->prompt == P_PAUSE)
				resume_game(a);
			else {
				a->prompt = P_PAUSE;
				a->pc_hover = a->pc_down = -1;
			}
		} else if (code == K_ENTER) {
			if (a->prompt == P_PAUSE)
				resume_game(a);
			else
				card_action(a, a->prompt == P_NEW ? 1 : 0);
		}
		return;
	}
	if (code == K_ESC) {
		pause_game(a, P_PAUSE);
		save_all(a, false);
		return;
	}
	if (code == K_F1) {
		pause_game(a, P_HELP);
		return;
	}
	if (code == K_E) {
		pause_game(a, P_PALETTE);
		return;
	}
	if (code == K_G && a->playing) {                            /* survival <-> creative */
		a->survival = !a->survival;
		a->dig_on = false;
		a->dig_t = 0.0f;
		if (a->survival)
			ensure_keep(a, a->pl.x, a->pl.z);
		else
			keep_sync(a);
		say(a, a->survival ? "Survival: blocks take time and are used up, defend the keep" : "Creative: every block, at once");
		return;
	}
	if (code == K_X && a->playing) {
		if (a->survival)
			craft_hand(a);
		else
			say(a, "Nothing to make in creative: every block is there");
		return;
	}
	if (code == K_M && a->playing) {
		a->mobs_on = !a->mobs_on;
		a->mobs.enabled = a->mobs_on;
		if (!a->mobs_on)
			bl_mobs_clear(&a->mobs);
		say(a, a->mobs_on ? "The goblins come out at night" : "No goblins");
		return;
	}
	if (code == K_P && a->playing) {
		a->pets_on = !a->pets_on;
		a->pets.on = a->pets_on;
		if (a->pets_on && !a->pets.placed) {                /* the first time: a yard near where the player is now (and a pond if there is no water) */
			float near[3] = { a->pl.x, a->pl.y, a->pl.z };
			pets_setup(a, near, false);
		}
		say(a, a->pets_on ? "Tom and Filip are here" : "Tom and Filip have gone");
		return;
	}
	if (code >= K_1 && code <= K_9) {
		choose_slot(a, (int)(code - K_1));
		return;
	}
	if (!a->playing)
		return;
	if (code == K_F) {
		a->pl.flying = !a->pl.flying;
		a->pl.vy = 0;
		say(a, a->pl.flying ? "Flying" : "Walking");
	} else if (code == K_F3) {
		a->debug = !a->debug;
	} else if (code == K_T) {
		a->clockm = (a->clockm + 1) % 3;
		a->real_ms = 0;
		say(a, a->clockm == 0 ? "The clock stands" : a->clockm == 1 ? "The clock runs" : "The clock follows the sun");
	} else if (code == K_LBRACE || code == K_RBRACE) {
		a->time_h += code == K_RBRACE ? 1.0f : -1.0f;
		if (a->time_h < 0)
			a->time_h += 24.0f;
		if (a->time_h >= 24.0f)
			a->time_h -= 24.0f;
	}
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t version)
{
	struct app *a = w->data;
	(void)version;
	if (!strcmp(iface, "mellagui_settings_v1"))
		a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, NULL, a);
}

static bool close_event(struct wlwin *w)
{
	(void)w;
	return true;
}

/* ================================================================ the selftest ================================================================ */

static int selftest(void)
{
	int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)

	/* the render size keeps the window's shape and the budget */
	int rw, rh;
	render_size(1920, 1080, 300000, &rw, &rh);
	CHECK(rw * rh <= 300000 && rw * rh > 250000, "300000 pixels came out as %d x %d", rw, rh);
	CHECK(abs(rw * 1080 - rh * 1920) <= 1920, "the shape of the window is lost: %d x %d", rw, rh);
	render_size(800, 600, 10000000, &rw, &rh);
	CHECK(rw == 800 && rh == 600, "a big budget is the whole window: %d x %d", rw, rh);
	render_size(800, 600, 100, &rw, &rh);
	CHECK(rw >= 64 && rh >= 36, "a tiny budget is not smaller than 64 x 36: %d x %d", rw, rh);
	render_size(1, 1, 1000, &rw, &rh);
	CHECK(rw >= 1 && rh >= 1, "a window of one pixel does not divide by zero");

	/* the budget goes down when frames are slow, up when they are quick, and stays between its limits */
	double b = 300000;
	for (int i = 0; i < 40; i++)
		b = budget_next(b, 60.0, 2000000);
	CHECK(b == MIN_BUDGET, "slow frames end at the least budget (%.0f)", b);
	for (int i = 0; i < 200; i++)
		b = budget_next(b, 5.0, 900000);
	CHECK(b == 900000, "quick frames end at the whole window (%.0f)", b);
	double s = budget_next(500000, 20.0, 2000000);
	CHECK(s == 500000, "a frame of 20 ms keeps the budget");

	/* every block that can be in the hand has an icon: see-through corners, a middle that shows something */
	enum { S = 64 };
	static uint32_t px[S * S];
	for (int blk = 1; blk < BL_COUNT; blk++) {
		if (blk == BL_BEDROCK || (blk >= BL_FLOW1 && blk <= BL_FLOW7))
			continue;
		icon_build(px, S, blk);
		CHECK((px[0] >> 24) == 0 && (px[S - 1] >> 24) == 0 && (px[(S - 1) * S] >> 24) == 0, "the corners of the icon of %s are not see-through", bl_block_name(blk));
		int solid = 0;
		for (int i = 0; i < S * S; i++)
			solid += (px[i] >> 24) > 0;
		if (blk != BL_GLASS && blk != BL_TORCH)                /* a pane is a frame round a clear middle, a torch a stick and a flame */
			CHECK(solid > S * S / 2 && solid < S * S * 9 / 10, "the icon of %s covers %d of %d pixels", bl_block_name(blk), solid, S * S);
		if (blk != BL_GLASS && blk != BL_WATER)
			CHECK((px[S / 2 * S + S / 2] >> 24) == 255 || blk == BL_TORCH, "the middle of the icon of %s is not solid", bl_block_name(blk));
		if (blk == BL_TORCH) {
			int stick = 0, flame = 0;
			for (int i = 0; i < S * S; i++) {
				uint32_t c = px[i];
				stick += (c >> 24) && (c & 0xff) < 0x40 && ((c >> 16) & 255) < 0x90;
				flame += (c >> 24) && ((c >> 16) & 255) > 0xe0 && ((c >> 8) & 255) > 0x70;
			}
			CHECK(stick > 40 && flame > 40, "the icon of a torch has a stick and a flame (%d, %d pixels)", stick, flame);
		}
	}
	icon_build(px, S, BL_GRASS);
	CHECK((px[8 * S + S / 2] & 0xffffff) != (px[S * 3 / 4 * S + S / 4] & 0xffffff), "the top and the side of the grass icon look alike");

	/* the files: a folder is made, the hand and the player come back */
	char dir[128];
	snprintf(dir, sizeof dir, "/tmp/mellagui-blocks-selftest-%d", (int)getpid());
	static struct app a;
	memset(&a, 0, sizeof a);
	snprintf(a.dir, sizeof a.dir, "%s/a/b", dir);
	CHECK(mkdirs(a.dir) == 0, "mkdirs");
	struct stat st;
	CHECK(stat(a.dir, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 077) == 0, "the folder is made and private");
	a.world_size = 64;
	a.fov = 1.2f;
	a.sens = 0.0025f;
	memcpy(a.hotbar, k_default_hotbar, sizeof a.hotbar);
	CHECK(make_world(&a, 5, true), "a flat world");
	a.pl.x = 12.5f;
	a.pl.z = 9.25f;
	a.pl.yaw = 1.25f;
	a.pl.pitch = -0.5f;
	a.time_h = 17.5f;
	a.slot = 3;
	a.hotbar[2] = BL_LEAVES;
	a.pl.flying = true;
	bl_set(a.world, 5, 20, 5, BL_BRICK);
	CHECK(save_all(&a, false), "saved");
	char path[600];
	world_path(&a, path, sizeof path);
	CHECK(stat(path, &st) == 0, "the world file is there");
	struct app b2;
	memset(&b2, 0, sizeof b2);
	snprintf(b2.dir, sizeof b2.dir, "%s", a.dir);
	b2.fov = 1.2f;
	b2.sens = 0.0025f;
	memcpy(b2.hotbar, k_default_hotbar, sizeof b2.hotbar);
	conf_load(&b2, true);
	CHECK(fabsf(b2.pl.x - 12.5f) < 0.01f && fabsf(b2.pl.z - 9.25f) < 0.01f && fabsf(b2.pl.yaw - 1.25f) < 0.001f && fabsf(b2.time_h - 17.5f) < 0.01f,
	      "where the player stood comes back (%.2f %.2f %.3f %.2f)", b2.pl.x, b2.pl.z, b2.pl.yaw, b2.time_h);
	CHECK(b2.slot == 3 && b2.hotbar[2] == BL_LEAVES && b2.pl.flying, "the hand comes back");
	const char *err = NULL;
	struct bl_world *lw = bl_load(path, &err);
	CHECK(lw && bl_get(lw, 5, 20, 5) == BL_BRICK, "the edit is in the saved world");
	bl_world_free(lw);
	/* a hand with a block that is none is not believed */
	snprintf(path, sizeof path, "%s/player.conf", a.dir);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "hotbar = 1,2,999,0,10,16,4,14,9\nfov = 500\nsensitivity = -3\nslot = 99\nclock = 7\nsound = 5\n");
		fclose(f);
	}
	struct app c2;
	memset(&c2, 0, sizeof c2);
	snprintf(c2.dir, sizeof c2.dir, "%s", a.dir);
	c2.fov = 1.2f;
	c2.sens = 0.0025f;
	conf_load(&c2, true);
	CHECK(c2.fov == 1.2f && c2.sens == 0.0025f, "a field of view of 500 degrees and a negative speed are not taken");
	CHECK(c2.hotbar[0] == BL_GRASS && c2.hotbar[1] == BL_DIRT && c2.hotbar[2] == BL_SAND && c2.hotbar[3] == BL_TORCH && c2.hotbar[4] == BL_WATER,
	      "only blocks that can be put go in the hand: not air, bedrock, flowing water (%d %d %d %d %d)", c2.hotbar[0], c2.hotbar[1], c2.hotbar[2], c2.hotbar[3], c2.hotbar[4]);
	CHECK(c2.clockm == 0 && !c2.sound_on, "a clock mode of 7 and a sound setting of 5 are not taken");
	CHECK(c2.slot == 0, "slot 99 is not a slot");
	bl_world_free(a.world);
	char cmd[200];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		fprintf(stderr, "(could not remove %s)\n", dir);

	/* the real clock: the real sunrise is the model's 6, the real sunset its 18, noon between, the night stretched evenly (sunrise 08:00, sunset 16:00) */
	CHECK(fabsf(solar_hours(480, 480, 960) - 6.0f) < 0.01f && fabsf(solar_hours(720, 480, 960) - 12.0f) < 0.01f && fabsf(solar_hours(960, 480, 960) - 18.0f) < 0.01f,
	      "sunrise, noon and sunset map to 6, 12, 18 (%.2f %.2f %.2f)", solar_hours(480, 480, 960), solar_hours(720, 480, 960), solar_hours(960, 480, 960));
	CHECK(fabsf(solar_hours(0, 480, 960) - 0.0f) < 0.01f || fabsf(solar_hours(0, 480, 960) - 24.0f) < 0.01f, "midnight is 0 for a night that is split evenly (%.2f)", solar_hours(0, 480, 960));
	float prev = solar_hours(0, 450, 1000);
	int wraps = 0;
	for (int m = 1; m < 1440; m++) {
		float h = solar_hours(m, 450, 1000);
		if (h < prev - 0.001f)
			wraps++;
		CHECK(h >= 0.0f && h < 24.0f, "the model's hour is in 0 .. 24 (%.3f at minute %d)", h, m);
		prev = h;
	}
	CHECK(wraps <= 1, "the model's hour only goes forward (and wraps once a day): %d wraps", wraps);
	CHECK(fabsf(solar_hours(720, -1, 5000) - 12.0f) < 0.01f, "no sun times: the plain 6 and 18");
	float all = solar_hours(1439.5, 0, 1439);
	CHECK(all >= 0.0f && all < 24.0f && isfinite(all), "a day that lasts all day does not divide by zero (%.3f)", all);

	/* the palette: every block in it can be in the hand and none is there twice */
	for (int i = 0; i < PAL_N; i++) {
		CHECK(hand_ok(k_palette[i]), "palette block %d (%s) cannot be in the hand", i, bl_block_name(k_palette[i]));
		for (int j = i + 1; j < PAL_N; j++)
			CHECK(k_palette[i] != k_palette[j], "palette blocks %d and %d are the same", i, j);
	}
	CHECK(!hand_ok(BL_AIR) && !hand_ok(BL_BEDROCK) && !hand_ok(BL_FLOW4) && hand_ok(BL_WATER) && hand_ok(BL_TORCH) && hand_ok(BL_LAMP), "what may be in the hand");

	/* Tom and Filip: the fur has its 64 letters, a yard is found at least 10 blocks away, and the one the conf had is kept */
	{
		const uint8_t *tab[MCAT_GLYPHS];
		CHECK(fur_alphabet(tab) == MCAT_GLYPHS, "64 letters for the fur");
		int missing = 0;
		for (int i = 0; i < MCAT_GLYPHS; i++)
			missing += tab[i] == NULL;
		CHECK(missing == 0, "%d letters of the fur have no picture", missing);
		static struct app pa;
		memset(&pa, 0, sizeof pa);
		pa.world = bl_world_flat(64, 64, 64, 20);
		pa.pets_on = true;
		float near[3] = { 32.5f, 20.0f, 32.5f };
		pets_setup(&pa, near, false);
		CHECK(pa.pets.placed && hypotf(pa.pets.yard[0] - near[0], pa.pets.yard[2] - near[2]) >= 9.99f, "a yard 10 blocks from the player");
		CHECK(pa.pets.made_pond && pa.pets.filip.swimming, "and Filip is in a pond that was dug beside it");
		pa.have_yard = true;
		pa.yard_x = 20.0f;
		pa.yard_z = 40.0f;
		pa.have_tom = true;
		pa.tom_pos[0] = 21.0f;
		pa.tom_pos[1] = 20.0f;
		pa.tom_pos[2] = 41.0f;
		pets_setup(&pa, near, true);
		CHECK(pa.pets.yard[0] == 20.0f && pa.pets.yard[2] == 40.0f, "the yard of the conf is kept");
		CHECK(fabsf(pa.pets.tom.x - 21.0f) < 0.001f && fabsf(pa.pets.tom.z - 41.0f) < 0.001f, "and so is where Tom was");
		pets_setup(&pa, near, false);
		CHECK(pa.pets.yard[0] != 20.0f, "a new world finds its own");
		bl_world_free(pa.world);
	}

	/* the goblins: a blow takes hit points and pushes, hit points come back slowly, no hit points is a death card, respawn puts him back */
	{
		static struct app ma;
		memset(&ma, 0, sizeof ma);
		ma.world = bl_world_flat(64, 64, 64, 20);
		ma.mobs_on = true;
		ma.sound_on = false;
		ma.time_h = 0.0f;
		bl_player_init(&ma.pl, ma.world);
		settle_player(&ma);
		mobs_setup(&ma);
		CHECK(ma.hp == MAX_HP && ma.mobs.enabled && bl_mobs_count(&ma.mobs) == 0, "ten hearts and no goblins at the start");
		ma.mobs.spawn_t = 1e9f;
		bl_mobs_add(&ma.mobs, ma.world, BL_MOB_GOBLIN, ma.pl.x, ma.pl.y, ma.pl.z - 3.0f);
		float hp0 = ma.hp;
		int guard = 0;
		while (ma.hp >= hp0 && guard++ < 600)
			mobs_advance(&ma, 1.0f / 30.0f);
		CHECK(ma.hp == hp0 - BL_GOBLIN_HIT && ma.hurt_t > 0.3f, "a goblin that gets to him takes three hit points (%.1f) and the picture goes red", ma.hp);
		CHECK(ma.pl.vz > 3.0f && !ma.pl.on_ground, "and the blow throws him back, into the air (vz %.1f)", ma.pl.vz);
		bl_mobs_clear(&ma.mobs);
		ma.mobs.spawn_t = 1e9f;
		float hp1 = ma.hp;
		for (int i = 0; i < 30 * 5; i++)
			mobs_advance(&ma, 1.0f / 30.0f);
		CHECK(ma.hp == hp1, "five seconds on, nothing has mended yet");
		for (int i = 0; i < 30 * 10; i++)
			mobs_advance(&ma, 1.0f / 30.0f);
		CHECK(ma.hp > hp1 + 1.0f && ma.hp <= MAX_HP, "then a hit point every three seconds (%.1f)", ma.hp);
		ma.hp = 2.0f;
		bl_mobs_add(&ma.mobs, ma.world, BL_MOB_GOBLIN, ma.pl.x, ma.pl.y, ma.pl.z - 1.0f);
		ma.prompt = P_NONE;
		ma.playing = true;
		for (int i = 0; i < 90 && ma.hp > 0.0f; i++)
			mobs_advance(&ma, 1.0f / 30.0f);
		CHECK(ma.hp == 0.0f && ma.prompt == P_DEAD && !ma.playing, "no hit points left: the death card");
		respawn(&ma);
		CHECK(ma.hp == MAX_HP && bl_mobs_count(&ma.mobs) == 0 && ma.pl.on_ground, "respawn: full hearts, no goblins, on the ground");
		/* the sword: a goblin in front takes the blow and the block behind it does not break; too soon is no blow */
		bl_mobs_add(&ma.mobs, ma.world, BL_MOB_GOBLIN, ma.pl.x, ma.pl.y, ma.pl.z + 2.0f);
		ma.pl.yaw = 0.0f;
		ma.pl.pitch = 0.0f;
		ma.hp = MAX_HP;
		aim_update(&ma);
		float gh = ma.mobs.mob[0].hp;
		CHECK(sword_swing(&ma, 1000) && ma.mobs.mob[0].hp == gh - BL_SWORD_DAMAGE && ma.hit_t > 0.0f, "a blow at a goblin 2 m ahead hurts it");
		CHECK(sword_swing(&ma, 1100) && ma.mobs.mob[0].hp == gh - BL_SWORD_DAMAGE, "a second blow 0.1 s later is too soon, and still no block is broken");
		CHECK(sword_swing(&ma, 1000 + SWORD_MS) && ma.mobs.mob[0].hp == gh - 2.0f * BL_SWORD_DAMAGE, "and one a moment later lands");
		CHECK(sword_swing(&ma, 5000) && !ma.mobs.mob[0].alive && ma.kills == 1, "the third kills (a kill is counted)");
		CHECK(!sword_swing(&ma, 9000), "with no goblin in front the button breaks blocks as before");
		ma.mobs_on = false;
		bl_mobs_add(&ma.mobs, ma.world, BL_MOB_GOBLIN, ma.pl.x, ma.pl.y, ma.pl.z + 2.0f);
		CHECK(!sword_swing(&ma, 20000), "with the goblins switched off there is no sword");
		bl_world_free(ma.world);
	}

	/* survival: breaking takes its time and leaves what it leaves, a block put is used up, X makes things, the keep stands near the start, goblins pull it down */
	{
		static struct app sa;
		memset(&sa, 0, sizeof sa);
		sa.world = bl_world_flat(64, 64, 64, 20);
		sa.survival = true;
		sa.mobs_on = true;
		sa.sound_on = false;
		sa.time_h = 12.0f;
		memcpy(sa.hotbar, k_default_hotbar, sizeof sa.hotbar);
		bl_player_init(&sa.pl, sa.world);
		settle_player(&sa);
		mobs_setup(&sa);
		survival_setup(&sa, sa.pl.x, sa.pl.z);
		CHECK(sa.have_keep && sa.keep_hp == KEEP_HP, "a keep is made at the start");
		float kd = hypotf((float)sa.keep[0] + 0.5f - sa.pl.x, (float)sa.keep[2] + 0.5f - sa.pl.z);
		CHECK(kd >= 3.9f && kd <= 10.0f, "4 to 10 blocks from where he starts (%.1f)", kd);
		CHECK(bl_get(sa.world, sa.keep[0], sa.keep[1], sa.keep[2]) == BL_KEEP && bl_get(sa.world, sa.keep[0], sa.keep[1] + 2, sa.keep[2]) == BL_KEEP &&
		      bl_get(sa.world, sa.keep[0], sa.keep[1] + 3, sa.keep[2]) == BL_AIR && bl_get(sa.world, sa.keep[0], sa.keep[1] - 1, sa.keep[2]) != BL_AIR, "three blocks high, on the ground");
		CHECK(sa.mobs.keep_on && fabsf(sa.mobs.keep[0] - ((float)sa.keep[0] + 0.5f)) < 0.01f, "the goblins know where it is");
		int ninv = 0;
		for (int b = 0; b < BL_COUNT; b++)
			ninv += sa.inv[b];
		CHECK(ninv == 0, "the pockets are empty");
		/* collecting: a block he has not seen goes into a slot that is empty */
		inv_add(&sa, BL_LOG, 3);
		CHECK(sa.inv[BL_LOG] == 3, "three logs");
		inv_add(&sa, BL_LEAVES, 1);
		bool leaves_in_bar = false;
		for (int i = 0; i < SLOTS; i++)
			leaves_in_bar |= sa.hotbar[i] == BL_LEAVES;
		CHECK(leaves_in_bar, "a block that was not in the hotbar takes the place of an empty slot");
		int slot_log = -1;
		for (int i = 0; i < SLOTS; i++)
			if (sa.hotbar[i] == BL_LOG)
				slot_log = i;
		inv_add(&sa, BL_LOG, 2);
		CHECK(slot_log >= 0 && sa.hotbar[slot_log] == BL_LOG && sa.inv[BL_LOG] == 5, "one that is already in it stays where it is");
		memcpy(sa.hotbar, k_default_hotbar, sizeof sa.hotbar);                 /* (the hotbar as it started: the tests below look for their blocks in it) */
		sa.inv[BL_LEAVES] = 0;
		/* making: a log is four planks, nothing is made of what he has not */
		sa.slot = 0;
		for (int i = 0; i < SLOTS; i++)
			if (sa.hotbar[i] == BL_LOG)
				sa.slot = i;
		CHECK(craft_hand(&sa) && sa.inv[BL_LOG] == 4 && sa.inv[BL_PLANKS] == 4, "X: a log makes four planks");
		for (int i = 0; i < SLOTS; i++)
			if (sa.hotbar[i] == BL_DIRT)
				sa.slot = i;
		CHECK(!craft_hand(&sa) && sa.inv[BL_DIRT] == 0, "dirt makes nothing");
		for (int i = 0; i < SLOTS; i++)
			if (sa.hotbar[i] == BL_SAND)
				sa.slot = i;
		CHECK(!craft_hand(&sa), "and what he has not he cannot make anything of");
		for (int i = 0; i < N_RECIPES; i++)
			CHECK(hand_ok(k_recipes[i].from) && hand_ok(k_recipes[i].to) && k_recipes[i].n_out >= 1, "recipe %d is of blocks that can be had", i);
		/* digging: a block takes `bl_hardness` of held time, not before; what it leaves goes into the pockets; the sword has the first word */
		sa.pl.pitch = -1.5f;                                                     /* looking down at the ground he stands on */
		sa.pl.y = 20.0f;
		aim_update(&sa);
		CHECK(sa.aimed && bl_get(sa.world, sa.aim.x, sa.aim.y, sa.aim.z) == BL_GRASS, "he aims at the grass under him");
		uint32_t rev = sa.world->rev;
		dig_step(&sa, 1000, 0.0f);
		dig_step(&sa, 1000, 0.3f);
		CHECK(sa.world->rev == rev && sa.dig_on && fabsf(sa.dig_t - 0.3f) < 0.001f, "0.3 s of 0.6: nothing is broken yet");
		dig_step(&sa, 1000, 0.31f);
		CHECK(sa.world->rev != rev && sa.inv[BL_DIRT] == 1 && !sa.dig_on, "0.6 s: the grass is gone and he has a dirt");
		aim_update(&sa);
		CHECK(sa.aimed && bl_get(sa.world, sa.aim.x, sa.aim.y, sa.aim.z) == BL_DIRT, "now he aims at dirt");
		sa.dig_on = false;
		for (int i = 0; i < 40; i++)                                             /* three blocks down is stone */
			dig_step(&sa, 1000, 0.1f), aim_update(&sa);
		CHECK(sa.inv[BL_COBBLE] >= 1 && sa.inv[BL_DIRT] >= 3, "digging down 4 s: cobble from the stone (%d), dirt (%d)", sa.inv[BL_COBBLE], sa.inv[BL_DIRT]);
		sa.pl.y = 17.0f;                                                         /* (he has gone down the hole: the next blocks are in reach) */
		aim_update(&sa);
		/* looking at something else starts again */
		sa.dig_on = true;
		sa.dig_x = 0;
		sa.dig_t = 0.55f;
		dig_step(&sa, 1000, 0.01f);
		CHECK(sa.dig_t < 0.1f, "another block: the time starts again");
		/* putting: used up, and not at all with none */
		sa.pl.pitch = -1.5f;
		aim_update(&sa);
		for (int i = 0; i < SLOTS; i++)
			if (sa.hotbar[i] == BL_PLANKS)
				sa.slot = i;
		CHECK(sa.aimed && sa.inv[BL_PLANKS] >= 4, "planks in the hand and something to put them on");
		int have = sa.inv[BL_PLANKS];
		do_place(&sa);
		CHECK(sa.inv[BL_PLANKS] == have - 1, "a block put is used up (%d -> %d)", have, sa.inv[BL_PLANKS]);
		sa.inv[BL_PLANKS] = 0;
		uint32_t rev2 = sa.world->rev;
		aim_update(&sa);
		do_place(&sa);
		CHECK(sa.world->rev == rev2 && sa.inv[BL_PLANKS] == 0, "with none, nothing is put");
		sa.survival = false;
		sa.pl.x = 30.5f;
		sa.pl.z = 30.5f;
		sa.pl.y = 20.0f;
		sa.pl.yaw = 0.0f;
		sa.pl.pitch = -0.6f;
		aim_update(&sa);
		do_place(&sa);
		CHECK(sa.world->rev != rev2 && sa.inv[BL_PLANKS] == 0, "(in creative it is put as before, with nothing in the pockets)");
		sa.survival = true;
		/* the keep is not broken, and the goblins' blows wear it down: mended by day, at 0 the tower goes and the game is over */
		struct bl_hit kh = { .hit = true, .x = sa.keep[0], .y = sa.keep[1], .z = sa.keep[2] };
		CHECK(!bl_break(sa.world, &kh), "a hand cannot break the keep");
		sa.keep_hp = 10.0f;
		sa.time_h = 12.0f;
		survival_tick(&sa, 2.0f);
		CHECK(sa.keep_hp > 12.5f && sa.keep_hp < 13.5f, "by day it mends one and a half a second (%.1f)", sa.keep_hp);
		sa.time_h = 0.0f;
		float h0 = sa.keep_hp;
		survival_tick(&sa, 2.0f);
		CHECK(sa.keep_hp == h0 && sa.in_night, "by night it does not");
		sa.time_h = 12.0f;
		int n0 = sa.nights;
		survival_tick(&sa, 0.1f);
		CHECK(sa.nights == n0 + 1 && !sa.in_night, "when the sun is up again a night is counted");
		survival_tick(&sa, 0.1f);
		CHECK(sa.nights == n0 + 1, "once");
		sa.prompt = P_NONE;
		sa.playing = true;
		sa.keep_hp = 3.0f;
		bl_mobs_clear(&sa.mobs);
		sa.mobs.spawn_t = 1e9f;
		sa.time_h = 0.0f;
		sa.pl.x = 5.0f;
		sa.pl.z = 5.0f;
		sa.pl.y = 20.0f;
		bl_mobs_add(&sa.mobs, sa.world, BL_MOB_GOBLIN, (float)sa.keep[0] + 3.5f, (float)sa.keep[1], (float)sa.keep[2] + 0.5f);
		for (int i = 0; i < 30 * 12 && sa.have_keep; i++)
			mobs_advance(&sa, 1.0f / 30.0f);
		CHECK(!sa.have_keep && sa.prompt == P_LOST && !sa.playing, "a goblin at the keep pulls it down: the game is over");
		CHECK(bl_get(sa.world, sa.keep[0], sa.keep[1], sa.keep[2]) == BL_AIR && bl_get(sa.world, sa.keep[0], sa.keep[1] + 2, sa.keep[2]) == BL_AIR && !sa.mobs.keep_on, "its blocks are gone and the goblins have no keep to go to");
		/* a keep is made again when there is none and he is in survival */
		ensure_keep(&sa, sa.pl.x, sa.pl.z);
		CHECK(sa.have_keep && sa.keep_hp == KEEP_HP && sa.mobs.keep_on, "ensure_keep makes a new one");
		/* creative: the goblins do not know of any */
		sa.survival = false;
		keep_sync(&sa);
		CHECK(!sa.mobs.keep_on, "in creative there is no keep to defend");
		bl_world_free(sa.world);
	}

	/* the settings of a survival world are kept: mode, keep, nights, kills and what he has; a bad line is not believed */
	{
		char dir2[128];
		snprintf(dir2, sizeof dir2, "/tmp/mellagui-blocks-selftest2-%d", (int)getpid());
		static struct app sb, sc;
		memset(&sb, 0, sizeof sb);
		snprintf(sb.dir, sizeof sb.dir, "%s/x", dir2);
		CHECK(mkdirs(sb.dir) == 0, "mkdirs");
		sb.world_size = 64;
		sb.fov = 1.2f;
		sb.sens = 0.0025f;
		memcpy(sb.hotbar, k_default_hotbar, sizeof sb.hotbar);
		CHECK(make_world(&sb, 5, true), "a flat world");
		sb.survival = true;
		sb.have_keep = true;
		sb.keep[0] = 40;
		sb.keep[1] = 21;
		sb.keep[2] = 33;
		sb.keep_hp = 41.5f;
		sb.nights = 4;
		sb.kills = 9;
		sb.inv[BL_COBBLE] = 17;
		sb.inv[BL_LOG] = 3;
		conf_save(&sb);
		memset(&sc, 0, sizeof sc);
		snprintf(sc.dir, sizeof sc.dir, "%s", sb.dir);
		sc.fov = 1.2f;
		sc.sens = 0.0025f;
		memcpy(sc.hotbar, k_default_hotbar, sizeof sc.hotbar);
		sc.pl.x = 1;
		conf_load(&sc, true);
		CHECK(sc.conf_read && sc.conf_mode && sc.survival && sc.have_keep && sc.keep[0] == 40 && sc.keep[1] == 21 && sc.keep[2] == 33 && fabsf(sc.keep_hp - 41.5f) < 0.01f, "the keep and the mode come back");
		CHECK(sc.nights == 4 && sc.kills == 9 && sc.inv[BL_COBBLE] == 17 && sc.inv[BL_LOG] == 3 && sc.inv[BL_STONE] == 0, "and the nights, the kills and the pockets");
		char path2[600];
		snprintf(path2, sizeof path2, "%s/player.conf", sb.dir);
		FILE *f2 = fopen(path2, "w");
		if (f2) {
			fprintf(f2, "survival = 7\nkeep_hp = 900\nnights = -3\ninv = 3:5,999:2,4:-1,6:4\nkeep_x = 5\n");
			fclose(f2);
		}
		memset(&sc, 0, sizeof sc);
		snprintf(sc.dir, sizeof sc.dir, "%s", sb.dir);
		conf_load(&sc, true);
		CHECK(!sc.conf_mode && sc.keep_hp == 0.0f && sc.nights == 0 && sc.inv[3] == 5 && sc.inv[4] == 0 && sc.inv[6] == 4, "a survival of 7, 900 hit points, minus three nights and a block number that is none are not believed");
		FILE *f3 = fopen(path2, "w");
		if (f3) {
			fprintf(f3, "pets = 1\n");
			fclose(f3);
		}
		memset(&sc, 0, sizeof sc);
		snprintf(sc.dir, sizeof sc.dir, "%s", sb.dir);
		conf_load(&sc, true);
		CHECK(sc.conf_read && !sc.conf_mode, "an old file without a mode line says so (the world is then a creative one)");
		bl_world_free(sb.world);
		char cmd2[200];
		snprintf(cmd2, sizeof cmd2, "rm -rf '%s'", dir2);
		if (system(cmd2) != 0)
			fprintf(stderr, "(could not remove %s)\n", dir2);
	}

	/* the hand: slots wrap round */
	memset(&a, 0, sizeof a);
	a.slot = 0;
	choose_slot(&a, -1);
	CHECK(a.slot == SLOTS - 1, "slot -1 wraps to the last");
	choose_slot(&a, SLOTS);
	CHECK(a.slot == 0, "slot 9 wraps to the first");

	if (!bad)
		printf("mellagui-blocks selftest: ok\n");
	return bad ? 1 : 0;
#undef CHECK
}

/* ================================================================ main ================================================================ */

int main(int argc, char **argv)
{
	wlwin_hidpi();
	lm_theme_load_env();
	signal(SIGPIPE, SIG_IGN);
	static struct app a;
	a.world_size = 256;
	a.fov = 70.0f * (float)M_PI / 180.0f;
	a.sens = 0.0025f;
	a.time_h = 9.0f;
	a.clockm = 1;
	a.sound_on = true;
	a.pal_hover = -1;
	a.pets_on = true;
	a.mobs_on = true;
	a.survival = true;
	int clock_cli = -1;
	bool no_pets = false, no_mobs = false;
	int mode_cli = -1;                                      /* --survival 1, --creative 0 */
	char give[256] = "";
	int ngob = 0;
	float gob[4][2] = { { 0 } };
	float pet_at = -1.0f;
	a.day_minutes = 20.0f;
	a.threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (a.threads < 1)
		a.threads = 1;
	if (a.threads > 8)
		a.threads = 8;
	a.pc_hover = a.pc_down = -1;
	a.first = true;
	a.budget = 300000;
	memcpy(a.hotbar, k_default_hotbar, sizeof a.hotbar);
	int win_w = 960, win_h = 600;
	float yaw = NAN, pitch = NAN;
	bool want_save = false, no_save = false, no_sound = false;
	for (int i = 1; i < argc; i++) {
		char junk;
		if (!strcmp(argv[i], "--selftest")) {
			return selftest();
		} else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			a.seed_given = (uint32_t)strtoul(argv[++i], NULL, 10);
			a.have_seed = true;
		} else if (!strcmp(argv[i], "--flat")) {
			a.flat = true;
		} else if (!strcmp(argv[i], "--world-size") && i + 1 < argc) {
			a.world_size = atoi(argv[++i]);
			if (a.world_size < BL_MIN_SIZE || a.world_size > BL_MAX_SIZE) {
				fprintf(stderr, "mellagui-blocks: --world-size wants %d to %d\n", BL_MIN_SIZE, BL_MAX_SIZE);
				return 2;
			}
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 320 || win_h < 240) {
				fprintf(stderr, "mellagui-blocks: --size wants a size like 960x600\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--render") && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d%c", &a.fix_w, &a.fix_h, &junk) != 2 || a.fix_w < 16 || a.fix_h < 16 || a.fix_w > 4096 || a.fix_h > 4096) {
				fprintf(stderr, "mellagui-blocks: --render wants a size like 320x200\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
			int n = atoi(argv[++i]);                           /* (LM_CLAMP evaluates its argument three times: read it first) */
			a.threads = LM_CLAMP(n, 1, 8);
		} else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
			a.time_h = (float)atof(argv[++i]);
			if (!(a.time_h >= 0 && a.time_h < 24)) {
				fprintf(stderr, "mellagui-blocks: --time wants an hour from 0 to 24\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--no-clock")) {
			clock_cli = 0;
		} else if (!strcmp(argv[i], "--real-clock")) {
			clock_cli = 2;
		} else if (!strcmp(argv[i], "--clock")) {
			clock_cli = 1;
		} else if (!strcmp(argv[i], "--no-pets")) {
			no_pets = true;
		} else if (!strcmp(argv[i], "--no-mobs")) {
			no_mobs = true;
		} else if (!strcmp(argv[i], "--survival")) {
			mode_cli = 1;
		} else if (!strcmp(argv[i], "--creative")) {
			mode_cli = 0;
		} else if (!strcmp(argv[i], "--give") && i + 1 < argc) {                   /* NAME=N,NAME=N: what is in the pockets at the start (for the tests, and for cheating) */
			snprintf(give, sizeof give, "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--day-minutes") && i + 1 < argc) {
			a.day_minutes = (float)atof(argv[++i]);
			if (!(a.day_minutes >= 0.05f && a.day_minutes <= 600.0f)) {
				fprintf(stderr, "mellagui-blocks: --day-minutes wants 0.05 to 600\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--goblin") && i + 1 < argc) {                 /* a goblin at DX,DZ blocks from where the player starts (for the tests) */
			if (ngob >= 4 || sscanf(argv[++i], "%f,%f%c", &gob[ngob][0], &gob[ngob][1], &junk) != 2) {
				fprintf(stderr, "mellagui-blocks: --goblin wants DX,DZ (at most four)\n");
				return 2;
			}
			ngob++;
		} else if (!strcmp(argv[i], "--pet-at") && i + 1 < argc) {
			pet_at = (float)atof(argv[++i]);
		} else if (!strcmp(argv[i], "--no-sound")) {
			a.sound_on = false;
			no_sound = true;
		} else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) {
			yaw = (float)(atof(argv[++i]) * M_PI / 180.0);
		} else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) {
			pitch = (float)(atof(argv[++i]) * M_PI / 180.0);
		} else if (!strcmp(argv[i], "--fov") && i + 1 < argc) {
			double d = atof(argv[++i]);
			if (d < 40 || d > 120) {
				fprintf(stderr, "mellagui-blocks: --fov wants 40 to 120 degrees\n");
				return 2;
			}
			a.fov = (float)(d * M_PI / 180.0);
		} else if (!strcmp(argv[i], "--save")) {
			want_save = true;
		} else if (!strcmp(argv[i], "--no-save")) {
			no_save = true;
		} else {
			fprintf(stderr, "usage: mellagui-blocks [--seed N] [--flat] [--world-size N] [--size WxH] [--render WxH] [--threads N] [--time H] [--no-clock | --clock | --real-clock] [--no-sound] [--no-pets] [--no-mobs] [--survival | --creative] [--give NAME=N,...] [--day-minutes M] [--goblin DX,DZ] [--pet-at SECONDS] [--yaw D] [--pitch D] "
			                "[--fov D] [--save | --no-save] | --selftest\n");
			return 2;
		}
	}
	g_dump = getenv("MELLAGUI_BLOCKS_DUMP");
	data_dir(a.dir, sizeof a.dir);

	/* the world: the saved one, unless a seed or a flat world was asked for */
	bool fresh = a.have_seed || a.flat;
	bool from_file = false;
	if (!fresh) {
		char path[600];
		world_path(&a, path, sizeof path);
		if (access(path, R_OK) == 0) {
			const char *err = NULL;
			a.world = bl_load(path, &err);
			if (a.world) {
				from_file = true;
				a.saved_rev = a.world->rev;
				bl_player_init(&a.pl, a.world);
				conf_load(&a, true);
			} else {
				fprintf(stderr, "mellagui-blocks: %s: %s (the file is left alone and a new world is not saved)\n", path, err ? err : "cannot be read");
				a.nosave = true;
			}
		}
	}
	if (!a.world) {
		if (!make_world(&a, a.have_seed ? a.seed_given : fresh_seed(), a.flat)) {
			fprintf(stderr, "mellagui-blocks: cannot make a world\n");
			return 1;
		}
		conf_load(&a, false);
		if (fresh)
			a.nosave = !want_save;
	}
	if (no_save)
		a.nosave = true;
	if (clock_cli >= 0)
		a.clockm = clock_cli;
	if (no_sound)
		a.sound_on = false;
	if (no_pets)
		a.pets_on = false;
	if (no_mobs)
		a.mobs_on = false;
	if (pet_at >= 0.0f)
		a.pet_t = pet_at;
	settle_player(&a);
	if (a.pets_on) {                                        /* (without them no yard is looked for and no pond is dug: the world is left as it is) */
		float near[3];
		bl_spawn(a.world, near);
		pets_setup(&a, near, true);
	}
	mobs_setup(&a);
	if (from_file && a.conf_read && !a.conf_mode && mode_cli < 0)
		a.survival = false;                                     /* a world saved before survival was a creative world */
	if (mode_cli >= 0)
		a.survival = mode_cli != 0;
	if (from_file)
		ensure_keep(&a, a.pl.x, a.pl.z);
	else
		survival_setup(&a, a.pl.x, a.pl.z);
	for (char *tok = strtok(give, ","); tok; tok = strtok(NULL, ",")) {
		char *eq = strchr(tok, '=');
		int n = eq ? atoi(eq + 1) : 1;
		if (eq)
			*eq = 0;
		for (int b = 1; b < BL_COUNT; b++)
			if (!strcmp(tok, bl_block_name(b)) && hand_ok(b) && n > 0)
				inv_add(&a, b, n);
	}
	for (int i = 0; i < ngob && a.mobs_on; i++) {
		int gx = (int)floorf(a.pl.x + gob[i][0]), gz = (int)floorf(a.pl.z + gob[i][1]);
		int top = bl_top(a.world, gx, gz);
		if (top >= 0)
			bl_mobs_add(&a.mobs, a.world, BL_MOB_GOBLIN, (float)gx + 0.5f, (float)(top + 1), (float)gz + 0.5f);
	}
	if (!isnan(yaw))
		a.pl.yaw = yaw;
	if (!isnan(pitch))
		a.pl.pitch = pitch;
	(void)from_file;
	aim_update(&a);

	static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event,
	                                      .lock = lock_event, .relative = relative_event };
	if (wlwin_connect(&a.win, "mellagui-blocks", &ops, &a) < 0)
		return 1;
	if (wlwin_open(&a.win, "Blocks", "mellagui.blocks", win_w, win_h, 480, 320) < 0)
		return 1;
	static uint32_t cur[24 * 24];
	arrow_cursor(cur, 24, 24);
	wlwin_cursor(&a.win, cur, 24, 24, 1, 1);
	a.prompt = P_PAUSE;
	a.last_save_ms = now_ms();
	a.fps_t0 = now_ms();

	int64_t last = now_ms();
	while (a.win.running) {
		int timeout = a.playing ? 1 : (a.msg[0] || (!a.nosave && a.world->rev != a.saved_rev)) ? 500 : -1;
		if (!wlwin_step(&a.win, timeout))
			break;
		int64_t now = now_ms();
		float dt = (float)(now - last) / 1000.0f;
		last = now;
		if (dt > 0.1f)
			dt = 0.1f;
		advance(&a, now, dt);
		if (a.playing || a.msg[0])
			a.win.redraw = true;
	}
	save_all(&a, false);
	wlwin_lock_pointer(&a.win, false);
	icons_free(&a);
	free(a.frame);
	bl_world_free(a.world);
	wlwin_destroy(&a.win);
	return 0;
}

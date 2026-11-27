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
 * T stops and starts it, F3 shows the numbers, F1 the keys, Ctrl+S saves (Ctrl+Shift+S as another world), Ctrl+O opens a world, Ctrl+N makes one, Ctrl+Q quits (it saves).
 *
 * Every world is a file of its own: the edits to what its seed makes, and after them where you stand, what you have, the keep and the units (`bl_save_with`).
 * The worlds live in $MELLAGUI_BLOCKS_DIR (default $XDG_DATA_HOME/mellagui/blocks): world.bl, world-2.bl ... and whatever Save as made; the open / save as
 * card (wlpicker.h) reaches other folders too. player.conf holds the person's choices and which world was open, and that one is opened the next time. A world
 * is saved a minute after a change, when you pause, and when the window closes. `--seed` and `--flat` make a world with no file (so a look at a seed never
 * replaces yours) unless `--save` is given. The buttons at the left (wlrail.h) are the new / open / save / play ... of the other programs.
 *
 * The player has a body (client/blockshero.c): his arm and what it holds in the first person, the whole figure from above; H hides it.
 *
 * Survival has a keep to defend, workers and soldiers commanded from above (Tab), and buildings the workers put up there: a barracks, a watchtower, walls and a
 * gate (3 .. 6; client/blocksbuild.c). F1 in the game explains it all.
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
#include "blocksbuild.h"
#include "blockshero.h"
#include "blockspets.h"
#include "blocksmobs.h"
#include "blocksview.h"
#include "font.h"
#include "gamesound.h"
#include "keymap.h"
#include "metrics.h"
#include "pathcomp.h"
#include "protocol.h"
#include "smartsun.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlclip.h"
#include "wlhelp.h"
#include "wlpicker.h"
#include "wlrail.h"
#include "wltext.h"
#include "wlwin.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* evdev key codes: the place of a key, not the letter it makes */
enum {
	K_ESC = 1, K_1 = 2, K_9 = 10, K_Q = 16, K_W = 17, K_T = 20, K_LBRACE = 26, K_RBRACE = 27, K_ENTER = 28, K_LCTRL = 29, K_A = 30, K_S = 31, K_D = 32,
	K_E = 18, K_F = 33, K_P = 25, K_M = 50, K_G = 34, K_X = 45, K_TAB = 15, K_B = 48, K_BACKSPACE = 14, K_HOME = 102, K_LSHIFT = 42, K_N = 49, K_RSHIFT = 54, K_SPACE = 57, K_F1 = 59, K_F3 = 61, K_RCTRL = 97, K_UP = 103, K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108, K_END = 107, K_O = 24, K_Y = 21, K_K = 37, K_PGUP = 104, K_PGDN = 109, K_V = 47, K_R = 19, K_DEL = 111, K_H = 35, K_I = 23,
};

#define SLOTS 9
#define REPEAT_FIRST_MS 500                                 /* a button held this long breaks / places again (a click, even a slow one, does it once) ... */
#define REPEAT_MS 250                                       /* ... and then every this often */
#define DIG_PAUSE 0.3f                                      /* survival: after a block gives way the next one is not begun for this long, unless the button is pressed anew */
#define AUTOSAVE_MS 60000
#define LOOK_RATE 1.7f                                      /* radians a second with the arrow keys */
#define MIN_BUDGET 40000                                    /* the least pixels the picture is made of, and the most it is asked to take (ms) */
#define SLOW_MS 27.0
#define MAX_HP 20.0f                                        /* ten hearts, two hit points each */
#define SWORD_SHOWN 7.0f                                    /* a goblin this near: the sword is in the hand */
#define SWORD_MS 400                                        /* the least time between two blows */
#define KEEP_HP 150.0f                                      /* what the keep can take: a goblin's blow is 2 */
#define KEEP_GOLD 20                                        /* what a new keep costs, when one has fallen: whoever cannot pay it has lost */
#define KEEP_BACK 30.0f                                     /* seconds until the player's stands again ... */
#define RIVAL_BACK 45.0f                                    /* ... and the Red Banner's */
#define RIVAL_GOLD 20                                       /* the gold a new camp has ... */
#define RIVAL_GOLD_MAX 60                                   /* ... and the most it keeps */
#define CHIEF_BACK 90.0f                                    /* seconds until their chief is back after he has fallen */
#define DEPOSITS 5                                          /* gold: one deposit near each side and three between them */
#define MINE_EVERY 10.0f                                    /* seconds between two pieces a mine takes ... */
#define MINE_RANGE 6                                        /* ... of the gold this near it */
#define HEAL_RANGE 10.0f                                    /* this near an infirmary the player's own mend */
#define SPECIAL_MAX 8
#define FAST_MS 15.0

#define RIVAL_HP 200.0f                                     /* what the Red Banner's keep can take: a blow of the sword takes 3, of a soldier 4 */
#define RIVAL_SWORD 3.0f
#define RIVAL_GUARDS 6                                      /* the guards of their camp besides the archers; one more comes every RIVAL_REINFORCE seconds while there are fewer ... */
#define RIVAL_REINFORCE 45.0f
#define RIVAL_GUARDS_MORE 8                                 /* ... and this many once their longhouse stands */
#define RIVAL_GUARD_COST 5                                  /* what the camp pays from its store of material for a guard ... */
#define RIVAL_RAIDER_COST 4                                 /* ... and for each man of a raiding party */
#define RIVAL_STOCK 12                                      /* the store a new camp has */
#define RIVAL_STOCK_MAX 60                                  /* (so a camp cannot save up for ever: catching its workers is felt) */
#define RIVAL_SPARE 8                                       /* what a building leaves in the store */
#define RIVAL_WORKER_EVERY 90.0f                            /* seconds until a worker of theirs who is gone is replaced */
#define CAMP_CHECK 5.0f                                     /* how often the camp looks over its buildings and starts the next one */
#define RIVAL_NAME "Red Banner"
#define CHIEF_NAME "Red Captain"
#define RAID_FIRST 300.0f                                   /* seconds of play by day until their first raiding party sets out in survival (time to get soldiers) ... */
#define RAID_FIRST_CREATIVE 120.0f                          /* ... in creative, where nothing has to be gathered ... */
#define RAID_PAUSE 240.0f                                   /* ... and from the end of one until the next */
#define RAID_SMALL 2                                        /* the men of the first one; every party has one more than the one before (BL_RAIDER_MAX at most) */
#define RAID_NEAR 28.0f                                     /* this near where they are bound: "the raiders are here" */
#define RAID_NO_HOUSE 4                                     /* a party is at most this big until their longhouse stands (then BL_RAIDER_MAX) */

/* the buildings of their camp, in the order they are made (the first tower is there from the start; one that is brought down is built again) */
enum { CAMP_TOWER, CAMP_HOUSE, CAMP_TOWER2, CAMP_TOWER3, CAMP_BUILDINGS };
static const struct {
	const char *name;
	int cost, dx, dz;                                       /* (a tower's middle, from their keep) */
} k_camp[CAMP_BUILDINGS] = { { "watchtower", 20, -7, -7 }, { "longhouse", 30, 0, 0 }, { "watchtower", 20, 7, 7 }, { "watchtower", 20, 7, -7 } };

#define PANEL_W 340                                          /* the panel of text beside the window (I) */
#define PANEL_H 640
#define LOG_N 24                                            /* the messages it remembers */

#define N_SFX 33                                            /* the sounds of k_sfx */

enum prompt { P_NONE, P_PAUSE, P_NEW, P_HELP, P_PALETTE, P_DEAD, P_LOST, P_OPEN, P_SAVEAS, P_REPLACE, P_GOAL, P_WON, P_ARMS };

struct app {
	struct wlhelp help;                                     /* the pages of F1 */
	uint32_t test_seed;                                     /* the selftest: the seed of the next new world (0 = a fresh one) */
	bool panel;                                             /* the text stands in a panel of its own beside the window, where there are card windows (I) */
	struct logline {
		char text[200];
		int64_t ms;
		int times;                                          /* said this many times in a row */
	} log[LOG_N];                                           /* the messages, the newest first */
	int nlog;
	struct wlwin win;
	struct bl_world *world;
	struct bl_player pl;
	char dir[512];
	bool nosave, force_save;
	char path[1024];                                        /* the world file that is open ("" = this world has none: a seed's, or one that could not be read) */
	char last_world[1024];                                  /* player.conf's `world`: the file that was open the last time */
	int cli_clock;                                          /* what the command line said holds for every world of this run: the clock (-1 = nothing said) ... */
	bool cli_no_pets, cli_no_mobs, cli_no_save;             /* ... and no pets, no goblins, nothing saved by itself */
	bool cli_goblins;                                       /* --goblin / --brute: they are there in a free world, too */
	char gname[64];                                         /* the card of a new game: its name ... */
	bool new_survival;                                      /* ... a game (else free building) ... */
	bool new_first;                                         /* ... and there is no world to go back to */
	uint32_t arms;                                          /* the weapons he has, bit 1 << BL_WP_* */
	int weapon;                                             /* the one in his hand */
	int charges[BL_WP_COUNT];                               /* shots left of the gold that was paid into the fire staff and the laser */
	int64_t weapon_until;                                   /* the weapon is shown until this (just chosen) */
	bool keep_down;                                         /* his keep has fallen and a new one is on its way ... */
	float keep_back;                                        /* ... in this many seconds */
	int ndeposits;
	int deposit[BL_DEPOSITS_MAX][3];                        /* where gold lies: x, the ground, z */
	int ore[BL_DEPOSITS_MAX];                               /* the pieces that are left of each (counted once a second) */
	int nsmithy, nmage, ninfirm, nmine;                     /* the special buildings (looked for once a second) */
	int infirm[SPECIAL_MAX][3], mine[SPECIAL_MAX][3];
	float look_t, mine_t;
	float rkeep_back;                                       /* their keep has fallen: seconds until it stands again (0 = it stands) */
	int rgold;                                              /* their gold */
	uint32_t chief_arms;                                    /* the weapons of their chief */
	float chief_t, chief_hp;                                /* seconds until he is there again; what the file said of him */
	float chief_at[3];
	bool chief_saved, chief_was;                            /* the file had him about; he was about at the last step */
	enum prompt back;                                       /* where Cancel of the new / open / save as card goes */
	char field[1024];                                       /* the path in the open / save as card ... */
	char target[1024];                                      /* ... the file it is about to replace ... */
	char card_msg[200];                                     /* ... and why it did not do what was asked */
	struct wlpicker pk;
	struct wlclip clip;
	struct wlrail_anim ran;                                 /* the springs of the rail */
	int rival;                                              /* the rival clan: 0 it has never come to this world, 1 its camp stands, 2 it has been beaten */
	bool rival_on;                                          /* ... and it is switched on (its guards are about, its keep can be attacked) */
	bool cli_rival, cli_no_rival;                           /* --rival (it comes at once), --no-rival (a new world gets none) */
	int rkeep[3];                                           /* the lowest block of its keep */
	float rhp;                                              /* what the keep can still take */
	int rguards;                                            /* how many guards there are while it is switched off, or as the file said */
	int rarchers;                                           /* ... and which towers have an archer (a bit for each of k_camp) */
	int rstock;                                             /* the material in their store */
	int rbuilt;                                             /* the buildings of theirs that stand (bits of k_camp) ... */
	int rsite;                                              /* ... and the one that is being built (-1 = none) */
	int rhands;                                             /* how many workers they have while switched off, or as the file said */
	float rworker_t, rcheck_t;                              /* seconds until a lost worker is replaced, until the buildings are looked over */
	float rguard_t;                                         /* seconds until the next one comes */
	float rhit_t;                                           /* seconds the keep's bar shows after a blow at it */
	int raids;                                              /* raiding parties that have set out */
	float raid_t;                                           /* seconds of play by day until the next one does */
	bool raid_out, raid_here, raid_home;                    /* one is out; it has been said that it has come; some of its men went home */
	float cli_raid;                                         /* --raid-after: seconds until the first one (0 = nothing said) */
	int nraiders_conf;                                      /* the raiders of the world's file */
	float raider_conf[BL_RAIDER_MAX][3];
	int rail_hover;
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
	int64_t sfx_ms[N_SFX];                                  /* when each sound came last */
	int last_sfx;                                           /* the last one sent: an index of k_sfx (-1 = none) */
	float growl_t;                                          /* seconds until a goblin may be heard again */
	int growls;
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
	float dig_wait;                                         /* seconds of DIG_PAUSE that are left */
	float dig_tick;                                         /* the dig_t at which the next blow is heard */
	bool have_keep;
	int keep[3];                                            /* the lowest block of the keep */
	float keep_hp;
	float keep_msg_t;                                       /* seconds until "the keep is under attack" may be said again */
	int nights;                                             /* nights survived with the keep standing */
	bool in_night;

	bool cmd;                                               /* the command view: the camera is above, the pointer is free, left chooses workers, right orders them */
	float cam_f[3];                                         /* the point the command camera looks at */
	float cam_yaw, cam_dist;
	bool drag_sel;                                          /* a box is being pulled with the left button */
	double drag_x, drag_y;
	int order_mode;                                         /* what the right button orders: 0 go there, 1 break that block and its kind */
	bool hover;                                             /* the pointer is over a block of the world */
	int hover_b[3];
	int plan;                                               /* what is being placed (enum bl_plan), -1 = nothing: the buttons act on the units */
	int plan_rot;                                           /* quarter turns on top of the way the camera looks (R) */
	struct bl_ghosts ghosts;                                /* the planned blocks and the preview, for this frame */
	float siege_msg_t;                                      /* seconds until "the goblins are breaking through" may be said again */
	int nsites_conf;                                        /* `site` lines of player.conf */
	struct bl_site site_conf[BL_SITES_MAX];
	bool workers_saved;                                     /* player.conf had a `workers` line */
	int nworkers_conf;
	float worker_conf[BL_WORKER_MAX][3];
	bool soldiers_saved;                                    /* player.conf had a `soldiers` line */
	int nsoldiers_conf;
	float soldier_conf[BL_SOLDIER_MAX][3];
	bool wave_msg_done;                                     /* the message of the wave's end was given */
	bool brute_warned;                                      /* the first brute of the game has been told about */

	bool body;                                              /* the player has a body in the picture (H) */
	struct bl_hero hero;                                    /* ... as it was posed for the last frame */
	float walk_phase, walk_stride, arm_swing;               /* its walk (a stride is one turn, how far the legs go) and the swing of the arm */

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
	char line[sizeof a->log[0].text];
	snprintf(line, sizeof line, "%s", a->msg);
	if (a->nlog > 0 && !strcmp(a->log[0].text, line)) {
		a->log[0].times++;
	} else {
		memmove(&a->log[1], &a->log[0], sizeof a->log[0] * (LOG_N - 1));
		snprintf(a->log[0].text, sizeof a->log[0].text, "%s", line);
		a->log[0].times = 1;
		a->nlog = LM_MIN(a->nlog + 1, LOG_N);
	}
	a->log[0].ms = a->msg_ms;
	a->win.redraw = true;
}

/* ================================================================ the hand and the clock ================================================================ */

static const int k_default_hotbar[SLOTS] = { BL_STONE, BL_DIRT, BL_GRASS, BL_COBBLE, BL_PLANKS, BL_LOG, BL_BRICK, BL_SAND, BL_GLASS };

/* the worlds the open / save as card lists */
static bool world_file(const char *name)
{
	size_t l = strlen(name);
	return l > 3 && !strcmp(name + l - 3, ".bl");
}

/* a block that can be in the hand: any but air, bedrock and flowing water (water in the hand is a source) */
static bool hand_ok(int b)
{
	return b > BL_AIR && b < BL_COUNT && b != BL_BEDROCK && !(b >= BL_FLOW1 && b <= BL_FLOW7);
}

#define PAL_N 22
#define PAL_COLS 8
static const int k_palette[PAL_N] = { BL_GRASS, BL_DIRT, BL_STONE, BL_SAND, BL_LOG, BL_LEAVES, BL_PLANKS, BL_GLASS, BL_WATER, BL_BRICK, BL_SNOW, BL_COBBLE, BL_TORCH, BL_LAMP, BL_BARRACKS, BL_GATE, BL_TOWERPOST,
	                                      BL_GOLD, BL_SMITHY, BL_MAGE, BL_INFIRMARY, BL_MINE };

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

/* "x:y:z,x:y:z": the places of some workers or soldiers; how many were read (at most `max`) */
static int parse_units(const char *v, float out[][3], int max)
{
	int n = 0;
	for (const char *p = v; *p && n < max;) {
		float c[3];
		int used = 0;
		if (sscanf(p, "%f:%f:%f%n", &c[0], &c[1], &c[2], &used) != 3 || used <= 0)
			break;
		p += used;
		memcpy(out[n++], c, sizeof c);
		while (*p == ',' || *p == ' ')
			p++;
	}
	return n;
}

/* "plan n x:y:z:block,...": a building under way */
static bool parse_site(const char *v, struct bl_site *s)
{
	int plan, n, used = 0;
	memset(s, 0, sizeof *s);
	if (sscanf(v, "%d %d %n", &plan, &n, &used) != 2 || plan < 0 || plan >= BL_PLAN_COUNT || n < 1 || n > BL_SITE_CELLS || used <= 0)
		return false;
	s->plan = plan;
	for (const char *p = v + used; s->n < n;) {
		int x, y, z, b, m = 0;
		if (sscanf(p, "%d:%d:%d:%d%n", &x, &y, &z, &b, &m) != 4 || m <= 0 || x < 0 || x >= BL_MAX_SIZE || z < 0 || z >= BL_MAX_SIZE || y < 1 || y >= BL_MAX_H || !bl_siege_hp(b))
			return false;
		s->c[s->n++] = (struct bl_cell){ (int16_t)x, (int16_t)y, (int16_t)z, (uint8_t)b };
		p += m;
		while (*p == ',' || *p == ' ')
			p++;
	}
	s->on = true;
	return true;
}

/* Lines `key = value`. `with_player`: also what belongs to one world (where he stood, what he had, the keep, the units); `prefs`: also what is the person's,
 * whatever world is open (the field of view, the pointer's speed, sound, the body). */
static void conf_read(struct app *a, FILE *f, bool with_player, bool prefs)
{
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
		if (!prefs && (!strcmp(k, "fov") || !strcmp(k, "sensitivity") || !strcmp(k, "sound") || !strcmp(k, "panel")))
			continue;
		if (!strcmp(k, "world")) {
			v[strcspn(v, "\r\n")] = 0;
			snprintf(a->last_world, sizeof a->last_world, "%s", v);
		} else if (with_player && !strcmp(k, "rival")) {
			int state, on, x, y, z, guards, archer;
			float hp;
			if (sscanf(v, "%d %d %d %d %d %f %d %d", &state, &on, &x, &y, &z, &hp, &guards, &archer) == 8 && state >= 1 && state <= 2 && x > 0 && y > 0 && z > 0 &&
			    x < BL_MAX_SIZE && z < BL_MAX_SIZE && y < BL_MAX_H && hp >= 0.0f && hp <= RIVAL_HP && guards >= 0 && guards <= BL_GUARD_MAX && archer >= 0 && archer < 1 << CAMP_BUILDINGS) {
				(void)on;                                   /* (a camp could once be switched off) */
				a->rival = state;
				a->rkeep[0] = x;
				a->rkeep[1] = y;
				a->rkeep[2] = z;
				a->rhp = hp;
				a->rguards = guards;
				a->rarchers = archer;
			}
		} else if (with_player && !strcmp(k, "supply")) {
			int stock, built, site, hands;
			if (sscanf(v, "%d %d %d %d", &stock, &built, &site, &hands) == 4 && stock >= 0 && stock <= RIVAL_STOCK_MAX && built >= 0 && built < 1 << CAMP_BUILDINGS &&
			    site >= -1 && site < CAMP_BUILDINGS && hands >= 0 && hands <= BL_RWORKER_MAX) {
				a->rstock = stock;
				a->rbuilt = built;
				a->rsite = site;
				a->rhands = hands;
			}
		} else if (with_player && !strcmp(k, "raid")) {
			int raids, used = 0;
			float t;
			if (sscanf(v, "%d %f%n", &raids, &t, &used) == 2 && used > 0 && raids >= 0 && raids < 100000 && t >= 0.0f && t <= 3600.0f) {
				a->raids = raids;
				a->raid_t = t;
				a->nraiders_conf = parse_units(v + used, a->raider_conf, BL_RAIDER_MAX);
			}
		} else if (with_player && !strcmp(k, "gold")) {
			float at[BL_DEPOSITS_MAX][3];
			int n = parse_units(v, at, BL_DEPOSITS_MAX);
			a->ndeposits = 0;
			for (int i = 0; i < n; i++)
				if (at[i][0] >= 1 && at[i][1] >= 1 && at[i][2] >= 1 && at[i][0] < BL_MAX_SIZE && at[i][1] < BL_MAX_H && at[i][2] < BL_MAX_SIZE) {
					for (int c = 0; c < 3; c++)
						a->deposit[a->ndeposits][c] = (int)at[i][c];
					a->ndeposits++;
				}
		} else if (with_player && !strcmp(k, "arms")) {
			unsigned mask;
			int wp, fire, laser;
			if (sscanf(v, "%u %d %d %d", &mask, &wp, &fire, &laser) == 4 && mask < 1u << BL_WP_COUNT && wp >= 0 && wp < BL_WP_COUNT && fire >= 0 && fire < 1000 && laser >= 0 && laser < 1000) {
				a->arms = mask | 1u << BL_WP_SWORD;
				a->weapon = a->arms & 1u << wp ? wp : BL_WP_SWORD;
				a->charges[BL_WP_FIRE] = fire;
				a->charges[BL_WP_LASER] = laser;
			}
		} else if (with_player && !strcmp(k, "keep_back") && d > 0 && d <= KEEP_BACK) {
			a->keep_down = true;
			a->keep_back = (float)d;
		} else if (with_player && !strcmp(k, "rkeep_back") && d > 0 && d <= RIVAL_BACK) {
			a->rkeep_back = (float)d;
		} else if (with_player && !strcmp(k, "rgold") && d >= 0 && d <= RIVAL_GOLD_MAX) {
			a->rgold = (int)d;
		} else if (with_player && !strcmp(k, "chief")) {
			unsigned mask;
			int alive;
			float t, hp, x, y, z;
			if (sscanf(v, "%u %d %f %f %f %f %f", &mask, &alive, &t, &hp, &x, &y, &z) == 7 && mask < 1u << BL_WP_COUNT && t >= 0 && t <= CHIEF_BACK && hp >= 0 && hp <= BL_CHIEF_HP &&
			    x >= 0 && x < BL_MAX_SIZE && y >= 0 && y < 2 * BL_MAX_H && z >= 0 && z < BL_MAX_SIZE) {
				a->chief_arms = mask | 1u << BL_WP_SWORD;
				a->chief_saved = alive != 0 && hp > 0;
				a->chief_t = t;
				a->chief_hp = hp;
				a->chief_at[0] = x;
				a->chief_at[1] = y;
				a->chief_at[2] = z;
			}
		} else if (!strcmp(k, "fov") && d >= 40 && d <= 120)
			a->fov = (float)(d * M_PI / 180.0);
		else if (!strcmp(k, "sensitivity") && d >= 0.0005 && d <= 0.02)
			a->sens = (float)d;
		else if (!strcmp(k, "clock") && d >= 0 && d <= 2)
			a->clockm = (int)d;
		else if (!strcmp(k, "sound") && (d == 0 || d == 1))
			a->sound_on = d != 0;
		else if (!strcmp(k, "panel") && (d == 0 || d == 1))
			a->panel = d != 0;
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
		else if (with_player && !strcmp(k, "site")) {
			if (a->nsites_conf < BL_SITES_MAX && parse_site(v, &a->site_conf[a->nsites_conf]))
				a->nsites_conf++;
		} else if (with_player && !strcmp(k, "workers")) {
			a->workers_saved = true;
			a->nworkers_conf = parse_units(v, a->worker_conf, BL_WORKER_MAX);
		} else if (with_player && !strcmp(k, "soldiers")) {
			a->soldiers_saved = true;
			a->nsoldiers_conf = parse_units(v, a->soldier_conf, BL_SOLDIER_MAX);
		} else if (with_player && !strcmp(k, "pet_t") && d >= 0 && d < 1e7)
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
}

/* player.conf */
static void conf_load(struct app *a, bool with_player)
{
	char path[600];
	conf_path(a, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	conf_read(a, f, with_player, true);
	fclose(f);
}

/* the guards of the Red Banner's camp (raiders are not counted): those on the tower (`posted`) or those on the ground */
static int rival_count(const struct app *a, bool posted)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += a->mobs.mob[i].alive && a->mobs.mob[i].kind == BL_MOB_GUARD && !a->mobs.mob[i].raider && a->mobs.mob[i].posted == posted;
	return n;
}

/* the towers of their camp that have an archer, as bits of k_camp */
static int rival_archers(const struct app *a)
{
	int mask = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		for (int k = 0; m->alive && m->kind == BL_MOB_GUARD && m->posted && k < CAMP_BUILDINGS; k++)
			if (k != CAMP_HOUSE && m->gx == a->rkeep[0] + k_camp[k].dx && m->gz == a->rkeep[2] + k_camp[k].dz)
				mask |= 1 << k;
	}
	return mask;
}

/* everything about the game as it is now, as lines conf_read understands */
static void conf_write(const struct app *a, FILE *f)
{
	fprintf(f, "x = %.3f\ny = %.3f\nz = %.3f\nyaw = %.4f\npitch = %.4f\ntime = %.3f\nflying = %d\nslot = %d\nclock = %d\nsound = %d\npet_t = %.2f\n", a->pl.x, a->pl.y, a->pl.z, a->pl.yaw,
	        a->pl.pitch, a->time_h, a->pl.flying, a->slot + 1, a->clockm, a->sound_on, a->pet_t);
	fprintf(f, "arms = %u %d %d %d\n", a->arms, a->weapon, a->charges[BL_WP_FIRE], a->charges[BL_WP_LASER]);
	if (a->keep_down)
		fprintf(f, "keep_back = %.1f\n", fmaxf(a->keep_back, 0.1f));
	if (a->ndeposits) {
		fprintf(f, "gold = ");
		for (int i = 0; i < a->ndeposits; i++)
			fprintf(f, "%s%d:%d:%d", i ? "," : "", a->deposit[i][0], a->deposit[i][1], a->deposit[i][2]);
		fprintf(f, "\n");
	}
	if (a->pets.placed)
		fprintf(f, "yard_x = %.2f\nyard_z = %.2f\ntom_x = %.3f\ntom_y = %.3f\ntom_z = %.3f\nfilip_x = %.3f\nfilip_y = %.3f\nfilip_z = %.3f\n", a->pets.yard[0], a->pets.yard[2],
		        a->pets.tom.x, a->pets.tom.y, a->pets.tom.z, a->pets.filip.x, a->pets.filip.y, a->pets.filip.z);
	fprintf(f, "survival = %d\nnights = %d\nkills = %d\npanel = %d\n", a->survival, a->nights, a->kills, a->panel);
	if (a->have_keep)
		fprintf(f, "keep_x = %d\nkeep_y = %d\nkeep_z = %d\nkeep_hp = %.1f\n", a->keep[0], a->keep[1], a->keep[2], a->keep_hp);
	fprintf(f, "workers = ");
	{
		bool first = true;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (a->mobs.mob[i].alive && a->mobs.mob[i].kind == BL_MOB_WORKER) {
				fprintf(f, "%s%.2f:%.2f:%.2f", first ? "" : ",", a->mobs.mob[i].x, a->mobs.mob[i].y, a->mobs.mob[i].z);
				first = false;
			}
	}
	fprintf(f, "\nsoldiers = ");
	{
		bool first = true;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (a->mobs.mob[i].alive && a->mobs.mob[i].kind == BL_MOB_SOLDIER) {
				fprintf(f, "%s%.2f:%.2f:%.2f", first ? "" : ",", a->mobs.mob[i].x, a->mobs.mob[i].y, a->mobs.mob[i].z);
				first = false;
			}
	}
	for (int i = 0; i < BL_SITES_MAX; i++)
		if (a->mobs.site[i].on) {
			const struct bl_site *st = &a->mobs.site[i];
			fprintf(f, "\nsite = %d %d ", st->plan, st->n);
			for (int k = 0; k < st->n; k++)
				fprintf(f, "%s%d:%d:%d:%d", k ? "," : "", st->c[k].x, st->c[k].y, st->c[k].z, st->c[k].block);
		}
	if (a->rival) {
		int guards = a->rguards;
		int archers = a->rarchers, hands = a->rhands;
		if (a->rival_on) {
			guards = rival_count(a, false);
			archers = rival_archers(a);
			hands = bl_mobs_count_kind(&a->mobs, BL_MOB_RWORKER);
		}
		fprintf(f, "\nrival = %d %d %d %d %d %.1f %d %d", a->rival, a->rival_on, a->rkeep[0], a->rkeep[1], a->rkeep[2], a->rhp, guards, archers);
		if (a->rival == 1) {
			const int ci = a->rival_on ? bl_mobs_chief(&a->mobs) : -1;
			const struct bl_mob *c = ci >= 0 ? &a->mobs.mob[ci] : NULL;
			fprintf(f, "\nsupply = %d %d %d %d\nrgold = %d", a->rstock, a->rbuilt, a->rsite, hands, a->rgold);
			fprintf(f, "\nchief = %u %d %.1f %.1f %.2f %.2f %.2f", a->chief_arms, c != NULL, a->chief_t, c ? c->hp : 0.0f, c ? c->x : 0.0f, c ? c->y : 0.0f, c ? c->z : 0.0f);
			if (a->rkeep_back > 0.0f)
				fprintf(f, "\nrkeep_back = %.1f", a->rkeep_back);
		}
		if (a->rival == 1) {                                    /* the raids so far, the time to the next, and the men who are out (not those on their way home) */
			fprintf(f, "\nraid = %d %.1f ", a->raids, fmaxf(a->raid_t, 0.0f));     /* (while one is out it has run under 0) */
			bool first = true;
			for (int i = 0; i < BL_MOBS_MAX; i++) {
				const struct bl_mob *m = &a->mobs.mob[i];
				if (m->alive && m->kind == BL_MOB_GUARD && m->raider && !m->retreat) {
					fprintf(f, "%s%.2f:%.2f:%.2f", first ? "" : ",", m->x, m->y, m->z);
					first = false;
				}
			}
		}
	}
	fprintf(f, "\ninv = ");
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
}

/* player.conf: the same and which world is open (the next start opens it) */
static void conf_save(const struct app *a)
{
	char path[600], tmp[620];
	conf_path(a, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	conf_write(a, f);
	if (a->path[0])
		fprintf(f, "world = %s\n", a->path);
	if (fclose(f) != 0 || rename(tmp, path) != 0)
		unlink(tmp);
}

/* the name a world goes by: its file's without the folder and ".bl" */
static void world_name(const char *path, char *out, size_t n)
{
	if (!path[0]) {
		snprintf(out, n, "Unsaved world");
		return;
	}
	const char *sl = strrchr(path, '/');
	snprintf(out, n, "%s", sl ? sl + 1 : path);
	size_t l = strlen(out);
	if (l > 3 && !strcmp(out + l - 3, ".bl"))
		out[l - 3] = 0;
}

static void title_update(struct app *a)
{
	if (!a->win.c)
		return;
	char name[300], t[340];
	world_name(a->path, name, sizeof name);
	snprintf(t, sizeof t, "%s - Blocks", name);
	WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

/* The world and everything about the game into the world's file, and player.conf. False when it has no file, is not to be saved or cannot be written. */
static bool save_all(struct app *a, bool say_it)
{
	if (a->nosave || !a->path[0]) {
		if (say_it)
			say(a, "This world is not saved: Save as gives it a file");
		return false;
	}
	if (mkdirs(a->dir) != 0) {
		say(a, "Cannot make %s", a->dir);
		return false;
	}
	char *text = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&text, &len);
	if (!m)
		return false;
	conf_write(a, m);
	bool ok = fclose(m) == 0 && bl_save_with(a->world, a->path, text, len) == 0;
	free(text);
	if (!ok) {
		say(a, "Saving failed");
		return false;
	}
	a->saved_rev = a->world->rev;
	a->force_save = false;
	conf_save(a);
	a->last_save_ms = now_ms();
	if (say_it) {
		char name[300];
		world_name(a->path, name, sizeof name);
		say(a, "Saved %s", name);
	}
	return true;
}

/* The game's own sounds (src/gamesound.c, by family name): how soon one may come again, and whether it is a small one that gives way when something else has
 * only just sounded. */
static const struct sfx {
	const char *name;
	int gap_ms;
	bool soft;
} k_sfx[] = {
	{ "break-stone", 90, false }, { "break-wood", 90, false }, { "break-dirt", 90, false }, { "break-glass", 90, false }, { "break-leaves", 150, true },
	{ "dig-stone", 200, true }, { "dig-wood", 200, true }, { "dig-dirt", 200, true },
	{ "place-stone", 90, false }, { "place-wood", 90, false }, { "place-dirt", 90, false }, { "place-glass", 90, false }, { "splash", 200, false },
	{ "hit", 200, false }, { "goblin-die", 200, false }, { "goblin-near", 2500, true }, { "brute", 2500, false }, { "hurt", 300, false }, { "die", 1000, false },
	{ "arrow", 250, true }, { "clash", 300, true }, { "wall-hit", 450, true }, { "wall-break", 300, false },
	{ "horn", 1000, false }, { "alarm", 6000, false }, { "keep-fall", 1000, false }, { "dawn", 1000, false }, { "built", 500, false }, { "join", 200, false },
	{ "lost", 800, false }, { "order", 150, false }, { "no", 200, false }, { "hammer", 350, true },
};
_Static_assert(sizeof k_sfx / sizeof k_sfx[0] == N_SFX, "N_SFX is the number of sounds in k_sfx");

static int sfx_index(const char *name)
{
	for (int i = 0; i < N_SFX; i++)
		if (!strcmp(k_sfx[i].name, name))
			return i;
	return -1;
}

/* may this sound come now? (it is then counted as sounding) */
static bool sfx_due(struct app *a, const char *name, int64_t now)
{
	int i = sfx_index(name);
	if (i < 0 || (a->sfx_ms[i] && now - a->sfx_ms[i] < k_sfx[i].gap_ms) || (k_sfx[i].soft && a->last_sound_ms && now - a->last_sound_ms < 70))
		return false;
	a->sfx_ms[i] = a->last_sound_ms = now;
	return true;
}

/* The compositor plays them (write-only setting game-sound), so they follow Settings > Sound > Sound feedback and the chosen output. */
static void sound(struct app *a, const char *name)
{
	if (!a->sound_on || !a->settings || !a->win.c || !sfx_due(a, name, now_ms()))
		return;
	a->nsounds++;
	a->last_sfx = sfx_index(name);
	WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "game-sound" }, { .s = name });
	wlc_flush(a->win.c);
}

/* what a block sounds like */
static const char *material(int b)
{
	switch (b) {
	case BL_LOG: case BL_PLANKS: case BL_TORCH: case BL_GATE: case BL_TOWERPOST:
		return "wood";
	case BL_GRASS: case BL_DIRT: case BL_SAND: case BL_SNOW:
		return "dirt";
	case BL_GLASS: case BL_LAMP:
		return "glass";
	case BL_LEAVES:
		return "leaves";
	default:
		return "stone";
	}
}

/* "break", "dig" or "place" for a block of its material (there is no dig sound for glass and leaves, no place sound for leaves) */
static const char *block_sfx(const char *kind, int b, char buf[24])
{
	const char *m = material(b);
	if (kind[0] == 'd' && !strcmp(m, "glass"))
		m = "stone";
	if (kind[0] != 'b' && !strcmp(m, "leaves"))
		m = "dirt";
	if (kind[0] == 'p' && bl_water(b))
		return "splash";
	snprintf(buf, 24, "%s-%s", kind, m);
	return buf;
}

static void block_sound(struct app *a, const char *kind, int b)
{
	char buf[24];
	sound(a, block_sfx(kind, b, buf));
}

/* is (x, z) near enough to be heard? (the ears are the player, or the point the command camera looks at) */
static bool heard(const struct app *a, float x, float z, float range)
{
	float lx = a->cmd ? a->cam_f[0] : a->pl.x, lz = a->cmd ? a->cam_f[2] : a->pl.z;
	return hypotf(x - lx, z - lz) <= range;
}

#define HEAR 32.0f

/* now and then a goblin that is about makes itself heard */
static void goblin_voices(struct app *a, float dt)
{
	a->growl_t -= dt;
	if (a->growl_t > 0.0f)
		return;
	a->growl_t = 1.0f;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		if (m->alive && m->kind == BL_MOB_GOBLIN && heard(a, m->x, m->z, 18.0f)) {
			sound(a, m->brute ? "brute" : "goblin-near");
			a->growl_t = 3.0f + (float)(++a->growls * 7 % 11) * 0.4f;       /* 3 .. 7 s, not in step */
			break;
		}
	}
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
	a->chief_was = false;
	a->hp = MAX_HP;
	a->hurt_t = a->hit_t = 0.0f;
	a->regen_t = 0.0f;
	a->warned = false;
	a->plan = -1;
	a->siege_msg_t = 0.0f;
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

/* the height of the ground at (x, z): the top block that is not part of a tree */
static int ground_at(const struct bl_world *w, int x, int z)
{
	for (int y = w->h - 2; y > 0; y--) {
		int b = bl_get(w, x, y, z);
		if (b != BL_AIR && b != BL_LOG && b != BL_LEAVES && !bl_water(b) && b != BL_TORCH)
			return y;
	}
	return 0;
}

/* is there something a hand has made (or a keep) in this column from y0 to y1? */
static bool column_made(const struct bl_world *w, int x, int y0, int y1, int z)
{
	for (int y = y0; y <= y1; y++) {
		int b = bl_get(w, x, y, z);
		if (bl_siege_hp(b) > 0 || bl_own_keep(b) || bl_red_keep(b))
			return true;
	}
	return false;
}

/* The castle: what is in its way is cleared, what is missing under it is filled with its own stone (so it cannot be dug under), and its cells are set. */
static void keep_build(struct app *a, const int at[3], int block)
{
	struct bl_world *w = a->world;
	for (int dz = -BL_KEEP_R; dz <= BL_KEEP_R; dz++)
		for (int dx = -BL_KEEP_R; dx <= BL_KEEP_R; dx++) {
			for (int y = at[1]; y < w->h - 2 && y < at[1] + BL_KEEP_TALL + 2; y++)
				if (bl_get(w, at[0] + dx, y, at[2] + dz) != BL_AIR)
					bl_set(w, at[0] + dx, y, at[2] + dz, BL_AIR);
			for (int y = at[1] - 1; y > 1; y--) {
				int b = bl_get(w, at[0] + dx, y, at[2] + dz);
				if (b != BL_AIR && !bl_water(b) && b != BL_LEAVES && b != BL_LOG)
					break;
				bl_set(w, at[0] + dx, y, at[2] + dz, block);
			}
		}
	struct bl_cell c[BL_KEEP_CELLS];
	const int n = bl_keep_cells(c, at[0], at[1], at[2], block);
	for (int i = 0; i < n; i++)
		bl_set(w, c[i].x, c[i].y, c[i].z, c[i].block);
}

/* it falls: every block of it goes, rubble stays on its floor */
static void keep_remove(struct app *a, const int at[3], int block)
{
	struct bl_cell c[BL_KEEP_CELLS];
	const int n = bl_keep_cells(c, at[0], at[1], at[2], block);
	for (int i = 0; i < n; i++)
		if (bl_get(a->world, c[i].x, c[i].y, c[i].z) == c[i].block)
			bl_set(a->world, c[i].x, c[i].y, c[i].z, c[i].y == at[1] && (c[i].x * 7 + c[i].z * 3) % 4 == 0 ? BL_COBBLE : BL_AIR);
}

/* The keep's place: a square of open ground 9 to 40 blocks from (cx, cz), as level as there is, not where the player stands or appears; the nearest good one. */
static bool keep_place(struct app *a, float cx, float cz)
{
	struct bl_world *w = a->world;
	float spawn[3];
	bl_spawn(w, spawn);
	float best = 1e9f;
	int at[3] = { 0 };
	for (int r = 11; r <= 40 && best > (float)r; r++)
		for (int k = 0; k < 24; k++) {
			const float ang = (float)k * 6.2831853f / 24.0f;
			const int x = (int)floorf(cx + sinf(ang) * (float)r), z = (int)floorf(cz + cosf(ang) * (float)r);
			if (x < BL_KEEP_R + 3 || z < BL_KEEP_R + 3 || x >= w->w - BL_KEEP_R - 3 || z >= w->d - BL_KEEP_R - 3)
				continue;
			if (hypotf((float)x - a->pl.x, (float)z - a->pl.z) < 8.0f || hypotf((float)x - spawn[0], (float)z - spawn[2]) < 8.0f)
				continue;
			int lo = 1000, hi = 0, wet = 0, sum = 0;
			bool ok = true;
			for (int dz = -BL_KEEP_R; ok && dz <= BL_KEEP_R; dz++)
				for (int dx = -BL_KEEP_R; ok && dx <= BL_KEEP_R; dx++) {
					const int g = ground_at(w, x + dx, z + dz);
					wet += bl_water(bl_get(w, x + dx, g + 1, z + dz));
					lo = g < lo ? g : lo;
					hi = g > hi ? g : hi;
					sum += g;
					ok = !column_made(w, x + dx, g - 2, g + 6, z + dz);
				}
			const int cells = (2 * BL_KEEP_R + 1) * (2 * BL_KEEP_R + 1), level = (sum + cells / 2) / cells + 1;
			if (!ok || wet > cells / 5 || hi - lo > 9 || level < 3 || level + BL_KEEP_TALL + 2 >= w->h)
				continue;
			const float score = (float)r + (float)(hi - lo) * 6.0f + (float)wet * 4.0f + (1.0f - cosf(ang)) * 8.0f;     /* (ahead of him, if it can be) */
			if (score < best) {
				best = score;
				at[0] = x;
				at[1] = level;
				at[2] = z;
			}
		}
	if (best >= 1e9f)
		return false;
	keep_build(a, at, BL_KEEP);
	memcpy(a->keep, at, sizeof a->keep);
	a->have_keep = true;
	return true;
}

/* the enemies know where the keep is, and only in a game, while it stands */
static void keep_sync(struct app *a)
{
	bl_mobs_set_keep(&a->mobs, a->survival && a->have_keep && !a->keep_down, (float)a->keep[0] + 0.5f, (float)a->keep[1], (float)a->keep[2] + 0.5f);
}

/* In a game there is a keep: the one the world has (its blocks are in the world), else a new one near (cx, cz) with all its hit points. A keep of the old kind
 * (a tower of three blocks) is built anew as a castle there or nearby. */
static void ensure_keep(struct app *a, float cx, float cz)
{
	if (a->survival && !a->keep_down) {
		const bool there = a->have_keep && bl_get(a->world, a->keep[0], a->keep[1], a->keep[2]) == BL_KEEP;
		if (there && bl_get(a->world, a->keep[0] + BL_KEEP_R, a->keep[1], a->keep[2]) != BL_KEEP) {
			const float kx = (float)a->keep[0], kz = (float)a->keep[2], hp = a->keep_hp;
			for (int i = 0; i < 3; i++)
				if (bl_get(a->world, a->keep[0], a->keep[1] + i, a->keep[2]) == BL_KEEP)
					bl_set(a->world, a->keep[0], a->keep[1] + i, a->keep[2], BL_AIR);
			a->have_keep = keep_place(a, kx, kz);
			a->keep_hp = hp;
		} else if (!there) {
			a->have_keep = false;
			if (keep_place(a, cx, cz))
				a->keep_hp = KEEP_HP;
		}
	}
	keep_sync(a);
}

/* ---- gold: a few deposits, and no more than that ---- */

/* a deposit `rmin` to `rmax` blocks from (cx, cz): an outcrop of gold ore on dry ground, away from what has been made */
static bool gold_place(struct app *a, float cx, float cz, float rmin, float rmax)
{
	struct bl_world *w = a->world;
	if (a->ndeposits >= BL_DEPOSITS_MAX)
		return false;
	uint32_t k = w->seed * 2654435761u + (uint32_t)a->ndeposits * 40503u + 7u;
	for (int tries = 0; tries < 300; tries++) {
		k = k * 1664525u + 1013904223u;
		const float ang = (float)(k >> 8 & 1023) * 6.2831853f / 1024.0f, r = rmin + (rmax - rmin + (float)tries * 0.1f) * (float)(k >> 18 & 1023) / 1024.0f;
		const int x = (int)floorf(cx + sinf(ang) * r), z = (int)floorf(cz + cosf(ang) * r);
		if (x < 6 || z < 6 || x >= w->w - 6 || z >= w->d - 6 || hypotf((float)x - a->pl.x, (float)z - a->pl.z) < 8.0f)
			continue;
		const int g = ground_at(w, x, z);
		bool ok = g > 2 && g + 6 < w->h;
		for (int i = 0; ok && i < a->ndeposits; i++)
			ok = hypotf((float)(x - a->deposit[i][0]), (float)(z - a->deposit[i][2])) > 12.0f;
		for (int dz = -3; ok && dz <= 3; dz++)
			for (int dx = -3; ok && dx <= 3; dx++) {
				const int h = ground_at(w, x + dx, z + dz);
				ok = abs(h - g) <= 2 && !bl_water(bl_get(w, x + dx, h + 1, z + dz)) && !column_made(w, x + dx, h - 2, h + 4, z + dz);
			}
		if (!ok)
			continue;
		for (int dz = -2; dz <= 2; dz++)
			for (int dx = -2; dx <= 2; dx++) {
				const int d2 = dx * dx + dz * dz;
				if (d2 > 5)
					continue;
				const int h = ground_at(w, x + dx, z + dz);
				for (int y = h + 1; y <= h + 4; y++)
					if (bl_get(w, x + dx, y, z + dz) != BL_AIR)
						bl_set(w, x + dx, y, z + dz, BL_AIR);
				bl_set(w, x + dx, h, z + dz, d2 <= 2 ? BL_GOLD : BL_STONE);                     /* (nine pieces in the ground, three on top) */
				if ((dx == 0 && dz == 0) || (dx == 1 && dz == 0) || (dx == 0 && dz == -1))
					bl_set(w, x + dx, h + 1, z + dz, BL_GOLD);
			}
		a->deposit[a->ndeposits][0] = x;
		a->deposit[a->ndeposits][1] = g;
		a->deposit[a->ndeposits][2] = z;
		a->ndeposits++;
		return true;
	}
	return false;
}

/* a game has its gold: one deposit near each side, the rest between them (all of it far from nobody for long) */
static void gold_setup(struct app *a)
{
	if (!a->survival || a->ndeposits || !a->have_keep)
		return;
	const float kx = (float)a->keep[0], kz = (float)a->keep[2];
	gold_place(a, kx, kz, 15.0f, 24.0f);
	if (a->rival == 1) {
		const float rx = (float)a->rkeep[0], rz = (float)a->rkeep[2], dx = rx - kx, dz = rz - kz;
		gold_place(a, rx, rz, 19.0f, 28.0f);
		gold_place(a, kx + dx * 0.5f, kz + dz * 0.5f, 0.0f, 10.0f);
		gold_place(a, kx + dx * 0.4f - dz * 0.35f, kz + dz * 0.4f + dx * 0.35f, 0.0f, 12.0f);
		gold_place(a, kx + dx * 0.6f + dz * 0.35f, kz + dz * 0.6f - dx * 0.35f, 0.0f, 12.0f);
	}
	for (int i = 0; i < 12 && a->ndeposits < DEPOSITS; i++)
		gold_place(a, kx, kz, 30.0f, 60.0f);
}

/* every cell that holds `block`: how many, and the first `max` of them */
static int blocks_find(const struct bl_world *w, int block, int (*at)[3], int max)
{
	const size_t n = (size_t)w->w * (size_t)w->d * (size_t)w->h;
	int found = 0;
	for (const uint8_t *p = w->b; (p = memchr(p, block, n - (size_t)(p - w->b))) != NULL; p++) {
		const size_t i = (size_t)(p - w->b);
		if (found < max) {
			at[found][0] = (int)(i / (size_t)w->h % (size_t)w->w);
			at[found][1] = (int)(i % (size_t)w->h);
			at[found][2] = (int)(i / (size_t)w->h / (size_t)w->w);
		}
		found++;
	}
	return found;
}

/* what is left of a deposit */
static int ore_left(const struct app *a, int i)
{
	int n = 0;
	for (int y = a->deposit[i][1] - 4; y <= a->deposit[i][1] + 4; y++)
		for (int z = a->deposit[i][2] - 3; z <= a->deposit[i][2] + 3; z++)
			for (int x = a->deposit[i][0] - 3; x <= a->deposit[i][0] + 3; x++)
				n += bl_get(a->world, x, y, z) == BL_GOLD;
	return n;
}

/* the special buildings and the gold: looked at again */
static void survey(struct app *a)
{
	int at[SPECIAL_MAX][3];
	a->nsmithy = blocks_find(a->world, BL_SMITHY, at, SPECIAL_MAX);
	a->nmage = blocks_find(a->world, BL_MAGE, at, SPECIAL_MAX);
	a->ninfirm = LM_MIN(blocks_find(a->world, BL_INFIRMARY, a->infirm, SPECIAL_MAX), SPECIAL_MAX);
	a->nmine = LM_MIN(blocks_find(a->world, BL_MINE, a->mine, SPECIAL_MAX), SPECIAL_MAX);
	for (int i = 0; i < a->ndeposits; i++)
		a->ore[i] = ore_left(a, i);
	bl_mobs_set_deposits(&a->mobs, (const int (*)[3])a->deposit, a->ndeposits);
}

/* a mine takes the nearest piece of gold that is within its reach */
static bool mine_take(struct app *a, const int at[3])
{
	int best = 1 << 20, gx = 0, gy = 0, gz = 0;
	for (int dy = -7; dy <= 3; dy++)
		for (int dz = -MINE_RANGE; dz <= MINE_RANGE; dz++)
			for (int dx = -MINE_RANGE; dx <= MINE_RANGE; dx++) {
				const int d = dx * dx + dz * dz + dy * dy;
				if (d >= best || bl_get(a->world, at[0] + dx, at[1] + dy, at[2] + dz) != BL_GOLD)
					continue;
				best = d;
				gx = at[0] + dx;
				gy = at[1] + dy;
				gz = at[2] + dz;
			}
	return best < 1 << 20 && bl_set(a->world, gx, gy, gz, BL_AIR);
}

static void workers_start(struct app *a, int n);

/* a new world in survival: nothing in the hands, no nights behind him, a keep near where he starts */
static void survival_setup(struct app *a, float cx, float cz)
{
	memset(a->inv, 0, sizeof a->inv);
	a->nights = 0;
	a->in_night = false;
	a->dig_on = false;
	a->dig_t = 0.0f;
	a->have_keep = false;
	a->keep_down = false;
	a->keep_hp = KEEP_HP;
	a->ndeposits = 0;
	ensure_keep(a, cx, cz);
	if (a->have_keep)
		workers_start(a, 2);
}

/* ================================================================ workers and the command view ================================================================ */

#define WORKER_COST 8                                           /* planks */
#define CMD_PITCH (-0.95f)                                      /* the command camera looks down at about 54 degrees */

static int nworkers(const struct app *a)
{
	return bl_mobs_count_kind(&a->mobs, BL_MOB_WORKER);
}

/* a free place on the ground near (cx, cz): three rings of eight round it, none higher than `maxy` (so nobody appears on a roof) */
static bool worker_spot(const struct app *a, float cx, float cz, float r0, float maxy, float out[3])
{
	for (int ri = 0; ri < 3; ri++)
		for (int k = 0; k < 8; k++) {
			float ang = (float)(k + 4) * 0.785398f + 0.3f * (float)ri, r = r0 + 0.8f * (float)ri,      /* (first on the near side, by the keep's door) */ x = cx + sinf(ang) * r, z = cz + cosf(ang) * r;
			int top = bl_top(a->world, (int)floorf(x), (int)floorf(z));
			if (top < 1)
				continue;
			float y = (float)top + 1.0f;
			if (y > maxy || bl_box_hits(a->world, x, y, z, BL_GOBLIN_HW, BL_SOLDIER_H) || bl_water(bl_get(a->world, (int)floorf(x), (int)y, (int)floorf(z))))
				continue;
			bool crowded = false;
			for (int i = 0; i < BL_MOBS_MAX; i++) {
				const struct bl_mob *m = &a->mobs.mob[i];
				if (m->alive && fabsf(m->x - x) < 1.0f && fabsf(m->z - z) < 1.0f)
					crowded = true;
			}
			if (crowded)
				continue;
			out[0] = x;
			out[1] = y;
			out[2] = z;
			return true;
		}
	return false;
}

/* the middle of what the workers gather round: the keep, else the player */
static void worker_home(const struct app *a, float *cx, float *cz)
{
	*cx = a->have_keep ? (float)a->keep[0] + 0.5f : a->pl.x;
	*cz = a->have_keep ? (float)a->keep[2] + 0.5f : a->pl.z;
}

static bool worker_add(struct app *a)
{
	float cx, cz, p[3];
	worker_home(a, &cx, &cz);
	if (nworkers(a) >= BL_WORKER_MAX || !worker_spot(a, cx, cz, a->have_keep ? BL_KEEP_EDGE + 1.6f : 2.2f, a->have_keep ? (float)a->keep[1] + 2.5f : 1e9f, p))
		return false;
	return bl_mobs_add(&a->mobs, a->world, BL_MOB_WORKER, p[0], p[1], p[2]);
}

/* the workers a new survival world starts with (or a creative one is asked for): free */
static void workers_start(struct app *a, int n)
{
	for (int i = 0; i < n; i++)
		worker_add(a);
}

/* a saved world: the workers where they were; an older survival world with a keep but no `workers` line gets two */
static void workers_restore(struct app *a)
{
	if (a->workers_saved) {
		for (int i = 0; i < a->nworkers_conf; i++)
			bl_mobs_add(&a->mobs, a->world, BL_MOB_WORKER, a->worker_conf[i][0], a->worker_conf[i][1], a->worker_conf[i][2]);
	} else if (a->survival && a->have_keep) {
		workers_start(a, 2);
	}
}

/* more of them cost planks (a log is four, X makes them); at most eight */
static bool train_worker(struct app *a)
{
	if (nworkers(a) >= BL_WORKER_MAX) {
		say(a, "You cannot have more than %d workers", BL_WORKER_MAX);
		return false;
	}
	if (a->survival && a->inv[BL_PLANKS] < WORKER_COST) {
		sound(a, "no");
		say(a, "A worker costs %d planks (X turns a log into four)", WORKER_COST);
		return false;
	}
	if (!worker_add(a)) {
		say(a, "There is no room for a worker");
		return false;
	}
	if (a->survival)
		a->inv[BL_PLANKS] -= WORKER_COST;
	sound(a, "join");
	say(a, "A worker joins you (%d)", nworkers(a));
	return true;
}

/* ---- soldiers: trained at a barracks ---- */

#define SOLDIER_COBBLE 6                                        /* what a soldier costs ... */
#define SOLDIER_PLANKS 4                                        /* ... in survival */
#define SOLDIERS_PER_BARRACKS 4
#define BARRACKS_RANGE 40                                       /* a barracks counts when it is this near the keep (else: the hero) */

static int nsoldiers(const struct app *a)
{
	return bl_mobs_count_kind(&a->mobs, BL_MOB_SOLDIER);
}

/* the barracks round the keep: how many, and the first one's place (the block's middle at its foot) */
static int barracks_find(const struct app *a, float first[3])
{
	float cx = a->have_keep ? (float)a->keep[0] : a->pl.x, cy = a->have_keep ? (float)a->keep[1] : a->pl.y, cz = a->have_keep ? (float)a->keep[2] : a->pl.z;
	const struct bl_world *w = a->world;
	int x0 = (int)cx - BARRACKS_RANGE, x1 = (int)cx + BARRACKS_RANGE, z0 = (int)cz - BARRACKS_RANGE, z1 = (int)cz + BARRACKS_RANGE;
	int y0 = (int)cy - 14, y1 = (int)cy + 14, n = 0;
	x0 = x0 < 0 ? 0 : x0;
	z0 = z0 < 0 ? 0 : z0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 >= w->w ? w->w - 1 : x1;
	z1 = z1 >= w->d ? w->d - 1 : z1;
	y1 = y1 >= w->h ? w->h - 1 : y1;
	for (int z = z0; z <= z1; z++)
		for (int x = x0; x <= x1; x++) {
			if (w->hi[(size_t)z * (size_t)w->w + (size_t)x] < y0)
				continue;
			for (int y = y0; y <= y1; y++)
				if (bl_get(w, x, y, z) == BL_BARRACKS) {
					if (n == 0 && first) {
						first[0] = (float)x + 0.5f;
						first[1] = (float)y;
						first[2] = (float)z + 0.5f;
					}
					n++;
				}
		}
	return n;
}

/* a soldier: needs a barracks (four soldiers to each, eight in all), and in survival six cobble and four planks; he stands near the barracks and it is his post */
static bool train_soldier(struct app *a)
{
	float where[3] = { 0, 0, 0 };
	int nb = barracks_find(a, where);
	if (nb == 0) {
		sound(a, "no");
		say(a, "A barracks is needed: Tab, then 3, and a worker builds it near the keep");
		return false;
	}
	if (nsoldiers(a) >= BL_SOLDIER_MAX || nsoldiers(a) >= nb * SOLDIERS_PER_BARRACKS) {
		sound(a, "no");
		say(a, nsoldiers(a) >= BL_SOLDIER_MAX ? "You cannot have more than %d soldiers" : "Each barracks takes %d soldiers: another barracks is needed", nsoldiers(a) >= BL_SOLDIER_MAX ? BL_SOLDIER_MAX : SOLDIERS_PER_BARRACKS);
		return false;
	}
	if (a->survival && (a->inv[BL_COBBLE] < SOLDIER_COBBLE || a->inv[BL_PLANKS] < SOLDIER_PLANKS)) {
		sound(a, "no");
		say(a, "A soldier costs %d cobble and %d planks", SOLDIER_COBBLE, SOLDIER_PLANKS);
		return false;
	}
	float p[3];
	if (!worker_spot(a, where[0], where[2], 2.6f, where[1] + 0.5f, p) || !bl_mobs_add(&a->mobs, a->world, BL_MOB_SOLDIER, p[0], p[1], p[2])) {
		say(a, "There is no room for a soldier by the barracks");
		return false;
	}
	if (a->survival) {
		a->inv[BL_COBBLE] -= SOLDIER_COBBLE;
		a->inv[BL_PLANKS] -= SOLDIER_PLANKS;
	}
	sound(a, "join");
	say(a, "A soldier joins you (%d)", nsoldiers(a));
	return true;
}

/* a saved world: the soldiers where they stood (and at their posts; one that stood on a watchtower goes back onto it) */
static void soldiers_restore(struct app *a)
{
	for (int i = 0; a->soldiers_saved && i < a->nsoldiers_conf; i++) {
		const float *p = a->soldier_conf[i];
		if (!bl_mobs_add(&a->mobs, a->world, BL_MOB_SOLDIER, p[0], p[1], p[2]))
			continue;
		int x = (int)floorf(p[0]), y = (int)floorf(p[1] - 0.5f), z = (int)floorf(p[2]);
		if (bl_get(a->world, x, y, z) != BL_TOWERPOST)
			continue;
		for (int k = 0; k < BL_MOBS_MAX; k++) {
			const struct bl_mob *m = &a->mobs.mob[k];
			if (m->alive && m->kind == BL_MOB_SOLDIER && m->x == p[0] && m->y == p[1] && m->z == p[2])
				bl_mobs_order_tower(&a->mobs, a->world, k, x, y, z);
		}
	}
}

/* the buildings that were under way: back, and the workers that are free go on with them */
static void sites_restore(struct app *a)
{
	for (int i = 0; i < a->nsites_conf; i++) {
		bool inside = true;
		for (int k = 0; k < a->site_conf[i].n; k++)
			inside &= bl_inside(a->world, a->site_conf[i].c[k].x, a->site_conf[i].c[k].y, a->site_conf[i].c[k].z);
		int si = inside ? bl_mobs_add_site(&a->mobs, &a->site_conf[i]) : -1;
		for (int k = 0; si >= 0 && k < BL_MOBS_MAX; k++)
			if (a->mobs.mob[k].alive && a->mobs.mob[k].kind == BL_MOB_WORKER && a->mobs.mob[k].order == BL_ORDER_IDLE)
				bl_mobs_order_build(&a->mobs, a->world, k, si);
	}
}

/* ================================================================ the rival clan ================================================================ */

static void pause_game(struct app *a, enum prompt p);

#define CAMP_R 12                                               /* the camp is levelled this far from its keep; its wall runs CAMP_WALL from it */
#define CAMP_WALL 10

/* A place for their camp, 80 to 120 blocks (less in a small world) from the player's keep (else from him): as level and dry as there is, with nothing a hand has
 * made near it and nobody of the player's within 50. out = x, the height of the ground, z. */
static bool rival_site(const struct app *a, int out[3])
{
	const struct bl_world *w = a->world;
	float hx = a->have_keep ? (float)a->keep[0] : a->pl.x, hz = a->have_keep ? (float)a->keep[2] : a->pl.z;
	int size = w->w < w->d ? w->w : w->d;
	float dmin = fminf(80.0f, (float)size * 0.35f), best = 1e9f, apart = fminf(50.0f, dmin * 0.7f);
	for (float r = dmin; r <= dmin + 40.0f; r += 8.0f)
		for (int k = 0; k < 32; k++) {
			float ang = (float)k * 6.2831853f / 32.0f;
			int x = (int)floorf(hx + sinf(ang) * r), z = (int)floorf(hz + cosf(ang) * r);
			if (x < CAMP_R + 3 || z < CAMP_R + 3 || x >= w->w - CAMP_R - 3 || z >= w->d - CAMP_R - 3)
				continue;
			bool ok = hypotf((float)x - a->pl.x, (float)z - a->pl.z) >= apart;
			for (int i = 0; ok && i < BL_MOBS_MAX; i++)
				ok = !(a->mobs.mob[i].alive && bl_mob_is_unit(a->mobs.mob[i].kind) && hypotf((float)x - a->mobs.mob[i].x, (float)z - a->mobs.mob[i].z) < apart);
			int lo = 1000, hi = -1000, wet = 0, sum = 0, n = 0;
			for (int dz = -CAMP_R; ok && dz <= CAMP_R; dz++)
				for (int dx = -CAMP_R; ok && dx <= CAMP_R; dx++) {
					int g = ground_at(w, x + dx, z + dz);
					wet += bl_water(bl_get(w, x + dx, g + 1, z + dz));
					lo = g < lo ? g : lo;
					hi = g > hi ? g : hi;
					sum += g;
					n++;
					for (int y = g - 3; y <= g + 5 && ok; y++) {
						int b = bl_get(w, x + dx, y, z + dz);
						ok = !(bl_siege_hp(b) > 0 || bl_own_keep(b) || bl_red_keep(b));
					}
				}
			if (!ok || wet > n / 4)
				continue;
			float score = (float)(hi - lo) * 4.0f + (float)wet * 3.0f + (r - dmin) * 0.1f;
			if (score < best) {
				int level = (sum + n / 2) / n;
				if (w->kind == 0 && level < w->h * 22 / 64)             /* (never under the sea) */
					level = w->h * 22 / 64;
				if (level < 3 || level + 12 >= w->h)
					continue;
				best = score;
				out[0] = x;
				out[1] = level;
				out[2] = z;
			}
		}
	return best < 1e9f;
}

/* The camp itself: the ground levelled, a wall with a gateway on two sides, the keep in the middle and a watchtower in a corner. */
static void rival_build(struct app *a, int x, int level, int z)
{
	struct bl_world *w = a->world;
	for (int dz = -CAMP_R; dz <= CAMP_R; dz++)
		for (int dx = -CAMP_R; dx <= CAMP_R; dx++) {
			for (int y = w->h - 2; y > level; y--)
				if (bl_get(w, x + dx, y, z + dz) != BL_AIR)
					bl_set(w, x + dx, y, z + dz, BL_AIR);
			for (int y = level - 1; y > 1; y--) {
				int b = bl_get(w, x + dx, y, z + dz);
				if (b != BL_AIR && !bl_water(b) && b != BL_LEAVES && b != BL_LOG)
					break;
				bl_set(w, x + dx, y, z + dz, BL_DIRT);
			}
			bl_set(w, x + dx, level, z + dz, BL_GRASS);
		}
	float to_x = (a->have_keep ? (float)a->keep[0] : a->pl.x) - (float)x, to_z = (a->have_keep ? (float)a->keep[2] : a->pl.z) - (float)z;
	bool gate_x = fabsf(to_x) > fabsf(to_z);                    /* the gateways face the player, and away from him */
	for (int d = -CAMP_WALL; d <= CAMP_WALL; d++)
		for (int y = 1; y <= 2; y++) {
			bool open = d >= -1 && d <= 1;
			if (!(open && !gate_x)) {
				bl_set(w, x + d, level + y, z - CAMP_WALL, BL_COBBLE);
				bl_set(w, x + d, level + y, z + CAMP_WALL, BL_COBBLE);
			}
			if (!(open && gate_x)) {
				bl_set(w, x - CAMP_WALL, level + y, z + d, BL_COBBLE);
				bl_set(w, x + CAMP_WALL, level + y, z + d, BL_COBBLE);
			}
		}
	struct bl_site tower;
	const char *why = NULL;
	bl_plan_lay(w, BL_PLAN_TOWER, 0, x + k_camp[CAMP_TOWER].dx, level + 1, z + k_camp[CAMP_TOWER].dz, NULL, 0, &tower, &why);
	for (int i = 0; i < tower.n; i++)
		bl_set(w, tower.c[i].x, tower.c[i].y, tower.c[i].z, tower.c[i].block);
	a->rkeep[0] = x;
	a->rkeep[1] = level + 1;
	a->rkeep[2] = z;
	keep_build(a, a->rkeep, BL_REDKEEP);
}

/* a guard comes out at a free post of the camp; false when there is none */
static bool rival_guard(struct app *a)
{
	static const int k_post[RIVAL_GUARDS_MORE][2] = { { 6, 0 }, { -6, 0 }, { 0, 6 }, { 0, -6 }, { 5, 5 }, { -5, 5 }, { 5, -5 }, { 6, 3 } };
	for (int i = 0; i < (int)(sizeof k_post / sizeof k_post[0]); i++) {
		float x = (float)(a->rkeep[0] + k_post[i][0]) + 0.5f, y = (float)a->rkeep[1], z = (float)(a->rkeep[2] + k_post[i][1]) + 0.5f;
		bool taken = false;
		for (int k = 0; k < BL_MOBS_MAX && !taken; k++) {
			const struct bl_mob *m = &a->mobs.mob[k];
			taken = m->alive && m->kind == BL_MOB_GUARD && !m->raider && !m->posted && hypotf(m->tx - x, m->tz - z) < 0.5f;
		}
		if (!taken && bl_mobs_add(&a->mobs, a->world, BL_MOB_GUARD, x, y, z))
			return true;
	}
	return false;
}

/* an archer goes up on the tower `k` of their camp, if it stands */
static void camp_post(struct app *a, int k)
{
	for (int y = a->rkeep[1]; k != CAMP_HOUSE && y < a->rkeep[1] + 9; y++)
		if (bl_mobs_add_archer(&a->mobs, a->world, a->rkeep[0] + k_camp[k].dx, y, a->rkeep[2] + k_camp[k].dz))
			break;
}

/* their men at their places: `guards` on the ground and one on each tower of `archers` that stands */
static void rival_man(struct app *a, int guards, int archers)
{
	bl_mobs_clear_kind(&a->mobs, BL_MOB_GUARD);
	for (int i = 0; i < guards; i++)
		if (!rival_guard(a))
			break;
	for (int k = 0; k < CAMP_BUILDINGS; k++)
		if (archers & 1 << k)
			camp_post(a, k);
}

/* a worker of theirs comes out beside the keep */
static bool rival_worker(struct app *a)
{
	static const int k_at[4][2] = { { 5, 2 }, { -5, 2 }, { 5, -2 }, { -5, -2 } };
	const int n = bl_mobs_count_kind(&a->mobs, BL_MOB_RWORKER);
	for (int i = 0; i < 4; i++) {
		const int *p = k_at[(n + i) % 4];
		if (bl_mobs_add(&a->mobs, a->world, BL_MOB_RWORKER, (float)(a->rkeep[0] + p[0]) + 0.5f, (float)a->rkeep[1], (float)(a->rkeep[2] + p[1]) + 0.5f))
			return true;
	}
	return false;
}

static void rival_workers(struct app *a, int n)
{
	bl_mobs_clear_kind(&a->mobs, BL_MOB_RWORKER);
	for (int i = 0; i < n; i++)
		if (!rival_worker(a))
			break;
}

/* The longhouse stands outside the wall on a side that has no gateway: 5 wide, 4 deep, planks with cobble corners and a red brick roof, its door to the camp.
 * Of the two such sides it is the one where most of it is there already, else the first where nothing a hand has made is in the way; false when neither will do. */
static bool camp_house(const struct app *a, struct bl_site *s)
{
	static const int k_side[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
	const struct bl_world *w = a->world;
	const int x = a->rkeep[0], y = a->rkeep[1], z = a->rkeep[2], first = CAMP_WALL + 2;
	int best = -1;
	for (int k = 0; k < 4; k++) {
		const int sx = k_side[k][0], sz = k_side[k][1];
		if (bl_get(w, x + sx * CAMP_WALL, y, z + sz * CAMP_WALL) == BL_AIR)                /* a gateway */
			continue;
		struct bl_site t = { .on = true, .plan = BL_PLAN_BARRACKS };
		for (int h = 0; h < 4; h++)
			for (int v = 0; v < 4; v++)
				for (int u = -2; u <= 2; u++) {
					const bool side = u == -2 || u == 2 || v == 0 || v == 3, corner = (u == -2 || u == 2) && (v == 0 || v == 3);
					if (h < 3 && (!side || (u == 0 && v == 0 && h < 2)))                       /* (the inside, the door) */
						continue;
					int cx = x + sx * (first + v) + sz * u, cz = z + sz * (first + v) + sx * u;
					int b = h == 3 ? BL_BRICK : corner ? BL_COBBLE : BL_PLANKS;
					t.c[t.n++] = (struct bl_cell){ (int16_t)cx, (int16_t)(y + h), (int16_t)cz, (uint8_t)b };
				}
		int lo[3], hi[3], there = 0;
		bl_site_box(&t, lo, hi);
		if (lo[0] < 2 || lo[2] < 2 || hi[0] >= w->w - 2 || hi[2] >= w->d - 2 || hi[1] >= w->h - 2)
			continue;
		for (int i = 0; i < t.n; i++)
			there += bl_get(w, t.c[i].x, t.c[i].y, t.c[i].z) == t.c[i].block;
		bool clean = true;
		for (int cz = lo[2] - 1; cz <= hi[2] + 1 && clean && !there; cz++)
			for (int cx = lo[0] - 1; cx <= hi[0] + 1 && clean; cx++)
				for (int cy = y - 2; cy <= y + 4 && clean; cy++) {
					int b = bl_get(w, cx, cy, cz);
					clean = !(bl_siege_hp(b) > 0 || bl_own_keep(b));
				}
		if ((there || clean) && there > best) {
			best = there;
			*s = t;
		}
	}
	return best >= 0;
}

/* the cells of the building `k` of their camp; false when there is no place for it */
static bool camp_plan(const struct app *a, int k, struct bl_site *s)
{
	if (k == CAMP_HOUSE)
		return camp_house(a, s);
	const char *why = NULL;
	bl_plan_lay(a->world, BL_PLAN_TOWER, 0, a->rkeep[0] + k_camp[k].dx, a->rkeep[1], a->rkeep[2] + k_camp[k].dz, NULL, 0, s, &why);
	s->on = true;                                               /* (what is in the way of some of it does not stop them) */
	return s->n > 0;
}

/* is at least half of it there? */
static bool camp_standing(const struct app *a, int k)
{
	struct bl_site s;
	if (!camp_plan(a, k, &s))
		return false;
	int there = 0;
	for (int i = 0; i < s.n; i++)
		there += bl_get(a->world, s.c[i].x, s.c[i].y, s.c[i].z) == s.c[i].block;
	return there * 2 >= s.n;
}

/* the ground under a building outside the wall, and a block round it: level with the camp's (what a hand has made stays) */
static void camp_level(struct app *a, const struct bl_site *s)
{
	struct bl_world *w = a->world;
	const int level = a->rkeep[1] - 1;
	int lo[3], hi[3];
	bl_site_box(s, lo, hi);
	for (int z = lo[2] - 1; z <= hi[2] + 1; z++)
		for (int x = lo[0] - 1; x <= hi[0] + 1; x++) {
			for (int y = w->h - 2; y > level; y--) {
				int b = bl_get(w, x, y, z);
				if (b != BL_AIR && bl_siege_hp(b) == 0 && !bl_own_keep(b))
					bl_set(w, x, y, z, BL_AIR);
			}
			for (int y = level - 1; y > 1; y--) {
				int b = bl_get(w, x, y, z);
				if (b != BL_AIR && !bl_water(b) && b != BL_LEAVES && b != BL_LOG)
					break;
				bl_set(w, x, y, z, BL_DIRT);
			}
			if (bl_siege_hp(bl_get(w, x, level, z)) == 0)
				bl_set(w, x, level, z, BL_GRASS);
		}
}

/* their workers start on the building `k`; false when it cannot be built */
static bool camp_start(struct app *a, int k)
{
	struct bl_site s;
	if (!camp_plan(a, k, &s))
		return false;
	if (k == CAMP_HOUSE)
		camp_level(a, &s);
	if (bl_site_missing(a->world, &s) == 0)                     /* (its places are filled with something else) */
		return false;
	a->rsite = k;
	bl_mobs_set_rsite(&a->mobs, &s);
	return true;
}

/* a world that was saved (or a camp that was switched off) with a building under way: they go on with it */
static void camp_resume(struct app *a)
{
	struct bl_site s;
	if (a->rsite >= 0 && camp_plan(a, a->rsite, &s))
		bl_mobs_set_rsite(&a->mobs, &s);
	else
		a->rsite = -1;
}

/* it is whole */
static void camp_done(struct app *a)
{
	const int k = a->rsite;
	if (k < 0)
		return;
	a->rsite = -1;
	a->rbuilt |= 1 << k;
	if (k == CAMP_HOUSE) {
		say(a, "The %s has built a longhouse: more men", RIVAL_NAME);
	} else {
		camp_post(a, k);
		say(a, "The %s has built a watchtower", RIVAL_NAME);
	}
}

/* The camp itself: lost workers are replaced, buildings that have been pulled down are noticed, and when the store allows it the next one is started. */
static void camp_step(struct app *a, float dt)
{
	if (bl_mobs_count_kind(&a->mobs, BL_MOB_RWORKER) >= BL_RWORKER_MAX) {
		a->rworker_t = RIVAL_WORKER_EVERY;
	} else if ((a->rworker_t -= dt) <= 0.0f) {
		rival_worker(a);
		a->rworker_t = RIVAL_WORKER_EVERY;
	}
	a->rcheck_t -= dt;
	if (a->rcheck_t > 0.0f)
		return;
	a->rcheck_t = CAMP_CHECK;
	for (int k = 0; k < CAMP_BUILDINGS; k++)
		if ((a->rbuilt & 1 << k) && !camp_standing(a, k)) {
			a->rbuilt &= ~(1 << k);
			say(a, "A %s of the %s is down", k_camp[k].name, RIVAL_NAME);
		}
	for (int k = 0; a->rsite < 0 && k < CAMP_BUILDINGS; k++) {
		if (a->rbuilt & 1 << k)
			continue;
		if (a->rstock < k_camp[k].cost + RIVAL_SPARE)
			break;                                              /* (they save up for it) */
		if (camp_start(a, k))
			a->rstock -= k_camp[k].cost;
	}
}

static void rival_sync(struct app *a)
{
	bl_mobs_set_rival(&a->mobs, a->rival == 1 && a->rival_on, (float)a->rkeep[0] + 0.5f, (float)a->rkeep[1], (float)a->rkeep[2] + 0.5f);
	a->mobs.rkeep_down = a->rkeep_back > 0.0f;
	a->mobs.rgold_wanted = a->rgold < RIVAL_GOLD_MAX;
	a->mobs.chief_arms = a->chief_arms;
}

/* their chief comes out before the keep */
static bool chief_spawn(struct app *a, float hp)
{
	for (int k = 0; k < 4; k++) {
		const float x = (float)a->rkeep[0] + 0.5f + (float)(k - 1) * 1.5f, z = (float)a->rkeep[2] + 0.5f - 6.0f;
		if (!bl_mobs_add_chief(&a->mobs, a->world, x, (float)a->rkeep[1], z))
			continue;
		a->mobs.mob[bl_mobs_chief(&a->mobs)].hp = hp;
		a->chief_was = true;
		return true;
	}
	return false;
}

/* what he buys for the gold they have to spare (the price of a new keep is not touched) */
static void chief_shop(struct app *a)
{
	static const struct {
		int weapon, price;
	} k_buy[] = { { BL_WP_SPEAR, 3 }, { BL_WP_HAMMER, 6 }, { BL_WP_FIRE, 10 }, { BL_WP_LASER, 15 } };
	for (size_t i = 0; i < sizeof k_buy / sizeof k_buy[0]; i++) {
		if (a->chief_arms & 1u << k_buy[i].weapon)
			continue;
		if (a->rgold >= KEEP_GOLD + k_buy[i].price) {
			a->rgold -= k_buy[i].price;
			a->chief_arms |= 1u << k_buy[i].weapon;
			say(a, "The %s has got himself a %s", CHIEF_NAME, bl_weapon(k_buy[i].weapon)->name);
		}
		return;                                             /* (one at a time, in this order) */
	}
}

/* he falls, he is away, he is back */
static void chief_step(struct app *a, float dt)
{
	const bool there = bl_mobs_chief(&a->mobs) >= 0;
	if (a->chief_was && !there) {
		a->chief_t = CHIEF_BACK;
		say(a, "The %s has fallen! He is back in %.0f seconds if their keep stands", CHIEF_NAME, CHIEF_BACK);
		sound(a, "dawn");
	}
	a->chief_was = there;
	if (!there && a->rkeep_back <= 0.0f && (a->chief_t -= dt) <= 0.0f) {
		if (!chief_spawn(a, BL_CHIEF_HP))
			a->chief_t = 5.0f;
		else
			a->chief_t = 0.0f;
	}
}

/* which way (tx, tz) is from the player, as he faces, and how far */
static const char *bearing_to(const struct app *a, float tx, float tz, float *dist)
{
	float dx = tx - a->pl.x, dz = tz - a->pl.z, d = atan2f(dx, dz) - a->pl.yaw;
	while (d > (float)M_PI)
		d -= 2.0f * (float)M_PI;
	while (d < -(float)M_PI)
		d += 2.0f * (float)M_PI;
	*dist = hypotf(dx, dz);
	return fabsf(d) < 0.6f ? "ahead" : fabsf(d) > 2.5f ? "behind" : d > 0.0f ? "to the right" : "to the left";
}

/* which way their keep is, and how far */
static const char *rival_bearing(const struct app *a, float *dist)
{
	return bearing_to(a, (float)a->rkeep[0] + 0.5f, (float)a->rkeep[2] + 0.5f, dist);
}

static float raid_first(const struct app *a)
{
	return a->cli_raid > 0.0f ? a->cli_raid : a->survival ? RAID_FIRST : RAID_FIRST_CREATIVE;
}

/* where their raiders are bound: the player's keep, when there is none the player himself */
static void raid_target(const struct app *a, float at[3])
{
	at[0] = a->have_keep ? (float)a->keep[0] + 0.5f : a->pl.x;
	at[1] = a->have_keep ? (float)a->keep[1] : a->pl.y;
	at[2] = a->have_keep ? (float)a->keep[2] + 0.5f : a->pl.z;
}

/* A raiding party comes out of the gateway of the camp that faces where it is bound: RAID_SMALL men the first time, one more every time after. */
static bool raid_send(struct app *a)
{
	static const int k_side[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	float at[3];
	raid_target(a, at);
	float cx = (float)a->rkeep[0] + 0.5f, cz = (float)a->rkeep[2] + 0.5f;
	int side = 0;
	float best = -1e9f;
	for (int k = 0; k < 4; k++) {
		bool open = bl_get(a->world, a->rkeep[0] + k_side[k][0] * CAMP_WALL, a->rkeep[1], a->rkeep[2] + k_side[k][1] * CAMP_WALL) == BL_AIR;
		float score = (open ? 1e6f : 0.0f) + (at[0] - cx) * (float)k_side[k][0] + (at[2] - cz) * (float)k_side[k][1];
		if (score > best) {
			best = score;
			side = k;
		}
	}
	const float ox = (float)k_side[side][0], oz = (float)k_side[side][1];
	int want = LM_MIN(RAID_SMALL + a->raids, a->rbuilt & 1 << CAMP_HOUSE ? BL_RAIDER_MAX : RAID_NO_HOUSE), came = 0;
	want = LM_MIN(want, a->rstock / RIVAL_RAIDER_COST);
	if (want < RAID_SMALL)                                      /* (not enough in the store for a party) */
		return false;
	bl_mobs_set_raid(&a->mobs, at[0], at[1], at[2]);
	for (int i = 0; i < 36 && came < want; i++) {               /* three abreast, outside the wall */
		float along = (float)(CAMP_WALL + 2) + 1.5f * (float)(i / 3), across = 1.4f * (float)(i % 3 - 1);
		float x = cx + ox * along - oz * across, z = cz + oz * along + ox * across;
		if (x < 1.0f || z < 1.0f || x >= (float)a->world->w - 1.0f || z >= (float)a->world->d - 1.0f)
			continue;
		int g = ground_at(a->world, (int)floorf(x), (int)floorf(z));
		came += bl_mobs_add_raider(&a->mobs, a->world, x, (float)(g + 1), z);
	}
	if (!came)
		return false;
	a->rstock -= came * RIVAL_RAIDER_COST;
	a->raids++;
	a->raid_out = true;
	a->raid_here = a->raid_home = false;
	float dist;
	const char *way = rival_bearing(a, &dist);
	if (a->raids % 2 == 0 && bl_mobs_chief_raid(&a->mobs))          /* every second party has him at its head */
		say(a, "The %s leads %d raiders at you! Their camp is %s", CHIEF_NAME, came, way);
	else
		say(a, "The %s sends %d raiders at you! Their camp is %s", RIVAL_NAME, came, way);
	sound(a, "horn");
	return true;
}

/* The raids: by day the time to the next one runs; while one is out, its coming and its end are told. */
static void raid_step(struct app *a, float dt)
{
	if (a->rival != 1 || !a->rival_on)
		return;
	float at[3];
	raid_target(a, at);
	bl_mobs_set_raid(&a->mobs, at[0], at[1], at[2]);
	if (a->raid_out) {
		if (bl_mobs_count_raiders(&a->mobs) == 0) {
			a->raid_out = false;
			a->raid_t = RAID_PAUSE;
			say(a, a->raid_home ? "The raiders have gone home" : "The raiding party is beaten");
			return;
		}
		for (int i = 0; i < BL_MOBS_MAX && !a->raid_here; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			if (m->alive && m->raider && !m->retreat && hypotf(m->x - at[0], m->z - at[2]) < RAID_NEAR) {
				a->raid_here = true;
				say(a, "The raiders are here!");
				sound(a, "alarm");
			}
		}
	} else if (a->hp > 0.0f && bl_daylight(a->time_h) > 0.55f && a->rkeep_back <= 0.0f) {
		a->raid_t -= dt;
		if (a->raid_t <= 0.0f && !raid_send(a))
			a->raid_t = 30.0f;                                  /* (nothing to pay with, or no room at the gateway: later) */
	}
}

/* The Red Banner comes to this world: its camp is made and manned. */
static bool rival_start(struct app *a)
{
	int at[3] = { 0, 0, 0 };
	if (!rival_site(a, at)) {
		say(a, "There is no room for the camp of the %s", RIVAL_NAME);
		return false;
	}
	rival_build(a, at[0], at[1], at[2]);
	a->rival = 1;
	a->rival_on = true;
	a->rhp = RIVAL_HP;
	a->rguard_t = RIVAL_REINFORCE;
	a->rhit_t = 0.0f;
	a->raids = 0;
	a->raid_t = raid_first(a);
	a->raid_out = false;
	a->rstock = RIVAL_STOCK;
	a->rgold = RIVAL_GOLD;
	a->rkeep_back = 0.0f;
	a->chief_arms = 1u << BL_WP_SWORD | 1u << BL_WP_BOW;
	a->chief_t = 0.0f;
	a->rbuilt = 1 << CAMP_TOWER;
	a->rsite = -1;
	a->rworker_t = RIVAL_WORKER_EVERY;
	a->rcheck_t = CAMP_CHECK;
	bl_mobs_set_rsite(&a->mobs, NULL);
	rival_man(a, RIVAL_GUARDS, 1 << CAMP_TOWER);
	rival_workers(a, BL_RWORKER_MAX);
	a->rguards = RIVAL_GUARDS;
	a->rarchers = 1 << CAMP_TOWER;
	a->rhands = BL_RWORKER_MAX;
	rival_sync(a);
	chief_spawn(a, BL_CHIEF_HP);
	float dist;
	const char *way = rival_bearing(a, &dist);
	say(a, "The %s has made camp %.0f blocks away, %s", RIVAL_NAME, dist, way);
	sound(a, "horn");
	return true;
}

/* their keep has no hit points left. With the price of a new one they build it again; without it they are beaten and the game is won. */
static void rival_fall(struct app *a)
{
	keep_remove(a, a->rkeep, BL_REDKEEP);
	a->rhp = 0.0f;
	sound(a, "keep-fall");
	if (a->rgold >= KEEP_GOLD) {
		a->rgold -= KEEP_GOLD;
		a->rkeep_back = RIVAL_BACK;
		rival_sync(a);
		say(a, "Their keep is down, but they pay %d gold: it stands again in %.0f seconds (%d gold left)", KEEP_GOLD, RIVAL_BACK, a->rgold);
		return;
	}
	bl_mobs_clear_kind(&a->mobs, BL_MOB_GUARD);
	bl_mobs_clear_kind(&a->mobs, BL_MOB_RWORKER);
	bl_mobs_clear_kind(&a->mobs, BL_MOB_CHIEF);
	bl_mobs_set_rsite(&a->mobs, NULL);
	a->rsite = -1;
	a->rival = 2;
	a->rival_on = false;
	a->raid_out = false;
	a->chief_was = false;
	rival_sync(a);
	sound(a, "dawn");
	pause_game(a, P_WON);
	say(a, "The %s has fallen", RIVAL_NAME);
}

static void rival_hit(struct app *a, float damage)
{
	if (a->rival != 1 || !a->rival_on || a->rkeep_back > 0.0f)
		return;
	a->rhp -= damage;
	a->rhit_t = 6.0f;
	sound(a, "wall-hit");
	if (a->rhp <= 0.0f)
		rival_fall(a);
}

/* while the camp stands, its workers keep it supplied; from the store men come to it again, one at a time, raiding parties set out and it grows */
static void rival_step(struct app *a, float dt)
{
	a->rhit_t = fmaxf(0.0f, a->rhit_t - dt);
	raid_step(a, dt);
	if (a->rival != 1 || !a->rival_on)
		return;
	if (a->rkeep_back > 0.0f && (a->rkeep_back -= dt) <= 0.0f) {
		a->rkeep_back = 0.0f;
		keep_build(a, a->rkeep, BL_REDKEEP);
		a->rhp = RIVAL_HP;
		say(a, "The %s has built its keep again", RIVAL_NAME);
		sound(a, "horn");
	}
	rival_sync(a);
	chief_step(a, dt);
	if ((a->rcheck_t <= dt))
		chief_shop(a);
	camp_step(a, dt);
	if (rival_count(a, false) >= (a->rbuilt & 1 << CAMP_HOUSE ? RIVAL_GUARDS_MORE : RIVAL_GUARDS)) {
		a->rguard_t = RIVAL_REINFORCE;
		return;
	}
	a->rguard_t -= dt;
	if (a->rguard_t <= 0.0f && a->rstock >= RIVAL_GUARD_COST && rival_guard(a)) {
		a->rstock -= RIVAL_GUARD_COST;
		a->rguard_t = RIVAL_REINFORCE;
	}
}

/* what their workers did in a step: what they took from the land goes, what they brought in is in the store */
static void camp_take(struct app *a, const struct bl_mobs_result *R)
{
	bool said = false;
	for (int k = 0; k < R->nraze; k++) {
		const struct bl_break *c = &R->raze[k];
		int b = bl_get(a->world, c->x, c->y, c->z);
		if ((b == BL_LOG || b == BL_LEAVES || b == BL_STONE || b == BL_GOLD) && bl_set(a->world, c->x, c->y, c->z, BL_AIR) && b != BL_LEAVES && !said &&
		    heard(a, (float)c->x, (float)c->z, HEAR)) {
			block_sound(a, "break", b);
			said = true;
		}
	}
	a->rstock = LM_MIN(a->rstock + R->delivered, RIVAL_STOCK_MAX);
	a->rgold = LM_MIN(a->rgold + R->delivered_gold, RIVAL_GOLD_MAX);
}

/* what the workers have finished: the blocks go, what they leave goes into the pockets */
static void apply_breaks(struct app *a, const struct bl_mobs_result *R)
{
	for (int k = 0; k < R->nbreak; k++) {
		int b = bl_get(a->world, R->brk[k].x, R->brk[k].y, R->brk[k].z);
		if (!bl_breakable(b) || !bl_set(a->world, R->brk[k].x, R->brk[k].y, R->brk[k].z, BL_AIR))
			continue;
		if (heard(a, (float)R->brk[k].x, (float)R->brk[k].z, HEAR))
			block_sound(a, "break", b);
		int d = bl_drop(b);
		if (d != BL_AIR)
			inv_add(a, d, 1);
	}
}

/* what the builders put and the goblins broke goes into the world */
static void apply_build(struct app *a, const struct bl_mobs_result *R, float dt)
{
	a->siege_msg_t = fmaxf(0.0f, a->siege_msg_t - dt);
	for (int k = 0; k < R->nplace; k++)
		if (bl_cell_missing(a->world, &R->place[k]) && bl_set(a->world, R->place[k].x, R->place[k].y, R->place[k].z, R->place[k].block) &&
		    heard(a, (float)R->place[k].x, (float)R->place[k].z, HEAR))
			sound(a, "hammer");
	int smashed = 0;
	for (int k = 0; k < R->nsmash; k++) {
		const struct bl_break *c = &R->smash[k];
		if (bl_siege_hp(bl_get(a->world, c->x, c->y, c->z)) > 0)
			smashed += bl_set(a->world, c->x, c->y, c->z, BL_AIR);
	}
	if (smashed) {
		say(a, "Your wall has been broken through!");
		sound(a, "wall-break");
		a->siege_msg_t = 8.0f;
	} else if (R->siege_hits > 0 && a->siege_msg_t <= 0.0f) {
		say(a, "Your walls are being hammered");
		a->siege_msg_t = 12.0f;
	}
	if (R->siege_hits > 0 || R->keep_hits > 0)
		sound(a, "wall-hit");
	for (int k = 0; k < R->nbuilt; k++) {
		static const char *const k_done[BL_PLAN_COUNT] = { "The barracks is ready: V trains a soldier", "The watchtower is ready: right-click it with a soldier chosen", "A wall is finished", "A gate is finished" };
		if (R->built[k] >= 0 && R->built[k] < BL_PLAN_COUNT)
			say(a, "%s", k_done[R->built[k]]);
		sound(a, "built");
	}
}

/* ---- the command camera: it looks at a point of the ground from `cam_dist` away, down at an angle, turned by `cam_yaw` ---- */

static void cam_basis(const struct bl_camera *c, float f[3], float r[3], float u[3])
{
	float cp = cosf(c->pitch), sp = sinf(c->pitch), sy = sinf(c->yaw), cy = cosf(c->yaw);
	f[0] = cp * sy;
	f[1] = sp;
	f[2] = cp * cy;
	r[0] = cy;
	r[1] = 0.0f;
	r[2] = -sy;
	u[0] = -sp * sy;
	u[1] = cp;
	u[2] = -sp * cy;
}

static void cmd_camera(const struct app *a, struct bl_camera *cam)
{
	float cp = cosf(CMD_PITCH), sp = sinf(CMD_PITCH), sy = sinf(a->cam_yaw), cy = cosf(a->cam_yaw);
	cam->x = a->cam_f[0] - cp * sy * a->cam_dist;
	cam->y = a->cam_f[1] - sp * a->cam_dist;
	cam->z = a->cam_f[2] - cp * cy * a->cam_dist;
	if (cam->y > (float)a->world->h - 1.5f)
		cam->y = (float)a->world->h - 1.5f;
	cam->yaw = a->cam_yaw;
	cam->pitch = CMD_PITCH;
	cam->fov = a->fov;
}

/* the unit vector through a point of the window (the picture is the render buffer stretched over the window) */
static void cmd_ray(const struct app *a, const struct bl_camera *c, double px, double py, float dir[3])
{
	float f[3], r[3], u[3];
	cam_basis(c, f, r, u);
	float th = tanf(c->fov * 0.5f), aspect = a->rh > 0 ? (float)a->rw / (float)a->rh : (float)a->win.bw / (float)(a->win.bh > 0 ? a->win.bh : 1);
	float nx = (float)(px / (double)(a->win.bw > 0 ? a->win.bw : 1) * 2.0 - 1.0) * aspect * th, ny = (float)(1.0 - py / (double)(a->win.bh > 0 ? a->win.bh : 1) * 2.0) * th;
	float d[3] = { f[0] + r[0] * nx + u[0] * ny, f[1] + r[1] * nx + u[1] * ny, f[2] + r[2] * nx + u[2] * ny };
	float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	for (int i = 0; i < 3; i++)
		dir[i] = d[i] / l;
}

/* where a point of the world is in the window; false when it is behind the camera */
static bool cmd_project(const struct app *a, const struct bl_camera *c, float x, float y, float z, float *sx, float *sy)
{
	float f[3], r[3], u[3];
	cam_basis(c, f, r, u);
	float v[3] = { x - c->x, y - c->y, z - c->z };
	float zc = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
	if (zc < 0.2f)
		return false;
	float th = tanf(c->fov * 0.5f), aspect = a->rh > 0 ? (float)a->rw / (float)a->rh : (float)a->win.bw / (float)(a->win.bh > 0 ? a->win.bh : 1);
	float ndx = (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / (zc * aspect * th), ndy = (v[0] * u[0] + v[1] * u[1] + v[2] * u[2]) / (zc * th);
	*sx = (ndx + 1.0f) * 0.5f * (float)a->win.bw;
	*sy = (1.0f - ndy) * 0.5f * (float)a->win.bh;
	return true;
}

static int cmd_selected(const struct app *a)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += a->mobs.mob[i].alive && bl_mob_is_unit(a->mobs.mob[i].kind) && a->mobs.mob[i].selected;
	return n;
}

static void cmd_select_none(struct app *a)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		a->mobs.mob[i].selected = false;
}

/* a click: the worker under the pointer (not one behind a block), else nobody; Shift adds / takes away one */
static void cmd_select_click(struct app *a, bool add)
{
	struct bl_camera cam;
	cmd_camera(a, &cam);
	float dir[3], eye[3] = { cam.x, cam.y, cam.z }, tw = 0.0f;
	cmd_ray(a, &cam, a->px, a->py, dir);
	int wi = bl_units_pick(&a->mobs, eye, dir, 200.0f, &tw);
	struct bl_hit h;
	if (wi >= 0 && bl_pick(a->world, eye, dir, tw, &h))
		wi = -1;
	if (wi >= 0) {
		if (add) {
			a->mobs.mob[wi].selected = !a->mobs.mob[wi].selected;
		} else {
			cmd_select_none(a);
			a->mobs.mob[wi].selected = true;
		}
	} else if (!add) {
		cmd_select_none(a);
	}
}

/* a box pulled over the picture: the workers whose heads are inside it */
static void cmd_select_box(struct app *a, double x0, double y0, double x1, double y1, bool add)
{
	struct bl_camera cam;
	cmd_camera(a, &cam);
	double lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
	if (!add)
		cmd_select_none(a);
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		struct bl_mob *m = &a->mobs.mob[i];
		float sx, sy;
		if (m->alive && bl_mob_is_unit(m->kind) && cmd_project(a, &cam, m->x, m->y + 1.0f, m->z, &sx, &sy) && sx >= lx && sx <= hx && sy >= ly && sy <= hy)
			m->selected = true;
	}
}

/* ---- building: pick a plan (3 .. 6 or the bar), point at the ground, a worker builds it ---- */

#define KEY_PLAN0 (K_1 + 2)                                     /* the keys 3 4 5 6 7 8 9 0: barracks, watchtower, wall, gate, smithy, mage tower, infirmary, mine */

static const char *plan_title(int plan)
{
	static const char *const k[BL_PLAN_COUNT] = { "Barracks", "Watchtower", "Wall", "Gate", "Smithy", "Mage tower", "Infirmary", "Mine" };
	return plan >= 0 && plan < BL_PLAN_COUNT ? k[plan] : "?";
}

static bool plan_affordable(const struct app *a, int plan)
{
	int cob, pl;
	bl_plan_cost(plan, &cob, &pl);
	return !a->survival || (a->inv[BL_COBBLE] >= cob && a->inv[BL_PLANKS] >= pl && a->inv[BL_GOLD] >= bl_plan_gold(plan));
}

/* "12 cobble 8 planks": what a plan costs, in words; `brief`: "12c 8p" */
static void plan_cost_text(int plan, bool brief, char *out, size_t n)
{
	int cob, pl, gold = bl_plan_gold(plan);
	bl_plan_cost(plan, &cob, &pl);
	int at = snprintf(out, n, brief ? "%dc" : "%d cobble", cob);
	if (pl && at < (int)n)
		at += snprintf(out + at, n - (size_t)at, brief ? " %dp" : " %d planks", pl);
	if (gold && at < (int)n)
		snprintf(out + at, n - (size_t)at, brief ? " %dg" : " %d gold", gold);
}

/* The plan at the block under the pointer, as it would be built: false with *why when it cannot be. `*any` = there is something to show. */
static bool plan_at_pointer(const struct app *a, struct bl_site *s, const char **why, bool *any)
{
	struct bl_camera cam;
	cmd_camera(a, &cam);
	float dir[3], eye[3] = { cam.x, cam.y, cam.z };
	cmd_ray(a, &cam, a->px, a->py, dir);
	struct bl_hit h;
	*any = false;
	*why = "Point at the ground";
	if (!bl_pick(a->world, eye, dir, 200.0f, &h))
		return false;
	int x = h.face == BL_PY ? h.x : h.px, y = h.face == BL_PY ? h.y + 1 : h.py, z = h.face == BL_PY ? h.z : h.pz;
	int quarter = (((int)lroundf(a->cam_yaw / (float)(M_PI * 0.5)) + a->plan_rot) % 4 + 4) % 4;
	bool ok = bl_plan_lay(a->world, a->plan, quarter, x, y, z, a->mobs.site, BL_SITES_MAX, s, why);
	*any = s->n > 0;
	if (ok && bl_mobs_count_sites(&a->mobs) >= BL_SITES_MAX) {
		*why = "Too many buildings are under way: wait for one to be finished";
		ok = false;
	}
	if (ok && a->plan == BL_PLAN_BARRACKS) {
		float cx = a->have_keep ? (float)a->keep[0] : a->pl.x, cz = a->have_keep ? (float)a->keep[2] : a->pl.z, cy = a->have_keep ? (float)a->keep[1] : a->pl.y;
		if (fabsf((float)x - cx) > (float)(BARRACKS_RANGE - 3) || fabsf((float)z - cz) > (float)(BARRACKS_RANGE - 3) || fabsf((float)y - cy) > 11.0f) {
			*why = a->have_keep ? "Too far from the keep for a barracks" : "Too far from you for a barracks";
			ok = false;
		}
	}
	if (ok && a->plan == BL_PLAN_MINE) {
		bool gold = false;
		for (int dy = -5; dy <= 2 && !gold; dy++)
			for (int dz = -MINE_RANGE; dz <= MINE_RANGE && !gold; dz++)
				for (int dx = -MINE_RANGE; dx <= MINE_RANGE && !gold; dx++)
					gold = bl_get(a->world, x + dx, y + 3 + dy, z + dz) == BL_GOLD;
		if (!gold) {
			*why = "A mine needs gold beside it";
			ok = false;
		}
	}
	if (ok && !plan_affordable(a, a->plan)) {
		static char msg[120];
		char cost[64];
		plan_cost_text(a->plan, false, cost, sizeof cost);
		snprintf(msg, sizeof msg, "The %s costs %s", bl_plan_name(a->plan), cost);
		*why = msg;
		ok = false;
	}
	return ok;
}

/* the left button with a plan in hand: pay, lay the site, send the builders (the chosen workers, else every idle one) */
static bool build_place(struct app *a)
{
	struct bl_site s;
	const char *why = NULL;
	bool any;
	if (a->plan < 0)
		return false;
	if (!plan_at_pointer(a, &s, &why, &any)) {
		sound(a, "no");
		say(a, "%s", why ? why : "It cannot be built there");
		return false;
	}
	int si = bl_mobs_add_site(&a->mobs, &s);
	if (si < 0)
		return false;
	if (a->survival) {
		int cob, pl;
		bl_plan_cost(a->plan, &cob, &pl);
		a->inv[BL_COBBLE] -= cob;
		a->inv[BL_PLANKS] -= pl;
		a->inv[BL_GOLD] -= bl_plan_gold(a->plan);
	}
	bool chosen = false;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		chosen |= a->mobs.mob[i].alive && a->mobs.mob[i].kind == BL_MOB_WORKER && a->mobs.mob[i].selected;
	int sent = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		if (m->alive && m->kind == BL_MOB_WORKER && (chosen ? m->selected : m->order == BL_ORDER_IDLE))
			sent += bl_mobs_order_build(&a->mobs, a->world, i, si);
	}
	sound(a, "order");
	if (sent)
		say(a, "%s: %d worker%s to build it", plan_title(a->plan), sent, sent == 1 ? "" : "s");
	else
		say(a, "%s planned. No worker is free: choose one and right-click the plan", plan_title(a->plan));
	if (a->plan != BL_PLAN_WALL && a->plan != BL_PLAN_GATE)     /* (walls and gates come in rows: the plan stays in hand) */
		a->plan = -1;
	return true;
}

/* the building under way that the pointer is on (a planned block, or one of its blocks that is there already), or -1 */
static int site_at_pointer(const struct app *a)
{
	struct bl_camera cam;
	cmd_camera(a, &cam);
	float dir[3], eye[3] = { cam.x, cam.y, cam.z };
	cmd_ray(a, &cam, a->px, a->py, dir);
	struct bl_hit h;
	float best = bl_pick(a->world, eye, dir, 200.0f, &h) ? h.t + 0.1f : 200.0f;
	int found = -1;
	for (int i = 0; i < BL_SITES_MAX; i++) {
		if (!a->mobs.site[i].on)
			continue;
		static struct bl_ghosts g;
		float rgb[3], al, t;
		bl_ghosts_clear(&g);
		bl_ghosts_add(&g, NULL, &a->mobs.site[i], 0);
		if (bl_ghosts_ray(&g, eye, dir, best, rgb, &al, &t)) {
			best = t;
			found = i;
		}
	}
	return found;
}

/* Delete: the building under the pointer is given up; the part of its price that is not built yet comes back */
static bool build_cancel(struct app *a)
{
	int si = site_at_pointer(a);
	if (si < 0) {
		say(a, "Point at a planned building to take it away");
		return false;
	}
	const struct bl_site *s = &a->mobs.site[si];
	int plan = s->plan, left = bl_site_missing(a->world, s), cob, pl;
	bl_plan_cost(plan, &cob, &pl);
	cob = cob * left / s->n;
	pl = pl * left / s->n;
	bl_mobs_remove_site(&a->mobs, si);
	if (a->survival) {
		inv_add(a, BL_COBBLE, cob);
		inv_add(a, BL_PLANKS, pl);
		say(a, "The %s is given up: %d cobble and %d planks come back", bl_plan_name(plan), cob, pl);
	} else {
		say(a, "The %s is given up", bl_plan_name(plan));
	}
	return true;
}

/* the watchpost of the tower that the block is a part of (or stands under), if any */
static bool tower_at(const struct app *a, const struct bl_hit *h, int post[3])
{
	for (int y = h->y - 1; y <= h->y + 6; y++)
		for (int z = h->z - 1; z <= h->z + 1; z++)
			for (int x = h->x - 1; x <= h->x + 1; x++)
				if (bl_get(a->world, x, y, z) == BL_TOWERPOST) {
					post[0] = x;
					post[1] = y;
					post[2] = z;
					return true;
				}
	return false;
}

/* the bar of the plans at the bottom of the command view (it takes the hotbar's place) */
static void bar_rect(const struct app *a, int i, int *x, int *y, int *bw, int *bh)
{
	*bw = LM_CLAMP((a->win.bw - 16) / BL_PLAN_COUNT - 4, 40, 150);
	*bh = 42;
	*x = (a->win.bw - (BL_PLAN_COUNT * (*bw + 4) - 4)) / 2 + i * (*bw + 4);
	*y = a->win.bh - *bh - 12;
}

static int bar_at(const struct app *a, double px, double py)
{
	for (int i = 0; i < BL_PLAN_COUNT; i++) {
		int x, y, bw, bh;
		bar_rect(a, i, &x, &y, &bw, &bh);
		if (px >= x && px < x + bw && py >= y && py < y + bh)
			return i;
	}
	return -1;
}

static void choose_plan(struct app *a, int plan)
{
	a->plan = a->plan == plan ? -1 : plan;
	a->drag_sel = false;
	if (a->plan >= 0)
		say(a, "%s: left click builds it, R turns it, right click puts it aside", plan_title(plan));
}

/* the right button: the chosen workers go there, or break the block under the pointer and then the next of its kind */
static void cmd_order(struct app *a)
{
	int n = cmd_selected(a);
	if (n == 0) {
		say(a, "Choose workers or soldiers first: the left button");
		return;
	}
	if (a->order_mode == 0) {                                   /* a planned building: the chosen workers build it */
		int si = site_at_pointer(a), sent = 0, workers = 0;
		for (int i = 0; si >= 0 && i < BL_MOBS_MAX; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			if (m->alive && m->kind == BL_MOB_WORKER && m->selected) {
				workers++;
				sent += bl_mobs_order_build(&a->mobs, a->world, i, si);
			}
		}
		if (si >= 0 && workers) {
			say(a, sent ? "%d worker%s: build the %s" : "They cannot reach it", sent, sent == 1 ? "" : "s", bl_plan_name(a->mobs.site[si].plan));
			sound(a, sent ? "order" : "no");
			return;
		}
	}
	struct bl_camera cam;
	cmd_camera(a, &cam);
	float dir[3], eye[3] = { cam.x, cam.y, cam.z };
	cmd_ray(a, &cam, a->px, a->py, dir);
	struct bl_hit h;
	if (!bl_pick(a->world, eye, dir, 200.0f, &h)) {
		say(a, "Nothing there");
		return;
	}
	int ok = 0, k = 0, soldiers = 0, manned = -1, post[3], going = BL_AIR;
	if (a->order_mode == 0 && tower_at(a, &h, post))            /* a watchtower: one chosen soldier goes up (one that is not on it already) */
		for (int i = 0; i < BL_MOBS_MAX && manned < 0; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			if (m->alive && m->kind == BL_MOB_SOLDIER && m->selected && bl_mobs_order_tower(&a->mobs, a->world, i, post[0], post[1], post[2]))
				manned = i;
		}
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		struct bl_mob *m = &a->mobs.mob[i];
		if (!m->alive || !bl_mob_is_unit(m->kind) || !m->selected || i == manned)
			continue;
		if (a->order_mode == 1 && m->kind == BL_MOB_WORKER) {
			if (bl_mobs_order_gather(&a->mobs, a->world, i, h.x, h.y, h.z)) {
				ok++;
				going = m->gblock;                              /* (a click on a tree's leaves means its logs) */
			}
		} else {
			soldiers += m->kind == BL_MOB_SOLDIER;
			float gx = (h.face == BL_PY ? (float)h.x : (float)h.px) + 0.5f, gz = (h.face == BL_PY ? (float)h.z : (float)h.pz) + 0.5f;
			bl_mobs_order_move(&a->mobs, i, gx + (float)(k % 3 - 1) * 0.9f, gz + (float)(k / 3) * 0.9f);        /* (spread out, so they do not stand in each other) */
			ok++;
		}
		k++;
	}
	if (manned >= 0) {
		say(a, ok ? "A soldier goes up the watchtower, %d unit%s there" : "A soldier goes up the watchtower", ok, ok == 1 ? " goes" : "s go");
		ok++;
	} else if (a->order_mode == 1 && ok > soldiers)
		say(a, "%d unit%s: break %s%s", ok - soldiers, ok - soldiers == 1 ? "" : "s", bl_block_name(going != BL_AIR ? going : bl_get(a->world, h.x, h.y, h.z)), soldiers ? " (the soldiers go and stand guard there)" : "");
	else if (a->order_mode == 1 && ok == 0)
		say(a, "They cannot reach that");
	else
		say(a, "%d unit%s: %s there", ok, ok == 1 ? "" : "s", soldiers == ok ? "guard" : "go");
	sound(a, ok ? "order" : "no");
}

static void enter_cmd(struct app *a)
{
	if (a->cmd)
		return;
	a->cmd = true;
	a->k_fwd = a->k_back = a->k_left = a->k_right = a->k_jump = a->k_sneak = a->k_ctrl = a->k_lk = a->k_rk = a->k_uk = a->k_dk = false;
	a->b_break = a->b_place = false;
	a->dig_on = false;
	a->dig_t = 0.0f;
	a->cam_f[0] = a->pl.x;
	a->cam_f[1] = (float)bl_top(a->world, (int)floorf(a->pl.x), (int)floorf(a->pl.z)) + 1.0f;
	a->cam_f[2] = a->pl.z;
	a->cam_yaw = a->pl.yaw;
	a->plan = -1;
	if (a->cam_dist < 7.0f)
		a->cam_dist = 16.0f;
	wlwin_lock_pointer(&a->win, false);
	wlwin_hide_cursor(&a->win, false);
	a->win.redraw = true;
}

static void leave_cmd(struct app *a)
{
	if (!a->cmd)
		return;
	a->cmd = false;
	a->drag_sel = false;
	a->hover = false;
	a->k_fwd = a->k_back = a->k_left = a->k_right = a->k_lk = a->k_rk = false;
	a->lock_ok = wlwin_lock_pointer(&a->win, true);
	wlwin_hide_cursor(&a->win, true);
	a->win.redraw = true;
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

/* a file name that is not taken yet, in the worlds' folder: world.bl, world-2.bl ... */
static bool free_world_path(const struct app *a, char *out, size_t n)
{
	for (int i = 1; i < 1000; i++) {
		if (i == 1)
			snprintf(out, n, "%.500s/world.bl", a->dir);
		else
			snprintf(out, n, "%.500s/world-%d.bl", a->dir, i);
		if (access(out, F_OK) != 0 && errno == ENOENT)
			return true;
	}
	out[0] = 0;
	return false;
}

/* A new world beside the one that was open: that one is saved first and stays in its file. */
static void state_reset(struct app *a);
static void world_begin(struct app *a, bool from_file, int mode, int workers, int soldiers);

/* the file of a new game called `name` in the worlds' folder; false with *why when the name will not do */
static bool game_path(const struct app *a, const char *name, char *out, size_t n, const char **why)
{
	while (*name == ' ')
		name++;
	size_t l = strlen(name);
	while (l && name[l - 1] == ' ')
		l--;
	if (!l) {
		*why = "Give the game a name";
		return false;
	}
	if (l > 60 || name[0] == '.' || memchr(name, '/', l)) {
		*why = "A name has no / in it, does not begin with a dot and is at most 60 letters";
		return false;
	}
	snprintf(out, n, "%.500s/%.*s.bl", a->dir, (int)l, name);
	if (access(out, F_OK) == 0) {
		*why = "There is a game of that name already";
		return false;
	}
	return true;
}

/* A new world beside the one that was open: that one is saved first and stays in its file. `path`: the new one's file (NULL: the first free world-N.bl);
 * `mode`: 1 a game, 0 free building. */
static void new_world(struct app *a, const char *path, int mode)
{
	char file[1024] = "";
	if (path)
		snprintf(file, sizeof file, "%s", path);
	else
		free_world_path(a, file, sizeof file);
	if (a->path[0] && !a->nosave)
		save_all(a, false);
	if (!make_world(a, a->test_seed ? a->test_seed : fresh_seed(), false)) {
		say(a, "Cannot make a world");
		return;
	}
	state_reset(a);
	snprintf(a->path, sizeof a->path, "%s", file);
	a->nosave = a->cli_no_save;
	world_begin(a, false, mode, -1, -1);
	a->force_save = true;
	save_all(a, false);                                     /* (it is in the list of worlds at once) */
	title_update(a);
	char name[300];
	world_name(a->path, name, sizeof name);
	say(a, "A new %s: %s", a->survival ? "game" : "world to build in", name);
}

/* what belongs to one world, as it is before the world's own lines are read */
static void state_reset(struct app *a)
{
	a->survival = true;
	a->conf_read = a->conf_mode = false;
	a->nlog = 0;
	memset(a->inv, 0, sizeof a->inv);
	a->dig_on = false;
	a->dig_t = 0.0f;
	a->have_keep = false;
	a->keep_down = false;
	a->keep_hp = KEEP_HP;
	a->keep_msg_t = 0.0f;
	a->keep_back = a->rkeep_back = 0.0f;
	a->ndeposits = a->nsmithy = a->nmage = a->ninfirm = a->nmine = 0;
	a->look_t = a->mine_t = 0.0f;
	a->arms = 1u << BL_WP_SWORD;
	a->weapon = BL_WP_SWORD;
	memset(a->charges, 0, sizeof a->charges);
	a->rgold = RIVAL_GOLD;                                  /* (a camp saved before it had gold: as a new one) */
	a->chief_arms = 1u << BL_WP_SWORD | 1u << BL_WP_BOW;
	a->chief_t = a->chief_hp = 0.0f;
	a->chief_saved = a->chief_was = false;
	a->cmd = false;
	a->plan = -1;
	a->nights = a->kills = 0;
	a->rival = 0;
	a->rival_on = false;
	a->rhp = a->rhit_t = 0.0f;
	a->rguards = 0;
	a->rarchers = 0;
	a->rstock = RIVAL_STOCK;                                /* (a camp saved before it had a store: as a new one) */
	a->rbuilt = 1 << CAMP_TOWER;
	a->rsite = -1;
	a->rhands = BL_RWORKER_MAX;
	a->rworker_t = RIVAL_WORKER_EVERY;
	a->rcheck_t = CAMP_CHECK;
	a->rguard_t = RIVAL_REINFORCE;
	a->raids = a->nraiders_conf = 0;
	a->raid_t = -1.0f;                                      /* (not said: world_begin, which knows the mode) */
	a->raid_out = a->raid_here = a->raid_home = false;
	a->in_night = a->brute_warned = a->wave_msg_done = false;
	a->cmd = a->drag_sel = false;
	a->order_mode = 0;
	a->nsites_conf = a->nworkers_conf = a->nsoldiers_conf = 0;
	a->workers_saved = a->soldiers_saved = false;
	a->have_yard = a->have_tom = a->have_filip = false;
	a->yard_x = a->yard_z = 0.0f;
	memset(&a->pets, 0, sizeof a->pets);
	a->pet_t = 0.0f;
	memcpy(a->hotbar, k_default_hotbar, sizeof a->hotbar);
	a->slot = 0;
	a->time_h = 9.0f;
	a->clockm = 1;
	a->real_ms = 0;
	a->pets_on = true;
	a->water_acc = 0.0f;
	a->walk_phase = a->walk_stride = a->arm_swing = 0.0f;
	a->next_sword = 0;
	a->growl_t = 0.0f;
	a->aimed = false;
	a->hist_ok = false;
}

/* The world that was just made or read gets going: the player stands, the pets and the goblins are set up, and a world from a file gets back its keep, its
 * workers and soldiers (a new one gets those of a new world). `mode`: 1 survival, 0 creative, -1 what the world says. */
static void world_begin(struct app *a, bool from_file, int mode, int workers, int soldiers)
{
	if (a->cli_clock >= 0)
		a->clockm = a->cli_clock;
	if (a->cli_no_pets)
		a->pets_on = false;
	if (from_file && a->conf_read && !a->conf_mode && mode < 0)
		a->survival = false;                                    /* a world saved before survival was a creative world */
	if (mode >= 0)
		a->survival = mode != 0;
	a->mobs_on = (a->survival || a->cli_goblins) && !a->cli_no_mobs;     /* a game has its goblins, free building has nobody */
	settle_player(a);
	if (a->pets_on) {                                       /* (without them no yard is looked for and no pond is dug: the world is left as it is) */
		float near[3];
		bl_spawn(a->world, near);
		pets_setup(a, near, true);
	}
	mobs_setup(a);
	if (!a->survival)
		a->arms = (1u << BL_WP_COUNT) - 1;                      /* (nothing costs anything there) */
	if (from_file) {
		ensure_keep(a, a->pl.x, a->pl.z);
		workers_restore(a);
		soldiers_restore(a);
	} else {
		survival_setup(a, a->pl.x, a->pl.z);
	}
	if (workers >= 0) {                                     /* --workers N: exactly that many, whatever the world had */
		bl_mobs_clear_kind(&a->mobs, BL_MOB_WORKER);
		workers_start(a, workers);
	}
	if (soldiers >= 0) {                                    /* --soldiers N: that many near the keep (no barracks needed: for the tests) */
		bl_mobs_clear_kind(&a->mobs, BL_MOB_SOLDIER);
		for (int i = 0; i < soldiers; i++) {
			float cx, cz, sp[3];
			worker_home(a, &cx, &cz);
			if (worker_spot(a, cx, cz, a->have_keep ? BL_KEEP_EDGE + 2.4f : 3.0f, a->have_keep ? (float)a->keep[1] + 2.5f : 1e9f, sp))
				bl_mobs_add(&a->mobs, a->world, BL_MOB_SOLDIER, sp[0], sp[1], sp[2]);
		}
	}
	a->rival_on = a->rival == 1 && (a->survival || a->cli_rival);       /* every game has them: they cannot be switched off */
	if (from_file) {
		sites_restore(a);
		if (a->rival_on) {
			if (a->rkeep_back <= 0.0f && bl_get(a->world, a->rkeep[0] + BL_KEEP_R, a->rkeep[1], a->rkeep[2]) != BL_REDKEEP &&
			    bl_get(a->world, a->rkeep[0], a->rkeep[1], a->rkeep[2]) == BL_REDKEEP) {     /* a camp of the old, small kind: made anew where it is */
				rival_build(a, a->rkeep[0], a->rkeep[1] - 1, a->rkeep[2]);
				a->rbuilt = 1 << CAMP_TOWER;
				a->rsite = -1;
				a->rarchers = 1 << CAMP_TOWER;
				a->nraiders_conf = 0;
			}
			rival_sync(a);
			rival_man(a, a->rguards, a->rarchers);
			rival_workers(a, a->rhands);
			camp_resume(a);
			for (int i = 0; i < a->nraiders_conf; i++)          /* the party that was out */
				a->raid_out |= bl_mobs_add_raider(&a->mobs, a->world, a->raider_conf[i][0], a->raider_conf[i][1], a->raider_conf[i][2]);
			if (a->chief_saved && bl_mobs_add_chief(&a->mobs, a->world, a->chief_at[0], a->chief_at[1], a->chief_at[2])) {
				a->mobs.mob[bl_mobs_chief(&a->mobs)].hp = a->chief_hp;
				a->chief_was = true;
			}
		}
		if ((a->cli_raid > 0.0f && !a->raid_out) || a->raid_t < 0.0f)
			a->raid_t = raid_first(a);
	}
	if (a->survival && a->rival == 0 && !a->cli_no_rival)
		rival_start(a);                                         /* a game has them from the start */
	gold_setup(a);
	rival_sync(a);
	survey(a);
}

/* Reads a world's file and makes it the one that is played; false = it cannot be read (the world that is open stays, *err says why). A world.bl in the worlds'
 * folder from before the worlds carried their own state takes it from player.conf. */
static bool world_load(struct app *a, const char *path, const char **err, int mode, int workers, int soldiers)
{
	char *note = NULL, deflt[600], keep_path[1024];
	snprintf(keep_path, sizeof keep_path, "%s", path);              /* (`path` may be one of the app's own strings) */
	struct bl_world *w = bl_load_with(keep_path, &note, err);
	if (!w)
		return false;
	state_reset(a);
	bl_world_free(a->world);
	a->world = w;
	snprintf(a->path, sizeof a->path, "%s", keep_path);
	a->nosave = a->cli_no_save;
	a->saved_rev = w->rev;
	a->force_save = false;
	bl_player_init(&a->pl, w);
	world_path(a, deflt, sizeof deflt);
	if (note) {
		FILE *f = fmemopen(note, strlen(note), "r");
		if (f) {
			conf_read(a, f, true, false);
			fclose(f);
		}
	} else if (!strcmp(a->path, deflt)) {
		conf_load(a, true);
	}
	free(note);
	world_begin(a, true, mode, workers, soldiers);
	return true;
}

/* the folder the open / save as card starts in: the world's own, else the worlds' folder */
static void card_base(const struct app *a, char *out, size_t n)
{
	snprintf(out, n, "%s", a->path[0] ? a->path : "");
	char *sl = strrchr(out, '/');
	if (sl && sl != out)
		*sl = 0;
	else
		snprintf(out, n, "%s", a->dir);
}

/* What the card's field names, as a path: "~/x" is in the home folder, a name alone is in the card's folder; ".bl" is added when saving (and when opening, if
 * only that file is there). False with *why when it names no file. */
static bool field_path(const struct app *a, bool saving, char *out, size_t n, const char **why)
{
	char base[1024];
	card_base(a, base, sizeof base);
	size_t l = strlen(a->field);
	if (l == 0) {
		*why = saving ? "Type a name for the world" : "Choose a world";
		return false;
	}
	lm_resolve_path(base, a->field, out, n - 4);
	struct stat st;
	if (a->field[l - 1] == '/' || (stat(out, &st) == 0 && S_ISDIR(st.st_mode))) {
		*why = saving ? "That is a folder: type a name after it" : "That is a folder: choose a world in it";
		return false;
	}
	size_t ol = strlen(out);
	bool bl = ol > 3 && !strcmp(out + ol - 3, ".bl");
	if (!bl && (saving || stat(out, &st) != 0))
		snprintf(out + ol, n - ol, ".bl");
	return true;
}

static bool same_file(const char *a, const char *b)
{
	struct stat x, y;
	return !strcmp(a, b) || (stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino);
}

/* Open the world the field names: the one that is open is saved first. False = the card stays (card_msg says why). */
static bool world_open(struct app *a)
{
	char path[1024];
	const char *why = NULL;
	if (!field_path(a, false, path, sizeof path, &why)) {
		snprintf(a->card_msg, sizeof a->card_msg, "%s", why);
		return false;
	}
	if (a->path[0] && !a->nosave) {
		if (same_file(path, a->path)) {
			say(a, "That world is open already");
			return true;
		}
		if (!save_all(a, false)) {
			snprintf(a->card_msg, sizeof a->card_msg, "The world that is open cannot be saved, so it stays open");
			return false;
		}
	}
	if (access(path, R_OK) != 0) {
		snprintf(a->card_msg, sizeof a->card_msg, "There is no such world");
		return false;
	}
	if (!world_load(a, path, &why, -1, -1, -1)) {
		snprintf(a->card_msg, sizeof a->card_msg, "%s", why ? why : "It cannot be read");
		return false;
	}
	conf_save(a);                                           /* (the next start opens this one; the world's own file is not touched) */
	title_update(a);
	char name[300];
	world_name(a->path, name, sizeof name);
	say(a, "Opened %s", name);
	return true;
}

/* The world goes into `path` and is that file's from now on. */
static bool world_save_to(struct app *a, const char *path)
{
	char old[1024];
	bool was_nosave = a->nosave;
	snprintf(old, sizeof old, "%s", a->path);
	snprintf(a->path, sizeof a->path, "%s", path);
	a->nosave = false;
	if (!save_all(a, true)) {
		snprintf(a->path, sizeof a->path, "%s", old);
		a->nosave = was_nosave;
		snprintf(a->card_msg, sizeof a->card_msg, "It cannot be written there");
		return false;
	}
	title_update(a);
	return true;
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
	a->cmd = false;
	a->drag_sel = false;
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

/* ---- weapons ------------------------------------------------------------------------------------------------------ */

static const struct {
	int price_gold, price_block, price_n;       /* what making it takes ... */
	int building;                               /* ... and where: BL_SMITHY, BL_MAGE */
	const char *what;
} k_arms[BL_WP_COUNT] = {
	{ 0, 0, 0, 0, "close by, quick" },
	{ 2, BL_PLANKS, 4, BL_SMITHY, "reaches farther" },
	{ 4, BL_COBBLE, 8, BL_SMITHY, "slow and heavy; breaks walls and keeps" },
	{ 3, BL_PLANKS, 6, BL_SMITHY, "far; an arrow costs a plank" },
	{ 10, 0, 0, BL_MAGE, "a burst that hurts all near it; 1 gold = 5 shots" },
	{ 15, 0, 0, BL_MAGE, "a beam, very far; 1 gold = 30 shots" },
};
#define FIRE_SHOTS 5
#define LASER_SHOTS 30

static void arms_price(int wp, char *out, size_t n)
{
	if (k_arms[wp].price_n)
		snprintf(out, n, "%d gold, %d %s", k_arms[wp].price_gold, k_arms[wp].price_n, bl_block_name(k_arms[wp].price_block));
	else
		snprintf(out, n, "%d gold", k_arms[wp].price_gold);
}

static bool arms_has(const struct app *a, int wp)
{
	return a->arms & 1u << wp;
}

static void arms_hold(struct app *a, int wp)
{
	a->weapon = wp;
	a->weapon_until = now_ms() + 2500;
	say(a, "In your hand: the %s", bl_weapon(wp)->name);
}

/* the next weapon he has (Q) */
static void arms_next(struct app *a)
{
	for (int i = 1; i <= BL_WP_COUNT; i++) {
		const int wp = (a->weapon + i) % BL_WP_COUNT;
		if (arms_has(a, wp)) {
			if (wp == a->weapon)
				say(a, "You have only the %s: R makes more at a smithy", bl_weapon(wp)->name);
			else
				arms_hold(a, wp);
			return;
		}
	}
}

/* the weapon of the armoury's row: taken in the hand, or made first */
static bool arms_choose(struct app *a, int wp)
{
	if (wp < 0 || wp >= BL_WP_COUNT)
		return false;
	if (arms_has(a, wp)) {
		arms_hold(a, wp);
		return true;
	}
	survey(a);
	if ((k_arms[wp].building == BL_SMITHY ? a->nsmithy : a->nmage) == 0) {
		sound(a, "no");
		say(a, "The %s is made at a %s: have your workers build one (Tab, %s)", bl_weapon(wp)->name, k_arms[wp].building == BL_SMITHY ? "smithy" : "mage tower",
		    k_arms[wp].building == BL_SMITHY ? "7" : "8");
		return false;
	}
	if (a->inv[BL_GOLD] < k_arms[wp].price_gold || (k_arms[wp].price_n && a->inv[k_arms[wp].price_block] < k_arms[wp].price_n)) {
		char p[64];
		arms_price(wp, p, sizeof p);
		sound(a, "no");
		say(a, "The %s costs %s", bl_weapon(wp)->name, p);
		return false;
	}
	a->inv[BL_GOLD] -= k_arms[wp].price_gold;
	if (k_arms[wp].price_n)
		a->inv[k_arms[wp].price_block] -= k_arms[wp].price_n;
	a->arms |= 1u << wp;
	sound(a, "built");
	arms_hold(a, wp);
	return true;
}

/* the shots that are left for the weapon in the hand (-1: it needs none) */
static int arms_shots(const struct app *a, int wp)
{
	if (!a->survival)
		return -1;
	if (wp == BL_WP_BOW)
		return a->inv[BL_PLANKS];
	if (wp == BL_WP_FIRE)
		return a->charges[wp] + a->inv[BL_GOLD] * FIRE_SHOTS;
	if (wp == BL_WP_LASER)
		return a->charges[wp] + a->inv[BL_GOLD] * LASER_SHOTS;
	return -1;
}

static void arms_spend(struct app *a, int wp)
{
	if (!a->survival)
		return;
	if (wp == BL_WP_BOW) {
		a->inv[BL_PLANKS]--;
	} else if (wp == BL_WP_FIRE || wp == BL_WP_LASER) {
		if (a->charges[wp] <= 0) {
			a->inv[BL_GOLD]--;
			a->charges[wp] = wp == BL_WP_FIRE ? FIRE_SHOTS : LASER_SHOTS;
		}
		a->charges[wp]--;
	}
}

/* The weapon in the hand: when an enemy (or their keep) is in its reach and in sight it takes the blow or the shot, and nothing is dug behind it. */
static bool sword_swing(struct app *a, int64_t now)
{
	const struct bl_weapon_info *wi = bl_weapon(a->weapon);
	float eye[3], look[3], t = 0.0f;
	bl_player_eye(&a->pl, eye);
	bl_player_look(&a->pl, look);
	int id = bl_mobs_pick(&a->mobs, eye, look, wi->reach, &t);
	if (id >= 0 && a->mobs.mob[id].kind == BL_MOB_GOBLIN && !a->mobs_on)
		id = -1;
	if (id >= 0)                                            /* a block is in the way (leaves are none: goblins stand among them) */
		for (float s = 0.0f; s < t; s += 0.1f)
			if (bl_solid_unit(bl_get(a->world, (int)floorf(eye[0] + look[0] * s), (int)floorf(eye[1] + look[1] * s), (int)floorf(eye[2] + look[2] * s)))) {
				id = -1;
				break;
			}
	struct bl_hit h;
	const bool keep = id < 0 && a->rival == 1 && a->rival_on && a->rkeep_back <= 0.0f && bl_pick(a->world, eye, look, fmaxf(wi->reach, BL_REACH), &h) &&
	                  bl_red_keep(bl_get(a->world, h.x, h.y, h.z));
	if (id < 0 && !keep)
		return false;
	if (now < a->next_sword)
		return true;                                         /* too soon for another blow, and the block behind it is not touched */
	if (arms_shots(a, a->weapon) == 0) {
		a->next_sword = now + 700;
		sound(a, "no");
		say(a, a->weapon == BL_WP_BOW ? "No arrows: each costs a plank (Q: another weapon)" : "No gold for another shot (Q: another weapon)");
		return true;
	}
	arms_spend(a, a->weapon);
	a->next_sword = now + (int64_t)(wi->cooldown * 1000.0f);
	a->arm_swing = 1.0f;
	a->hit_t = 0.18f;
	const float dist = keep ? h.t : t;
	const float to[3] = { eye[0] + look[0] * (dist + 0.2f), eye[1] + look[1] * (dist + 0.2f), eye[2] + look[2] * (dist + 0.2f) };
	if (wi->shot >= 0) {                                    /* it flies from the right hand */
		const float rx = cosf(a->pl.yaw), rz = -sinf(a->pl.yaw);
		const float from[3] = { eye[0] + look[0] * 0.7f + rx * 0.28f, eye[1] + look[1] * 0.7f - 0.24f, eye[2] + look[2] * 0.7f + rz * 0.28f };
		bl_mobs_shot(&a->mobs, from, to, wi->shot);
		sound(a, "arrow");
	}
	if (keep) {
		if (wi->splash > 0.0f)
			bl_mobs_splash(&a->mobs, to, wi->splash, wi->damage, -1);
		rival_hit(a, wi->keep_damage);
		return true;
	}
	const int kind = a->mobs.mob[id].kind;
	const bool guard = bl_mob_is_rival(kind), raider = a->mobs.mob[id].raider;
	const bool heavy = a->weapon != BL_WP_LASER;
	const bool dead = bl_mobs_hurt(&a->mobs, id, wi->damage, heavy ? look[0] : 0.0f, heavy ? look[2] : 0.0f);
	if (wi->splash > 0.0f)
		a->kills += bl_mobs_splash(&a->mobs, to, wi->splash, wi->damage * 0.7f, id);
	if (dead && kind == BL_MOB_CHIEF) {
		;                                                   /* (chief_step tells of it) */
	} else if (dead && guard) {
		sound(a, "goblin-die");
		say(a, kind == BL_MOB_RWORKER ? "A worker of the %s is down" : raider ? "A raider of the %s is down" : "A guard of the %s is down", RIVAL_NAME);
	} else if (dead) {
		a->kills++;
		sound(a, "goblin-die");
		say(a, "Goblin down (%d)", a->kills);
	} else if (wi->shot != BL_SHOT_LASER) {
		sound(a, "hit");
	}
	return true;
}

static void do_break(struct app *a, int64_t now)
{
	if (sword_swing(a, now))
		return;
	a->arm_swing = 1.0f;
	int b = a->aimed ? bl_get(a->world, a->aim.x, a->aim.y, a->aim.z) : BL_AIR;
	if (bl_red_keep(b))                                     /* (struck by sword_swing, not broken) */
		return;
	if (a->aimed && bl_break(a->world, &a->aim)) {
		block_sound(a, "break", b);
		aim_update(a);
	}
}

/* the war hammer goes through what a hand has built four times as fast */
static float dig_speed(const struct app *a, int block)
{
	return a->weapon == BL_WP_HAMMER && bl_siege_hp(block) > 0 ? 4.0f : 1.0f;
}

/* survival: the button is held on a block: the sword first, else the block takes its time (`bl_hardness`) and gives what it leaves (`bl_drop`) */
static void dig_step(struct app *a, int64_t now, float dt)
{
	if (sword_swing(a, now)) {
		a->dig_on = false;
		return;
	}
	if (a->dig_wait > 0.0f) {                                  /* one block has just given way: a held button does not go straight on to the next */
		a->dig_wait -= dt;
		return;
	}
	if (!a->aimed) {
		a->dig_on = false;
		return;
	}
	int b = bl_get(a->world, a->aim.x, a->aim.y, a->aim.z);
	if (bl_red_keep(b)) {                                       /* their keep: struck (sword_swing), not dug */
		a->dig_on = false;
		return;
	}
	if (!bl_breakable(b)) {
		if (bl_own_keep(b) && !a->dig_on) {
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
		a->dig_tick = 0.0f;
	}
	a->dig_t += dt * dig_speed(a, b);
	if (a->dig_t >= a->dig_tick && a->dig_t < bl_hardness(b)) {     /* a blow at it every quarter of a second */
		block_sound(a, "dig", b);
		a->dig_tick += 0.28f;
	}
	if (a->arm_swing <= 0.0f)                                   /* the arm goes on swinging while a block is being broken */
		a->arm_swing = 1.0f;
	if (a->dig_t >= bl_hardness(b)) {
		if (bl_break(a->world, &a->aim)) {
			int d = bl_drop(b);
			if (d != BL_AIR)
				inv_add(a, d, 1);
			block_sound(a, "break", b);
			aim_update(a);
			a->dig_wait = DIG_PAUSE;
		}
		a->dig_on = false;
		a->dig_t = 0.0f;
	}
}

static void do_place(struct app *a)
{
	if (!a->aimed)
		return;
	if ((bl_pets_hit_cell(&a->pets, a->aim.px, a->aim.py, a->aim.pz) || bl_mobs_hit_cell(&a->mobs, a->aim.px, a->aim.py, a->aim.pz)) &&
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
		a->arm_swing = 1.0f;
		if (a->survival)
			a->inv[hand_block(a)]--;
		block_sound(a, "place", hand_block(a));
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

/* The keep has no hit points left. With the price of a new one it is built again; without it the game is lost (a card). */
static void keep_fall(struct app *a)
{
	keep_remove(a, a->keep, BL_KEEP);
	a->keep_hp = 0.0f;
	sound(a, "keep-fall");
	if (a->inv[BL_GOLD] >= KEEP_GOLD) {
		a->inv[BL_GOLD] -= KEEP_GOLD;
		a->keep_down = true;
		a->keep_back = KEEP_BACK;
		keep_sync(a);
		bl_mobs_raid_retreat(&a->mobs);                     /* (they have had their day: no second fall to the same party) */
		say(a, "Your keep is down! You pay %d gold: it stands again in %.0f seconds", KEEP_GOLD, KEEP_BACK);
		return;
	}
	a->have_keep = false;
	keep_sync(a);
	pause_game(a, P_LOST);
	say(a, "The keep has fallen");
}

/* a new keep rises where the old one stood; whoever stands there is put on top of it */
static void keep_rise(struct app *a)
{
	a->keep_down = false;
	keep_build(a, a->keep, BL_KEEP);
	a->keep_hp = KEEP_HP;
	keep_sync(a);
	for (int i = 0; i < BL_KEEP_TALL + 3 && bl_box_hits(a->world, a->pl.x, a->pl.y, a->pl.z, BL_PLAYER_W * 0.5f, BL_PLAYER_H); i++)
		a->pl.y += 1.0f;
	say(a, "Your keep stands again");
	sound(a, "built");
}

/* the keep that is on its way, the gold the mines bring, the mending by the infirmaries */
static void works_tick(struct app *a, float dt)
{
	if (a->keep_down && (a->keep_back -= dt) <= 0.0f)
		keep_rise(a);
	if ((a->look_t -= dt) <= 0.0f) {
		a->look_t = 1.0f;
		survey(a);
	}
	if ((a->mine_t += dt) >= MINE_EVERY) {
		a->mine_t = 0.0f;
		int got = 0;
		for (int i = 0; i < a->nmine; i++)
			if (bl_get(a->world, a->mine[i][0], a->mine[i][1], a->mine[i][2]) == BL_MINE)
				got += mine_take(a, a->mine[i]);
		if (got)
			inv_add(a, BL_GOLD, got);
	}
	for (int i = 0; i < a->ninfirm; i++) {
		const float x = (float)a->infirm[i][0] + 0.5f, z = (float)a->infirm[i][2] + 0.5f;
		if (a->hp > 0.0f && hypotf(a->pl.x - x, a->pl.z - z) < HEAL_RANGE) {
			a->hp = fminf(MAX_HP, a->hp + 2.0f * dt);
			break;
		}
	}
	for (int k = 0; k < BL_MOBS_MAX; k++) {
		struct bl_mob *m = &a->mobs.mob[k];
		if (!m->alive || !bl_mob_is_unit(m->kind))
			continue;
		const float max = m->kind == BL_MOB_SOLDIER ? BL_SOLDIER_HP : BL_WORKER_HP;
		for (int i = 0; i < a->ninfirm && m->hp < max; i++)
			if (hypotf(m->x - (float)a->infirm[i][0] - 0.5f, m->z - (float)a->infirm[i][2] - 0.5f) < HEAL_RANGE) {
				m->hp = fminf(max, m->hp + 1.5f * dt);
				break;
			}
	}
}

/* The goblins move; their blows take hit points and push the player; he mends slowly when nobody has hit him for a while; no hit points left = dead (a card). */
static void mobs_advance(struct app *a, float dt)
{
	float feet[3] = { a->pl.x, a->pl.y, a->pl.z };
	struct bl_mobs_result R;
	int before = bl_mobs_count_kind(&a->mobs, BL_MOB_GOBLIN);
	bl_mobs_step(&a->mobs, a->world, feet, a->hp > 0.0f, a->time_h, dt, &R);
	if (R.nbreak > 0)
		apply_breaks(a, &R);
	apply_build(a, &R, dt);
	camp_take(a, &R);
	if (R.rbuilt)
		camp_done(a);
	if (R.rworkers_slain > 0) {
		say(a, "Your soldiers brought down a worker of the %s", RIVAL_NAME);
		sound(a, "goblin-die");
	}
	if (R.chief_slain > 0)
		a->kills += R.chief_slain;
	if (R.guards_slain > 0) {
		say(a, R.raiders_slain ? "Your soldiers brought down a raider of the %s" : "Your soldiers brought down a guard of the %s", RIVAL_NAME);
		sound(a, "goblin-die");
	}
	if (R.raiders_home > 0)
		a->raid_home = true;
	rival_step(a, dt);
	if (R.rkeep_damage > 0.0f)
		rival_hit(a, R.rkeep_damage);
	if (R.workers_lost > 0) {
		say(a, R.workers_lost == 1 ? "A worker has been killed!" : "Workers have been killed!");
		sound(a, "lost");
	}
	if (R.soldiers_lost > 0) {
		say(a, R.soldiers_lost == 1 ? "A soldier has been killed!" : "Soldiers have been killed!");
		sound(a, "lost");
	}
	if (R.slain > 0) {
		a->kills += R.slain;
		sound(a, "goblin-die");
	}
	for (int i = 0; (R.sword_hits > 0 || R.shots > 0) && i < BL_MOBS_MAX; i++) {         /* your soldiers fighting, if near enough to be heard */
		const struct bl_mob *m = &a->mobs.mob[i];
		if (!m->alive || m->kind != BL_MOB_SOLDIER || !heard(a, m->x, m->z, m->posted ? 48.0f : HEAR))
			continue;
		if (R.shots > 0 && m->posted)
			sound(a, "arrow");
		else if (R.sword_hits > 0 && !m->posted && m->swing > 0.6f)
			sound(a, "clash");
	}
	if (R.keep_damage > 0.0f && a->survival && a->have_keep && !a->keep_down) {
		a->keep_hp -= R.keep_damage;
		if (a->keep_msg_t <= 0.0f) {
			say(a, "The keep is under attack!");
			sound(a, "alarm");
			a->keep_msg_t = 8.0f;
		}
		if (a->keep_hp <= 0.0f)
			keep_fall(a);
	}
	if (R.born_brutes > 0 && !a->brute_warned) {
		a->brute_warned = true;
		say(a, "A brute! Seven blows of the sword, and it hits for five");
	}
	if (R.born_brutes > 0)
		sound(a, "brute");
	if (R.born && !a->warned && before == 0) {
		a->warned = true;
		say(a, "A goblin! The left button is the sword");
	}
	if (bl_mobs_count_kind(&a->mobs, BL_MOB_GOBLIN) == 0 && bl_daylight(a->time_h) > 0.6f)
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
		sound(a, a->hp > 0.0f ? "hurt" : "die");
		if (a->hp <= 0.0f) {
			a->hp = 0.0f;
			pause_game(a, P_DEAD);
			say(a, R.chief_hits ? "The Red Captain got you" : R.guard_hits ? "The Red Banner got you" : "A goblin got you");
		}
	} else if (a->hp < MAX_HP) {
		a->regen_t += dt;
		if (a->regen_t > 6.0f)
			a->hp = fminf(MAX_HP, a->hp + dt / 3.0f);       /* a hit point every three seconds */
	}
	if (a->prompt != P_DEAD && a->prompt != P_LOST && a->prompt != P_WON)
		goblin_voices(a, dt);
}

static void respawn(struct app *a)
{
	bl_player_init(&a->pl, a->world);
	settle_player(a);
	a->hp = MAX_HP;
	a->hurt_t = a->hit_t = 0.0f;
	a->regen_t = 0.0f;
	bl_mobs_clear_kind(&a->mobs, BL_MOB_GOBLIN);
	bl_mobs_raid_retreat(&a->mobs);                             /* (raiders do not wait for him at the start) */
	a->mobs.spawn_t = 8.0f;                                    /* a breather before the next one */
	say(a, "Back at the start");
}

/* survival: the keep mends by day, the nights are counted (a night is over when the sun is well up again) */
static void survival_tick(struct app *a, float dt)
{
	a->keep_msg_t = fmaxf(0.0f, a->keep_msg_t - dt);
	float day = bl_daylight(a->time_h);
	if (day < 0.3f) {
		if (!a->in_night && a->have_keep && !a->keep_down && a->mobs_on) {              /* a night begins: a wave comes for the keep (the bigger the more nights are behind him) */
			int n = 3 + 2 * a->nights > 18 ? 18 : 3 + 2 * a->nights, cap = 3 + a->nights > 10 ? 10 : 3 + a->nights;
			int brutes = a->nights == 0 ? 0 : (a->nights + 1) / 2 > 4 ? 4 : (a->nights + 1) / 2;       /* from the second night on: the big ones, last of the wave */
			bl_mobs_start_wave(&a->mobs, n, cap, brutes);
			a->wave_msg_done = false;
			if (brutes > 0)
				say(a, "Night %d: a wave of %d goblins, %d of them brute%s, comes for the keep!", a->nights + 1, n, brutes, brutes == 1 ? "" : "s");
			else
				say(a, "Night %d: a wave of %d goblins comes for the keep!", a->nights + 1, n);
			sound(a, "horn");
		}
		a->in_night = true;
	} else if (day > 0.6f) {
		if (a->in_night && a->have_keep) {
			a->nights++;
			if (a->mobs.wave_size > 0 && a->survival) {                 /* a wave that was fought: the tribute of the dawn */
				int planks = 4 + 2 * a->nights > 30 ? 30 : 4 + 2 * a->nights, cobble = 3 + 2 * a->nights > 24 ? 24 : 3 + 2 * a->nights;
				inv_add(a, BL_PLANKS, planks);
				inv_add(a, BL_COBBLE, cobble);
				say(a, "Night %d survived: %d planks and %d cobble were left at the keep", a->nights, planks, cobble);
			} else {
				say(a, "Night %d survived", a->nights);
			}
			sound(a, "dawn");
			a->mobs.wave_size = 0;
		}
		if (a->mobs.wave_on)
			bl_mobs_end_wave(&a->mobs);
		a->in_night = false;
		if (a->have_keep && !a->keep_down && a->keep_hp < KEEP_HP)
			a->keep_hp = fminf(KEEP_HP, a->keep_hp + 1.5f * dt);
	}
}

/* the command camera: WASD move the point it looks at (in the way it is turned), the arrow keys turn it; the point follows the ground */
static void cmd_pan(struct app *a, float dt)
{
	a->cam_yaw += ((a->k_rk ? 1.0f : 0.0f) - (a->k_lk ? 1.0f : 0.0f)) * 1.6f * dt;
	float sy = sinf(a->cam_yaw), cy = cosf(a->cam_yaw);
	float mf = (a->k_fwd ? 1.0f : 0.0f) - (a->k_back ? 1.0f : 0.0f), mr = (a->k_right ? 1.0f : 0.0f) - (a->k_left ? 1.0f : 0.0f);
	float sp = (6.0f + a->cam_dist * 0.6f) * dt;
	a->cam_f[0] = LM_CLAMP(a->cam_f[0] + (sy * mf + cy * mr) * sp, 2.0f, (float)a->world->w - 3.0f);
	a->cam_f[2] = LM_CLAMP(a->cam_f[2] + (cy * mf - sy * mr) * sp, 2.0f, (float)a->world->d - 3.0f);
	float gy = (float)bl_top(a->world, (int)floorf(a->cam_f[0]), (int)floorf(a->cam_f[2])) + 1.0f;
	a->cam_f[1] += (gy - a->cam_f[1]) * fminf(1.0f, 6.0f * dt);
}

static void advance(struct app *a, int64_t now, float dt)
{
	if (a->playing) {
		struct bl_input in = { 0 };
		if (a->cmd) {                                        /* he stands where he is while the camera is up */
			cmd_pan(a, dt);
		} else {
			if (a->k_lk || a->k_rk || a->k_uk || a->k_dk)
				look_by(a, ((a->k_rk ? 1 : 0) - (a->k_lk ? 1 : 0)) * LOOK_RATE * dt, ((a->k_uk ? 1 : 0) - (a->k_dk ? 1 : 0)) * LOOK_RATE * dt);
			in = (struct bl_input){ .fwd = (a->k_fwd ? 1.0f : 0.0f) - (a->k_back ? 1.0f : 0.0f), .strafe = (a->k_right ? 1.0f : 0.0f) - (a->k_left ? 1.0f : 0.0f),
			                        .jump = a->k_jump || a->jump_latch, .sneak = a->k_sneak, .sprint = a->k_ctrl && a->k_fwd };
		}
		float ox = a->pl.x, oz = a->pl.z;
		bl_player_step(a->world, &a->pl, &in, dt);
		a->jump_latch = false;
		{                                                   /* the legs go by the distance walked */
			float gone = hypotf(a->pl.x - ox, a->pl.z - oz), v = dt > 0.0f ? gone / dt : 0.0f;
			float want = a->pl.on_ground && v > 0.2f ? fminf(1.0f, v / 3.0f) : 0.0f;
			a->walk_stride += (want - a->walk_stride) * fminf(1.0f, 10.0f * dt);
			if (a->pl.on_ground)
				a->walk_phase = fmodf(a->walk_phase + gone / 1.5f, 1.0f);
			a->arm_swing = fmaxf(0.0f, a->arm_swing - 3.6f * dt);
		}
		if (a->pets_on && a->pets.placed) {
			bl_pets_step(&a->pets, a->world, dt);
			bl_pets_push_player(&a->pets, a->world, &a->pl);
		}
		mobs_advance(a, dt);                                 /* (the goblins only when they are on: the workers always) */
		bl_mobs_push_player(&a->mobs, a->world, &a->pl);     /* nobody walks through the player */
		a->hurt_t = fmaxf(0.0f, a->hurt_t - dt);
		a->hit_t = fmaxf(0.0f, a->hit_t - dt);
		if (a->playing && !a->cmd)                           /* (he may have died in that step) */
			aim_update(a);
		else
			a->aimed = false;
		if (a->survival) {
			if (a->b_break && a->playing)
				dig_step(a, now, dt);
			survival_tick(a, dt);
			works_tick(a, dt);
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
	if (!a->nosave && a->path[0] && a->world->rev != a->saved_rev && now - a->last_save_ms >= AUTOSAVE_MS) {
		a->last_save_ms = now;                               /* (a failure is tried again a minute later) */
		save_all(a, false);
	}
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

/* the weapon in the hand, over the right end of the hotbar */
static void paint_weapon(struct app *a, const struct lay *L)
{
	struct wlwin *w = &a->win;
	char l[80], name[32];
	snprintf(name, sizeof name, "%s", bl_weapon(a->weapon)->name);
	name[0] = (char)(name[0] - 'a' + 'A');
	const int shots = arms_shots(a, a->weapon);
	if (shots >= 0)
		snprintf(l, sizeof l, "%s (%d)   Q", name, shots);
	else
		snprintf(l, sizeof l, "%s   Q", name);
	const int right = L->x0 + SLOTS * L->slot + (SLOTS - 1) * L->gap;
	shadow_text(w, right - lm_font_text_width(l), L->y0 - LM_FONT_H - 18, l, shots == 0 ? 0xff6a5au : 0xffe8a0u);
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
	wlwin_round_fill(w, L->x0 - 7, L->y0 - 7, total + 14, L->slot + 14, lm_button_radius(14), lm_theme.bar);
	for (int i = 0; i < SLOTS; i++) {
		int x = L->x0 + i * (L->slot + L->gap), y = L->y0;
		if (i == a->slot) {
			wlwin_round_fill(w, x - 3, y - 3, L->slot + 6, L->slot + 6, lm_button_radius(11), lm_theme.accent);
			wlwin_round_fill(w, x - 1, y - 1, L->slot + 2, L->slot + 2, lm_button_radius(9), lm_theme.background);
		} else {
			wlwin_round_fill(w, x, y, L->slot, L->slot, lm_button_radius(9), lm_theme.background);
		}
		const uint32_t *ic = a->icon[a->hotbar[i]];
		if (ic)
			wlwin_blit_px_over(w, x + 4, y + 4, ic, isz * sc, isz * sc, isz * sc);
		char n[2] = { (char)('1' + i), 0 };
		wlwin_text(w, x + 4, y + 1, n, 1, i == a->slot ? lm_theme.text_bright : lm_theme.text_dim, 0);
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

/* is the text in its panel (else it is written over the picture)? */
static bool panel_on(const struct app *a)
{
	return a->panel && wlwin_panes(&a->win);
}

/* where the game stands: whether their keep can come back */
static void goal_theirs(const struct app *a, char *out, size_t n)
{
	if (a->rival == 2)
		snprintf(out, n, "You have beaten the %s", RIVAL_NAME);
	else if (a->rkeep_back > 0.0f)
		snprintf(out, n, "Their keep rises in %.0f s", ceilf(a->rkeep_back));
	else if (a->rgold >= KEEP_GOLD)
		snprintf(out, n, "Their gold %d: they can rebuild (needs %d)", a->rgold, KEEP_GOLD);
	else
		snprintf(out, n, "Their gold %d: pull their keep down and you win!", a->rgold);
}

/* the nearest deposit with gold left in it: its number, -1: there is none */
static int nearest_gold(const struct app *a)
{
	int best = -1;
	float bd = 1e9f;
	for (int i = 0; i < a->ndeposits; i++) {
		const float d = hypotf((float)a->deposit[i][0] - a->pl.x, (float)a->deposit[i][2] - a->pl.z);
		if (a->ore[i] > 0 && d < bd) {
			bd = d;
			best = i;
		}
	}
	return best;
}

/* the one thing to do next */
static const char *next_step(const struct app *a)
{
	if (a->rival == 2)
		return "Play on: build what you like";
	if (a->keep_down)
		return "Hold on: your keep is being built again";
	if (nworkers(a) < 2)
		return "Train workers: Tab, then B (8 planks; X makes planks of logs)";
	if (a->inv[BL_LOG] + a->inv[BL_PLANKS] / 4 < 3 && nsoldiers(a) == 0)
		return "Gather wood and stone: Tab, choose workers, 2, right-click a tree";
	if (a->inv[BL_GOLD] == 0 && a->nmine == 0 && a->arms == 1u << BL_WP_SWORD)
		return "Dig gold: Tab, choose workers, 2, right-click a gold deposit";
	if (nsoldiers(a) == 0)
		return "Build a barracks (Tab, 3) and train a soldier (V)";
	if (a->nsmithy == 0 && a->arms == 1u << BL_WP_SWORD)
		return "Build a smithy (Tab, 7) and take a better weapon (R)";
	if (a->inv[BL_GOLD] < KEEP_GOLD && nsoldiers(a) < 4)
		return "Save 20 gold for a spare keep and train more soldiers";
	if (a->rgold >= KEEP_GOLD)
		return "Rob their gold: dig the deposits, kill their diggers, strike their keep";
	return "Attack their keep now: they cannot build it again";
}

/* survival: the keep's hit points over the middle of the top, the nights survived, and under the crosshair how far the block is broken */
static void paint_survival(struct app *a)
{
	struct wlwin *w = &a->win;
	if (a->have_keep) {
		int bw = LM_CLAMP(w->bw / 3, 140, 260), x = (w->bw - bw) / 2, y = 8;
		float f = a->keep_hp / KEEP_HP;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		wlwin_round_fill(w, x - 3, y - 3, bw + 6, 18, 9, lm_theme.bar);
		wlwin_round_fill(w, x, y, bw, 12, 6, lm_theme.background);
		if (f > 0.0f)
			wlwin_round_fill(w, x, y, LM_MAX(8, (int)((float)bw * f)), 12, 5, f > 0.5f ? 0x4cb050u : f > 0.25f ? 0xe0a030u : 0xe04030u);
		char l[160];
		if (a->keep_down)
			snprintf(l, sizeof l, "Your keep rises in %.0f s", ceilf(a->keep_back));
		else if (a->rival >= 1)
			goal_theirs(a, l, sizeof l);
		else
			snprintf(l, sizeof l, "Keep %d   Nights %d", (int)ceilf(a->keep_hp), a->nights);
		if (!panel_on(a))
			shadow_text(w, (w->bw - lm_font_text_width(l)) / 2, y + 18, l, a->keep_down ? 0xff9a70u : 0xffffffu);
		if (!panel_on(a) && a->mobs.wave_on && bl_mobs_wave_remaining(&a->mobs) > 0) {
			int br = bl_mobs_count_brutes(&a->mobs) + a->mobs.wave_brutes;
			if (br > 0)
				snprintf(l, sizeof l, "Wave: %d goblins left, %d of them brute%s", bl_mobs_wave_remaining(&a->mobs), br, br == 1 ? "" : "s");
			else
				snprintf(l, sizeof l, "Wave: %d goblins left", bl_mobs_wave_remaining(&a->mobs));
			shadow_text(w, (w->bw - lm_font_text_width(l)) / 2, y + 18 + LM_FONT_H + 2, l, 0xff9a70u);
		}
	}
	if (a->dig_on) {
		float hd = bl_hardness(bl_get(a->world, a->dig_x, a->dig_y, a->dig_z)), f = hd > 0.0f ? a->dig_t / hd : 0.0f;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		int cx = w->bw / 2, cy = w->bh / 2 + 22;
		wlwin_fill(w, cx - 24, cy - 1, 48, 7, 0x101010u);
		wlwin_fill(w, cx - 23, cy, (int)(46.0f * f), 5, 0xf0e8d0u);
	}
}

/* the blocks of the list of what he has */
static int stock_list(const struct app *a, int list[BL_COUNT])
{
	int n = 0;
	static const int k_first[4] = { BL_LOG, BL_PLANKS, BL_COBBLE, BL_GOLD };
	for (int i = 0; i < 4; i++)
		if (a->survival || a->inv[k_first[i]] > 0)
			list[n++] = k_first[i];
	for (int b = 1; b < BL_COUNT; b++)
		if (a->inv[b] > 0 && b != BL_LOG && b != BL_PLANKS && b != BL_COBBLE && b != BL_GOLD)
			list[n++] = b;
	return n;
}

/* where the list of what he has lies: at the right, under the lines of the top; `rows` of it show. In survival the wood and the stone always, then whatever else
 * there is; in creative, where nothing is used up, only what has been gathered (no list when that is nothing). */
static int stock_layout(const struct app *a, int *x, int *y, int *wd, int list[BL_COUNT])
{
	const int n = stock_list(a, list);
	*wd = 0;
	for (int i = 0; i < n; i++)
		*wd = LM_MAX(*wd, lm_font_text_width(bl_block_name(list[i])));
	*wd += 72;
	*x = a->win.bw - *wd - 8;
	*y = a->rival == 1 && a->rival_on && bl_mobs_count_raiders(&a->mobs) > 0 ? 100 : 90;       /* (under the line about the raiders) */
	int fit = (a->win.bh - *y - 84) / (LM_FONT_H + 2);          /* (down to the hotbar or the plans) */
	return n < fit ? n : LM_MAX(fit, 0);
}

static void paint_stock(struct app *a)
{
	struct wlwin *w = &a->win;
	int x, y, wd, list[BL_COUNT];
	int rows = stock_layout(a, &x, &y, &wd, list);
	if (rows <= 0)
		return;
	wlwin_round_fill(w, x, y, wd, rows * (LM_FONT_H + 2) + 10, lm_button_radius(10), lm_theme.bar);
	for (int i = 0; i < rows; i++) {
		int b = list[i], ry = y + 5 + i * (LM_FONT_H + 2), have = a->inv[b];
		char n[16];
		snprintf(n, sizeof n, "%d", have > 99999 ? 99999 : have);
		wlwin_round_fill(w, x + 8, ry + 3, 11, 11, 3, bl_block_color(b) & 0xffffffu);
		wlwin_text(w, x + 26, ry + 1, bl_block_name(b), 1, have ? lm_theme.text_soft : lm_theme.text_dim, 0);
		wlwin_text(w, x + wd - 8 - lm_font_text_width(n), ry + 1, n, 1, have ? lm_theme.text_bright : lm_theme.text_dim, 0);
	}
}

/* the Red Banner: how far its camp is and which way, at the top right, and under it what its keep can still take once one is near or it has been struck */
static void paint_rival(struct app *a)
{
	struct wlwin *w = &a->win;
	if (a->rival != 1 || !a->rival_on)
		return;
	float dist;
	const char *way = rival_bearing(a, &dist);
	char l[120];
	if (a->cmd)                                                 /* (from above the camp is in the picture, or End looks at it) */
		snprintf(l, sizeof l, "%s %.0f", RIVAL_NAME, a->rhp);
	else
		snprintf(l, sizeof l, "%s %.0f, %s", RIVAL_NAME, dist, way);
	shadow_text(w, w->bw - lm_font_text_width(l) - 8, 40, l, 0xff8a7au);
	if (dist < 45.0f || a->rhit_t > 0.0f || a->cmd) {
		int bw = 150, x = w->bw - bw - 8, y = 40 + LM_FONT_H + 6;
		float f = a->rhp / RIVAL_HP;
		wlwin_round_fill(w, x - 3, y - 3, bw + 6, 16, 8, lm_theme.bar);
		wlwin_round_fill(w, x, y, bw, 10, 5, lm_theme.background);
		if (f > 0.0f)
			wlwin_round_fill(w, x, y, LM_MAX(8, (int)((float)bw * f)), 10, 5, 0xd02828u);
	}
	int n = 0;
	float near = 1e9f, nx = 0.0f, nz = 0.0f;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		if (!m->alive || !m->raider)
			continue;
		n++;
		float d = hypotf(m->x - a->pl.x, m->z - a->pl.z);
		if (d < near) {
			near = d;
			nx = m->x;
			nz = m->z;
		}
	}
	if (n) {
		const char *to = bearing_to(a, nx, nz, &near);
		if (a->cmd)
			snprintf(l, sizeof l, "Raiders %d", n);
		else
			snprintf(l, sizeof l, "Raiders %d: %.0f, %s", n, near, to);
		shadow_text(w, w->bw - lm_font_text_width(l) - 8, 40 + LM_FONT_H + 6 + 16, l, 0xffb040u);
	}
}

/* what a worker is doing, in a word */
static const char *order_text(const struct app *a, const struct bl_mob *m, char *buf, size_t n)
{
	if (m->order == BL_ORDER_GATHER) {
		snprintf(buf, n, "%s %s", m->work > 0.0f ? "breaking" : "going to", bl_block_name(m->gblock));
		return buf;
	}
	if (m->order == BL_ORDER_BUILD && m->site >= 0 && m->site < BL_SITES_MAX && a->mobs.site[m->site].on) {
		snprintf(buf, n, "%s %s", m->at_site ? "building the" : "going to the", bl_plan_name(a->mobs.site[m->site].plan));
		return buf;
	}
	if (m->order == BL_ORDER_TOWER)
		return m->posted ? "on the watchtower" : "going to the tower";
	if (m->kind == BL_MOB_SOLDIER)
		return hypotf(m->tx - m->x, m->tz - m->z) > 1.2f ? "going to its post" : "on guard";
	return m->order == BL_ORDER_MOVE ? "going" : "idle";
}

/* the command view's overlay: what it is, a marker over every worker (the chosen ones green, with what they do), the box being pulled, and the keys */
static void paint_cmd(struct app *a, const struct bl_camera *cam)
{
	struct wlwin *w = &a->win;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		float sx, sy;
		if (!m->alive || !bl_mob_is_unit(m->kind) || !cmd_project(a, cam, m->x, m->y + bl_mob_h(m->kind) + 0.35f, m->z, &sx, &sy))
			continue;
		int x = (int)sx, y = (int)sy;
		const bool sol = m->kind == BL_MOB_SOLDIER;
		const float maxhp = sol ? BL_SOLDIER_HP : BL_WORKER_HP;
		wlwin_round_fill(w, x - 7, y - 7, 14, 14, 7, m->selected ? 0xffffffu : 0x10141cu);
		wlwin_round_fill(w, x - 5, y - 5, 10, 10, 5, m->selected ? 0x4cd98cu : sol ? 0xe05050u : 0x4a90e0u);
		if (m->hp < maxhp) {                                                 /* hurt: a small bar */
			wlwin_fill(w, x - 10, y + 9, 20, 4, 0x101010u);
			wlwin_fill(w, x - 9, y + 10, (int)(18.0f * m->hp / maxhp), 2, m->hp > maxhp * 0.5f ? 0x4cb050u : 0xe04030u);
		}
		if (m->selected) {
			char buf[64];
			const char *t = order_text(a, m, buf, sizeof buf);
			shadow_text(w, x - lm_font_text_width(t) / 2, y + 14, t, 0xffffffu);
		}
	}
	if (a->drag_sel) {                                                       /* the box */
		int x0 = (int)fminf((float)a->drag_x, (float)a->px), x1 = (int)fmaxf((float)a->drag_x, (float)a->px);
		int y0 = (int)fminf((float)a->drag_y, (float)a->py), y1 = (int)fmaxf((float)a->drag_y, (float)a->py);
		if (x1 - x0 > 6 || y1 - y0 > 6) {
			wlwin_fill(w, x0, y0, x1 - x0, 2, 0xffffffu);
			wlwin_fill(w, x0, y1 - 2, x1 - x0, 2, 0xffffffu);
			wlwin_fill(w, x0, y0, 2, y1 - y0, 0xffffffu);
			wlwin_fill(w, x1 - 2, y0, 2, y1 - y0, 0xffffffu);
		}
	}
	if (a->rival == 1 && a->rival_on) {                                      /* their guards and their keep */
		float sx, sy;
		for (int i = 0; i < BL_MOBS_MAX; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			if (m->alive && m->kind == BL_MOB_GUARD && cmd_project(a, cam, m->x, m->y + BL_SOLDIER_H + 0.35f, m->z, &sx, &sy)) {
				wlwin_fill(w, (int)sx - 5, (int)sy - 5, 10, 10, m->raider ? 0xff9820u : 0xd02828u);
				wlwin_fill(w, (int)sx - 3, (int)sy - 3, 6, 6, 0x18080au);
			} else if (m->alive && m->kind == BL_MOB_RWORKER && cmd_project(a, cam, m->x, m->y + BL_WORKER_H + 0.35f, m->z, &sx, &sy)) {
				wlwin_fill(w, (int)sx - 4, (int)sy - 4, 8, 8, 0xc87868u);                      /* a worker of theirs: smaller and paler */
				wlwin_fill(w, (int)sx - 2, (int)sy - 2, 4, 4, 0x18080au);
			}
		}
		if (cmd_project(a, cam, (float)a->rkeep[0] + 0.5f, (float)a->rkeep[1] + 4.0f, (float)a->rkeep[2] + 0.5f, &sx, &sy)) {
			char l[80];
			if (panel_on(a))
				snprintf(l, sizeof l, "%s", RIVAL_NAME);
			else
				snprintf(l, sizeof l, "%s: %d material", RIVAL_NAME, a->rstock);
			shadow_text(w, (int)sx - lm_font_text_width(l) / 2, (int)sy - LM_FONT_H, l, 0xff8a7au);
		}
	}
	for (int i = 0; i < a->ndeposits; i++) {                                 /* the gold */
		float sx, sy;
		if (a->ore[i] > 0 && cmd_project(a, cam, (float)a->deposit[i][0] + 0.5f, (float)a->deposit[i][1] + 3.0f, (float)a->deposit[i][2] + 0.5f, &sx, &sy)) {
			char l[40];
			snprintf(l, sizeof l, "gold %d", a->ore[i]);
			shadow_text(w, (int)sx - lm_font_text_width(l) / 2, (int)sy - LM_FONT_H, l, 0xffd830u);
		}
	}
	{
		const int ci = bl_mobs_chief(&a->mobs);
		float sx, sy;
		if (ci >= 0 && cmd_project(a, cam, a->mobs.mob[ci].x, a->mobs.mob[ci].y + BL_CHIEF_H + 0.6f, a->mobs.mob[ci].z, &sx, &sy))
			shadow_text(w, (int)sx - lm_font_text_width(CHIEF_NAME) / 2, (int)sy - LM_FONT_H, CHIEF_NAME, 0xff7060u);
	}
	{                                                                        /* where he stands */
		float sx, sy;
		if (cmd_project(a, cam, a->pl.x, a->pl.y + (a->body ? 2.2f : 1.0f), a->pl.z, &sx, &sy)) {
			wlwin_round_fill(w, (int)sx - 6, (int)sy - 6, 12, 12, 6, 0x10141cu);
			wlwin_round_fill(w, (int)sx - 4, (int)sy - 4, 8, 8, 4, 0xffd34cu);
			shadow_text(w, (int)sx - lm_font_text_width("you") / 2, (int)sy - LM_FONT_H - 8, "you", 0xffe080u);
		}
	}
	char l[200];
	if (!panel_on(a)) {                                                      /* (else the panel says it) */
		int y0 = a->survival ? 88 : 6;                                       /* (the keep's bar and the message are at the top in survival) */
		shadow_text(w, 8, y0, "COMMAND VIEW", 0xffe080u);
		snprintf(l, sizeof l, "Left: choose (drag a box, Shift adds)   Right: %s   1 go there, 2 break blocks", a->order_mode == 1 ? "break that block and the rest of its kind" : "go there");
		shadow_text(w, 8, y0 + LM_FONT_H + 2, l, 0xffffffu);
		shadow_text(w, 8, y0 + 2 * (LM_FONT_H + 2), "Build: 3 to 0 or the bar below, R turns it, Delete takes a plan away", 0xffffffu);
		snprintf(l, sizeof l, "B: a worker (%d planks)   V: a soldier (a barracks, %d cobble, %d planks)", WORKER_COST, SOLDIER_COBBLE, SOLDIER_PLANKS);
		shadow_text(w, 8, y0 + 3 * (LM_FONT_H + 2), l, 0xffffffu);
		shadow_text(w, 8, y0 + 4 * (LM_FONT_H + 2), a->rival == 1 && a->rival_on ? "WASD move   arrows turn   wheel zoom   Home back   End: their camp   Tab or Esc: back to him" : "WASD move   arrows turn   wheel zoom   Home back   Tab or Esc: back to him", 0xffffffu);
		snprintf(l, sizeof l, "Workers %d, soldiers %d, chosen %d, being built %d", nworkers(a), nsoldiers(a), cmd_selected(a), bl_mobs_count_sites(&a->mobs));
		shadow_text(w, 8, y0 + 5 * (LM_FONT_H + 2), l, 0xc8e8ffu);
	}
	for (int i = 0; i < BL_PLAN_COUNT; i++) {                                /* the plans */
		int x, y, bw, bh;
		bar_rect(a, i, &x, &y, &bw, &bh);
		if (a->plan == i) {
			wlwin_round_fill(w, x - 3, y - 3, bw + 6, bh + 6, lm_button_radius(13), lm_theme.accent);
			wlwin_round_fill(w, x - 1, y - 1, bw + 2, bh + 2, lm_button_radius(11), lm_theme.background);
		} else {
			wlwin_round_fill(w, x - 1, y - 1, bw + 2, bh + 2, lm_button_radius(11), lm_theme.bar);
			wlwin_round_fill(w, x, y, bw, bh, lm_button_radius(10), lm_theme.background);
		}
		snprintf(l, sizeof l, "%d %s", (i + 3) % 10, plan_title(i));
		wlwin_text(w, x + LM_MAX(3, (bw - lm_font_text_width(l)) / 2), y + 3, l, 1, lm_theme.text_bright, bw - 6);
		if (!a->survival)
			snprintf(l, sizeof l, "free");
		else
			plan_cost_text(i, false, l, sizeof l);
		if (a->survival && lm_font_text_width(l) > bw - 6)
			plan_cost_text(i, true, l, sizeof l);
		wlwin_text(w, x + LM_MAX(3, (bw - lm_font_text_width(l)) / 2), y + 5 + LM_FONT_H, l, 1, plan_affordable(a, i) ? lm_theme.text_dim : lm_theme.caution, bw - 6);
	}
	if (a->plan >= 0 && a->has_pointer && bar_at(a, a->px, a->py) < 0) {     /* what a click would do, by the pointer */
		struct bl_site s;
		const char *why = NULL;
		bool any, ok = plan_at_pointer(a, &s, &why, &any);
		const char *t = ok ? "Click to build" : why ? why : "";
		int tw = lm_font_text_width(t);
		shadow_text(w, LM_CLAMP((int)a->px - tw / 2, 4, LM_MAX(4, w->bw - tw - 4)), (int)a->py + 18, t, ok ? 0x90ff90u : 0xff9080u);
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

/* ---- the help: pages of text with a list of topics (F1; wlhelp.h; a feature of the game needs its line here) ---- */
static const char *const HP_WELCOME[] = {
	"# What this is",
	"A world made of blocks, in the first person. A new game is either a game or free building; you choose when you give it its name, and it stays so.",
	"- A game: you have a keep, the Red Banner has one too, there is gold to fight over, and at night goblins come out of the ground.",
	"- Free building: every block, breaking at once, nothing used up, nobody about.",
	"# How to win",
	"Pull down the Red Banner's keep when they have under 20 gold, so that they cannot build it again. You lose when your keep falls and you have under 20 gold. The page Winning tells how; G in the game shows it in short, and the panel says what to do next.",
	"# The window",
	"- The row of nine squares at the bottom is the hotbar: what is in your hand is the lit one. In a game the number under a block is how many you have.",
	"- Ten hearts over the hotbar are your hit points, at the right the weapon in your hand. Your keep's bar is at the top.",
	"- The panel beside the window holds everything the game says in words. Drag it anywhere while the game is paused or in the command view; I switches between the panel and text over the picture.",
	"# To start",
	"|Click|Click in the window: the pointer is locked to it and the game goes on. Esc lets the pointer go and pauses.",
	"|F1|These pages.",
	"|Ctrl+Q|Quit.",
	"# Saved games",
	"Every game is a file of its own with the name you gave it. The one that was open last is opened the next time.",
	"|Ctrl+N|A new game: it asks for its name. The old one stays saved.",
	"|Ctrl+O|Open another game; the one you are in is saved first.",
	"|Ctrl+S|Save (the game also saves itself every minute and when you pause or quit).",
	"|Ctrl+Shift+S|Save as: the game goes into another file and goes on there.",
};

static const char *const HP_WINNING[] = {
	"# The two keeps",
	"A keep is the castle with corner turrets and a tall tower: yours grey with red shields, theirs dark with red banners. Yours cannot be broken by your own hand. A keep is not a barracks.",
	"# A keep can be built again - for gold",
	"A keep that falls rises again if its owner has 20 gold: yours after half a minute, theirs after forty-five seconds. Whoever cannot pay has lost.",
	"They start with 20 gold, enough for one new keep. Their workers dig more of it from the deposits. So take their gold away:",
	"- Kill their gold diggers (a deposit near their camp is where they go).",
	"- Dig the deposits yourself before they do: the gold is limited, there is no more of it in the world.",
	"- Pull their keep down once to make them spend it, then again.",
	"# A way to win",
	"- 1. Tab: choose workers, press 2, right-click trees and stone. Press B for more workers.",
	"- 2. Send some to the gold near your keep (Tab shows every deposit and what is left in it). A mine beside a deposit digs by itself.",
	"- 3. Build a barracks (3) and train soldiers (V). Build a smithy (7) and take a better weapon (R).",
	"- 4. Guard a deposit near their camp, or raid their workers there.",
	"- 5. Strike their keep: the war hammer and the fire staff do it quickest. Soldiers you send to it (right-click it in the command view) strike too.",
};

static const char *const HP_MOVING[] = {
	"# Walking about",
	"|W A S D|Walk forward, left, back, right. The keys count by their place, whatever the letters on them are.",
	"|Mouse|Look round. The arrow keys look round too.",
	"|Space|Jump. In water it swims you up; while flying it goes up.",
	"|Shift|Sneak: slow, lower, and you do not walk off an edge. While flying it goes down.",
	"|Ctrl|Run.",
	"|F|Fly (double speed with Ctrl). Press it again to come down.",
	"A fall does not hurt. Water slows you down and holds you up.",
	"# Blocks",
	"|Left button|Break the block you aim at (hold it). In a game a block takes time.",
	"|Right button|Put the block in your hand against the face you aim at.",
	"|Middle button|Take the block you aim at into your hand.",
	"|1 to 9, wheel|Choose the square of the hotbar.",
	"|E|The palette: every block you can put; in a game only what you have.",
	"|X|Make something of the block in your hand: a log makes four planks, planks two torches, sand glass, cobble stone, stone brick.",
	"- Dirt, sand and snow take half a second, planks and logs about one, stone, brick and gold two and more. Grass leaves dirt, stone leaves cobble, leaves and glass nothing.",
	"- A torch gives light and stands on a block. Water you put by hand is a source and flows.",
	"# Time",
	"|[ and ]|Move the clock an hour back or on.",
	"|T|The clock stands still, runs (a day is twenty minutes), or follows the real sun.",
	"|F3|Numbers.",
};

static const char *const HP_WEAPONS[] = {
	"# Fighting",
	"The left button uses the weapon in your hand when an enemy (or their keep) is within its reach and in sight; else it breaks the block you aim at. The crosshair turns red when a blow lands.",
	"|Q|Take the next weapon you have.",
	"|R|The weapons: take one, or have one made. In a game a smithy or a mage tower has to stand somewhere.",
	"# The weapons",
	"|Sword|You have it. Quick, close by.",
	"|Spear|Smithy: 2 gold, 4 planks. Reaches farther.",
	"|War hammer|Smithy: 4 gold, 8 cobble. Slow; much harder on the keep, and it breaks walls four times as fast.",
	"|Bow|Smithy: 3 gold, 6 planks. Far; each arrow costs a plank.",
	"|Fire staff|Mage tower: 10 gold. A burst that hurts everybody near the one it hits; one gold is five shots. Good against the keep.",
	"|Laser|Mage tower: 15 gold. A beam that reaches very far; one gold is thirty shots.",
	"# Hit points",
	"Hit points come back, one every three seconds, six seconds after the last blow; near an infirmary much faster. At nought you die: Respawn puts you back at the start with the world as you left it.",
};

static const char *const HP_COMMAND[] = {
	"# The command view",
	"|Tab|Look down from above and command your people. Tab or Esc takes you back to the hero, who stands still meanwhile.",
	"|Left button|Click one to choose him, or pull a box over several. Shift adds or takes one away.",
	"|Ctrl+A|Choose everybody.",
	"|Right button|Send the chosen where the pointer is (mode 1), or have workers break that block and the next of its kind (mode 2).",
	"|1, 2|Mode 1: go there. Mode 2: break (trees, stone, gold).",
	"|Backspace|Stop what the chosen are doing.",
	"|B|Train a worker: 8 planks, up to eight.",
	"|V|Train a soldier: 6 cobble, 4 planks, at a barracks.",
	"# Gathering",
	"A worker goes on to the next block of the same kind within six blocks. For a tree click its trunk or leaves. What he breaks goes into your pockets.",
	"# The camera",
	"|W A S D|Move the view.",
	"|Arrows|Turn it.",
	"|Wheel|Come nearer or go further.",
	"|Home, End|Back to your keep; look at theirs.",
};

static const char *const HP_BUILDINGS[] = {
	"# How to build",
	"In the command view pick a building with the keys 3 to 0 or the bar at the bottom. The preview is green where it can be built. The left button places it and the price is taken from your pockets. The chosen workers build it, else every idle one.",
	"|R|Turn the building.",
	"|Delete|Point at a planned building: it is given up and the price comes back.",
	"|Esc, right button|Put the plan aside.",
	"# The buildings",
	"|3 Barracks|12 cobble, 8 planks. Soldiers are trained there, four to each. Within forty blocks of the keep.",
	"|4 Watchtower|20 cobble, 10 planks. A soldier on it shoots arrows (right-click it with him chosen).",
	"|5 Wall|6 cobble. Stays in your hand, so a row is quick.",
	"|6 Gate|6 cobble, 4 planks. Your people walk through, goblins and the Red Banner do not.",
	"|7 Smithy|12 cobble, 6 planks. Spear, war hammer and bow.",
	"|8 Mage tower|24 cobble, 8 planks, 8 gold. Fire staff and laser.",
	"|9 Infirmary|6 cobble, 12 planks. You and your people near it mend quickly.",
	"|0 Mine|6 cobble, 10 planks. Beside gold: it digs one piece every ten seconds from within six blocks.",
	"The block with the sign (banner, anvil, crystal, cross, winch) is what counts: without it the house does nothing.",
};

static const char *const HP_SOLDIERS[] = {
	"# Soldiers",
	"A soldier has fourteen hit points and a sword like yours. He goes after an enemy that comes near his post, then goes back. Send him somewhere with the right button and that is his new post; send him to the Red Banner's keep and he strikes it.",
	"# Goblins",
	"At night goblins come out of the ground. They go for you, your people and your keep. In daylight those in the open burn up. From the second night on the last of a wave are brutes: bigger, twenty-six hit points, five a blow.",
	"# Waves",
	"When night begins a wave comes for your keep: three the first night, two more each night. Surviving a night with a wave leaves planks and cobble at the keep.",
	"# Walls",
	"Goblins and raiders that cannot get round a wall hammer at it until it breaks. Stone and earth of the land they leave alone.",
};

static const char *const HP_RIVAL[] = {
	"# The Red Banner",
	"The rival clan: every game has it, 80 to 120 blocks from your keep. A wall with two gateways, their keep in the middle, a watchtower with an archer, guards in black with red helmets and workers in dark red with a sack.",
	"# The Red Captain",
	"Their chief wears a crown and a red cape. He flies, over walls and water too, and takes the best weapon he has for the distance: sword and bow to begin with, then, with gold to spare, spear, war hammer, fire staff and laser. He guards the camp and leads every second raiding party; badly hurt he flies home. When he falls he is back after ninety seconds, as long as their keep stands.",
	"# Their workers",
	"They fell trees, cut stone and dig gold. Trees and stone fill their store, which pays for guards (5), raiders (4) and buildings; gold pays for a new keep and the Captain's weapons. A worker runs home when you or your people come near.",
	"# The camp grows",
	"With material to spare they build a longhouse (then more men) and two more watchtowers. What you pull down they build again.",
	"# Raids",
	"By day a raiding party sets out for your keep, the first after five minutes of play, then four minutes after the last is over: two men, one more each time, up to six.",
	"# Finding them",
	"The top right says how far their camp is and which way. In the command view End looks at it.",
};

static const char *const HP_KEYS[] = {
	"# Moving and blocks",
	"|W A S D|Walk",
	"|Space, Shift, Ctrl|Jump, sneak, run",
	"|F|Fly",
	"|Left, right, middle|Fight or break, put, take",
	"|1 to 9, wheel|Choose a square",
	"|E|Palette",
	"|X|Make something",
	"# Fighting",
	"|Q|Next weapon",
	"|R|Weapons",
	"|G|How to win",
	"# Command view",
	"|Tab|Command view",
	"|B, V|Train a worker, a soldier",
	"|3 to 0|Buildings",
	"|1, 2|Go there, break",
	"|R, Delete|Turn a building, give a planned one up",
	"|End, Home|Their keep, yours",
	"# Others",
	"|I|The panel of text",
	"|[ ]|The clock an hour",
	"|T|Stand / run / real sun",
	"|F1|These pages",
	"|F3|Numbers",
	"|Esc|Pause",
	"|Ctrl+S, Ctrl+Shift+S|Save, save as",
	"|Ctrl+O, Ctrl+N, Ctrl+Q|Open a game, new game, quit",
};

static const struct wlhelp_page BLOCKS_HELP[] = {
	{ "Welcome", HP_WELCOME, WLHELP_N(HP_WELCOME) },
	{ "Winning", HP_WINNING, WLHELP_N(HP_WINNING) },
	{ "Moving, blocks", HP_MOVING, WLHELP_N(HP_MOVING) },
	{ "Weapons", HP_WEAPONS, WLHELP_N(HP_WEAPONS) },
	{ "The command view", HP_COMMAND, WLHELP_N(HP_COMMAND) },
	{ "Buildings", HP_BUILDINGS, WLHELP_N(HP_BUILDINGS) },
	{ "Soldiers, goblins", HP_SOLDIERS, WLHELP_N(HP_SOLDIERS) },
	{ "The Red Banner", HP_RIVAL, WLHELP_N(HP_RIVAL) },
	{ "All the keys", HP_KEYS, WLHELP_N(HP_KEYS) },
};
#define BLOCKS_HELP_N WLHELP_N(BLOCKS_HELP)

static int pal_cell(const struct app *a)                   /* the size of a square of the palette */
{
	struct lay L;
	layout(a, &L);
	return L.slot + 10;
}

/* is the card one with a path in it (open, save as)? */
static bool file_card(const struct app *a)
{
	return a->prompt == P_OPEN || a->prompt == P_SAVEAS;
}

/* the card of a new game: the name's field and under it the two modes (their rectangles) */
static void name_layout(const struct app *a, struct wlcard *c, int *fx, int *fy, int *fw, int *my, int *mw)
{
	c->w = LM_MIN(540, a->win.bw - 16);
	c->h = LM_MIN(256, a->win.bh - 16);
	wlcard_center(&a->win, c);
	*fx = c->x + WLCARD_PAD;
	*fy = c->y + WLCARD_PAD + 40;
	*fw = c->w - 2 * WLCARD_PAD;
	*my = *fy + 40;
	*mw = (*fw - 10) / 2;
}
#define MODE_H 52
#define ARMS_ROW 40

/* a card that has to be answered before anything else: the rail waits */
static bool modal_card(const struct app *a)
{
	return a->prompt != P_NONE && a->prompt != P_PAUSE;
}

/* the open / save as card: the field, the folder list under it (`rows` of it, 0 in a very low window) */
static void file_layout(const struct app *a, struct wlcard *c, int *fx, int *fy, int *fw, int *ly, int *rows)
{
	*rows = wlpicker_fit(a->win.bh, 180);
	c->w = LM_MIN(560, a->win.bw - 16);
	c->h = LM_MIN(WLCARD_PAD + 24 + 34 + (*rows ? *rows * WLPICKER_ROW_H + 12 : 0) + 44 + WLCARD_BTN_H + WLCARD_PAD, a->win.bh - 16);
	wlcard_center(&a->win, c);
	*fx = c->x + WLCARD_PAD;
	*fy = c->y + WLCARD_PAD + 24;
	*fw = c->w - 2 * WLCARD_PAD;
	*ly = *fy + 34;
}

static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
	*nb = a->prompt == P_PAUSE ? 4 : (a->prompt == P_NEW || a->prompt == P_REPLACE || file_card(a)) ? 2 : 1;
	if (a->prompt == P_NEW) {
		int fx, fy, fw, my, mw;
		name_layout(a, c, &fx, &fy, &fw, &my, &mw);
		return;
	}
	if (file_card(a)) {
		int fx, fy, fw, ly, rows;
		file_layout(a, c, &fx, &fy, &fw, &ly, &rows);
		return;
	}
	int ch = a->prompt == P_HELP ? 580 : a->prompt == P_PAUSE ? 222 : (a->prompt == P_DEAD || a->prompt == P_LOST || a->prompt == P_WON) ? 150 : a->prompt == P_GOAL ? 290 :
	         a->prompt == P_ARMS ? 116 + BL_WP_COUNT * ARMS_ROW : 130;
	int cw = a->prompt == P_HELP ? 940 : a->prompt == P_PAUSE ? 540 : a->prompt == P_GOAL ? 600 : a->prompt == P_ARMS ? 560 : 470;
	if (a->prompt == P_PALETTE) {
		int cell = pal_cell(a);
		cw = PAL_COLS * cell + 2 * WLCARD_PAD;
		ch = 124 + ((PAL_N + PAL_COLS - 1) / PAL_COLS) * cell;
	}
	c->w = LM_MIN(cw, a->win.bw - 16);
	c->h = LM_MIN(ch, a->win.bh - 16);
	wlcard_center(&a->win, c);
}

/* the armoury's rows: which is at a point, -1 none */
static int arms_at(const struct app *a, double px, double py)
{
	if (a->prompt != P_ARMS)
		return -1;
	struct wlcard c;
	int nb;
	card_rect(a, &c, &nb);
	const int i = (int)floor((py - c.y - 38) / ARMS_ROW);
	return px >= c.x + WLCARD_PAD && px < c.x + c.w - WLCARD_PAD && py >= c.y + 38 && i < BL_WP_COUNT ? i : -1;
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
	const char *title = a->prompt == P_LOST ? "The keep has fallen" : a->prompt == P_DEAD ? "You died" : a->prompt == P_NEW ? "A new game" : a->prompt == P_HELP ? "Help" : a->prompt == P_PALETTE ? "Blocks" :
	                    a->prompt == P_GOAL ? "How to win" : a->prompt == P_ARMS ? "Weapons" : a->prompt == P_WON ? "Victory" : a->prompt == P_OPEN ? "Open a world" : a->prompt == P_SAVEAS ? "Save the world as" : a->prompt == P_REPLACE ? "Replace the world" : a->first ? "Blocks" : "Paused";
	wlcard_frame(w, &c, c.w, c.h, title, lm_theme.accent, lm_theme.background, lm_theme.text_bright);
	char name[300];
	world_name(a->path, name, sizeof name);
	bool kept = a->path[0] && !a->nosave;
	const char *labels[4] = { "Cancel", "OK" };
	uint32_t cols[4] = { lm_theme.btn_grey, lm_theme.btn_blue };
	int tx = c.x + WLCARD_PAD, ty = c.y + 44, tw = c.w - 2 * WLCARD_PAD;
	if (a->prompt == P_PAUSE) {
		labels[0] = "Quit";
		labels[1] = "Save";
		labels[2] = "New game";
		labels[3] = "Play";
		cols[0] = lm_theme.btn_red;
		cols[1] = lm_theme.btn_blue;
		cols[2] = lm_theme.btn_amber;
		cols[3] = lm_theme.btn_green;
		char l[400];
		snprintf(l, sizeof l, "World: %s%s", name, kept || !a->path[0] ? "" : "  (not saved)");
		wlwin_text(w, tx, ty, l, 1, lm_theme.accent, tw);
		ty += LM_FONT_H + 6;
		wlwin_text(w, tx, ty, a->first ? "Click in the window to look around and play." : "Click in the window to go on.", 1, lm_theme.text, tw);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, "W A S D walk, Space jumps, Shift sneaks, Ctrl runs, F flies.", 1, lm_theme.text_dim, tw);
		wlwin_text(w, tx, ty + 2 * (LM_FONT_H + 5), "Left button breaks, right button puts a block, 1 to 9 choose it.", 1, lm_theme.text_dim, tw);
		wlwin_text(w, tx, ty + 3 * (LM_FONT_H + 5), a->survival ? "G: how to win.  Q and R: weapons.  F1: every key." : "Esc pauses. F1 lists every key.", 1, lm_theme.text_dim, tw);
	} else if (a->prompt == P_LOST) {
		labels[0] = "New game";
		cols[0] = lm_theme.btn_amber;
		char l[120];
		wlwin_text(w, tx, ty, "It fell and you had not 20 gold for another. The game is over.", 1, lm_theme.text, tw);
		snprintf(l, sizeof l, "Nights survived: %d     Goblins killed: %d", a->nights, a->kills);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, l, 1, lm_theme.text_dim, tw);
	} else if (a->prompt == P_DEAD) {
		labels[0] = "Respawn";
		cols[0] = lm_theme.btn_green;
		char l[120];
		wlwin_text(w, tx, ty, "You were cut down. Your world is as you left it.", 1, lm_theme.text, tw);
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
			wlwin_round_fill(w, x + 1, y + 1, cell - 2, cell - 2, 6, cur ? lm_theme.accent : a->pal_hover == i ? lm_theme.button_hover : lm_theme.panel_line);
			wlwin_round_fill(w, x + 3, y + 3, cell - 6, cell - 6, 5, lm_theme.bar);
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
		wlwin_text(w, tx, gy + ((PAL_N + PAL_COLS - 1) / PAL_COLS) * cell + 8, l, 1, lm_theme.text, tw);
	} else if (a->prompt == P_NEW) {
		struct wlcard nc;
		int fx, fy, fw, my, mw;
		name_layout(a, &nc, &fx, &fy, &fw, &my, &mw);
		wlwin_text(w, fx, fy - LM_FONT_H - 5, "What is the game called?", 1, lm_theme.text, fw);
		wlwin_round_fill(w, fx, fy, fw, 28, 14, lm_theme.bar);
		wltext_paint(w, fx + 12, fy + 6, fw - 24, a->gname, true, lm_theme.text_soft, lm_theme.button_hover, lm_theme.text_soft);
		for (int i = 0; i < 2; i++) {
			const int x = fx + i * (mw + 10);
			const bool on = (i == 0) == a->new_survival;
			wlwin_round_fill(w, x, my, mw, MODE_H, 12, on ? lm_theme.accent : lm_theme.panel_line);
			wlwin_round_fill(w, x + 2, my + 2, mw - 4, MODE_H - 4, 10, on ? lm_theme.bar : lm_theme.background);
			wlwin_text_bold(w, x + 12, my + 8, i == 0 ? "Game" : "Free building", 1, on ? lm_theme.text_bright : lm_theme.text_dim, mw - 20);
			wlwin_text(w, x + 12, my + 10 + LM_FONT_H, i == 0 ? "fight the Red Banner" : "every block, nobody about", 1, lm_theme.text_dim, mw - 20);
		}
		wlwin_text(w, fx, my + MODE_H + 8, a->card_msg, 1, lm_theme.caution, fw);
		labels[0] = a->new_first ? "Quit" : "Cancel";
		labels[1] = "Start";
		cols[0] = a->new_first ? lm_theme.btn_red : lm_theme.btn_slate;
		cols[1] = lm_theme.btn_green;
	} else if (a->prompt == P_GOAL) {
		static const char *const k_goal[] = {
			"Win: pull down the Red Banner's keep when they have under 20 gold,",
			"so that they cannot build it again. They start with 20.",
			"You lose when your keep falls and you have under 20 gold.",
			"",
			"1.  Tab: send workers to wood, stone and gold (choose, 2, right-click).",
			"2.  Build a barracks (3) and train soldiers (V).",
			"3.  Build a smithy (7), then take a weapon (R).",
			"4.  Take their gold: dig it first, kill their diggers.",
			"5.  Strike their keep. The Red Captain will not like it.",
		};
		for (int i = 0; i < (int)(sizeof k_goal / sizeof k_goal[0]); i++)
			wlwin_text(w, tx, ty + i * (LM_FONT_H + 4), k_goal[i], 1, i < 3 ? lm_theme.text_bright : lm_theme.text, tw);
		labels[0] = "Play";
		cols[0] = lm_theme.btn_green;
	} else if (a->prompt == P_ARMS) {
		for (int i = 0; i < BL_WP_COUNT; i++) {
			const int y = c.y + 38 + i * ARMS_ROW;
			const bool has = arms_has(a, i), cur = a->weapon == i;
			wlwin_round_fill(w, tx - 4, y + 1, tw + 8, ARMS_ROW - 3, 10, cur ? lm_theme.panel_line : a->pal_hover == i ? lm_theme.button_hover : lm_theme.background);
			char l[120], p[64];
			snprintf(l, sizeof l, "%d  %s", i + 1, bl_weapon(i)->name);
			l[3] = (char)(l[3] - 'a' + 'A');
			wlwin_text_bold(w, tx + 4, y + 3, l, 1, has ? lm_theme.text_bright : lm_theme.text_dim, tw / 2);
			wlwin_text(w, tx + 22, y + 3 + LM_FONT_H, k_arms[i].what, 1, lm_theme.text_dim, tw - 30);
			arms_price(i, p, sizeof p);
			if (cur)
				snprintf(l, sizeof l, "in your hand");
			else if (has)
				snprintf(l, sizeof l, "yours");
			else
				snprintf(l, sizeof l, "%s: %s", k_arms[i].building == BL_SMITHY ? "smithy" : "mage tower", p);
			wlwin_text(w, tx + tw - 4 - lm_font_text_width(l), y + 3, l, 1, cur ? lm_theme.accent : has ? lm_theme.text : lm_theme.caution, 0);
		}
		char l[220];
		snprintf(l, sizeof l, "You have %d gold.  %s", a->inv[BL_GOLD], a->msg);
		wlwin_text(w, tx, c.y + 42 + BL_WP_COUNT * ARMS_ROW, l, 1, lm_theme.text, tw);
		labels[0] = "Close";
		cols[0] = lm_theme.btn_green;
	} else if (a->prompt == P_WON) {
		char l[120];
		wlwin_text(w, tx, ty, "Their keep is down for good. The Red Banner is beaten.", 1, lm_theme.text, tw);
		snprintf(l, sizeof l, "Nights survived: %d     Goblins killed: %d", a->nights, a->kills);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, l, 1, lm_theme.text_dim, tw);
		labels[0] = "Play on";
		cols[0] = lm_theme.btn_green;
	} else if (a->prompt == P_REPLACE) {
		char l[400], other[300];
		world_name(a->target, other, sizeof other);
		snprintf(l, sizeof l, "There is a world called %s already.", other);
		wlwin_text(w, tx, ty, l, 1, lm_theme.text, tw);
		wlwin_text(w, tx, ty + LM_FONT_H + 5, "Replace it with this one?", 1, lm_theme.caution, tw);
		labels[1] = "Replace";
		cols[0] = lm_theme.btn_slate;
		cols[1] = lm_theme.btn_red;
	} else if (file_card(a)) {
		struct wlcard fc;
		int fx, fy, fw, ly, rows;
		file_layout(a, &fc, &fx, &fy, &fw, &ly, &rows);
		const char *h = "Tab completes";
		wlwin_text(w, c.x + c.w - WLCARD_PAD - lm_font_text_width(h), c.y + WLCARD_PAD - 2, h, 1, lm_theme.text_dim, 0);
		wlwin_round_fill(w, fx, fy, fw, 28, 14, lm_theme.bar);              /* the field */
		wltext_paint(w, fx + 12, fy + 6, fw - 24, a->field, true, lm_theme.text_soft, lm_theme.button_hover, lm_theme.text_soft);
		if (rows) {
			char base[1024];
			card_base(a, base, sizeof base);
			wlpicker_paint(w, &a->pk, base, a->field, fx, ly, fw, rows, a->px, a->py, a->has_pointer);
		}
		int my = c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 40;
		const char *note = a->prompt == P_SAVEAS ? "A world is one file; .bl is added to the name." : kept ? "" : "The world you are in is not saved: opening another leaves it behind.";
		wlwin_text(w, fx, my, note, 1, a->prompt == P_OPEN ? lm_theme.caution : lm_theme.text_dim, fw);
		wlwin_text(w, fx, my + LM_FONT_H + 2, a->card_msg, 1, lm_theme.caution, fw);
		labels[1] = a->prompt == P_OPEN ? "Open" : "Save";
		cols[0] = lm_theme.btn_slate;
		cols[1] = a->prompt == P_OPEN ? lm_theme.btn_blue : lm_theme.btn_green;
	} else {
		labels[0] = "Close";
		cols[0] = lm_theme.btn_blue;
		wlhelp_paint(&a->help, w, &c, a->px, a->py, a->has_pointer);
	}
	wlcard_buttons(w, &c, labels, nb, cols, a->pc_hover, a->pc_down, lm_theme.on_colour);
}

static void rail_build(const struct app *a, struct wlrail *L);

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
	           "prompt=%d slot=%d hand=%s rev=%u time=%.3f clock=%d aimed=%d rw=%d rh=%d fps=%d budget=%.0f debug=%d under=%s saved=%d nosave=%d wpend=%zu light=%d sound=%d sounds=%d sfx=%s\n",
	        w->seed, w->kind, w->w, w->d, w->h, a->pl.x, a->pl.y, a->pl.z, a->pl.yaw, a->pl.pitch, a->pl.on_ground, a->pl.in_water, a->pl.flying, a->pl.sneaking,
	        a->pl.vy, a->playing, a->win.locked, a->lock_ok, (int)a->prompt, a->slot + 1, bl_block_name(hand_block(a)), w->rev, a->time_h, a->clockm, a->aimed,
	        a->rw, a->rh, a->fps, a->budget, a->debug, bl_block_name(under), w->rev == a->saved_rev, a->nosave, bl_water_pending(w),
	        bl_light(w, (int)floorf(a->pl.x), (int)floorf(a->pl.y + 0.5f), (int)floorf(a->pl.z)), a->sound_on, a->nsounds, a->nsounds ? k_sfx[a->last_sfx].name : "none");
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
	fprintf(f, "hero on=%d phase=%.3f stride=%.2f swing=%.2f sword=%d held=%s\n", a->body, a->walk_phase, a->walk_stride, a->arm_swing, a->body && a->hero.sword,
	        a->survival && a->inv[hand_block(a)] < 1 ? "air" : bl_block_name(hand_block(a)));
	fprintf(f, "arms mask=%u weapon=%d fire=%d laser=%d\n", a->arms, a->weapon, a->charges[BL_WP_FIRE], a->charges[BL_WP_LASER]);
	fprintf(f, "works keep_down=%d keep_back=%.1f smithy=%d mage=%d infirm=%d mine=%d\n", a->keep_down, a->keep_down ? a->keep_back : 0.0f, a->nsmithy, a->nmage, a->ninfirm, a->nmine);
	for (int i = 0; i < a->ndeposits; i++)
		fprintf(f, "deposit %d x=%d y=%d z=%d ore=%d\n", i, a->deposit[i][0], a->deposit[i][1], a->deposit[i][2], a->ore[i]);
	{
		const int ci = bl_mobs_chief(&a->mobs);
		const struct bl_mob *c = ci >= 0 ? &a->mobs.mob[ci] : NULL;
		fprintf(f, "chief alive=%d x=%.3f y=%.3f z=%.3f hp=%.1f weapon=%d flying=%d raider=%d back=%.1f arms=%u rgold=%d rkeep_back=%.1f\n", c != NULL, c ? c->x : 0.0f, c ? c->y : 0.0f,
		        c ? c->z : 0.0f, c ? c->hp : 0.0f, c ? c->weapon : 0, c ? c->flying : 0, c ? c->raider : 0, a->chief_t, a->chief_arms, a->rgold, a->rkeep_back);
	}
	if (a->prompt == P_NEW) {
		struct wlcard c;
		int fx, fy, fw, my, mw;
		name_layout(a, &c, &fx, &fy, &fw, &my, &mw);
		fprintf(f, "newgame survival=%d first=%d field=%d,%d,%d modes=%d,%d,%d name=%s\nnmsg %s\n", a->new_survival, a->new_first, fx, fy, fw, my, mw, MODE_H, a->gname, a->card_msg);
	}
	fprintf(f, "panel on=%d shown=%d messages=%d\n", a->panel, wlwin_pane_shown(&a->win, WLPANE_TOOL), a->nlog);
	if (a->nlog)
		fprintf(f, "said %s\n", a->log[0].text);
	fprintf(f, "game survival=%d have_keep=%d keep=%d,%d,%d keep_hp=%.1f nights=%d dig=%.2f wave=%d/%d/%d barracks=%d brutes=%d\n", a->survival, a->have_keep, a->keep[0], a->keep[1], a->keep[2], a->keep_hp, a->nights,
	        a->dig_on ? a->dig_t : -1.0f, a->mobs.wave_on ? bl_mobs_wave_remaining(&a->mobs) : 0, a->mobs.wave_on ? a->mobs.wave_size : 0, a->mobs.wave_on, barracks_find(a, NULL), bl_mobs_count_brutes(&a->mobs));
	{
		struct bl_camera cam;
		cmd_camera(a, &cam);
		fprintf(f, "cmd on=%d mode=%d sel=%d workers=%d soldiers=%d dist=%.1f yaw=%.3f focus=%.2f,%.2f,%.2f eye=%.2f,%.2f,%.2f plan=%d rot=%d sites=%d\n", a->cmd, a->order_mode, cmd_selected(a), nworkers(a), nsoldiers(a), a->cam_dist, a->cam_yaw,
		        a->cam_f[0], a->cam_f[1], a->cam_f[2], cam.x, cam.y, cam.z, a->plan, a->plan_rot, bl_mobs_count_sites(&a->mobs));
		for (int i = 0; i < BL_PLAN_COUNT; i++) {
			int x, y, bw, bh;
			bar_rect(a, i, &x, &y, &bw, &bh);
			fprintf(f, "bar %d x=%d y=%d w=%d h=%d\n", i, x, y, bw, bh);
		}
		for (int i = 0; i < BL_SITES_MAX; i++) {
			const struct bl_site *st = &a->mobs.site[i];
			if (!st->on)
				continue;
			int lo[3], hi[3];
			float sx = -1, sy = -1;
			bl_site_box(st, lo, hi);
			cmd_project(a, &cam, 0.5f * (float)(lo[0] + hi[0] + 1), (float)(hi[1] + 1), 0.5f * (float)(lo[2] + hi[2] + 1), &sx, &sy);
			fprintf(f, "site %d plan=%s n=%d missing=%d lo=%d,%d,%d hi=%d,%d,%d sx=%.1f sy=%.1f\n", i, bl_plan_name(st->plan), st->n, bl_site_missing(a->world, st), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], sx, sy);
		}
		for (int i = 0; i < BL_MOBS_MAX; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			float sx = -1, sy = -1;
			if (!m->alive || !bl_mob_is_unit(m->kind))
				continue;
			bool vis = cmd_project(a, &cam, m->x, m->y + 1.0f, m->z, &sx, &sy);
			fprintf(f, "%s %d x=%.3f y=%.3f z=%.3f hp=%.1f order=%d sel=%d vis=%d sx=%.1f sy=%.1f gather=%d,%d,%d post=%.2f,%.2f posted=%d site=%d\n", m->kind == BL_MOB_SOLDIER ? "soldier" : "worker", i, m->x, m->y, m->z, m->hp, m->order, m->selected, vis, sx, sy, m->gx, m->gy,
			        m->gz, m->tx, m->tz, m->posted, m->order == BL_ORDER_BUILD ? m->site : -1);
		}
	}
	{
		char name[300], file[1024];
		world_name(a->path, name, sizeof name);
		snprintf(file, sizeof file, "%s", a->path[0] ? a->path : "-");
		for (char *q = name; *q; q++)
			*q = *q == ' ' ? '_' : *q;
		for (char *q = file; *q; q++)
			*q = *q == ' ' ? '_' : *q;
		fprintf(f, "world name=%s file=%s\n", name, file);
		struct wlrail rail;
		int ry[WLRAIL_MAX];
		rail_build(a, &rail);
		wlrail_layout(&rail, ry);
		for (int i = 0; i < rail.n; i++)                        /* (x like everything else here: from the picture's left edge, so the rail's is negative) */
			if (ry[i] >= 0)
				fprintf(f, "rail %d x=%d y=%d w=%d h=%d idle=%d lit=%d\n", i, (WLRAIL_W - WLRAIL_D) / 2 - WLRAIL_W, ry[i], WLRAIL_D, WLRAIL_D, rail.it[i].idle, rail.it[i].lit);
	}
	if (file_card(a)) {
		struct wlcard c;
		int fx, fy, fw, ly, rows;
		char base[1024], msg[200];
		file_layout(a, &c, &fx, &fy, &fw, &ly, &rows);
		card_base(a, base, sizeof base);
		snprintf(msg, sizeof msg, "%s", a->card_msg);
		for (char *q = msg; *q; q++)
			*q = *q == ' ' ? '_' : *q;
		fprintf(f, "field x=%d y=%d w=%d h=28 text=%s\ncmsg %s\n", fx, fy, fw, a->field, msg);
		if (rows)
			wlpicker_dump(f, &a->pk, base, a->field, fx, ly, fw, rows, 0);
	}
	{
		int x, y, wd, list[BL_COUNT];
		int rows = stock_layout(a, &x, &y, &wd, list);
		if (rows > 0) {
			fprintf(f, "stock x=%d y=%d w=%d rows=%d list=", x, y, wd, rows);
			for (int i = 0; i < rows; i++)
				fprintf(f, "%s%s:%d", i ? "," : "", bl_block_name(list[i]), a->inv[list[i]]);
			fprintf(f, "\n");
		}
	}
	{
		float dist = 0.0f;
		rival_bearing(a, &dist);
		fprintf(f, "rival state=%d on=%d keep=%d,%d,%d hp=%.1f guards=%d archer=%d dist=%.1f raids=%d next=%.1f raiders=%d here=%d stock=%d built=%d site=%d workers=%d\n", a->rival,
		        a->rival_on, a->rkeep[0], a->rkeep[1], a->rkeep[2], a->rhp, rival_count(a, false), rival_count(a, true), a->rival ? dist : -1.0f, a->raids, a->raid_t,
		        bl_mobs_count_raiders(&a->mobs), a->raid_here, a->rstock, a->rbuilt, a->rsite, bl_mobs_count_kind(&a->mobs, BL_MOB_RWORKER));
		struct bl_camera cam;
		cmd_camera(a, &cam);
		for (int i = 0; i < BL_MOBS_MAX; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			float sx = -1, sy = -1;
			if (m->alive && m->kind == BL_MOB_RWORKER) {
				cmd_project(a, &cam, m->x, m->y + 1.0f, m->z, &sx, &sy);
				fprintf(f, "rworker %d x=%.3f y=%.3f z=%.3f hp=%.1f order=%d load=%d sx=%.1f sy=%.1f\n", i, m->x, m->y, m->z, m->hp, m->order, m->load, sx, sy);
			}
			if (!m->alive || m->kind != BL_MOB_GUARD)
				continue;
			cmd_project(a, &cam, m->x, m->y + 1.0f, m->z, &sx, &sy);
			fprintf(f, "guard %d x=%.3f y=%.3f z=%.3f hp=%.1f posted=%d sx=%.1f sy=%.1f raider=%d retreat=%d\n", i, m->x, m->y, m->z, m->hp, m->posted, sx, sy, m->raider, m->retreat);
		}
	}
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
		if (a->prompt == P_ARMS)
			for (int i = 0; i < BL_WP_COUNT; i++)
				fprintf(f, "armrow %d x=%d y=%d w=%d h=%d\n", i, c.x + WLCARD_PAD, c.y + 38 + i * ARMS_ROW, c.w - 2 * WLCARD_PAD, ARMS_ROW);
		if (a->prompt == P_HELP)
			wlhelp_dump(&a->help, f, &c);
		for (int k = 0; k < nb; k++) {
			int x, y, bw, bh;
			wlcard_button_rect(&c, k, nb, &x, &y, &bw, &bh);
			fprintf(f, "cbtn %d %d %d %d %d\n", k, x, y, bw, bh);
		}
	}
	fclose(f);
	rename(tmp, g_dump);
}

/* the player's body for a frame: whole from above, from his own eyes the arm with the sword (a goblin is near) or the block he holds */
static void hero_pose(struct app *a, const struct bl_camera *cam)
{
	struct bl_hero *h = &a->hero;
	memset(h, 0, sizeof *h);
	h->x = a->pl.x;
	h->y = a->pl.y;
	h->z = a->pl.z;
	h->yaw = a->pl.yaw;
	h->first = !a->cmd;
	h->pitch = h->first ? a->pl.pitch : 0.0f;
	h->phase = a->walk_phase;
	h->stride = a->walk_stride;
	h->swing = a->arm_swing;
	h->hurt = a->hurt_t / 0.45f;
	h->sneak = a->pl.sneaking && !a->pl.flying;
	h->weapon = a->weapon;
	h->sword = now_ms() < a->weapon_until;
	const float shown = fmaxf(SWORD_SHOWN, bl_weapon(a->weapon)->reach);
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *m = &a->mobs.mob[i];
		if (m->alive && bl_mob_is_foe(m->kind) && hypotf(m->x - a->pl.x, m->z - a->pl.z) < shown && fabsf(m->y - a->pl.y) < 8.0f)
			h->sword = true;
	}
	if (a->rival == 1 && a->rival_on && a->aimed && bl_red_keep(bl_get(a->world, a->aim.x, a->aim.y, a->aim.z)))
		h->sword = true;
	h->held = a->survival && a->inv[hand_block(a)] < 1 ? 0 : hand_block(a);
	h->eye[0] = cam->x;
	h->eye[1] = cam->y;
	h->eye[2] = cam->z;
	h->fov = cam->fov;
	h->aspect = a->rh > 0 ? (float)a->rw / (float)a->rh : 1.6f;
	bl_hero_set(h, a->world, a->time_h);
}

/* ================================================================ the rail ================================================================ */

/* the buttons at the left, as in MellaGUI's other programs; the item's number is its place */
enum { RB_NEW, RB_OPEN, RB_SAVE, RB_SAVEAS, RB_PLAY, RB_CMD, RB_BLOCKS, RB_SOUND, RB_GOAL, RB_HELP, RB_N };

static void ic_play(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
	(void)bg;
	for (int i = 0; i < 7; i++)                             /* a triangle pointing right */
		wlwin_fill(w, cx - 4 + i, cy - 7 + i, 2, 15 - 2 * i, c);
}

static void ic_cube(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
	(void)bg;
	wlrail_line(w, cx - 1, cy - 8, cx - 8, cy - 4, 2, c);
	wlrail_line(w, cx - 1, cy - 8, cx + 6, cy - 4, 2, c);
	wlrail_line(w, cx - 8, cy - 4, cx - 1, cy, 2, c);
	wlrail_line(w, cx + 6, cy - 4, cx - 1, cy, 2, c);
	wlwin_fill(w, cx - 8, cy - 4, 2, 8, c);
	wlwin_fill(w, cx + 6, cy - 4, 2, 8, c);
	wlwin_fill(w, cx - 1, cy, 2, 8, c);
	wlrail_line(w, cx - 8, cy + 3, cx - 1, cy + 7, 2, c);
	wlrail_line(w, cx + 6, cy + 3, cx - 1, cy + 7, 2, c);
}

static void ic_speaker(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
	(void)bg;
	wlwin_fill(w, cx - 8, cy - 3, 4, 7, c);
	for (int i = 0; i < 5; i++)
		wlwin_fill(w, cx - 4 + i, cy - 3 - i, 1, 7 + 2 * i, c);
	wlwin_fill(w, cx + 3, cy - 3, 2, 7, c);
	wlwin_fill(w, cx + 6, cy - 6, 2, 13, c);
}

static void ic_mute(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
	(void)bg;
	wlwin_fill(w, cx - 8, cy - 3, 4, 7, c);
	for (int i = 0; i < 5; i++)
		wlwin_fill(w, cx - 4 + i, cy - 3 - i, 1, 7 + 2 * i, c);
	wlrail_line(w, cx + 3, cy - 3, cx + 8, cy + 3, 2, c);
	wlrail_line(w, cx + 8, cy - 3, cx + 3, cy + 3, 2, c);
}

static void ic_flag(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
	(void)bg;
	wlwin_fill(w, cx - 6, cy - 8, 2, 16, c);
	wlwin_fill(w, cx - 4, cy - 8, 11, 8, c);
	wlwin_fill(w, cx + 5, cy - 5, 2, 2, bg);
}

static void rail_build(const struct app *a, struct wlrail *L)
{
	memset(L, 0, sizeof *L);
	L->x = 0;
	L->y0 = 0;
	L->y1 = a->win.bh;
	bool wait = modal_card(a);
	const struct wlrail_item it[RB_N] = {
		[RB_NEW] = { .tip = "A new world  Ctrl+N", .icon = wlrail_ic_new, .base = lm_theme.btn_teal, .lite = lm_theme.btn_teal_lit },
		[RB_OPEN] = { .tip = "Open a world  Ctrl+O", .icon = wlrail_ic_open, .base = lm_theme.btn_blue, .lite = lm_theme.btn_blue_lit },
		[RB_SAVE] = { .tip = "Save  Ctrl+S", .icon = wlrail_ic_save, .base = lm_theme.btn_green, .lite = lm_theme.btn_green_lit },
		[RB_SAVEAS] = { .tip = "Save as  Ctrl+Shift+S", .icon = wlrail_ic_saveas, .base = lm_theme.btn_olive, .lite = lm_theme.btn_olive_lit },
		[RB_PLAY] = { .tip = a->playing ? "Pause  Esc" : "Play  Esc", .icon = a->playing ? wlrail_ic_pause : ic_play, .base = lm_theme.btn_amber, .lite = lm_theme.btn_amber_lit, .gap = true },
		[RB_CMD] = { .tip = "Command view  Tab", .icon = wlrail_ic_eye, .base = lm_theme.btn_purple, .lite = lm_theme.btn_purple_lit, .lit = a->cmd },
		[RB_BLOCKS] = { .tip = "Blocks  E", .icon = ic_cube, .base = lm_theme.btn_rust, .lite = lm_theme.btn_rust_lit },
		[RB_SOUND] = { .tip = a->sound_on ? "Sound is on" : "Sound is off", .icon = a->sound_on ? ic_speaker : ic_mute, .base = lm_theme.btn_slate, .lite = lm_theme.btn_slate_lit },
		[RB_GOAL] = { .tip = "How to win  G", .icon = ic_flag, .base = lm_theme.btn_maroon, .lite = lm_theme.btn_maroon_lit },
		[RB_HELP] = { .tip = "Help  F1", .text = "?", .base = lm_theme.btn_grey, .lite = lm_theme.btn_slate_lit, .bottom = true },
	};
	for (int i = 0; i < RB_N; i++) {
		L->it[i] = it[i];
		L->it[i].show = true;
		L->it[i].idle = wait;
	}
	L->n = RB_N;
}

/* the rail can be used while the pointer is free: not while it is locked to the game */
static bool rail_usable(const struct app *a)
{
	return !a->playing || a->cmd;
}

/* ---- the panel: everything the game has to say in words, in a window of its own beside the game's ---- */

/* text broken at blanks to `width` pixels, from y down; no further than `bottom`; where the next line would go */
static int panel_wrap(struct wlwin *w, int x, int y, int width, int bottom, const char *text, uint32_t rgb)
{
	while (*text && y + LM_FONT_H <= bottom) {
		char line[220];
		size_t n = 0, cut = 0;
		for (; text[n] && n < sizeof line - 1; n++) {
			line[n] = text[n];
			line[n + 1] = 0;
			if (lm_font_text_width(line) > width) {
				break;
			}
			if (text[n] == ' ')
				cut = n;
		}
		if (text[n] && cut > 0)
			n = cut;
		memcpy(line, text, n);
		line[n] = 0;
		wlwin_text(w, x, y, line, 1, rgb, width);
		y += LM_FONT_H + 2;
		text += n ? n : 1;
		while (*text == ' ')
			text++;
	}
	return y;
}

static int panel_head(struct wlwin *w, int y, const char *s)
{
	wlwin_fill(w, 14, y + 3, PANEL_W - 28, 1, lm_theme.border);
	wlwin_text(w, 14, y + 9, s, 1, lm_theme.accent, PANEL_W - 28);
	return y + 9 + LM_FONT_H + 4;
}

static int panel_line(struct wlwin *w, int y, const char *s, uint32_t rgb)
{
	wlwin_text(w, 14, y, s, 1, rgb, PANEL_W - 28);
	return y + LM_FONT_H + 2;
}

static int panel_bar(struct wlwin *w, int y, float f, uint32_t rgb)
{
	f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
	wlwin_round_fill(w, 14, y + 1, PANEL_W - 28, 8, 4, lm_theme.bar);
	if (f > 0.0f)
		wlwin_round_fill(w, 14, y + 1, LM_MAX(8, (int)((float)(PANEL_W - 28) * f)), 8, 4, rgb);
	return y + 14;
}

static void paint_panel(struct app *a)
{
	struct wlwin *w = &a->win;
	if (!wlwin_pane_begin(w, WLPANE_TOOL, 0, 0, PANEL_W, PANEL_H, w->bw + 6, 0, 14, lm_theme.accent, false))
		return;
	wlwin_round_fill(w, 0, 0, PANEL_W, PANEL_H, 14, lm_theme.accent);
	wlwin_round_fill(w, 1, 1, PANEL_W - 2, PANEL_H - 2, 13, lm_theme.background);
	wlwin_text_bold(w, 14, 10, a->cmd ? "Command view" : "Blocks", 1, lm_theme.text_bright, 0);
	wlwin_text(w, PANEL_W - 14 - lm_font_text_width("drag me   I hides"), 10, "drag me   I hides", 1, lm_theme.text_dim, 0);
	const int bottom = PANEL_H - 10;
	int y = 28;
	char l[200];
	if (a->survival && a->rival >= 1) {
		y = panel_head(w, y, "Goal");
		goal_theirs(a, l, sizeof l);
		y = panel_line(w, y, l, a->rgold < KEEP_GOLD ? 0x90ff90u : lm_theme.text);
		snprintf(l, sizeof l, "Your gold %d (a new keep costs %d)", a->inv[BL_GOLD], KEEP_GOLD);
		y = panel_line(w, y, l, a->inv[BL_GOLD] >= KEEP_GOLD ? lm_theme.text : lm_theme.caution);
		const int gi = nearest_gold(a);
		if (gi >= 0) {
			float d;
			const char *to = bearing_to(a, (float)a->deposit[gi][0] + 0.5f, (float)a->deposit[gi][2] + 0.5f, &d);
			snprintf(l, sizeof l, "Gold: %d pieces %.0f blocks, %s", a->ore[gi], d, to);
		} else {
			snprintf(l, sizeof l, "There is no gold left in the world");
		}
		y = panel_line(w, y, l, 0xe8c040u);
		snprintf(l, sizeof l, "Next: %s", next_step(a));
		y = panel_wrap(w, 14, y, PANEL_W - 28, PANEL_H - 10, l, lm_theme.accent) + 2;
	}
	if (a->survival && a->have_keep) {
		if (a->keep_down)
			snprintf(l, sizeof l, "Your keep rises in %.0f s     Nights %d", ceilf(a->keep_back), a->nights);
		else
			snprintf(l, sizeof l, "Your keep %d of %d      Nights %d", (int)ceilf(a->keep_hp), (int)KEEP_HP, a->nights);
		y = panel_head(w, y, "Keep");
		y = panel_line(w, y, l, lm_theme.text);
		float f = a->keep_hp / KEEP_HP;
		y = panel_bar(w, y, f, f > 0.5f ? 0x4cb050u : f > 0.25f ? 0xe0a030u : 0xe04030u);
		if (a->mobs.wave_on && bl_mobs_wave_remaining(&a->mobs) > 0) {
			int br = bl_mobs_count_brutes(&a->mobs) + a->mobs.wave_brutes;
			if (br > 0)
				snprintf(l, sizeof l, "Wave: %d goblins left, %d of them brute%s", bl_mobs_wave_remaining(&a->mobs), br, br == 1 ? "" : "s");
			else
				snprintf(l, sizeof l, "Wave: %d goblins left", bl_mobs_wave_remaining(&a->mobs));
			y = panel_line(w, y, l, 0xff9a70u);
		}
	}
	snprintf(l, sizeof l, "Workers %d   Soldiers %d", nworkers(a), nsoldiers(a));
	y = panel_line(w, y + 2, l, lm_theme.text_dim);
	if (!a->cmd)
		y = panel_line(w, y, "Tab: the command view", lm_theme.text_dim);
	if (a->cmd) {
		snprintf(l, sizeof l, "Chosen %d   Being built %d", cmd_selected(a), bl_mobs_count_sites(&a->mobs));
		y = panel_line(w, y, l, lm_theme.text_dim);
	}
	if (a->rival == 1 && a->rival_on) {
		float dist;
		const char *way = rival_bearing(a, &dist);
		y = panel_head(w, y, RIVAL_NAME);
		snprintf(l, sizeof l, "Camp %.0f blocks, %s", dist, way);
		y = panel_line(w, y, l, lm_theme.text);
		snprintf(l, sizeof l, "Their keep %.0f of %.0f      Material %d", a->rhp, RIVAL_HP, a->rstock);
		y = panel_line(w, y, l, lm_theme.text_dim);
		y = panel_bar(w, y, a->rhp / RIVAL_HP, 0xd02828u);
		{
			const int ci = bl_mobs_chief(&a->mobs);
			if (ci >= 0)
				snprintf(l, sizeof l, "%s %.0f: %s, %s", CHIEF_NAME, ceilf(a->mobs.mob[ci].hp), a->mobs.mob[ci].raider ? "raiding" : "at camp", bl_weapon(a->mobs.mob[ci].weapon)->name);
			else
				snprintf(l, sizeof l, "%s is away: back in %.0f s", CHIEF_NAME, ceilf(fmaxf(a->chief_t, 0.0f)));
			y = panel_line(w, y, l, 0xff8a7au);
		}
		int n = 0;
		float near = 1e9f, nx = 0.0f, nz = 0.0f;
		for (int i = 0; i < BL_MOBS_MAX; i++) {
			const struct bl_mob *m = &a->mobs.mob[i];
			if (!m->alive || !m->raider)
				continue;
			n++;
			float d = hypotf(m->x - a->pl.x, m->z - a->pl.z);
			if (d < near) {
				near = d;
				nx = m->x;
				nz = m->z;
			}
		}
		if (n) {
			const char *to = bearing_to(a, nx, nz, &near);
			snprintf(l, sizeof l, "Raiders %d: %.0f blocks, %s", n, near, to);
			y = panel_line(w, y, l, 0xffb040u);
		}
	}
	int list[BL_COUNT];
	const int n = stock_list(a, list);
	if (n) {
		y = panel_head(w, y, "You have");
		const int rows = LM_MIN((n + 1) / 2, a->cmd ? 3 : 5);
		for (int i = 0; i < n && i < rows * 2; i++) {
			int b = list[i], cx = 14 + (i % 2) * ((PANEL_W - 28) / 2), cy = y + (i / 2) * (LM_FONT_H + 2), cw = (PANEL_W - 28) / 2 - 12;
			snprintf(l, sizeof l, "%d", a->inv[b] > 99999 ? 99999 : a->inv[b]);
			wlwin_round_fill(w, cx, cy + 3, 11, 11, 3, bl_block_color(b) & 0xffffffu);
			wlwin_text(w, cx + 18, cy + 1, bl_block_name(b), 1, a->inv[b] ? lm_theme.text_soft : lm_theme.text_dim, cw - 60);
			wlwin_text(w, cx + cw - lm_font_text_width(l), cy + 1, l, 1, a->inv[b] ? lm_theme.text_bright : lm_theme.text_dim, 0);
		}
		y += rows * (LM_FONT_H + 2);
	}
	if (a->debug) {
		y = panel_head(w, y, "Numbers (F3)");
		snprintf(l, sizeof l, "x %.1f  y %.1f  z %.1f      seed %u", a->pl.x, a->pl.y, a->pl.z, a->world->seed);
		y = panel_line(w, y, l, lm_theme.text_dim);
		snprintf(l, sizeof l, "%d frames a second, %dx%d in %.0f ms", a->fps, a->rw, a->rh, a->ms_render);
		y = panel_line(w, y, l, lm_theme.text_dim);
		if (a->aimed)
			snprintf(l, sizeof l, "aiming at %s (%d %d %d)", bl_block_name(bl_get(a->world, a->aim.x, a->aim.y, a->aim.z)), a->aim.x, a->aim.y, a->aim.z);
		else
			snprintf(l, sizeof l, "aiming at nothing");
		y = panel_line(w, y, l, lm_theme.text_dim);
	}
	static const char *const k_keys[][2] = {
		{ "Left", "choose; box; Shift adds" }, { "Right", "go, or break (1 / 2)" }, { "3 - 0", "build; R turns it" }, { "Delete", "give a plan up" },
		{ "B / V", "a worker / a soldier" }, { "WASD", "move; arrows turn" }, { "Wheel", "nearer, farther" }, { "Home / End", "to him / their camp" }, { "Tab, Esc", "back to him" },
	};
	const int nkeys = (int)(sizeof k_keys / sizeof k_keys[0]);
	const int msg_bottom = a->cmd ? bottom - (nkeys * (LM_FONT_H + 2) + LM_FONT_H + 13) : bottom;
	if (a->nlog && y + 2 * LM_FONT_H + 16 <= msg_bottom) {
		y = panel_head(w, y, "Messages");
		const int64_t now = now_ms();
		for (int i = 0; i < a->nlog && y + LM_FONT_H <= msg_bottom; i++) {
			const struct logline *g = &a->log[i];
			const int64_t age = (now - g->ms) / 1000;
			char when[24];
			if (age < 10)
				snprintf(when, sizeof when, "now");
			else if (age < 60)
				snprintf(when, sizeof when, "%d s", (int)(age / 10 * 10));
			else
				snprintf(when, sizeof when, "%d min", (int)(age / 60));
			wlwin_text(w, PANEL_W - 14 - lm_font_text_width(when), y, when, 1, lm_theme.text_dim, 0);
			if (g->times > 1)
				snprintf(l, sizeof l, "%s (%d times)", g->text, g->times);
			else
				snprintf(l, sizeof l, "%s", g->text);
			y = panel_wrap(w, 14, y, PANEL_W - 28 - 52, msg_bottom, l, age < 10 ? lm_theme.text_bright : lm_theme.text_soft) + 3;
		}
	}
	if (a->cmd) {                                                /* the orders keep their place at the bottom: the messages end above them */
		y = panel_head(w, msg_bottom, a->order_mode == 1 ? "Orders: break that block and its kind" : "Orders: go there");
		for (int i = 0; i < nkeys && y + LM_FONT_H <= bottom; i++) {
			wlwin_text(w, 14, y, k_keys[i][0], 1, lm_theme.text_bright, 90);
			wlwin_text(w, 108, y, k_keys[i][1], 1, lm_theme.text_dim, PANEL_W - 122);
			y += LM_FONT_H + 2;
		}
	}
	wlwin_pane_end(w);
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
		render_size(w->bw * wlwin_scale(w), w->bh * wlwin_scale(w), a->budget, &a->rw, &a->rh);
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
	if (a->cmd) {                                                          /* the command view: from above */
		cmd_camera(a, &cam);
		eye[0] = cam.x;
		eye[1] = cam.y;
		eye[2] = cam.z;
	}
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
	bool flies = false;
	for (int i = 0; i < BL_ARROWS_MAX; i++)
		flies |= a->mobs.arrow[i].t > 0.0f;
	if (bl_mobs_count(&a->mobs) > 0 || flies) {
		bl_mobs_light(&a->mobs, a->world, a->time_h);
		v.mobs = &a->mobs;
	}
	a->hover = false;
	if (a->cmd && a->has_pointer) {                                        /* the block under the pointer gets the dark rim */
		float dir[3], ce[3] = { cam.x, cam.y, cam.z };
		cmd_ray(a, &cam, a->px, a->py, dir);
		struct bl_hit hh;
		if (bl_pick(a->world, ce, dir, 200.0f, &hh)) {
			a->hover = true;
			a->hover_b[0] = hh.x;
			a->hover_b[1] = hh.y;
			a->hover_b[2] = hh.z;
			v.target = true;
			v.tx = hh.x;
			v.ty = hh.y;
			v.tz = hh.z;
		}
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
	bl_ghosts_clear(&a->ghosts);
	for (int i = 0; i < BL_SITES_MAX; i++)
		if (a->mobs.site[i].on)
			bl_ghosts_add(&a->ghosts, a->world, &a->mobs.site[i], 0x80c8ffu);
	if (a->cmd && a->plan >= 0 && a->prompt == P_NONE && a->has_pointer && bar_at(a, a->px, a->py) < 0) {    /* the preview: green where it can be built, red where not */
		struct bl_site s;
		const char *why = NULL;
		bool any, ok = plan_at_pointer(a, &s, &why, &any);
		if (any)
			bl_ghosts_add(&a->ghosts, NULL, &s, ok ? 0x50f070u : 0xff4030u);
	}
	if (a->ghosts.n)
		v.ghosts = &a->ghosts;
	if (a->body) {
		hero_pose(a, &cam);
		v.hero = &a->hero;
	}
	bl_render(a->world, &cam, &v, a->frame, a->rw, a->rh, a->rw, a->threads);
	if (a->hurt_t > 0.0f || a->hp <= 0.0f)
		red_flash(a);
	int64_t t1 = now_ms();
	a->ms_render = (double)(t1 - t0);
	if (a->fix_w <= 0 && a->prompt == P_NONE)
		a->budget = budget_next(a->budget, a->ms_render, (double)w->bw * (double)w->bh * wlwin_scale(w) * wlwin_scale(w));
	wlwin_blit(w, 0, 0, w->bw, w->bh, a->frame, a->rw, a->rh, a->rw);

	struct lay L;
	layout(a, &L);
	if (!a->cmd || a->prompt != P_NONE)
		paint_hotbar(a, &L);
	paint_hearts(a, &L);
	if (a->survival) {
		paint_survival(a);
		if (!a->cmd && a->prompt == P_NONE)
			paint_weapon(a, &L);
	}
	if (!panel_on(a)) {
		paint_stock(a);
		paint_rival(a);
	}
	if (a->cmd && a->prompt == P_NONE)
		paint_cmd(a, &cam);
	if (a->prompt == P_NONE && !a->cmd)
		paint_crosshair(w, a->hit_t > 0.0f);
	if (a->debug && !panel_on(a))
		paint_debug(a);
	if (a->msg[0]) {
		if (!panel_on(a))
			shadow_text(w, (w->bw - lm_font_text_width(a->msg)) / 2, a->survival ? 64 : 26, a->msg, 0xffffffu);   /* (under the keep's bar in survival) */
		if (t1 - a->msg_ms > 3000)
			a->msg[0] = 0;
	}
	if (a->prompt != P_NONE)
		paint_card(a);
	wlwin_pane_end(w);
	{
		struct wlrail rail;
		rail_build(a, &rail);
		wlrail_paint(w, &rail, &a->ran, rail_usable(a) && !modal_card(a) && a->has_pointer ? a->rail_hover : -1, wlrail_now(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text_soft);
	}
	wlwin_pane_through(w, WLPANE_TOOL, a->playing && !a->cmd);               /* (it may lie over the window, and the pointer is to be locked to that) */
	if (panel_on(a))
		paint_panel(a);
	a->fps_n++;
	if (t1 - a->fps_t0 >= 1000) {
		a->fps = (int)((int64_t)a->fps_n * 1000 / (t1 - a->fps_t0));
		a->fps_n = 0;
		a->fps_t0 = t1;
	}
	write_dump(a);
}

/* ================================================================ the pointer and the keys ================================================================ */

/* Puts up the new / open / save as card; the game waits, and Cancel goes back to where it came from. */
static void open_dialog(struct app *a, enum prompt p)
{
	if (a->prompt == P_DEAD || a->prompt == P_LOST)
		a->back = a->prompt;
	else if (a->prompt != P_NEW && a->prompt != P_OPEN && a->prompt != P_SAVEAS && a->prompt != P_REPLACE)
		a->back = P_PAUSE;
	if (a->playing) {
		pause_game(a, p);
		save_all(a, false);
	} else {
		a->prompt = p;
		a->cmd = false;
	}
	a->pc_hover = a->pc_down = -1;
	a->card_msg[0] = 0;
	if (p == P_NEW) {
		a->gname[0] = 0;
		a->new_survival = true;
		wltext_end(a->gname);
	}
	if (p == P_OPEN || p == P_SAVEAS) {
		mkdirs(a->dir);
		a->field[0] = 0;
		if (p == P_SAVEAS && a->path[0]) {                  /* the name it has, chosen: typing replaces it */
			const char *sl = strrchr(a->path, '/');
			snprintf(a->field, sizeof a->field, "%s", sl ? sl + 1 : a->path);
		}
		wltext_end(a->field);
		if (a->field[0])
			wltext_select_all(a->field);
		wlpicker_reset(&a->pk);                             /* (the folder is read again) */
	}
	a->win.redraw = true;
}

/* the Save button and Ctrl+S: a world with no file is asked for one */
static void do_save(struct app *a)
{
	if (a->nosave || !a->path[0])
		open_dialog(a, P_SAVEAS);
	else
		save_all(a, true);
}

static void card_close(struct app *a, enum prompt p)
{
	a->prompt = p;
	a->pc_hover = a->pc_down = -1;
	a->pk.sb_drag = -1;
}

static void card_action(struct app *a, int which)
{
	if (a->prompt == P_LOST) {                                  /* the game is over: a new one (the lost one stays in its file) */
		open_dialog(a, P_NEW);
		return;
	}
	if (a->prompt == P_DEAD) {
		respawn(a);
		resume_game(a);
		return;
	}
	if (a->prompt == P_PALETTE || a->prompt == P_WON || a->prompt == P_GOAL || a->prompt == P_ARMS) {
		resume_game(a);
		return;
	}
	if (a->prompt == P_PAUSE) {
		if (which == 0) {
			save_all(a, false);
			a->win.running = false;
		} else if (which == 1) {
			do_save(a);
		} else if (which == 2) {
			open_dialog(a, P_NEW);
		} else {
			resume_game(a);
		}
		return;
	}
	if (a->prompt == P_NEW) {
		if (which == 0) {
			if (a->new_first)
				a->win.running = false;
			else
				card_close(a, a->back);
			return;
		}
		char path[1024];
		const char *why = NULL;
		if (!game_path(a, a->gname, path, sizeof path, &why)) {
			snprintf(a->card_msg, sizeof a->card_msg, "%s", why);
			return;
		}
		card_close(a, P_PAUSE);
		a->new_first = false;
		new_world(a, path, a->new_survival);
		if (a->survival)
			pause_game(a, P_GOAL);
		else
			resume_game(a);
		return;
	}
	if (a->prompt == P_OPEN) {
		if (which == 0) {
			card_close(a, a->back);
			return;
		}
		size_t l = strlen(a->field);
		char base[1024], full[1100];
		struct stat st;
		card_base(a, base, sizeof base);
		lm_resolve_path(base, a->field, full, sizeof full);
		if (l && a->field[l - 1] != '/' && l + 2 < sizeof a->field && stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {    /* a folder: go into it */
			strcat(a->field, "/");
			wltext_end(a->field);
			wlpicker_typed(&a->pk);
			return;
		}
		if (world_open(a))
			card_close(a, P_PAUSE);
		return;
	}
	if (a->prompt == P_SAVEAS) {
		if (which == 0) {
			card_close(a, a->back);
			return;
		}
		char path[1024];
		const char *why = NULL;
		if (!field_path(a, true, path, sizeof path, &why)) {
			snprintf(a->card_msg, sizeof a->card_msg, "%s", why);
			return;
		}
		if (access(path, F_OK) == 0 && !(a->path[0] && !a->nosave && same_file(path, a->path))) {
			snprintf(a->target, sizeof a->target, "%s", path);
			card_close(a, P_REPLACE);
			return;
		}
		if (world_save_to(a, path))
			card_close(a, a->back == P_LOST || a->back == P_DEAD ? a->back : P_PAUSE);
		return;
	}
	if (a->prompt == P_REPLACE) {
		if (which == 1 && world_save_to(a, a->target))
			card_close(a, a->back == P_LOST || a->back == P_DEAD ? a->back : P_PAUSE);
		else
			card_close(a, P_SAVEAS);                            /* (the field is as it was) */
		return;
	}
	card_close(a, P_PAUSE);                                     /* the keys card closes back to the pause card */
}

static void rail_action(struct app *a, int id)
{
	if (modal_card(a))
		return;
	switch (id) {
	case RB_NEW: open_dialog(a, P_NEW); break;
	case RB_OPEN: open_dialog(a, P_OPEN); break;
	case RB_SAVE: do_save(a); break;
	case RB_SAVEAS: open_dialog(a, P_SAVEAS); break;
	case RB_PLAY:
		if (a->prompt == P_PAUSE) {
			resume_game(a);
		} else {
			pause_game(a, P_PAUSE);
			save_all(a, false);
		}
		break;
	case RB_CMD:
		if (a->cmd) {
			leave_cmd(a);
		} else {
			if (!a->playing)
				resume_game(a);
			enter_cmd(a);
		}
		break;
	case RB_BLOCKS: pause_game(a, P_PALETTE); break;
	case RB_SOUND:
		a->sound_on = !a->sound_on;
		say(a, a->sound_on ? "Sound on" : "Sound off");
		break;
	case RB_GOAL: pause_game(a, P_GOAL); break;
	case RB_HELP:
		pause_game(a, P_HELP);
		wlhelp_open(&a->help);
		break;
	default: break;
	}
	a->win.redraw = true;
}

/* the pointer on the open / save as card */
static void file_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
	struct wlcard c;
	int fx, fy, fw, ly, rows;
	file_layout(a, &c, &fx, &fy, &fw, &ly, &rows);
	char base[1024];
	card_base(a, base, sizeof base);
	if (op == LM_WL_POINTER_EVT_AXIS) {
		if (args[1].u == 0)
			wlpicker_wheel(&a->pk, lm_fixed_to_double(args[2].f) > 0 ? 1 : -1);
	} else if (op == LM_WL_POINTER_EVT_MOTION) {
		if (!wlcard_drag_motion(&a->win, &c, a->px, a->py)) {
			wlpicker_motion(&a->pk, base, a->field, ly, rows, a->py);
			a->pc_hover = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
		}
	} else if (op == LM_WL_POINTER_EVT_BUTTON) {
		uint32_t button = args[2].u, state = args[3].u;
		int k = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
		bool on_field = a->px >= fx && a->px < fx + fw && a->py >= fy && a->py < fy + 28;
		a->clip.serial = args[0].u;
		if (button == 273 && state == 1 && on_field) {
			wlwin_menu(&a->win, wltext_menu(a->field));
		} else if (button != 272) {
			return;
		} else if (state == 0) {
			wlcard_drag_release(&a->win);
			wlpicker_release(&a->pk);
			if (a->pc_down >= 0 && a->pc_down == k)
				card_action(a, k);
			a->pc_down = -1;
		} else if (k >= 0) {
			a->pc_down = k;
		} else if (wlcard_drag_press(&a->win, &c, a->px, a->py)) {
			;
		} else if (on_field) {
			wltext_click(a->field, (int)a->px - (fx + 12), (a->win.mods & LM_MOD_SHIFT) != 0);
		} else if (rows && wlpicker_inside(fx, ly, fw, rows, a->px, a->py)) {
			a->card_msg[0] = 0;
			if (wlpicker_press(&a->pk, base, a->field, sizeof a->field, fx, ly, fw, rows, a->px, a->py, args[1].u, (uint32_t)lm_metrics.double_click_ms) == WLPICKER_OPEN)
				card_action(a, 1);
		} else if (!wlcard_has(&c, (int)a->px, (int)a->py)) {
			card_action(a, 0);
		}
	}
	a->win.redraw = true;
}

/* the pointer on the card of a new game */
static void name_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
	struct wlcard c;
	int fx, fy, fw, my, mw;
	name_layout(a, &c, &fx, &fy, &fw, &my, &mw);
	if (op == LM_WL_POINTER_EVT_MOTION) {
		if (!wlcard_drag_motion(&a->win, &c, a->px, a->py))
			a->pc_hover = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
	} else if (op == LM_WL_POINTER_EVT_BUTTON) {
		uint32_t button = args[2].u, state = args[3].u;
		int k = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
		bool on_field = a->px >= fx && a->px < fx + fw && a->py >= fy && a->py < fy + 28;
		a->clip.serial = args[0].u;
		if (button == 273 && state == 1 && on_field) {
			wlwin_menu(&a->win, wltext_menu(a->gname));
		} else if (button != 272) {
			return;
		} else if (state == 0) {
			wlcard_drag_release(&a->win);
			if (a->pc_down >= 0 && a->pc_down == k)
				card_action(a, k);
			a->pc_down = -1;
		} else if (k >= 0) {
			a->pc_down = k;
		} else if (wlcard_drag_press(&a->win, &c, a->px, a->py)) {
			;
		} else if (on_field) {
			wltext_click(a->gname, (int)a->px - (fx + 12), (a->win.mods & LM_MOD_SHIFT) != 0);
		} else if (a->py >= my && a->py < my + MODE_H && a->px >= fx && a->px < fx + fw) {
			a->new_survival = a->px < fx + mw + 5;
		}
	}
	a->win.redraw = true;
}

/* the keys of the card of a new game: the name's field has them */
static void name_key(struct app *a, uint32_t code)
{
	uint32_t ks = lm_keymap_keysym(code, a->win.mods);
	if (ks == LM_KS_Escape) {
		if (!a->new_first)                                      /* (at the first start there is nothing to go back to) */
			card_action(a, 0);
	} else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
		card_action(a, 1);
	} else if (ks == LM_KS_Tab) {
		a->new_survival = !a->new_survival;
	} else if (wltext_edit(&a->clip, a->gname, sizeof a->gname, ks, a->win.mods)) {
		a->card_msg[0] = 0;
	}
}

/* the keys of the open / save as card */
static void file_key(struct app *a, uint32_t code)
{
	uint32_t ks = lm_keymap_keysym(code, a->win.mods);
	char base[1024];
	card_base(a, base, sizeof base);
	int rows = wlpicker_fit(a->win.bh, 180);
	if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down || ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->pk.sel >= 0)) {
		if (wlpicker_key(&a->pk, base, a->field, sizeof a->field, rows, ks) == WLPICKER_OPEN)
			card_action(a, 1);
	} else if (ks == LM_KS_Escape) {
		card_action(a, 0);
	} else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
		card_action(a, 1);
	} else if (ks == LM_KS_Tab) {
		char list[160];
		int n = lm_complete_path(base, a->field, sizeof a->field, true, list, sizeof list);
		if (n == 0)
			snprintf(a->card_msg, sizeof a->card_msg, "Nothing here starts like that");
		else if (n > 1)
			snprintf(a->card_msg, sizeof a->card_msg, "%d matches: %s", n, list);
		else
			a->card_msg[0] = 0;
		wltext_end(a->field);
		wlpicker_typed(&a->pk);
	} else if (wltext_edit(&a->clip, a->field, sizeof a->field, ks, a->win.mods)) {
		wlpicker_typed(&a->pk);                             /* (the list follows what is typed) */
		a->card_msg[0] = 0;
	}
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
	if (file_card(a)) {
		file_pointer(a, op, args);
		return;
	}
	if (a->prompt == P_NEW) {
		name_pointer(a, op, args);
		return;
	}
	struct wlcard c;
	int nb;
	card_rect(a, &c, &nb);
	if (a->prompt == P_HELP && op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
		wlhelp_scroll(&a->help, &c, lm_fixed_to_double(args[2].f) > 0 ? WLHELP_LH * 3 : -WLHELP_LH * 3);
		a->win.redraw = true;
		return;
	}
	if (op == LM_WL_POINTER_EVT_MOTION) {
		if (wlcard_drag_motion(&a->win, &c, a->px, a->py))
			return;
		if (a->prompt == P_HELP)
			wlhelp_motion(&a->help, &c, a->px, a->py);
		a->pc_hover = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
		a->pal_hover = a->prompt == P_ARMS ? arms_at(a, a->px, a->py) : pal_cell_at(a, a->px, a->py);
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
		const int arm = arms_at(a, a->px, a->py);
		if (arm >= 0) {
			arms_choose(a, arm);
			a->win.redraw = true;
			return;
		}
		if (k >= 0)
			a->pc_down = k;
		else if (wlcard_drag_press(&a->win, &c, a->px, a->py))
			;
		else if (a->prompt == P_HELP && wlhelp_press(&a->help, &c, a->px, a->py))
			;
		else if (a->prompt == P_PAUSE) {                     /* a click anywhere else goes on playing */
			resume_game(a);
			a->ignore_release = true;
		} else if (!wlcard_has(&c, (int)a->px, (int)a->py) && a->prompt != P_DEAD && a->prompt != P_LOST) {
			card_action(a, 0);
		}
	} else {
		wlcard_drag_release(&a->win);
		if (a->prompt == P_HELP && a->pc_down < 0)
			wlhelp_release(&a->help, &c, a->px, a->py);
		else if (a->pc_down >= 0 && a->pc_down == k)
			card_action(a, k);
		if (a->prompt == P_HELP)
			a->help.grab = -1;
		a->pc_down = -1;
	}
	a->win.redraw = true;
}

/* the pointer in the command view: the left button chooses (a click, or a box pulled over the picture), the right button orders, the wheel zooms */
static void cmd_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
	if (op == LM_WL_POINTER_EVT_BUTTON) {
		uint32_t button = args[2].u, state = args[3].u;
		int bar = bar_at(a, a->px, a->py);
		if (button == 272 && state == 1 && bar >= 0) {
			choose_plan(a, bar);
		} else if (button == 272 && state == 1 && a->plan >= 0) {
			build_place(a);
		} else if (button == 273 && state == 1 && a->plan >= 0) {
			a->plan = -1;
			say(a, "Nothing is being placed");
		} else if (button == 273 && bar >= 0) {
			;
		} else if (button == 272 && state == 1) {
			a->drag_sel = true;
			a->drag_x = a->px;
			a->drag_y = a->py;
		} else if (button == 272 && state == 0 && a->drag_sel) {
			a->drag_sel = false;
			if (fabs(a->px - a->drag_x) < 6.0 && fabs(a->py - a->drag_y) < 6.0)
				cmd_select_click(a, a->k_sneak);
			else
				cmd_select_box(a, a->drag_x, a->drag_y, a->px, a->py, a->k_sneak);
		} else if (button == 273 && state == 1) {
			cmd_order(a);
		}
	} else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
		a->wheel += lm_fixed_to_double(args[2].f);
		if (fabs(a->wheel) >= 10.0) {
			a->cam_dist *= a->wheel > 0 ? 1.18f : 1.0f / 1.18f;
			a->cam_dist = a->cam_dist < 7.0f ? 7.0f : a->cam_dist > 70.0f ? 70.0f : a->cam_dist;
			a->wheel = 0;
		}
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
		if (a->playing && !a->cmd)
			wlwin_hide_cursor(w, true);
		w->redraw = true;
		return;
	case LM_WL_POINTER_EVT_LEAVE:
		a->has_pointer = false;
		a->pc_hover = -1;
		a->rail_hover = -1;
		wlrail_release(&a->ran);
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
	{                                                           /* the rail at the left (its pointer is in window pixels) */
		struct wlrail rail;
		rail_build(a, &rail);
		bool over = a->has_pointer && w->rpx < WLRAIL_W && rail_usable(a);
		int at = over ? wlrail_at(&rail, w->rpx, w->rpy) : -1;
		if (at != a->rail_hover) {
			a->rail_hover = at;
			w->redraw = true;
		}
		if (op == LM_WL_POINTER_EVT_BUTTON && args[3].u == 0)
			wlrail_release(&a->ran);
		if (over && !a->win.card_drag && ((op == LM_WL_POINTER_EVT_BUTTON && args[3].u == 1) || op == LM_WL_POINTER_EVT_AXIS)) {
			if (op == LM_WL_POINTER_EVT_BUTTON && args[2].u == 272 && at >= 0 && !modal_card(a)) {
				wlrail_press(&a->ran, at, wlrail_now());
				rail_action(a, at);
			}
			w->redraw = true;
			return;
		}
	}
	if (a->prompt != P_NONE) {
		card_pointer(a, op, args);
		return;
	}
	if (a->cmd) {
		cmd_pointer(a, op, args);
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
				a->dig_wait = 0.0f;                             /* (a new press begins at once) */
				if (a->survival)
					dig_step(a, now, 0.0f);                    /* (the sword at once; a block takes its time) */
				else
					do_break(a, now);
				a->next_break = now + REPEAT_FIRST_MS;
			} else {
				a->dig_on = false;
				a->dig_t = 0.0f;
			}
		} else if (button == 273) {
			a->b_place = state == 1;
			if (state == 1) {
				do_place(a);
				a->next_place = now + REPEAT_FIRST_MS;
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
	if (!a->playing || a->cmd)
		return;
	look_by(a, dx * a->sens, -dy * a->sens);
	w->redraw = true;
}

static void lock_event(struct wlwin *w, bool locked)
{
	struct app *a = w->data;
	if (!locked && a->playing && !a->cmd)                   /* the focus went elsewhere: the game waits */
		pause_game(a, P_PAUSE);
	w->redraw = true;
}

static void keys_release(struct app *a)
{
	a->k_fwd = a->k_back = a->k_left = a->k_right = a->k_jump = a->k_sneak = a->k_ctrl = a->k_lk = a->k_rk = a->k_uk = a->k_dk = false;
	a->b_break = a->b_place = false;
}

/* keys of the command view; true = used up here */
static bool cmd_key(struct app *a, uint32_t code)
{
	if (code >= KEY_PLAN0 && code < KEY_PLAN0 + BL_PLAN_COUNT) {
		choose_plan(a, (int)code - KEY_PLAN0);
		return true;
	}
	switch (code) {
	case K_G: case K_I: case K_E: case K_F1: case K_F3: case K_X:
		return false;
	case K_TAB:
		leave_cmd(a);
		return true;
	case K_ESC:
		if (a->plan >= 0) {                                     /* first the plan is put aside, then the view */
			a->plan = -1;
			say(a, "Nothing is being placed");
		} else {
			leave_cmd(a);
		}
		return true;
	case K_R:
		a->plan_rot = (a->plan_rot + 1) % 4;
		return true;
	case K_DEL:
		build_cancel(a);
		return true;
	case K_B:
		train_worker(a);
		return true;
	case K_V:
		train_soldier(a);
		return true;
	case K_1:
		a->order_mode = 0;
		a->plan = -1;
		say(a, "Right button: go there");
		return true;
	case K_1 + 1:
		a->order_mode = 1;
		a->plan = -1;
		say(a, "Right button: break that block and the next of its kind");
		return true;
	case K_BACKSPACE:
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (a->mobs.mob[i].alive && bl_mob_is_unit(a->mobs.mob[i].kind) && a->mobs.mob[i].selected)
				bl_mobs_order_stop(&a->mobs, i);
		say(a, "Stopped");
		return true;
	case K_END:
		if (a->rival == 1 && a->rival_on) {
			a->cam_f[0] = (float)a->rkeep[0] + 0.5f;
			a->cam_f[1] = (float)a->rkeep[1];
			a->cam_f[2] = (float)a->rkeep[2] + 0.5f;
		}
		return true;
	case K_HOME: {
		float cx, cz;
		worker_home(a, &cx, &cz);
		a->cam_f[0] = cx;
		a->cam_f[2] = cz;
		a->cam_f[1] = (float)bl_top(a->world, (int)floorf(cx), (int)floorf(cz)) + 1.0f;
		return true;
	}
	default:
		return true;
	}
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
	if (down)
		a->clip.serial = args[0].u;
	if (down && (file_card(a) || a->prompt == P_NEW) && !((w->mods & LM_MOD_CTRL) && code == K_Q)) {     /* the keys are the text field's */
		if (a->prompt == P_NEW)
			name_key(a, code);
		else
			file_key(a, code);
		w->redraw = true;
		return;
	}
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
	if (ctrl && code == K_A && a->cmd && a->prompt == P_NONE) {
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (a->mobs.mob[i].alive && bl_mob_is_unit(a->mobs.mob[i].kind))
				a->mobs.mob[i].selected = true;
		return;
	}
	if (ctrl) {
		if (code == K_Q)
			w->running = false;
		else if (code == K_S && (w->mods & LM_MOD_SHIFT))
			open_dialog(a, P_SAVEAS);
		else if (code == K_S)
			do_save(a);
		else if (code == K_O)
			open_dialog(a, P_OPEN);
		else if (code == K_N)
			open_dialog(a, P_NEW);
		return;
	}
	if (a->prompt == P_REPLACE) {                               /* (Enter does not replace by accident: Y or the button does) */
		if (code == K_ESC || code == K_ENTER)
			card_action(a, 0);
		else if (code == K_Y)
			card_action(a, 1);
		return;
	}
	if (a->prompt == P_PALETTE) {
		if (code == K_ESC || code == K_E || code == K_ENTER)
			resume_game(a);
		else if (code >= K_1 && code <= K_9)
			choose_slot(a, (int)(code - K_1));
		return;
	}
	if (a->prompt == P_ARMS) {
		if (code == K_ESC || code == K_R || code == K_ENTER)
			resume_game(a);
		else if (code >= K_1 && code < K_1 + BL_WP_COUNT)
			arms_choose(a, (int)(code - K_1));
		return;
	}
	if (a->prompt == P_GOAL) {
		if (code == K_ESC || code == K_ENTER || code == K_SPACE || code == K_G)
			resume_game(a);
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
	if (a->prompt == P_HELP) {
		struct wlcard hc;
		int hnb;
		card_rect(a, &hc, &hnb);
		switch (code) {
		case K_ESC: case K_ENTER: case K_F1:
			card_action(a, 0);                                  /* back to the pause card */
			break;
		case K_LEFT: wlhelp_nav(&a->help, &hc, WLHELP_LEFT, 0); break;
		case K_RIGHT: wlhelp_nav(&a->help, &hc, WLHELP_RIGHT, 0); break;
		case K_UP: wlhelp_nav(&a->help, &hc, WLHELP_UP, 0); break;
		case K_DOWN: wlhelp_nav(&a->help, &hc, WLHELP_DOWN, 0); break;
		case K_PGUP: wlhelp_nav(&a->help, &hc, WLHELP_PGUP, 0); break;
		case K_PGDN: case K_SPACE: wlhelp_nav(&a->help, &hc, WLHELP_PGDN, 0); break;
		case K_HOME: wlhelp_nav(&a->help, &hc, WLHELP_HOME, 0); break;
		case K_END: wlhelp_nav(&a->help, &hc, WLHELP_END, 0); break;
		default:
			if (code >= K_1 && code <= K_9)
				wlhelp_nav(&a->help, &hc, WLHELP_PAGE, (int)(code - K_1));
			break;
		}
		return;
	}
	if (code == K_I && (a->prompt == P_NONE || a->prompt == P_PAUSE)) {           /* the panel of text, or the text over the picture */
		a->panel = !a->panel;
		say(a, !wlwin_panes(&a->win) ? "There are no panels here: the text stays over the picture" : a->panel ? "The text is in its panel (drag it anywhere)" : "The text is over the picture");
		return;
	}
	if (a->prompt == P_PAUSE && code == K_F1) {                  /* the pages from the pause card, too */
		a->prompt = P_HELP;
		a->pc_hover = a->pc_down = -1;
		wlhelp_open(&a->help);
		return;
	}
	if (a->prompt != P_NONE) {
		if (code == K_ESC) {
			if (a->prompt == P_PAUSE)
				resume_game(a);
			else if (a->prompt == P_WON)
				card_action(a, 0);
			else {
				a->prompt = P_PAUSE;
				a->pc_hover = a->pc_down = -1;
			}
		} else if (code == K_ENTER) {
			if (a->prompt == P_PAUSE)
				resume_game(a);
			else
				card_action(a, 0);
		}
		return;
	}
	if (a->cmd && cmd_key(a, code))
		return;
	if (code == K_TAB && a->playing) {
		enter_cmd(a);
		return;
	}
	if (code == K_ESC) {
		pause_game(a, P_PAUSE);
		save_all(a, false);
		return;
	}
	if (code == K_F1) {
		pause_game(a, P_HELP);
		wlhelp_open(&a->help);
		return;
	}
	if (code == K_E) {
		pause_game(a, P_PALETTE);
		return;
	}
	if (code == K_G && a->playing) {
		pause_game(a, P_GOAL);
		return;
	}
	if (code == K_R && a->playing && !a->cmd) {
		a->msg[0] = 0;
		pause_game(a, P_ARMS);
		return;
	}
	if (code == K_Q && a->playing && !a->cmd) {
		arms_next(a);
		return;
	}
	if (code == K_X && a->playing) {
		if (a->survival)
			craft_hand(a);
		else
			say(a, "Nothing to make in creative: every block is there");
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

static void menu_event(struct wlwin *w, int index)
{
	struct app *a = w->data;
	if (file_card(a))
		wltext_menu_do(&a->clip, a->field, sizeof a->field, index);
	else if (a->prompt == P_NEW)
		wltext_menu_do(&a->clip, a->gname, sizeof a->gname, index);
	w->redraw = true;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t version)
{
	struct app *a = w->data;
	wlclip_global(&a->clip, w, name, iface, version);
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
	CHECK(!save_all(&a, false), "a world with no file is not saved");
	char path[600];
	world_path(&a, a.path, sizeof a.path);
	CHECK(save_all(&a, false), "saved");
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

	/* worlds of their own: new, save as, open */
	{
		static struct app w;
		memset(&w, 0, sizeof w);
		snprintf(w.dir, sizeof w.dir, "%s/worlds", dir);
		w.world_size = 64;
		w.fov = 1.2f;
		w.sens = 0.0025f;
		w.cli_clock = -1;
		memcpy(w.hotbar, k_default_hotbar, sizeof w.hotbar);
		char one[600], two[600], castle[600], got[1024], name[100];
		snprintf(one, sizeof one, "%s/world.bl", w.dir);
		snprintf(two, sizeof two, "%s/world-2.bl", w.dir);
		snprintf(castle, sizeof castle, "%s/My castle.bl", w.dir);
		CHECK(mkdirs(w.dir) == 0 && free_world_path(&w, got, sizeof got) && !strcmp(got, one), "the first world's file is world.bl");
		CHECK(make_world(&w, 5, true), "a flat world");
		state_reset(&w);
		snprintf(w.path, sizeof w.path, "%s", one);
		world_begin(&w, false, 1, -1, -1);
		CHECK(w.survival && w.have_keep && nworkers(&w) == 2, "it starts with a keep and two workers");
		bl_set(w.world, 3, 20, 3, BL_LAMP);
		w.inv[BL_COBBLE] = 7;
		w.nights = 4;
		w.pl.x = 20.5f;
		w.pl.z = 22.5f;
		w.sens = 0.004f;
		CHECK(save_all(&w, false) && free_world_path(&w, got, sizeof got) && !strcmp(got, two), "the next free name is world-2.bl");
		uint32_t seed1 = w.world->seed;
		new_world(&w, NULL, 1);
		CHECK(!strcmp(w.path, two) && access(two, F_OK) == 0 && access(one, F_OK) == 0, "a new world has a file of its own beside the first (%s)", w.path);
		CHECK(w.inv[BL_COBBLE] == 0 && w.nights == 0 && w.world->seed != seed1 && !w.world->kind, "and nothing of the first");
		/* open the first again */
		snprintf(w.field, sizeof w.field, "world");
		CHECK(world_open(&w), "world is opened by its name (%s)", w.card_msg);
		CHECK(!strcmp(w.path, one) && w.world->kind == 1 && bl_get(w.world, 3, 20, 3) == BL_LAMP, "its blocks are back");
		CHECK(w.inv[BL_COBBLE] == 7 && w.nights == 4 && w.survival && w.have_keep && nworkers(&w) == 2, "and what he had, the keep and the workers (%d %d %d)", w.inv[BL_COBBLE], w.nights, nworkers(&w));
		CHECK(fabsf(w.pl.x - 20.5f) < 0.1f && fabsf(w.pl.z - 22.5f) < 0.1f && w.sens == 0.004f, "and where he stood; the pointer's speed is the person's, not the world's");
		/* the next start opens the one that was open */
		static struct app s2;
		memset(&s2, 0, sizeof s2);
		snprintf(s2.dir, sizeof s2.dir, "%s", w.dir);
		conf_load(&s2, false);
		CHECK(!strcmp(s2.last_world, one), "player.conf says which world was open (%s)", s2.last_world);
		/* save as: a copy that goes on; the old file stays */
		w.inv[BL_COBBLE] = 11;
		CHECK(save_all(&w, false), "saved");
		snprintf(w.field, sizeof w.field, "My castle");
		const char *why = NULL;
		CHECK(field_path(&w, true, got, sizeof got, &why) && !strcmp(got, castle), "\".bl\" is added to a name (%s)", got);
		CHECK(world_save_to(&w, got) && !strcmp(w.path, castle) && access(castle, F_OK) == 0, "saved as My castle");
		world_name(w.path, name, sizeof name);
		CHECK(!strcmp(name, "My castle"), "which is its name (%s)", name);
		w.inv[BL_COBBLE] = 99;
		CHECK(save_all(&w, false), "saved");
		snprintf(w.field, sizeof w.field, "world.bl");
		CHECK(world_open(&w) && w.inv[BL_COBBLE] == 11, "the first world is as it was before Save as (%d)", w.inv[BL_COBBLE]);
		snprintf(w.field, sizeof w.field, "My castle.bl");
		CHECK(world_open(&w) && w.inv[BL_COBBLE] == 99, "and the castle went on (%d)", w.inv[BL_COBBLE]);
		/* what cannot be opened or written leaves the world as it is */
		static const char *const no_world[] = { "", "nowhere", "../", "junk.bl" };
		char junk[600];
		snprintf(junk, sizeof junk, "%s/junk.bl", w.dir);
		FILE *jf = fopen(junk, "w");
		if (jf) {
			fputs("this is no world, and long enough to be read as one if the magic were not looked at", jf);
			fclose(jf);
		}
		for (int i = 0; i < 4; i++) {
			snprintf(w.field, sizeof w.field, "%s", no_world[i]);
			w.card_msg[0] = 0;
			CHECK(!world_open(&w) && w.card_msg[0] && !strcmp(w.path, castle) && w.inv[BL_COBBLE] == 99, "\"%s\" does not open: %s", no_world[i], w.card_msg);
		}
		snprintf(w.field, sizeof w.field, "sub/");
		CHECK(!field_path(&w, true, got, sizeof got, &why) && why, "a folder is no name to save as");
		CHECK(!world_save_to(&w, "/nonexistent/x.bl") && !strcmp(w.path, castle) && !w.nosave, "a file that cannot be written leaves the world in its own");
		/* a world with no file (a seed's) gets one by Save as */
		w.path[0] = 0;
		w.nosave = true;
		CHECK(!save_all(&w, false), "no file, nothing saved");
		snprintf(got, sizeof got, "%s/kept.bl", w.dir);
		CHECK(world_save_to(&w, got) && !w.nosave, "Save as gives it a file");
		/* a world.bl from before: the state is in player.conf */
		snprintf(w.path, sizeof w.path, "%s", one);
		CHECK(bl_save(w.world, one) == 0, "a bare world file");
		char pc[600];
		snprintf(pc, sizeof pc, "%s/player.conf", w.dir);
		jf = fopen(pc, "w");
		if (jf) {
			fprintf(jf, "survival = 0\nx = 30.5\nz = 31.5\ny = 20\ninv = %d:5\n", BL_BRICK);
			fclose(jf);
		}
		w.path[0] = 0;
		CHECK(world_load(&w, one, &why, -1, -1, -1) && !w.survival && w.inv[BL_BRICK] == 5 && fabsf(w.pl.x - 30.5f) < 0.1f, "an old world.bl takes its state from player.conf");
		bl_world_free(w.world);
	}
	bl_world_free(a.world);
	char cmd[200];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		fprintf(stderr, "(could not remove %s)\n", dir);

	/* the Red Banner: its camp in a made world, what is saved of it, the way to beat it */
	{
		static struct app rv;
		memset(&rv, 0, sizeof rv);
		snprintf(rv.dir, sizeof rv.dir, "%s/rival", dir);
		rv.world_size = 256;
		rv.fov = 1.2f;
		rv.cli_clock = -1;
		rv.survival = true;
		rv.cli_no_rival = true;
		memcpy(rv.hotbar, k_default_hotbar, sizeof rv.hotbar);
		int made = 0;
		for (uint32_t seed = 1; seed <= 4; seed++) {
			CHECK(make_world(&rv, seed, false), "a world");
			state_reset(&rv);
			rv.survival = true;
			rv.cli_no_rival = true;
			world_begin(&rv, false, 1, -1, -1);
			CHECK(rv.rival == 0 && rival_count(&rv, false) == 0, "no camp until it is asked for");
			if (!rival_start(&rv))
				continue;
			made++;
			float d = hypotf((float)(rv.rkeep[0] - rv.keep[0]), (float)(rv.rkeep[2] - rv.keep[2]));
			CHECK(d > 76.0f && d < 125.0f, "seed %u: the camp is 80 to 120 blocks from the keep (%.0f)", seed, d);
			CHECK(rival_count(&rv, false) == RIVAL_GUARDS && rival_count(&rv, true) == 1, "seed %u: six guards and an archer (%d, %d)", seed, rival_count(&rv, false), rival_count(&rv, true));
			int walls = 0;
			for (int k = -CAMP_WALL; k <= CAMP_WALL; k++)
				walls += (bl_get(rv.world, rv.rkeep[0] + k, rv.rkeep[1], rv.rkeep[2] - CAMP_WALL) == BL_COBBLE) + (bl_get(rv.world, rv.rkeep[0] - CAMP_WALL, rv.rkeep[1], rv.rkeep[2] + k) == BL_COBBLE);
			CHECK(bl_get(rv.world, rv.rkeep[0], rv.rkeep[1] + 2, rv.rkeep[2]) == BL_REDKEEP && walls >= 36 && walls < 42, "seed %u: the keep stands in a wall with a gateway (%d)", seed, walls);
			CHECK(bl_get(rv.world, rv.rkeep[0] + BL_KEEP_R, rv.rkeep[1] + 5, rv.rkeep[2] + BL_KEEP_R) == BL_REDKEEP && bl_mobs_chief(&rv.mobs) >= 0, "seed %u: it is a castle, and the Captain is there", seed);
			CHECK(rv.ndeposits >= 1 && rv.rgold == RIVAL_GOLD, "seed %u: they have %d gold (deposits %d)", seed, rv.rgold, rv.ndeposits);
			rv.rgold = 0;
			bl_mobs_clear_kind(&rv.mobs, BL_MOB_CHIEF);             /* (without their Captain: with him it takes your own hand as well) */
			rv.chief_was = false;
			rv.chief_t = 1e9f;
			/* eight soldiers sent from the keep: they find the way there and strike it */
			bl_mobs_clear_kind(&rv.mobs, BL_MOB_WORKER);
			rv.mobs.enabled = false;
			int sent = 0;
			for (int k = 0; k < BL_SOLDIER_MAX; k++) {
				float sp[3];
				if (worker_spot(&rv, (float)rv.keep[0] + 0.5f, (float)rv.keep[2] + 0.5f, BL_KEEP_EDGE + 2.4f, (float)rv.keep[1] + 2.5f, sp) && bl_mobs_add(&rv.mobs, rv.world, BL_MOB_SOLDIER, sp[0], sp[1], sp[2]))
					sent++;
			}
			for (int k = 0; k < BL_MOBS_MAX; k++)
				if (rv.mobs.mob[k].alive && rv.mobs.mob[k].kind == BL_MOB_SOLDIER)
					bl_mobs_order_move(&rv.mobs, k, (float)rv.rkeep[0] + 0.5f, (float)rv.rkeep[2] - 0.5f);
			rv.playing = true;
			rv.pl.flying = true;
			for (int i = 0; i < 30 * 150 && rv.rival == 1; i++)
				mobs_advance(&rv, 1.0f / 30.0f);
			CHECK(rv.rival == 2 && sent == BL_SOLDIER_MAX && nsoldiers(&rv) < sent, "seed %u: eight soldiers win, and not all come home (keep %.0f, %d of %d left)", seed, rv.rhp, nsoldiers(&rv), sent);
			rv.playing = false;
		}
		CHECK(made == 4, "a camp is found a place in each world (%d)", made);
		/* saved and read again */
		CHECK(make_world(&rv, 2, false), "a world");
		state_reset(&rv);
		rv.cli_no_rival = true;
		world_begin(&rv, false, 1, -1, -1);
		CHECK(rival_start(&rv), "a camp");
		rv.rhp = 33.0f;
		CHECK(mkdirs(rv.dir) == 0, "a folder");
		snprintf(rv.path, sizeof rv.path, "%s/world.bl", rv.dir);
		CHECK(save_all(&rv, false), "saved");
		int kx = rv.rkeep[0], kz = rv.rkeep[2];
		const char *why = NULL;
		char camp[1024];
		snprintf(camp, sizeof camp, "%s", rv.path);
		CHECK(world_load(&rv, camp, &why, -1, -1, -1) && rv.rival == 1 && rv.rival_on && rv.rkeep[0] == kx && rv.rkeep[2] == kz && rv.rhp == 33.0f &&
		      rival_count(&rv, false) == RIVAL_GUARDS && rival_count(&rv, true) == 1 && rv.mobs.rival_on, "the camp comes back: its place, what its keep takes, its men");
		CHECK(bl_mobs_chief(&rv.mobs) >= 0 && rv.rgold == RIVAL_GOLD, "the Captain and their gold come back too");
		for (int k = 0, killed = 0; k < BL_MOBS_MAX && killed < 2; k++)
			if (rv.mobs.mob[k].alive && rv.mobs.mob[k].kind == BL_MOB_GUARD && !rv.mobs.mob[k].posted) {
				rv.mobs.mob[k].alive = false;
				killed++;
			}
		rv.rstock = 0;
		rv.rguard_t = RIVAL_REINFORCE;
		rival_step(&rv, RIVAL_REINFORCE - 1.0f);
		CHECK(rival_count(&rv, false) == RIVAL_GUARDS - 2, "not yet");
		rival_step(&rv, 2.0f);
		CHECK(rival_count(&rv, false) == RIVAL_GUARDS - 2, "with an empty store nobody comes");
		rv.rstock = RIVAL_GUARD_COST + 1;
		rival_step(&rv, 0.1f);
		CHECK(rival_count(&rv, false) == RIVAL_GUARDS - 1 && rv.rstock == 1, "with material one does, and is paid for (%d)", rv.rstock);
		/* the keep falls: with the gold it is built again, without it the game is won */
		rv.playing = true;
		rv.rgold = KEEP_GOLD + 3;
		rival_hit(&rv, RIVAL_HP + 1.0f);
		CHECK(rv.rival == 1 && rv.rkeep_back == RIVAL_BACK && rv.rgold == 3 && bl_get(rv.world, rv.rkeep[0], rv.rkeep[1] + 3, rv.rkeep[2]) == BL_AIR && rv.mobs.rkeep_down,
		      "with gold their keep is only down for a while (%d gold left)", rv.rgold);
		rival_hit(&rv, 10.0f);
		CHECK(rv.rkeep_back == RIVAL_BACK && rv.rhp == 0.0f, "a keep that is down takes no blows");
		CHECK(save_all(&rv, false) && world_load(&rv, camp, &why, -1, -1, -1) && rv.rkeep_back > 40.0f && rv.rgold == 3, "saved while it is down");
		rv.playing = true;
		rival_step(&rv, RIVAL_BACK + 1.0f);
		CHECK(rv.rkeep_back == 0.0f && rv.rhp == RIVAL_HP && bl_get(rv.world, rv.rkeep[0], rv.rkeep[1] + 3, rv.rkeep[2]) == BL_REDKEEP, "and it stands again");
		rival_hit(&rv, RIVAL_HP + 1.0f);
		CHECK(rv.rival == 2 && !rv.rival_on && rv.prompt == P_WON && rival_count(&rv, false) == 0 && bl_mobs_chief(&rv.mobs) < 0 &&
		      bl_get(rv.world, rv.rkeep[0], rv.rkeep[1] + 3, rv.rkeep[2]) == BL_AIR && !rv.mobs.rival_on, "without gold it is gone, their men too, and the game is won");
		CHECK(save_all(&rv, false) && world_load(&rv, camp, &why, -1, -1, -1) && rv.rival == 2 && !rv.rival_on && rival_count(&rv, false) == 0, "and stays won");
		/* a new survival world has them from the start */
		rv.test_seed = 10;                                      /* (what follows depends on the land: in some worlds the keep stands where raiders do not get to) */
		rv.cli_no_rival = false;
		rv.survival = true;
		new_world(&rv, NULL, 1);
		CHECK(rv.rival == 1 && rv.rival_on && rival_count(&rv, false) == RIVAL_GUARDS, "a new world has the camp");
#define NO_CHIEF() (bl_mobs_clear_kind(&rv.mobs, BL_MOB_CHIEF), rv.chief_was = false, rv.chief_t = 1e9f)

		/* their raiding parties: by day, in time, bigger each time; saved; to the keep over real ground */
		CHECK(rv.raids == 0 && rv.raid_t == RAID_FIRST && !rv.raid_out, "the first one in five minutes of survival (%.0f)", rv.raid_t);
		rv.playing = true;
		rv.hp = MAX_HP;
		rv.time_h = 1.0f;
		rv.raid_t = 1.0f;
		NO_CHIEF();
		rival_step(&rv, 5.0f);
		CHECK(!rv.raid_out && rv.raid_t == 1.0f, "not by night");
		rv.time_h = 12.0f;
		rival_step(&rv, 0.5f);
		CHECK(!rv.raid_out, "not yet");
		rival_step(&rv, 0.6f);
		CHECK(rv.raid_out && rv.raids == 1 && bl_mobs_count_raiders(&rv.mobs) == RAID_SMALL && rival_count(&rv, false) == RIVAL_GUARDS,
		      "two set out, the camp keeps its six (%d)", bl_mobs_count_raiders(&rv.mobs));
		snprintf(camp, sizeof camp, "%s/raid.bl", rv.dir);
		snprintf(rv.path, sizeof rv.path, "%s", camp);
		rv.nosave = false;
		CHECK(save_all(&rv, false) && world_load(&rv, camp, &why, -1, -1, -1) && rv.raid_out && rv.raids == 1 && bl_mobs_count_raiders(&rv.mobs) == RAID_SMALL &&
		      rival_count(&rv, false) == RIVAL_GUARDS, "saved on the way, they are still on it");
		rv.playing = true;
		rv.hp = MAX_HP;
		rv.time_h = 12.0f;
		rv.mobs.enabled = false;
		rv.pl.flying = true;
		NO_CHIEF();
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_WORKER);
		for (int i = 0; i < 30 * 120 && !rv.raid_here; i++) {
			rv.pl.y = 60.0f;
			mobs_advance(&rv, 1.0f / 30.0f);
		}
		CHECK(rv.raid_here, "they get there");
		for (int i = 0; i < 30 * 60 && rv.prompt != P_LOST; i++) {
			rv.pl.y = 60.0f;
			mobs_advance(&rv, 1.0f / 30.0f);
		}
		CHECK(rv.prompt == P_LOST && !rv.have_keep, "nobody to hold them: the keep falls (%.0f)", rv.keep_hp);
		/* the next party is a man bigger */
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_GUARD);
		rv.raid_out = false;
		rv.prompt = P_NONE;
		NO_CHIEF();
		rv.rstock = RIVAL_STOCK_MAX;
		rv.raid_t = 0.1f;
		rival_step(&rv, 0.2f);
		CHECK(rv.raids == 2 && bl_mobs_count_raiders(&rv.mobs) == RAID_SMALL + 1, "three the second time (%d)", bl_mobs_count_raiders(&rv.mobs));
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_GUARD);
		rival_step(&rv, 0.1f);
		CHECK(!rv.raid_out && rv.raid_t == RAID_PAUSE, "beaten: the next one in four minutes");
		rv.raids = 20;
		rv.rstock = RIVAL_STOCK_MAX;
		rv.rbuilt &= ~(1 << CAMP_HOUSE);
		rv.rsite = -1;
		bl_mobs_set_rsite(&rv.mobs, NULL);
		rv.rcheck_t = 100.0f;
		rv.raid_t = 0.1f;
		rival_step(&rv, 0.2f);
		CHECK(bl_mobs_count_raiders(&rv.mobs) == RAID_NO_HOUSE && rv.rstock == RIVAL_STOCK_MAX - RAID_NO_HOUSE * RIVAL_RAIDER_COST, "four without a longhouse, each paid for (%d, %d)",
		      bl_mobs_count_raiders(&rv.mobs), rv.rstock);
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_GUARD);
		rival_step(&rv, 0.1f);
		rv.rstock = RIVAL_STOCK_MAX;
		rv.rbuilt |= 1 << CAMP_HOUSE;
		rv.raid_t = 0.1f;
		rival_step(&rv, 0.2f);
		CHECK(bl_mobs_count_raiders(&rv.mobs) == BL_RAIDER_MAX, "with it six, never more (%d)", bl_mobs_count_raiders(&rv.mobs));
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_GUARD);
		rival_step(&rv, 0.1f);
		rv.rstock = RIVAL_RAIDER_COST * 2 - 1;
		rv.raid_t = 0.1f;
		rival_step(&rv, 0.2f);
		CHECK(!rv.raid_out && rv.raid_t == 30.0f, "nothing to pay two with: no party yet");
		/* the Captain leads every second party; a death sends them home */
		bl_mobs_clear_kind(&rv.mobs, BL_MOB_GUARD);
		rival_step(&rv, 0.1f);
		rv.chief_t = 0.0f;
		rv.raid_t = 100.0f;
		rival_step(&rv, 0.1f);
		CHECK(bl_mobs_chief(&rv.mobs) >= 0, "the Captain is back");
		rv.raids = 1;
		rv.rstock = RIVAL_STOCK_MAX;
		rv.raid_t = 0.1f;
		rival_step(&rv, 0.2f);
		CHECK(rv.raid_out && rv.mobs.mob[bl_mobs_chief(&rv.mobs)].raider, "he leads the second one");
		respawn(&rv);
		int back = 0, out = 0;
		for (int k = 0; k < BL_MOBS_MAX; k++)
			if (rv.mobs.mob[k].alive && rv.mobs.mob[k].raider) {
				out++;
				back += rv.mobs.mob[k].retreat;
			}
		CHECK(out > 1 && back == out, "respawn: they turn back (%d of %d)", back, out);

		/* gold, the buildings, the weapons, the keep that is paid for */
		new_world(&rv, NULL, 1);
		survey(&rv);
		int ore = 0;
		for (int i = 0; i < rv.ndeposits; i++)
			ore += rv.ore[i];
		CHECK(rv.ndeposits == DEPOSITS && ore >= 40 && ore <= DEPOSITS * 14, "five deposits of gold (%d, %d pieces)", rv.ndeposits, ore);
		CHECK(rv.arms == 1u << BL_WP_SWORD && !arms_choose(&rv, BL_WP_BOW), "the sword only, and no bow without a smithy");
		bl_set(rv.world, 4, 50, 4, BL_SMITHY);
		bl_set(rv.world, 6, 50, 4, BL_MAGE);
		CHECK(!arms_choose(&rv, BL_WP_BOW), "nor without the price");
		rv.inv[BL_GOLD] = 40;
		rv.inv[BL_PLANKS] = 8;
		rv.inv[BL_COBBLE] = 8;
		CHECK(arms_choose(&rv, BL_WP_BOW) && rv.weapon == BL_WP_BOW && rv.inv[BL_GOLD] == 37 && rv.inv[BL_PLANKS] == 2, "a bow for 3 gold and 6 planks");
		CHECK(arms_choose(&rv, BL_WP_HAMMER) && arms_choose(&rv, BL_WP_FIRE) && rv.inv[BL_GOLD] == 23, "the hammer and the fire staff (%d)", rv.inv[BL_GOLD]);
		CHECK(arms_shots(&rv, BL_WP_FIRE) == 23 * FIRE_SHOTS, "shots by the gold");
		arms_spend(&rv, BL_WP_FIRE);
		CHECK(rv.inv[BL_GOLD] == 22 && rv.charges[BL_WP_FIRE] == FIRE_SHOTS - 1, "a gold pays for five");
		arms_next(&rv);
		CHECK(rv.weapon == BL_WP_SWORD, "Q goes round (%d)", rv.weapon);
		/* a mine digs by itself */
		const int *dp = rv.deposit[0];
		bl_set(rv.world, dp[0] + 4, dp[1] + 1, dp[2], BL_MINE);
		survey(&rv);
		const int had = rv.inv[BL_GOLD], left = rv.ore[0];
		rv.mine_t = MINE_EVERY;
		works_tick(&rv, 0.1f);
		survey(&rv);
		CHECK(rv.inv[BL_GOLD] == had + 1 && rv.ore[0] == left - 1, "a mine takes a piece (%d, %d)", rv.inv[BL_GOLD], rv.ore[0]);
		/* his keep: paid for, it stands again; else the game is over */
		keep_fall(&rv);
		CHECK(rv.keep_down && rv.have_keep && rv.inv[BL_GOLD] == had + 1 - KEEP_GOLD && !rv.mobs.keep_on && rv.prompt != P_LOST, "20 gold and it is built again");
		works_tick(&rv, KEEP_BACK + 1.0f);
		CHECK(!rv.keep_down && rv.keep_hp == KEEP_HP && bl_get(rv.world, rv.keep[0] + BL_KEEP_R, rv.keep[1] + 5, rv.keep[2] + BL_KEEP_R) == BL_KEEP, "it stands");
		rv.inv[BL_GOLD] = KEEP_GOLD - 1;
		keep_fall(&rv);
		CHECK(!rv.have_keep && rv.prompt == P_LOST, "without the gold it is lost");
		/* the name of a new game */
		char np[600];
		const char *nw = NULL;
		CHECK(!game_path(&rv, "  ", np, sizeof np, &nw) && !game_path(&rv, "a/b", np, sizeof np, &nw) && !game_path(&rv, ".x", np, sizeof np, &nw), "bad names");
		CHECK(game_path(&rv, " Lucas two ", np, sizeof np, &nw) && strstr(np, "/Lucas two.bl"), "a name is a file (%s)", np);
		new_world(&rv, np, 0);
		CHECK(!strcmp(rv.path, np) && access(np, F_OK) == 0 && !game_path(&rv, "Lucas two", np, sizeof np, &nw), "and taken once");
		/* free building: nobody and nothing to pay */
		CHECK(!rv.survival && rv.rival == 0 && !rv.mobs_on && !rv.have_keep && rv.ndeposits == 0 && rv.arms == (1u << BL_WP_COUNT) - 1 && arms_shots(&rv, BL_WP_BOW) < 0,
		      "free building has no clan, no goblins and no keep");
		bl_world_free(rv.world);
	}

	/* the sounds: every one the game asks for is one the compositor knows (gamesound.h) and the other way round; the materials; how often they may come */
	int variants = 0;
	for (int i = 0; i < N_SFX; i++) {
		int first = lm_gamesound_pick(k_sfx[i].name, (uint32_t)i);
		CHECK(first >= 0 && sfx_index(k_sfx[i].name) == i, "the sound \"%s\" is there, once", k_sfx[i].name);
	}
	for (int i = 0; i < lm_gamesound_count(); i++) {
		const char *n = lm_gamesound_name(i) + 5;                        /* (past "game-") */
		size_t len = strlen(n);
		if (len && n[len - 1] >= '2' && n[len - 1] <= '9')
			continue;
		variants++;
		CHECK(sfx_index(n) >= 0, "the game knows the sound \"%s\"", n);
	}
	CHECK(variants == N_SFX, "%d sounds, %d in the game", variants, N_SFX);
	static const char *const k_kind[] = { "break", "dig", "place" };
	for (int b = 1; b < BL_COUNT; b++)
		for (int k = 0; k < 3; k++) {
			char buf[24];
			CHECK(sfx_index(block_sfx(k_kind[k], b, buf)) >= 0, "%s of %s has a sound (%s)", k_kind[k], bl_block_name(b), block_sfx(k_kind[k], b, buf));
		}
	char mb[24];
	CHECK(!strcmp(block_sfx("break", BL_LOG, mb), "break-wood"), "a log breaks like wood");
	CHECK(!strcmp(block_sfx("place", BL_WATER, mb), "splash") && !strcmp(block_sfx("dig", BL_GRASS, mb), "dig-dirt"), "water splashes, grass is dug like earth");
	static struct app so;
	memset(&so, 0, sizeof so);
	CHECK(sfx_due(&so, "dig-stone", 1000) && !sfx_due(&so, "dig-stone", 1100) && sfx_due(&so, "dig-stone", 1250), "a dig sound comes at most every 0.2 s");
	CHECK(sfx_due(&so, "break-stone", 1260), "a block that breaks is heard at once after the last blow at it");
	CHECK(!sfx_due(&so, "hammer", 1290) && sfx_due(&so, "hammer", 1400), "a small sound gives way to one that has only just begun");
	CHECK(sfx_due(&so, "horn", 1401) && !sfx_due(&so, "horn", 2000) && !sfx_due(&so, "nothing", 3000), "the horn does not give way, and there is no sound called nothing");

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
		/* leaves are no shield to a goblin, a block is */
		int lx = (int)floorf(ma.pl.x), lz = (int)floorf(ma.pl.z + 1.0f), ly = (int)floorf(ma.pl.y + 1.6f);
		for (int k = 0; k < 2; k++) {
			bl_mobs_clear(&ma.mobs);
			bl_mobs_add(&ma.mobs, ma.world, BL_MOB_GOBLIN, ma.pl.x, ma.pl.y, ma.pl.z + 2.0f);
			bl_set(ma.world, lx, ly, lz, k ? BL_STONE : BL_LEAVES);
			aim_update(&ma);
			CHECK(ma.aimed && bl_get(ma.world, ma.aim.x, ma.aim.y, ma.aim.z) == (k ? BL_STONE : BL_LEAVES), "a block stands before his eyes");
			CHECK(sword_swing(&ma, 20000 + 1000 * k) == !k, "%s", k ? "a goblin behind stone is not reached" : "the sword goes through leaves to a goblin");
		}
		bl_set(ma.world, lx, ly, lz, BL_AIR);
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
		CHECK(kd >= 7.9f && kd <= 20.0f, "8 to 20 blocks from where he starts (%.1f)", kd);
		CHECK(bl_get(sa.world, sa.keep[0], sa.keep[1], sa.keep[2]) == BL_KEEP && bl_get(sa.world, sa.keep[0], sa.keep[1] + BL_KEEP_TALL - 1, sa.keep[2]) == BL_KEEP &&
		      bl_get(sa.world, sa.keep[0] + BL_KEEP_R, sa.keep[1], sa.keep[2] + BL_KEEP_R) == BL_KEEP && bl_get(sa.world, sa.keep[0], sa.keep[1], sa.keep[2] - BL_KEEP_R) == BL_AIR &&
		      bl_get(sa.world, sa.keep[0], sa.keep[1] - 1, sa.keep[2]) != BL_AIR, "a castle with a door, on the ground");
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
		sa.dig_wait = 0.0f;
		for (int i = 0; i < 50; i++)                                             /* three blocks down is stone */
			dig_step(&sa, 1000, 0.1f), aim_update(&sa);
		CHECK(sa.inv[BL_COBBLE] >= 1 && sa.inv[BL_DIRT] >= 3, "digging down 5 s: cobble from the stone (%d), dirt (%d)", sa.inv[BL_COBBLE], sa.inv[BL_DIRT]);
		sa.pl.y = 17.0f;                                                         /* (he has gone down the hole: the next blocks are in reach) */
		aim_update(&sa);
		/* after a block gave way a held button waits a moment before the next */
		sa.dig_on = false;
		sa.dig_wait = DIG_PAUSE;
		uint32_t waited = sa.world->rev;
		dig_step(&sa, 1000, 0.2f);
		CHECK(!sa.dig_on && sa.world->rev == waited, "0.2 s after a block: the next one is not begun");
		dig_step(&sa, 1000, 0.2f);
		dig_step(&sa, 1000, 0.1f);
		CHECK(sa.dig_on && sa.dig_t > 0.0f, "after the pause it is");
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
		CHECK(bl_mobs_add(&sa.mobs, sa.world, BL_MOB_GOBLIN, (float)sa.keep[0] + 6.5f, (float)sa.keep[1], (float)sa.keep[2] + 0.5f), "a goblin beside it");
		for (int i = 0; i < 30 * 12 && sa.have_keep; i++)
			mobs_advance(&sa, 1.0f / 30.0f);
		CHECK(!sa.have_keep && sa.prompt == P_LOST && !sa.playing, "a goblin at the keep pulls it down: the game is over");
		CHECK(bl_get(sa.world, sa.keep[0], sa.keep[1] + 2, sa.keep[2]) == BL_AIR && bl_get(sa.world, sa.keep[0], sa.keep[1] + BL_KEEP_TALL - 1, sa.keep[2]) == BL_AIR && !sa.mobs.keep_on, "its blocks are gone and the goblins have no keep to go to");
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

	/* workers and the command view */
	{
		static struct app wa;
		memset(&wa, 0, sizeof wa);
		wa.world_size = 64;
		wa.fov = 1.2f;
		wa.sens = 0.0025f;
		wa.rw = 640;
		wa.rh = 360;
		wa.win.bw = 640;
		wa.win.bh = 360;
		memcpy(wa.hotbar, k_default_hotbar, sizeof wa.hotbar);
		snprintf(wa.dir, sizeof wa.dir, "/tmp/mellagui-blocks-selftest3-%d", (int)getpid());
		CHECK(make_world(&wa, 5, true) && mkdirs(wa.dir) == 0, "a flat world for the workers");
		mobs_setup(&wa);
		wa.mobs_on = false;
		wa.survival = true;
		survival_setup(&wa, wa.pl.x, wa.pl.z);
		CHECK(wa.have_keep && nworkers(&wa) == 2, "a new survival world starts with a keep and two workers (%d)", nworkers(&wa));
		/* the camera: a point of the world seen through the pointer comes back to the same pointer */
		wa.cam_f[0] = 30.0f;
		wa.cam_f[1] = 21.0f;
		wa.cam_f[2] = 30.0f;
		wa.cam_yaw = 0.7f;
		wa.cam_dist = 20.0f;
		struct bl_camera cam;
		cmd_camera(&wa, &cam);
		float sx, sy, dir[3];
		CHECK(cmd_project(&wa, &cam, 30.0f, 21.0f, 30.0f, &sx, &sy) && fabsf(sx - 320.0f) < 1.0f && fabsf(sy - 180.0f) < 1.0f, "the point it looks at is in the middle of the window (%.1f %.1f)", sx, sy);
		bool rt = true;
		for (int i = 0; i < 20; i++) {
			float px = 40.0f + (float)(i * 29 % 560), py = 30.0f + (float)(i * 53 % 300), ox, oy;
			cmd_ray(&wa, &cam, px, py, dir);
			float p[3] = { cam.x + dir[0] * 25.0f, cam.y + dir[1] * 25.0f, cam.z + dir[2] * 25.0f };
			if (!cmd_project(&wa, &cam, p[0], p[1], p[2], &ox, &oy) || fabsf(ox - px) > 0.5f || fabsf(oy - py) > 0.5f)
				rt = false;
		}
		CHECK(rt, "a ray through a pixel and back gives the pixel");
		/* choosing: a click on a worker and a box */
		struct bl_mob *w0 = NULL;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (wa.mobs.mob[i].alive && wa.mobs.mob[i].kind == BL_MOB_WORKER) {
				w0 = &wa.mobs.mob[i];
				break;
			}
		CHECK(w0 != NULL, "a worker to choose");
		if (w0) {
			wa.cam_f[0] = w0->x;
			wa.cam_f[1] = w0->y;
			wa.cam_f[2] = w0->z;
			cmd_camera(&wa, &cam);
			cmd_project(&wa, &cam, w0->x, w0->y + 0.8f, w0->z, &sx, &sy);
			wa.px = sx;
			wa.py = sy;
			float d2[3], e2[3] = { cam.x, cam.y, cam.z }, t2 = -1;
			cmd_ray(&wa, &cam, sx, sy, d2);
			int pi = bl_units_pick(&wa.mobs, e2, d2, 200.0f, &t2);
			cmd_select_click(&wa, false);
			CHECK(pi >= 0 && wa.mobs.mob[pi].selected && cmd_selected(&wa) == 1, "a click on a worker chooses it");
			wa.px = 5;
			wa.py = 5;
			cmd_select_click(&wa, false);
			CHECK(cmd_selected(&wa) == 0, "a click on nothing lets go");
			cmd_select_box(&wa, 0, 0, 640, 360, false);
			CHECK(cmd_selected(&wa) >= 1, "a box over the picture chooses the workers in it");
		}
		/* training costs planks, at most eight */
		wa.inv[BL_PLANKS] = 20;
		int n0 = nworkers(&wa);
		CHECK(train_worker(&wa) && nworkers(&wa) == n0 + 1 && wa.inv[BL_PLANKS] == 12, "a worker costs eight planks");
		wa.inv[BL_PLANKS] = 5;
		CHECK(!train_worker(&wa) && nworkers(&wa) == n0 + 1 && wa.inv[BL_PLANKS] == 5, "not with five");
		wa.inv[BL_PLANKS] = 999;
		for (int i = 0; i < 12; i++)
			train_worker(&wa);
		CHECK(nworkers(&wa) == BL_WORKER_MAX, "no more than %d workers", BL_WORKER_MAX);
		/* what they break goes into the pockets */
		wa.inv[BL_LOG] = 0;
		bl_set(wa.world, 10, 25, 10, BL_LOG);
		struct bl_mobs_result R;
		memset(&R, 0, sizeof R);
		R.nbreak = 1;
		R.brk[0].x = 10;
		R.brk[0].y = 25;
		R.brk[0].z = 10;
		apply_breaks(&wa, &R);
		CHECK(bl_get(wa.world, 10, 25, 10) == BL_AIR && wa.inv[BL_LOG] == 1, "a block the workers finished is gone and in the pockets");
		/* building: the plan at the pointer, its price, the builders, giving it up */
		wa.hotbar[0] = BL_BRICK;
		wa.slot = 0;
		wa.inv[BL_BRICK] = 9;
		CHECK(craft_hand(&wa) == false && wa.inv[BL_BRICK] == 9, "bricks make nothing: a barracks is built");
		wa.cmd = true;
		wa.has_pointer = true;
		wa.cam_f[0] = (float)wa.keep[0] + 10.5f;
		wa.cam_f[1] = (float)wa.keep[1];
		wa.cam_f[2] = (float)wa.keep[2] + 0.5f;
		wa.cam_yaw = 0.0f;
		wa.cam_dist = 16.0f;
		wa.px = 320.0;
		wa.py = 180.0;
		wa.plan = -1;
		choose_plan(&wa, BL_PLAN_BARRACKS);
		CHECK(wa.plan == BL_PLAN_BARRACKS, "a plan in hand");
		struct bl_site ps;
		const char *why = NULL;
		bool any = false;
		wa.inv[BL_COBBLE] = 11;
		wa.inv[BL_PLANKS] = 8;
		CHECK(!plan_at_pointer(&wa, &ps, &why, &any) && any && strstr(why, "costs 12 cobble"), "11 cobble are not enough (%s)", why ? why : "-");
		CHECK(!build_place(&wa) && bl_mobs_count_sites(&wa.mobs) == 0 && wa.inv[BL_COBBLE] == 11, "nothing is planned or paid");
		wa.inv[BL_COBBLE] = 17;
		CHECK(build_place(&wa) && bl_mobs_count_sites(&wa.mobs) == 1 && wa.inv[BL_COBBLE] == 5 && wa.inv[BL_PLANKS] == 0 && wa.plan == -1, "the barracks is planned and paid for (%d %d)", wa.inv[BL_COBBLE], wa.inv[BL_PLANKS]);
		int sent = 0;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			sent += wa.mobs.mob[i].alive && wa.mobs.mob[i].order == BL_ORDER_BUILD;
		CHECK(sent == 2, "both idle workers go to build it (%d)", sent);
		CHECK(site_at_pointer(&wa) == 0, "the pointer finds the plan");
		choose_plan(&wa, BL_PLAN_BARRACKS);
		CHECK(!plan_at_pointer(&wa, &ps, &why, &any) && strstr(why, "planned"), "not twice on one place (%s)", why ? why : "-");
		wa.plan = -1;
		/* saved while it is under way */
		conf_save(&wa);
		static struct app ws;
		memset(&ws, 0, sizeof ws);
		memcpy(ws.dir, wa.dir, sizeof ws.dir);
		ws.fov = 1.2f;
		ws.sens = 0.0025f;
		memcpy(ws.hotbar, k_default_hotbar, sizeof ws.hotbar);
		conf_load(&ws, true);
		CHECK(ws.nsites_conf == 1 && ws.site_conf[0].plan == BL_PLAN_BARRACKS && ws.site_conf[0].n == wa.mobs.site[0].n &&
		      !memcmp(ws.site_conf[0].c, wa.mobs.site[0].c, (size_t)ws.site_conf[0].n * sizeof ws.site_conf[0].c[0]), "the plan is in the file, block for block");
		struct bl_site bad_site;
		CHECK(!parse_site("9 1 1:2:3:7", &bad_site) && !parse_site("0 2 1:2:3:7", &bad_site) && !parse_site("0 1 1:2:3:3", &bad_site) && !parse_site("0 1 -1:2:3:7", &bad_site) && !parse_site("0 999 1:2:3:7", &bad_site) &&
		      parse_site("2 1 10:20:30:13", &bad_site), "damaged site lines are not believed");
		/* the workers build it and the barracks counts; creative costs nothing */
		for (int i = 0; i < 60 * 60 && bl_mobs_count_sites(&wa.mobs); i++)
			mobs_advance(&wa, 1.0f / 60.0f);
		CHECK(bl_mobs_count_sites(&wa.mobs) == 0 && barracks_find(&wa, NULL) == 1, "it gets built and counts as a barracks (%d, %d)", bl_mobs_count_sites(&wa.mobs), barracks_find(&wa, NULL));
		wa.cam_f[0] = (float)wa.keep[0] - 8.5f;
		wa.inv[BL_COBBLE] = 12;
		wa.inv[BL_PLANKS] = 0;
		choose_plan(&wa, BL_PLAN_WALL);
		CHECK(build_place(&wa) && wa.plan == BL_PLAN_WALL && wa.inv[BL_COBBLE] == 6, "a wall costs six and stays in hand");
		wa.plan = -1;
		CHECK(build_cancel(&wa) && bl_mobs_count_sites(&wa.mobs) == 0 && wa.inv[BL_COBBLE] == 12, "given up before a block: all comes back (%d)", wa.inv[BL_COBBLE]);
		wa.cmd = false;
		wa.has_pointer = false;
		/* the body: the sword comes into the hand when a goblin is near, an empty pocket leaves the hand empty, and the choice is saved */
		{
			struct bl_camera hc = { wa.pl.x, wa.pl.y + BL_EYE, wa.pl.z, wa.pl.yaw, 0.0f, 1.2f };
			bool was = wa.cmd;
			wa.cmd = false;
			bl_mobs_clear_kind(&wa.mobs, BL_MOB_GOBLIN);
			wa.hotbar[wa.slot] = BL_STONE;
			wa.inv[BL_STONE] = 0;
			hero_pose(&wa, &hc);
			CHECK(wa.hero.first && !wa.hero.sword && wa.hero.held == 0, "first person, an empty hand");
			wa.inv[BL_STONE] = 3;
			hero_pose(&wa, &hc);
			CHECK(wa.hero.held == BL_STONE, "the stone in the hand");
			CHECK(bl_mobs_add(&wa.mobs, wa.world, BL_MOB_GOBLIN, wa.pl.x + 3.0f, (float)bl_top(wa.world, (int)floorf(wa.pl.x + 3.0f), (int)floorf(wa.pl.z)) + 1.0f, wa.pl.z), "a goblin beside him");
			hero_pose(&wa, &hc);
			CHECK(wa.hero.sword, "the sword is in the hand");
			bl_mobs_clear_kind(&wa.mobs, BL_MOB_GOBLIN);
			wa.cmd = true;
			hero_pose(&wa, &hc);
			CHECK(!wa.hero.first, "whole in the command view");
			wa.cmd = was;
		}
		/* soldiers: at the barracks that was built they stand on the ground, not on its roof */
		wa.inv[BL_COBBLE] = 100;
		wa.inv[BL_PLANKS] = 100;
		CHECK(train_soldier(&wa) && nsoldiers(&wa) == 1, "a soldier at the built barracks");
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (wa.mobs.mob[i].alive && wa.mobs.mob[i].kind == BL_MOB_SOLDIER)
				CHECK(wa.mobs.mob[i].y < (float)wa.keep[1] + 1.0f, "on the ground (%.1f)", wa.mobs.mob[i].y);
		bl_mobs_clear_kind(&wa.mobs, BL_MOB_SOLDIER);
		for (float b[3]; barracks_find(&wa, b) > 0;)
			bl_set(wa.world, (int)b[0], (int)b[1], (int)b[2], BL_AIR);
		wa.inv[BL_COBBLE] = 100;
		wa.inv[BL_PLANKS] = 100;
		CHECK(!train_soldier(&wa) && nsoldiers(&wa) == 0 && wa.inv[BL_COBBLE] == 100, "no soldier without a barracks");
		bl_set(wa.world, wa.keep[0] + 4, wa.keep[1], wa.keep[2], BL_BARRACKS);
		float where[3];
		CHECK(barracks_find(&wa, where) == 1 && (int)where[0] == wa.keep[0] + 4 && (int)where[2] == wa.keep[2], "the barracks is found near the keep");
		wa.inv[BL_COBBLE] = 5;
		CHECK(!train_soldier(&wa) && nsoldiers(&wa) == 0 && wa.inv[BL_COBBLE] == 5, "five cobble are not enough");
		wa.inv[BL_COBBLE] = 100;
		CHECK(train_soldier(&wa) && nsoldiers(&wa) == 1 && wa.inv[BL_COBBLE] == 100 - SOLDIER_COBBLE && wa.inv[BL_PLANKS] == 100 - SOLDIER_PLANKS, "a soldier costs six cobble and four planks");
		for (int i = 0; i < 3; i++)
			train_soldier(&wa);
		CHECK(nsoldiers(&wa) == 4 && !train_soldier(&wa) && nsoldiers(&wa) == 4, "four to a barracks");
		bl_set(wa.world, wa.keep[0] + 4, wa.keep[1], wa.keep[2] + 3, BL_BARRACKS);
		for (int i = 0; i < 6; i++)
			train_soldier(&wa);
		CHECK(nsoldiers(&wa) == BL_SOLDIER_MAX, "eight in all with two barracks (%d)", nsoldiers(&wa));
		wa.survival = false;
		bl_mobs_clear_kind(&wa.mobs, BL_MOB_SOLDIER);
		wa.inv[BL_COBBLE] = 0;
		CHECK(train_soldier(&wa) && wa.inv[BL_COBBLE] == 0, "creative: no cost");
		wa.survival = true;
		/* a night brings a wave, the day ends it */
		wa.mobs_on = true;
		wa.mobs.enabled = true;
		wa.nights = 0;
		wa.in_night = false;
		wa.time_h = 12.0f;
		survival_tick(&wa, 0.1f);
		CHECK(!wa.mobs.wave_on, "no wave by day");
		wa.time_h = 0.0f;
		survival_tick(&wa, 0.1f);
		CHECK(wa.mobs.wave_on && wa.mobs.wave_size == 3 && wa.mobs.wave_cap == 3 && wa.in_night && wa.mobs.wave_brutes == 0, "the first night: a wave of three, no brute");
		bl_mobs_start_wave(&wa.mobs, 99, 99, 0);
		int left = wa.mobs.wave_left;
		survival_tick(&wa, 0.1f);
		CHECK(wa.mobs.wave_left == left, "one wave a night (the night going on starts no other)");
		wa.time_h = 12.0f;
		survival_tick(&wa, 0.1f);
		CHECK(!wa.mobs.wave_on && wa.nights == 1, "the day ends it and the night is counted");
		wa.nights = 5;
		wa.time_h = 0.0f;
		survival_tick(&wa, 0.1f);
		CHECK(wa.mobs.wave_size == 13 && wa.mobs.wave_cap == 8 && wa.mobs.wave_brutes == 3, "after five nights thirteen goblins, eight at a time, three of them brutes (%d, %d, %d)", wa.mobs.wave_size, wa.mobs.wave_cap, wa.mobs.wave_brutes);
		wa.time_h = 12.0f;
		survival_tick(&wa, 0.1f);
		wa.nights = 30;
		wa.time_h = 0.0f;
		survival_tick(&wa, 0.1f);
		CHECK(wa.mobs.wave_size == 18 && wa.mobs.wave_cap == 10 && wa.mobs.wave_brutes == 4, "never more than eighteen, ten at a time, four brutes");
		wa.time_h = 12.0f;
		survival_tick(&wa, 0.1f);
		/* the second night brings one brute, and the dawn after a fought wave leaves planks and cobble at the keep */
		wa.nights = 1;
		wa.in_night = false;
		wa.inv[BL_PLANKS] = 0;
		wa.inv[BL_COBBLE] = 0;
		wa.time_h = 0.0f;
		survival_tick(&wa, 0.1f);
		CHECK(wa.mobs.wave_size == 5 && wa.mobs.wave_brutes == 1, "the second night: five goblins, one a brute");
		wa.time_h = 12.0f;
		survival_tick(&wa, 0.1f);
		CHECK(wa.nights == 2 && wa.inv[BL_PLANKS] == 8 && wa.inv[BL_COBBLE] == 7, "the dawn leaves 8 planks and 7 cobble (%d, %d)", wa.inv[BL_PLANKS], wa.inv[BL_COBBLE]);
		wa.in_night = true;                                    /* a night with no wave (the goblins were off) leaves nothing */
		wa.mobs.wave_size = 0;
		survival_tick(&wa, 0.1f);
		CHECK(wa.inv[BL_PLANKS] == 8 && wa.nights == 3, "no wave, no tribute");
		wa.mobs_on = false;
		wa.mobs.enabled = false;
		bl_mobs_clear_kind(&wa.mobs, BL_MOB_SOLDIER);
		wa.inv[BL_COBBLE] = 100;
		train_soldier(&wa);
		train_soldier(&wa);
		/* the workers and the soldiers are saved where they stand and come back */
		conf_save(&wa);
		static struct app wb;
		memset(&wb, 0, sizeof wb);
		memcpy(wb.dir, wa.dir, sizeof wb.dir > sizeof wa.dir ? sizeof wa.dir : sizeof wb.dir);
		wb.fov = 1.2f;
		wb.sens = 0.0025f;
		memcpy(wb.hotbar, k_default_hotbar, sizeof wb.hotbar);
		conf_load(&wb, true);
		CHECK(wb.workers_saved && wb.nworkers_conf == BL_WORKER_MAX, "the workers are in the file (%d)", wb.nworkers_conf);
		CHECK(wb.soldiers_saved && wb.nsoldiers_conf == 2, "and the soldiers (%d)", wb.nsoldiers_conf);
		bl_world_free(wa.world);
		char cmd3[600];
		snprintf(cmd3, sizeof cmd3, "rm -rf '%s'", wa.dir);
		if (system(cmd3) != 0)
			fprintf(stderr, "(could not remove %s)\n", wa.dir);
	}

	/* the help's pages: every one sets out at three widths with no line wider than its room and no stray characters */
	{
		static struct wlhelp hp;
		wlhelp_init(&hp, BLOCKS_HELP, BLOCKS_HELP_N);
		CHECK(hp.npages >= 8, "the help has pages (%d)", hp.npages);
		static const int widths[] = { 260, 480, 700 };
		for (int p = 0; p < hp.npages; p++)
			for (int wi = 0; wi < 3; wi++) {
				hp.page = p;
				hp.lpage = -1;
				wlhelp_layout(&hp, widths[wi]);
				CHECK(hp.h > 0 && hp.nl > 0, "help page %d at %d px has text", p, widths[wi]);
				bool fit = true, clean = true;
				for (int i = 0; i < hp.nl; i++) {
					char buf[600];
					int n = hp.L[i].len < 599 ? hp.L[i].len : 599;
					memcpy(buf, hp.L[i].p, (size_t)n);
					buf[n] = 0;
					bool one_word = strchr(buf, ' ') == NULL;
					if (hp.L[i].kind != WLHELP_DOT && !one_word && wlhelp_text_w(buf, hp.L[i].kind) + hp.L[i].x > widths[wi] + 2)
						fit = false;
					for (char *q = buf; *q; q++)
						if ((unsigned char)*q < 32)
							clean = false;
				}
				CHECK(fit, "help page %d (%s) at %d px: a line is too wide", p, hp.pages[p].title, widths[wi]);
				CHECK(clean, "help page %d at %d px: a control character in the text", p, widths[wi]);
			}
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
	wlhelp_init(&a.help, BLOCKS_HELP, BLOCKS_HELP_N);
	a.fov = 70.0f * (float)M_PI / 180.0f;
	a.sens = 0.0025f;
	a.time_h = 9.0f;
	a.clockm = 1;
	a.sound_on = true;
	a.pal_hover = -1;
	state_reset(&a);
	a.body = true;
	a.panel = true;
	int clock_cli = -1;
	bool no_pets = false, no_mobs = false, no_body = false, no_panel = false;
	int workers_cli = -1;                                   /* --workers N */
	int soldiers_cli = -1;                                  /* --soldiers N */
	int mode_cli = -1;                                      /* --survival 1, --creative 0 */
	char give[256] = "";
	int ngob = 0;
	float gob[4][2] = { { 0 } };
	bool gob_brute[4] = { false };
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
		} else if (!strcmp(argv[i], "--no-body")) {
			no_body = true;
		} else if (!strcmp(argv[i], "--no-panel")) {
			no_panel = true;
		} else if (!strcmp(argv[i], "--no-pets")) {
			no_pets = true;
		} else if (!strcmp(argv[i], "--rival")) {
			a.cli_rival = true;
		} else if (!strcmp(argv[i], "--no-rival")) {
			a.cli_no_rival = true;
		} else if (!strcmp(argv[i], "--raid-after") && i + 1 < argc) {   /* seconds until the first raiding party (for the tests, and to see one soon) */
			a.cli_raid = (float)atof(argv[++i]);
			if (!(a.cli_raid >= 1.0f && a.cli_raid <= 3600.0f)) {
				fprintf(stderr, "mellagui-blocks: --raid-after wants 1 to 3600 seconds\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--no-mobs")) {
			no_mobs = true;
		} else if (!strcmp(argv[i], "--survival")) {
			mode_cli = 1;
		} else if (!strcmp(argv[i], "--creative")) {
			mode_cli = 0;
		} else if (!strcmp(argv[i], "--soldiers") && i + 1 < argc) {
			soldiers_cli = atoi(argv[++i]);
			if (soldiers_cli < 0 || soldiers_cli > BL_SOLDIER_MAX) {
				fprintf(stderr, "mellagui-blocks: --soldiers wants 0 to %d\n", BL_SOLDIER_MAX);
				return 2;
			}
		} else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
			workers_cli = atoi(argv[++i]);
			if (workers_cli < 0 || workers_cli > BL_WORKER_MAX) {
				fprintf(stderr, "mellagui-blocks: --workers wants 0 to %d\n", BL_WORKER_MAX);
				return 2;
			}
		} else if (!strcmp(argv[i], "--give") && i + 1 < argc) {                   /* NAME=N,NAME=N: what is in the pockets at the start (for the tests, and for cheating) */
			snprintf(give, sizeof give, "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--day-minutes") && i + 1 < argc) {
			a.day_minutes = (float)atof(argv[++i]);
			if (!(a.day_minutes >= 0.05f && a.day_minutes <= 600.0f)) {
				fprintf(stderr, "mellagui-blocks: --day-minutes wants 0.05 to 600\n");
				return 2;
			}
		} else if ((!strcmp(argv[i], "--goblin") || !strcmp(argv[i], "--brute")) && i + 1 < argc) {   /* a goblin (a brute) at DX,DZ blocks from where the player starts (for the tests) */
			bool brute = !strcmp(argv[i], "--brute");
			if (ngob >= 4 || sscanf(argv[++i], "%f,%f%c", &gob[ngob][0], &gob[ngob][1], &junk) != 2) {
				fprintf(stderr, "mellagui-blocks: --goblin / --brute want DX,DZ (at most four together)\n");
				return 2;
			}
			gob_brute[ngob] = brute;
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
			fprintf(stderr, "usage: mellagui-blocks [--seed N] [--flat] [--world-size N] [--size WxH] [--render WxH] [--threads N] [--time H] [--no-clock | --clock | --real-clock] [--no-sound] [--no-pets] [--no-mobs] [--rival | --no-rival] [--raid-after SECONDS] [--no-body] [--no-panel] [--survival | --creative] [--give NAME=N,...] [--day-minutes M] [--workers N] [--soldiers N] [--goblin DX,DZ] [--brute DX,DZ] [--pet-at SECONDS] [--yaw D] [--pitch D] "
			                "[--fov D] [--save | --no-save] | --selftest\n");
			return 2;
		}
	}
	g_dump = getenv("MELLAGUI_BLOCKS_DUMP");
	data_dir(a.dir, sizeof a.dir);

	/* the world: the one that was open the last time (else world.bl), unless a seed or a flat world was asked for */
	bool fresh = a.have_seed || a.flat;
	bool from_file = false, bad_file = false;
	a.cli_clock = clock_cli;
	a.cli_no_pets = no_pets;
	a.cli_no_mobs = no_mobs;
	a.cli_goblins = ngob > 0;
	a.cli_no_save = no_save;
	conf_load(&a, false);                                   /* the person's choices, and which world was open */
	a.conf_read = false;
	if (no_sound)
		a.sound_on = false;
	if (no_body)
		a.body = false;
	if (no_panel)
		a.panel = false;
	if (!fresh) {
		char deflt[600], path[1024], gone[300] = "";
		world_path(&a, deflt, sizeof deflt);
		snprintf(path, sizeof path, "%s", a.last_world[0] ? a.last_world : deflt);
		if (a.last_world[0] && access(path, F_OK) != 0 && strcmp(path, deflt) != 0) {
			world_name(path, gone, sizeof gone);
			snprintf(path, sizeof path, "%s", deflt);
		}
		if (access(path, F_OK) == 0) {
			const char *err = NULL;
			from_file = world_load(&a, path, &err, mode_cli, workers_cli, soldiers_cli);
			if (!from_file) {
				fprintf(stderr, "mellagui-blocks: %s: %s (the file is left alone and a new world is not saved)\n", path, err ? err : "cannot be read");
				bad_file = true;
			}
		} else {
			a.new_first = true;                                 /* there is no game yet: its name is asked first */
		}
		if (gone[0])
			say(&a, "The world %s is not there any more", gone);
	}
	if (!from_file) {
		if (!make_world(&a, a.have_seed ? a.seed_given : fresh_seed(), a.flat)) {
			fprintf(stderr, "mellagui-blocks: cannot make a world\n");
			return 1;
		}
		if ((fresh && !want_save) || bad_file || a.new_first)
			a.nosave = true;                                    /* (no file: Save as gives it one) */
		else
			world_path(&a, a.path, sizeof a.path);
		if (no_save)
			a.nosave = true;
		world_begin(&a, false, a.new_first ? 0 : mode_cli < 0 ? 1 : mode_cli, workers_cli, soldiers_cli);   /* (behind the card of the first game: just a landscape) */
	}
	if (a.cli_rival && a.rival != 1)
		rival_start(&a);
	if (pet_at >= 0.0f)
		a.pet_t = pet_at;
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
		if (top >= 0 && gob_brute[i])
			bl_mobs_add_brute(&a.mobs, a.world, (float)gx + 0.5f, (float)(top + 1), (float)gz + 0.5f);
		else if (top >= 0)
			bl_mobs_add(&a.mobs, a.world, BL_MOB_GOBLIN, (float)gx + 0.5f, (float)(top + 1), (float)gz + 0.5f);
	}
	if (!isnan(yaw))
		a.pl.yaw = yaw;
	if (!isnan(pitch))
		a.pl.pitch = pitch;
	aim_update(&a);

	static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event,
	                                      .lock = lock_event, .relative = relative_event, .menu = menu_event };
	if (wlwin_connect(&a.win, "mellagui-blocks", &ops, &a) < 0)
		return 1;
	wlrail_init(&a.ran);
	wlwin_set_inset(&a.win, WLRAIL_W);                          /* the rail: the window is that much wider than the picture */
	if (wlwin_open(&a.win, "Blocks", "mellagui.blocks", win_w, win_h, 480, 320) < 0)
		return 1;
	wlclip_attach(&a.clip, &a.win);
	wlpicker_init(&a.pk, world_file);
	title_update(&a);
	a.rail_hover = -1;
	static uint32_t cur[24 * 24];
	arrow_cursor(cur, 24, 24);
	wlwin_cursor(&a.win, cur, 24, 24, 1, 1);
	a.prompt = P_PAUSE;
	if (a.new_first) {
		a.prompt = P_NEW;
		a.new_survival = mode_cli != 0;
		a.gname[0] = 0;
		wltext_end(a.gname);
	}
	a.last_save_ms = now_ms();
	a.fps_t0 = now_ms();

	int64_t last = now_ms();
	while (a.win.running) {
		int timeout = a.playing ? 1 : (a.msg[0] || (!a.nosave && a.path[0] && a.world->rev != a.saved_rev)) ? 500 : -1;
		struct wlrail rail;
		rail_build(&a, &rail);
		timeout = wlrail_loop(&a.win, &a.ran, &rail, a.rail_hover, timeout);
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

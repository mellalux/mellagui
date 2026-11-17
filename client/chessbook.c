/* chessbook.c - the opening lines and the table built from them; the contract is in chessbook.h. */
#include "chessbook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One line: a weight (how often the book should go that way), the name the position gets from the slash on, and the moves in SAN.
 * "e4 c5 / Nf3 d6": the name belongs to the position after the moves before the slash and to the positions after it (a longer line with
 * a more specific name takes the later positions over). */
struct line {
	int weight;
	const char *name, *moves;
};

static const struct line k_lines[] = {
	/* the first move */
	{ 1, "King's Pawn Opening", "e4 /" },
	{ 1, "Queen's Pawn Opening", "d4 /" },
	{ 1, "English Opening", "c4 /" },
	{ 1, "Zukertort Opening", "Nf3 /" },
	{ 1, "Bird's Opening", "f4 /" },
	{ 1, "Larsen's Opening", "b3 /" },

	/* 1.e4 e5 */
	{ 1, "Open Game", "e4 e5 /" },
	{ 3, "Italian Game", "e4 e5 Nf3 Nc6 Bc4 / Bc5 c3 Nf6 d3 d6 O-O a6 Bb3 Ba7 Re1 O-O Nbd2 h6" },
	{ 2, "Italian Game: Giuoco Piano", "e4 e5 Nf3 Nc6 Bc4 Bc5 / c3 Nf6 d4 exd4 cxd4 Bb4+ Bd2 Bxd2+ Nbxd2 d5 exd5 Nxd5 Qb3 Nce7 O-O O-O" },
	{ 2, "Italian Game: Two Knights Defence", "e4 e5 Nf3 Nc6 Bc4 Nf6 / Ng5 d5 exd5 Na5 Bb5+ c6 dxc6 bxc6 Be2 h6 Nf3 e4 Ne5 Bd6" },
	{ 1, "Italian Game: Two Knights Defence", "e4 e5 Nf3 Nc6 Bc4 Nf6 d3 Be7 O-O O-O Re1 d6 a4 a6" },
	{ 1, "Italian Game: Evans Gambit", "e4 e5 Nf3 Nc6 Bc4 Bc5 b4 / Bxb4 c3 Ba5 d4 exd4 O-O d6 cxd4 Bb6" },
	{ 2, "Scotch Game", "e4 e5 Nf3 Nc6 d4 / exd4 Nxd4 Nf6 Nxc6 bxc6 e5 Qe7 Qe2 Nd5 c4 Ba6 b3 g6" },
	{ 1, "Scotch Game: Classical Variation", "e4 e5 Nf3 Nc6 d4 exd4 Nxd4 Bc5 / Be3 Qf6 c3 Nge7 Bc4 Ne5 Be2 Qg6" },
	{ 4, "Ruy Lopez", "e4 e5 Nf3 Nc6 Bb5 / a6 Ba4 Nf6 O-O Be7 Re1 b5 Bb3 d6 c3 O-O h3 Nb8 d4 Nbd7" },
	{ 2, "Ruy Lopez: Open Variation", "e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6 O-O Nxe4 / d4 b5 Bb3 d5 dxe5 Be6 c3 Be7 Nbd2 O-O Qe2 Nc5 Bc2 Bf5" },
	{ 3, "Ruy Lopez: Berlin Defence", "e4 e5 Nf3 Nc6 Bb5 Nf6 / O-O Nxe4 d4 Nd6 Bxc6 dxc6 dxe5 Nf5 Qxd8+ Kxd8 Nc3 Ke8 h3 h5 Ne2 Ne7" },
	{ 1, "Ruy Lopez: Exchange Variation", "e4 e5 Nf3 Nc6 Bb5 a6 Bxc6 / dxc6 O-O f6 d4 exd4 Nxd4 c5 Nb3 Qxd1 Rxd1 Bg4 f3 Bd7" },
	{ 2, "Petrov's Defence", "e4 e5 Nf3 Nf6 / Nxe5 d6 Nf3 Nxe4 d4 d5 Bd3 Nc6 O-O Be7 c4 Nb4 Be2 O-O Nc3 Bf5" },
	{ 1, "Philidor Defence", "e4 e5 Nf3 d6 / d4 Nf6 Nc3 Nbd7 Bc4 Be7 O-O O-O Re1 c6 a4 b6" },
	{ 2, "Four Knights Game", "e4 e5 Nf3 Nc6 Nc3 Nf6 / Bb5 Bb4 O-O O-O d3 Bxc3 bxc3 d6 Bg5 Qe7 Re1 Nd8 d4 Ne6" },
	{ 1, "Vienna Game", "e4 e5 Nc3 / Nf6 f4 d5 fxe5 Nxe4 Nf3 Be7 d4 O-O Bd3 f5 exf6 Bxf6 O-O Nc6" },
	{ 1, "King's Gambit Accepted", "e4 e5 f4 exf4 / Nf3 d5 exd5 Nf6 Bb5+ c6 dxc6 Nxc6 d4 Bd6 O-O O-O" },
	{ 1, "King's Gambit Declined", "e4 e5 f4 Bc5 / Nf3 d6 c3 Nf6 d4 exd4 cxd4 Bb4+ Nc3 Nxe4" },
	{ 1, "Bishop's Opening", "e4 e5 Bc4 / Nf6 d3 c6 Nf3 d5 Bb3 Bd6 Nc3 dxe4 dxe4 Qc7" },

	/* 1.e4 c5 */
	{ 6, "Sicilian Defence", "e4 c5 /" },
	{ 4, "Sicilian Defence: Najdorf Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6 / Be3 e5 Nb3 Be6 f3 Be7 Qd2 O-O O-O-O Nbd7 g4 b5 g5 b4 Ne2 Ne8 f4 a5" },
	{ 2, "Sicilian Defence: Najdorf Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6 Be2 e5 Nb3 Be7 O-O O-O Be3 Be6 f4 Qc7" },
	{ 2, "Sicilian Defence: Najdorf Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6 Bg5 e6 f4 Be7 Qf3 Qc7 O-O-O Nbd7 g4 b5" },
	{ 3, "Sicilian Defence: Dragon Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 g6 / Be3 Bg7 f3 O-O Qd2 Nc6 Bc4 Bd7 O-O-O Rc8 Bb3 Ne5 h4 h5" },
	{ 2, "Sicilian Defence: Classical Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 Nc6 / Bg5 e6 Qd2 a6 O-O-O Bd7 f4 b5 Nxc6 Bxc6 Bxf6 gxf6 Bd3 Qb6" },
	{ 2, "Sicilian Defence: Scheveningen Variation", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 e6 / Be2 Be7 O-O O-O f4 Nc6 Be3 Qc7 Kh1 a6 Qe1 Nxd4 Bxd4 b5" },
	{ 3, "Sicilian Defence: Sveshnikov Variation", "e4 c5 Nf3 Nc6 d4 cxd4 Nxd4 Nf6 Nc3 e5 / Ndb5 d6 Bg5 a6 Na3 b5 Nd5 Be7 Bxf6 Bxf6 c3 O-O Nc2 Bg5 a4 bxa4 Rxa4 a5" },
	{ 2, "Sicilian Defence: Taimanov Variation", "e4 c5 Nf3 e6 d4 cxd4 Nxd4 Nc6 / Nc3 Qc7 Be3 a6 Qd2 Nf6 O-O-O Be7 f3 O-O g4 b5" },
	{ 1, "Sicilian Defence: Kan Variation", "e4 c5 Nf3 e6 d4 cxd4 Nxd4 a6 / Bd3 Nf6 O-O Qc7 Qe2 d6 c4 g6 Nc3 Bg7" },
	{ 2, "Sicilian Defence: Alapin Variation", "e4 c5 c3 / Nf6 e5 Nd5 d4 cxd4 Nf3 Nc6 cxd4 d6 Bc4 Nb6 Bb5 dxe5 Nxe5 Bd7" },
	{ 1, "Sicilian Defence: Closed Variation", "e4 c5 Nc3 / Nc6 g3 g6 Bg2 Bg7 d3 d6 Be3 e6 Qd2 Nge7 Nge2 O-O" },
	{ 1, "Sicilian Defence: Smith-Morra Gambit", "e4 c5 d4 cxd4 c3 / dxc3 Nxc3 Nc6 Nf3 d6 Bc4 e6 O-O Nf6 Qe2 Be7 Rd1 e5" },

	/* 1.e4 e6, c6, d5, d6, Nf6 */
	{ 4, "French Defence", "e4 e6 d4 d5 /" },
	{ 2, "French Defence: Classical Variation", "e4 e6 d4 d5 Nc3 Nf6 / Bg5 Be7 e5 Nfd7 Bxe7 Qxe7 f4 O-O Nf3 c5 Qd2 Nc6 O-O-O a6 dxc5 Nxc5 Kb1 b5" },
	{ 2, "French Defence: Winawer Variation", "e4 e6 d4 d5 Nc3 Bb4 / e5 c5 a3 Bxc3+ bxc3 Ne7 Qg4 Qc7 Qxg7 Rg8 Qxh7 cxd4 Ne2 Nbc6 f4 Bd7" },
	{ 2, "French Defence: Tarrasch Variation", "e4 e6 d4 d5 Nd2 / Nf6 e5 Nfd7 Bd3 c5 c3 Nc6 Ne2 cxd4 cxd4 f6 exf6 Nxf6 Nf3 Bd6 O-O O-O" },
	{ 2, "French Defence: Advance Variation", "e4 e6 d4 d5 e5 / c5 c3 Nc6 Nf3 Qb6 a3 Nh6 b4 cxd4 cxd4 Nf5 Bb2 Bd7 Be2 Rc8" },
	{ 1, "French Defence: Exchange Variation", "e4 e6 d4 d5 exd5 / exd5 Nf3 Nf6 Bd3 Bd6 O-O O-O Bg5 Bg4 Nbd2 Nbd7 c3 c6" },
	{ 3, "Caro-Kann Defence", "e4 c6 d4 d5 /" },
	{ 3, "Caro-Kann Defence: Classical Variation", "e4 c6 d4 d5 Nc3 dxe4 Nxe4 Bf5 / Ng3 Bg6 h4 h6 Nf3 Nd7 h5 Bh7 Bd3 Bxd3 Qxd3 e6 Bf4 Qa5+ Bd2 Qc7" },
	{ 2, "Caro-Kann Defence: Advance Variation", "e4 c6 d4 d5 e5 / Bf5 Nf3 e6 Be2 c5 Be3 cxd4 Nxd4 Nc6 O-O Qb6 Nb3 Be7" },
	{ 2, "Caro-Kann Defence: Panov Attack", "e4 c6 d4 d5 exd5 cxd5 c4 / Nf6 Nc3 e6 Nf3 Be7 cxd5 Nxd5 Bd3 Nc6 O-O O-O Re1 b6" },
	{ 2, "Scandinavian Defence", "e4 d5 / exd5 Qxd5 Nc3 Qa5 d4 c6 Nf3 Nf6 Bc4 Bf5 Bd2 e6 Nd5 Qd8 Nxf6+ Qxf6" },
	{ 2, "Pirc Defence", "e4 d6 d4 Nf6 Nc3 g6 / Nf3 Bg7 Be2 O-O O-O c6 a4 Nbd7 Re1 e5 h3 Qc7" },
	{ 1, "Pirc Defence: Austrian Attack", "e4 d6 d4 Nf6 Nc3 g6 f4 / Bg7 Nf3 c5 dxc5 Qa5 Bd3 Qxc5 Qe2 O-O Be3 Qa5 O-O Nc6" },
	{ 2, "Alekhine's Defence", "e4 Nf6 e5 Nd5 / d4 d6 Nf3 Bg4 Be2 e6 O-O Be7 h3 Bh5 c4 Nb6 Nc3 O-O" },

	/* 1.d4 d5 */
	{ 3, "Queen's Gambit", "d4 d5 c4 /" },
	{ 4, "Queen's Gambit Declined", "d4 d5 c4 e6 / Nc3 Nf6 Bg5 Be7 e3 O-O Nf3 Nbd7 Rc1 c6 Bd3 dxc4 Bxc4 Nd5 Bxe7 Qxe7 O-O Nxc3 Rxc3 e5" },
	{ 2, "Queen's Gambit Declined: Tarrasch Defence", "d4 d5 c4 e6 Nc3 c5 / cxd5 exd5 Nf3 Nc6 g3 Nf6 Bg2 Be7 O-O O-O Bg5 cxd4 Nxd4 h6 Be3 Re8" },
	{ 2, "Queen's Gambit Accepted", "d4 d5 c4 dxc4 / Nf3 Nf6 e3 e6 Bxc4 c5 O-O a6 Qe2 b5 Bb3 Nc6 Rd1 Qc7" },
	{ 3, "Slav Defence", "d4 d5 c4 c6 / Nf3 Nf6 Nc3 dxc4 a4 Bf5 e3 e6 Bxc4 Bb4 O-O Nbd7 Qe2 Bg6 e4 O-O" },
	{ 3, "Semi-Slav Defence", "d4 d5 c4 c6 Nf3 Nf6 Nc3 e6 / e3 Nbd7 Bd3 dxc4 Bxc4 b5 Bd3 Bb7 O-O a6 e4 c5 d5 c4 Bc2 Qc7" },
	{ 3, "London System", "d4 d5 Nf3 Nf6 Bf4 / e6 e3 c5 c3 Nc6 Nbd2 Bd6 Bg3 O-O Bd3 b6 O-O Bb7 Ne5 Nxe5 Bxe5 Bxe5 dxe5 Nd7" },
	{ 1, "Colle System", "d4 d5 Nf3 Nf6 e3 / e6 Bd3 c5 b3 Nc6 O-O Bd6 Bb2 O-O Nbd2 b6 dxc5 Bxc5" },

	/* 1.d4 Nf6 and the Indian defences */
	{ 4, "King's Indian Defence", "d4 Nf6 c4 g6 Nc3 Bg7 e4 d6 / Nf3 O-O Be2 e5 O-O Nc6 d5 Ne7 Ne1 Nd7 Be3 f5 f3 f4 Bf2 g5" },
	{ 2, "King's Indian Defence: Samisch Variation", "d4 Nf6 c4 g6 Nc3 Bg7 e4 d6 f3 / O-O Be3 e5 Nge2 c6 Qd2 Nbd7 O-O-O a6 Kb1 b5" },
	{ 3, "Grunfeld Defence", "d4 Nf6 c4 g6 Nc3 d5 / cxd5 Nxd5 e4 Nxc3 bxc3 Bg7 Nf3 c5 Rb1 O-O Be2 cxd4 cxd4 Qa5+ Bd2 Qxa2 O-O Bg4 Bg5 Nc6" },
	{ 4, "Nimzo-Indian Defence", "d4 Nf6 c4 e6 Nc3 Bb4 / e3 O-O Bd3 d5 Nf3 c5 O-O Nc6 a3 Bxc3 bxc3 dxc4 Bxc4 Qc7 Bd3 e5 Qc2 Re8" },
	{ 2, "Nimzo-Indian Defence: Classical Variation", "d4 Nf6 c4 e6 Nc3 Bb4 Qc2 / O-O a3 Bxc3+ Qxc3 b6 Bg5 Bb7 f3 h6 Bh4 d5 e3 Nbd7 cxd5 Nxd5" },
	{ 3, "Queen's Indian Defence", "d4 Nf6 c4 e6 Nf3 b6 / g3 Ba6 b3 Bb4+ Bd2 Be7 Bg2 c6 Bc3 d5 Ne5 Nfd7 Nxd7 Nxd7 Nd2 O-O" },
	{ 3, "Catalan Opening", "d4 Nf6 c4 e6 g3 / d5 Bg2 Be7 Nf3 O-O O-O dxc4 Qc2 a6 a4 Bd7 Qxc4 Bc6 Bg5 Nbd7 Nc3 Rc8" },
	{ 2, "Benoni Defence: Modern Benoni", "d4 Nf6 c4 c5 d5 e6 Nc3 exd5 cxd5 d6 e4 g6 / Nf3 Bg7 Be2 O-O O-O Re8 Nd2 Na6 f3 Nc7 a4 b6" },
	{ 1, "Benko Gambit", "d4 Nf6 c4 c5 d5 b5 / cxb5 a6 bxa6 Bxa6 Nc3 g6 e4 Bxf1 Kxf1 d6 Nf3 Bg7 g3 O-O Kg2 Nbd7" },
	{ 1, "Budapest Gambit", "d4 Nf6 c4 e5 / dxe5 Ng4 Bf4 Nc6 Nf3 Bb4+ Nbd2 Qe7 e3 Ngxe5 Nxe5 Nxe5 Be2 Bxd2+ Qxd2 d6" },
	{ 2, "Dutch Defence", "d4 f5 /" },
	{ 1, "Dutch Defence: Classical Variation", "d4 f5 g3 Nf6 Bg2 e6 Nf3 Be7 O-O O-O c4 d6 Nc3 Qe8 Re1 Qh5 e4 fxe4 Nxe4 Nxe4 Rxe4" },
	{ 1, "Dutch Defence: Stonewall Variation", "d4 f5 g3 Nf6 Bg2 e6 Nf3 d5 / O-O Bd6 c4 c6 b3 Qe7 Ba3 Bxa3 Nxa3 O-O" },

	/* 1.c4, 1.Nf3, 1.f4, 1.b3 */
	{ 3, "English Opening: Symmetrical Variation", "c4 c5 / Nf3 Nf6 Nc3 Nc6 g3 g6 Bg2 Bg7 O-O O-O d4 cxd4 Nxd4 Nxd4 Qxd4 d6 Bg5 Be6" },
	{ 3, "English Opening: Reversed Sicilian", "c4 e5 / Nc3 Nf6 Nf3 Nc6 g3 d5 cxd5 Nxd5 Bg2 Nb6 O-O Be7 d3 O-O a3 Be6" },
	{ 2, "English Opening: Anglo-Indian Defence", "c4 Nf6 Nc3 e6 / Nf3 d5 d4 Be7 Bg5 h6 Bxf6 Bxf6 e3 O-O Qb3 c6 Rc1 Nd7" },
	{ 2, "Reti Opening", "Nf3 d5 c4 / e6 g3 Nf6 Bg2 Be7 O-O O-O b3 c5 Bb2 Nc6 e3 b6 d4 cxd4" },
	{ 2, "Zukertort Opening: King's Indian Attack", "Nf3 d5 g3 / Nf6 Bg2 e6 O-O Be7 d3 O-O Nbd2 c5 e4 Nc6 Re1 b5" },
	{ 1, "Bird's Opening", "f4 d5 Nf3 Nf6 e3 g6 Be2 Bg7 O-O O-O d3 c5 Qe1 Nc6 Nc3 Qc7" },
	{ 1, "Larsen's Opening", "b3 e5 Bb2 Nc6 e3 d5 Bb5 Bd6 Bxc6+ bxc6 d3 Qe7 Nd2 Nf6 f4 O-O" },
};

#define N_LINES ((int)(sizeof(k_lines) / sizeof(k_lines[0])))

struct bk_move {
	ch_move  m;
	uint32_t w;
};

struct bk_ent {
	uint64_t key;
	uint8_t  n;                 /* 0 = the slot is free */
	int16_t  name;              /* an index into k_lines, -1 none */
	uint8_t  name_from;         /* the ply the name was given at (a later one wins) */
	struct bk_move mv[10];
};

struct chbook {
	struct bk_ent *tab;
	size_t         mask;
	int            positions, errors;
	char           err[160];
};

static struct bk_ent *find(const struct chbook *b, uint64_t key, int create)
{
	size_t i = (size_t)(key * 0x9e3779b97f4a7c15ULL >> 20) & b->mask;
	for (;;) {
		struct bk_ent *e = &b->tab[i];
		if (e->key == 0) {
			if (!create)
				return NULL;
			e->key = key;
			e->name = -1;
			return e;
		}
		if (e->key == key)
			return e;
		i = (i + 1) & b->mask;
	}
}

struct chbook *chbook_new(void)
{
	ch_init();
	struct chbook *b = calloc(1, sizeof *b);
	if (!b)
		return NULL;
	size_t tokens = 1;
	for (int i = 0; i < N_LINES; i++)
		for (const char *s = k_lines[i].moves; *s; s++)
			tokens += *s == ' ';
	size_t cap = 1024;
	while (cap < tokens * 3)
		cap *= 2;
	b->tab = calloc(cap, sizeof *b->tab);
	if (!b->tab) {
		free(b);
		return NULL;
	}
	b->mask = cap - 1;

	for (int li = 0; li < N_LINES; li++) {
		const struct line *ln = &k_lines[li];
		struct ch_pos pos;
		ch_pos_start(&pos);
		char buf[320];
		snprintf(buf, sizeof buf, "%s", ln->moves);
		int ply = 0, from = -1;
		struct bk_ent *prev = NULL;
		/* the position at the start can carry a name too (never: the slash comes after a move) */
		for (char *save = NULL, *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
			if (!strcmp(tok, "/")) {
				from = ply;
				if (prev && ply > 0) {
					struct bk_ent *cur = find(b, pos.key, 1);
					if (cur->name < 0 || cur->name_from <= ply) {
						cur->name = (int16_t)li;
						cur->name_from = (uint8_t)ply;
					}
				}
				continue;
			}
			ch_move m = ch_move_from_san(&pos, tok);
			if (!m) {
				if (!b->errors++)
					snprintf(b->err, sizeof b->err, "%s: %s (after %d plies)", ln->name, tok, ply);
				break;
			}
			struct bk_ent *e = find(b, pos.key, 1);
			int k = 0;
			while (k < e->n && e->mv[k].m != m)
				k++;
			if (k == e->n && e->n < 10) {
				e->mv[k].m = m;
				e->mv[k].w = 0;
				e->n++;
			}
			if (k < e->n)
				e->mv[k].w += (uint32_t)ln->weight;
			struct ch_undo u;
			ch_make(&pos, m, &u);
			ply++;
			prev = e;
			/* the position reached: it gets the line's name when the line has named it already (from the slash on) */
			if (from >= 0 && ply >= from) {
				struct bk_ent *cur = find(b, pos.key, 1);
				if (cur->name < 0 || cur->name_from <= ply) {
					cur->name = (int16_t)li;
					cur->name_from = (uint8_t)ply;
				}
			}
		}
		/* a line that ends with the slash: the position after its last move has the name */
		if (from >= 0 && from == ply && ply > 0) {
			struct bk_ent *cur = find(b, pos.key, 1);
			if (cur->name < 0 || cur->name_from <= ply) {
				cur->name = (int16_t)li;
				cur->name_from = (uint8_t)ply;
			}
		}
	}
	for (size_t i = 0; i <= b->mask; i++)
		b->positions += b->tab[i].key != 0;
	return b;
}

void chbook_free(struct chbook *b)
{
	if (!b)
		return;
	free(b->tab);
	free(b);
}

int chbook_lines(const struct chbook *b) { (void)b; return N_LINES; }
int chbook_positions(const struct chbook *b) { return b->positions; }
int chbook_errors(const struct chbook *b) { return b->errors; }
const char *chbook_error_text(const struct chbook *b) { return b->errors ? b->err : ""; }

int chbook_ply(const struct ch_pos *p)
{
	return ((int)p->full - 1) * 2 + (p->side == CH_BLACK);
}

int chbook_moves(const struct chbook *b, const struct ch_pos *p, ch_move *out, unsigned *weight, int cap)
{
	const struct bk_ent *e = find(b, p->key, 0);
	if (!e)
		return 0;
	int n = 0;
	for (int i = 0; i < e->n && n < cap; i++) {
		out[n] = e->mv[i].m;
		if (weight)
			weight[n] = e->mv[i].w;
		n++;
	}
	return n;
}

ch_move chbook_pick(const struct chbook *b, const struct ch_pos *p, uint32_t seed, int max_ply)
{
	if (chbook_ply(p) >= max_ply)
		return 0;
	const struct bk_ent *e = find(b, p->key, 0);
	if (!e || !e->n)
		return 0;
	uint64_t total = 0;
	for (int i = 0; i < e->n; i++)
		total += e->mv[i].w;
	if (!total)
		return 0;
	uint64_t h = (p->key ^ ((uint64_t)seed * 0x9e3779b97f4a7c15ULL)) * 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	uint64_t r = h % total;
	for (int i = 0; i < e->n; i++) {
		if (r < e->mv[i].w)
			return e->mv[i].m;
		r -= e->mv[i].w;
	}
	return e->mv[0].m;
}

const char *chbook_name(const struct chbook *b, uint64_t key)
{
	const struct bk_ent *e = find(b, key, 0);
	return e && e->name >= 0 ? k_lines[e->name].name : NULL;
}

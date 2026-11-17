/* chess_test.c - the rules engine of client/chess.c: perft against the published numbers, FEN / SAN / UCI round
 * trips, make / unmake and the hash on random games, the draw rules.
 *
 *   build/chess-test                 the quick set (a few seconds)
 *   build/chess-test --deep          also the big perfts (up to a billion nodes: a few minutes)
 *   build/chess-test --all           every number of the table (the 6.9 billion one takes ten minutes)
 *   build/chess-test --divide FEN N  the perft of each move of a position (to find a wrong count)
 *
 * Exit 0 = everything held.
 */
#include "chess.h"
#include "chessbook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
	g_checks++; \
	if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return (uint32_t)(g_rng >> 11);
}

static int pos_equal(const struct ch_pos *a, const struct ch_pos *b)
{
	return !memcmp(a->sq, b->sq, 64) && a->side == b->side && a->castle == b->castle && a->ep == b->ep &&
	       a->ksq[0] == b->ksq[0] && a->ksq[1] == b->ksq[1] && a->half == b->half && a->full == b->full &&
	       a->key == b->key;
}

static int set(struct ch_pos *p, const char *fen)
{
	const char *err = NULL;
	int r = ch_pos_set_fen(p, fen, &err);
	if (r < 0)
		printf("FEN refused: %s: %s\n", fen, err);
	return r;
}

/* ---- perft ---------------------------------------------------------------------------------------------------- */

#define START "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

struct perft_case {
	const char *name, *fen;
	uint64_t nodes[8];       /* nodes[d - 1] for depth d; 0 = not known, not tried */
};

/* The first six are the well known set of the Chess Programming Wiki (the numbers of every engine); the others are
 * the usual special cases (the pin along a rank that makes an en passant capture illegal, castling that gives check,
 * promotions out of check ...).  A number that does not match may be a wrong engine OR a wrong number from memory:
 * look at the first six first. */
static const struct perft_case g_cases[] = {
	{ "start", START, { 20, 400, 8902, 197281, 4865609, 119060324 } },
	{ "kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	  { 48, 2039, 97862, 4085603, 193690690 } },
	{ "position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", { 14, 191, 2812, 43238, 674624, 11030083, 178633661 } },
	{ "position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
	  { 6, 264, 9467, 422333, 15833292, 706045033 } },
	{ "position 4 mirrored", "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
	  { 6, 264, 9467, 422333, 15833292, 706045033 } },
	{ "position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", { 44, 1486, 62379, 2103487, 89941194 } },
	{ "position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
	  { 46, 2079, 89890, 3894594, 164075551, 6923051137ULL } },
	/* special cases, from memory */
	{ "castling", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", { 26, 568, 13744, 314346, 7594526, 179862938 } },
	{ "castle rights", "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", { 26, 1141, 27826, 1274206 } },
	{ "castling prevented", "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", { 44, 0, 0, 1720476 } },
	{ "promote out of check", "2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", { 11, 133, 1442, 19174, 266199, 3821001 } },
	{ "discovered check", "8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", { 0, 0, 0, 0, 1004658 } },
	{ "promote to give check", "4k3/1P6/8/8/8/8/K7/8 w - - 0 1", { 0, 0, 0, 0, 0, 217342 } },
	{ "underpromote to give check", "8/P1k5/K7/8/8/8/8/8 w - - 0 1", { 0, 0, 0, 0, 0, 92683 } },
	{ "self stalemate", "K1k5/8/P7/8/8/8/8/8 w - - 0 1", { 0, 0, 0, 0, 0, 2217 } },
	/* (the number remembered for this one was wrong: 1255208 is what an independent perft in Python gives, too) */
	{ "stalemate and mate", "8/k1P5/8/8/8/8/8/K7 w - - 0 1", { 7, 23, 253, 1199, 16196, 78858, 1255208 } },
	{ "double check", "8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", { 0, 0, 0, 23527 } },
	{ "illegal en passant 1", "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", { 0, 0, 0, 0, 0, 1134888 } },
	{ "illegal en passant 2", "8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", { 0, 0, 0, 0, 0, 1015133 } },
	{ "en passant gives check", "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", { 0, 0, 0, 0, 0, 1440467 } },
	{ "short castling gives check", "5k2/8/8/8/8/8/8/4K2R w K - 0 1", { 0, 0, 0, 0, 0, 661072 } },
	{ "long castling gives check", "3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", { 0, 0, 0, 0, 0, 803711 } },
};

static void test_perft(int deep, int all)
{
	const uint64_t quick_limit = deep ? 1000000000ULL : 6000000;   /* --deep: up to a billion nodes (a few minutes); the 6.9 billion one only with --all */
	uint64_t total = 0;
	double t0 = now_s();
	for (size_t i = 0; i < sizeof g_cases / sizeof g_cases[0]; i++) {
		const struct perft_case *c = &g_cases[i];
		struct ch_pos p;
		if (set(&p, c->fen) < 0) {
			g_fail++;
			continue;
		}
		for (int d = 1; d <= 8; d++) {
			uint64_t want = c->nodes[d - 1];
			if (!want || (!all && want > quick_limit))
				continue;
			struct ch_pos before = p;
			uint64_t got = ch_perft(&p, d);
			total += got;
			CHECK(got == want, "perft %s depth %d: %llu, expected %llu  (build/chess-test --divide \"%s\" %d)", c->name, d,
			      (unsigned long long)got, (unsigned long long)want, c->fen, d);
			CHECK(pos_equal(&p, &before), "perft %s left the position changed", c->name);
		}
	}
	double dt = now_s() - t0;
	printf("perft: %llu nodes in %.2f s (%.1f million a second)\n", (unsigned long long)total, dt,
	       dt > 0 ? (double)total / dt / 1e6 : 0.0);
}

/* ---- FEN -------------------------------------------------------------------------------------------------------- */

static void test_fen(void)
{
	static const char *good[] = {
		START,
		"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
		"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2",
		"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
		"8/8/8/KPp4r/8/8/8/7k w - c6 0 1",
		"4k3/8/8/8/8/8/8/4K3 w - - 99 120",
		"r3k2r/8/8/8/8/8/8/R3K2R b Kq - 12 40",
	};
	for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
		struct ch_pos p;
		char out[128];
		if (set(&p, good[i]) < 0) {
			g_fail++;
			continue;
		}
		ch_pos_fen(&p, out, sizeof out);
		CHECK(!strcmp(out, good[i]), "FEN round trip: %s -> %s", good[i], out);
		CHECK(p.key == ch_pos_compute_key(&p), "FEN key");
	}

	static const char *bad[] = {
		"", "8/8/8/8/8/8/8/8 w - - 0 1",                                  /* no kings */
		"4k3/8/8/8/8/8/8/3KK3 w - - 0 1",                                  /* two white kings */
		"P3k3/8/8/8/8/8/8/4K3 w - - 0 1",                                  /* a pawn on the last rank */
		"4k3/8/8/8/8/8/8/p3K3 w - - 0 1",                                  /* a pawn on the first rank */
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",        /* the side */
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkqX - 0 1",       /* castling */
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e4 0 1",       /* en passant on the wrong rank */
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e6 0 1",       /* ... or with no pawn that moved */
		"rnbqkbnr/ppppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",       /* nine squares */
		"rnbqkbnr/pppppppp/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",          /* seven ranks */
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 extra",
		"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNZ w KQkq - 0 1",        /* a letter that is no piece */
		"4k3/8/8/8/8/8/4R3/4K3 w - - 0 1",                                 /* the side not to move is in check */
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		struct ch_pos p;
		const char *err = NULL;
		CHECK(ch_pos_set_fen(&p, bad[i], &err) < 0 && err, "a bad FEN was taken: \"%s\"", bad[i]);
	}

	struct ch_pos p;
	CHECK(set(&p, "4k3/8/8/8/8/8/8/4K3 w - -") == 0 && p.half == 0 && p.full == 1, "the counters may be left out");
	CHECK(set(&p, "4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1") == 0 && p.castle == 0, "castling without king and rooks at home is dropped");
	CHECK(set(&p, "r3k2r/8/8/8/8/8/8/R3K1N1 w KQkq - 0 1") == 0 && p.castle == (CH_WQ | CH_BK | CH_BQ),
	      "a missing rook takes its right away (%d)", p.castle);
}

/* ---- make / unmake, the hash, SAN and UCI on random games ----------------------------------------------------------- */

static void test_random_games(void)
{
	static const char *starts[] = {
		START,
		"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
		"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
		"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
		"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
		"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
	};
	long moves_seen = 0, sans = 0, captures = 0, promos = 0, castles = 0, eps = 0;
	for (int game = 0; game < 200; game++) {
		struct ch_pos p;
		if (set(&p, starts[game % 6]) < 0)
			return;
		for (int ply = 0; ply < 200; ply++) {
			ch_move mv[CH_MAX_MOVES];
			int n = ch_gen_legal(&p, mv);
			if (n == 0)
				break;
			char sanlist[CH_MAX_MOVES][16];
			for (int i = 0; i < n; i++) {
				struct ch_pos before = p;
				struct ch_undo u;
				moves_seen++;
				captures += ch_move_is_capture(&p, mv[i]);
				promos += CH_PROMO(mv[i]) != 0;
				castles += CH_TYPE(p.sq[CH_FROM(mv[i])]) == CH_KING && abs(CH_TO(mv[i]) - CH_FROM(mv[i])) == 2;
				eps += CH_TYPE(p.sq[CH_FROM(mv[i])]) == CH_PAWN && CH_TO(mv[i]) == p.ep;
				ch_make(&p, mv[i], &u);
				CHECK(p.key == ch_pos_compute_key(&p), "the hash made step by step is not the one made from scratch (game %d ply %d)", game, ply);
				CHECK(p.sq[p.ksq[CH_WHITE]] == CH_PIECE(CH_WHITE, CH_KING) && p.sq[p.ksq[CH_BLACK]] == CH_PIECE(CH_BLACK, CH_KING),
				      "a king is not where ksq says");
				char fen[128];
				struct ch_pos q;
				ch_pos_fen(&p, fen, sizeof fen);
				CHECK(ch_pos_set_fen(&q, fen, NULL) == 0, "FEN of a position reached by moves is refused: %s", fen);
				ch_unmake(&p, mv[i], &u);
				CHECK(pos_equal(&p, &before), "unmake did not restore the position (game %d ply %d move %d)", game, ply, i);

				char uci[8];
				ch_move_uci(mv[i], uci);
				CHECK(ch_move_from_uci(&p, uci) == mv[i], "UCI round trip: %s", uci);
				ch_move_san(&p, mv[i], sanlist[i]);
				if (ply % 3 == 0) {       /* (reading SAN generates the moves again: the whole set costs seconds) */
					CHECK(ch_move_from_san(&p, sanlist[i]) == mv[i], "SAN round trip: %s", sanlist[i]);
					sans++;
				}
			}
			for (int i = 0; i < n; i++)
				for (int j = i + 1; j < n; j++)
					CHECK(strcmp(sanlist[i], sanlist[j]) != 0, "two moves with one SAN: %s", sanlist[i]);

			/* the pseudo legal list holds every legal move; the captures-only list is a part of it */
			ch_move ps[CH_MAX_MOVES], cs[CH_MAX_MOVES];
			int np = ch_gen_pseudo(&p, ps, 0), nc = ch_gen_pseudo(&p, cs, 1);
			CHECK(np >= n, "pseudo legal moves fewer than legal ones");
			for (int i = 0; i < nc; i++) {
				int found = 0;
				for (int j = 0; j < np; j++)
					found |= ps[j] == cs[i];
				CHECK(found && (ch_move_is_capture(&p, cs[i]) || CH_PROMO(cs[i])), "a captures-only move that is no capture / promotion");
			}
			for (int i = 0; i < np; i++)
				if (ch_move_is_capture(&p, ps[i]) || CH_PROMO(ps[i])) {
					int found = 0;
					for (int j = 0; j < nc; j++)
						found |= cs[j] == ps[i];
					CHECK(found, "a capture is missing from the captures-only list");
				}

			struct ch_undo u;
			ch_make(&p, mv[rnd() % (uint32_t)n], &u);
		}
	}
	printf("random games: %ld moves tried, %ld SAN / UCI round trips (captures %ld, promotions %ld, castlings %ld, en passant %ld)\n",
	       moves_seen, sans, captures, promos, castles, eps);
	CHECK(promos > 20 && castles > 20 && eps > 3, "the random games did not reach promotions / castling / en passant");
}

/* ---- SAN written the way a book writes it ------------------------------------------------------------------------------ */

static void expect_san(const char *fen, const char *uci, const char *san)
{
	struct ch_pos p;
	if (set(&p, fen) < 0) {
		g_fail++;
		return;
	}
	ch_move m = ch_move_from_uci(&p, uci);
	CHECK(m != 0, "%s is not a legal move in %s", uci, fen);
	if (!m)
		return;
	char out[16];
	ch_move_san(&p, m, out);
	CHECK(!strcmp(out, san), "SAN of %s in %s: \"%s\", expected \"%s\"", uci, fen, out, san);
}

static void test_san(void)
{
	expect_san(START, "e2e4", "e4");
	expect_san(START, "g1f3", "Nf3");
	expect_san("4k3/8/8/8/8/8/8/1N1K1N2 w - - 0 1", "b1d2", "Nbd2");
	expect_san("4k3/8/8/8/8/8/8/1N1K1N2 w - - 0 1", "f1d2", "Nfd2");
	expect_san("4k3/8/8/R7/8/8/8/R3K3 w - - 0 1", "a1a3", "R1a3");
	expect_san("4k3/8/8/R7/8/8/8/R3K3 w - - 0 1", "a5a3", "R5a3");
	expect_san("4k3/8/8/8/8/Q7/8/Q1Q1K3 w - - 0 1", "a1b2", "Qa1b2");
	expect_san("4k3/8/8/8/8/Q7/8/Q1Q1K3 w - - 0 1", "a3b2", "Q3b2");
	expect_san("4k3/8/8/8/8/Q7/8/Q1Q1K3 w - - 0 1", "c1b2", "Qcb2");
	expect_san("7k/P7/8/8/8/8/8/K7 w - - 0 1", "a7a8q", "a8=Q+");
	expect_san("7k/P7/8/8/8/8/8/K7 w - - 0 1", "a7a8n", "a8=N");
	expect_san("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1", "O-O");
	expect_san("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1", "O-O-O");
	expect_san("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "e8g8", "O-O");
	expect_san("r4k2/8/8/8/8/8/8/R3K2R w KQ - 0 1", "e1c1", "O-O-O");
	expect_san("5k2/8/8/8/8/8/8/R3K2R w KQ - 0 1", "e1g1", "O-O+");        /* the rook on f1 checks along the f file */
	expect_san("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", "exd6");
	expect_san("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", "exd5");
	expect_san("6k1/5ppp/8/8/8/8/8/R3K3 w - - 0 1", "a1a8", "Ra8#");

	/* reading */
	struct ch_pos p;
	set(&p, START);
	CHECK(ch_move_from_san(&p, "Nf3") == ch_move_from_uci(&p, "g1f3"), "read Nf3");
	CHECK(ch_move_from_san(&p, "Nf3+") == ch_move_from_uci(&p, "g1f3"), "a stray + is ignored");
	CHECK(ch_move_from_san(&p, "e5") == 0, "an illegal pawn move");
	CHECK(ch_move_from_san(&p, "Ng3") == 0, "a knight that cannot go there");
	CHECK(ch_move_from_san(&p, "") == 0 && ch_move_from_san(&p, "O-O") == 0, "empty / castling at the start");
	set(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
	CHECK(ch_move_from_san(&p, "0-0-0") == ch_move_from_uci(&p, "e1c1"), "0-0-0 reads as castling");
	CHECK(ch_move_from_uci(&p, "e1e1") == 0 && ch_move_from_uci(&p, "zz") == 0 && ch_move_from_uci(&p, NULL) == 0, "bad UCI");
	set(&p, "7k/P7/8/8/8/8/8/K7 w - - 0 1");
	CHECK(ch_move_from_uci(&p, "a7a8") == 0, "a promotion in UCI must name the piece");
	CHECK(ch_move_find(&p, 48, 56, 0) == ch_move_from_uci(&p, "a7a8q"), "ch_move_find: no piece named = a queen");
	CHECK(ch_move_find(&p, 48, 56, CH_KNIGHT) == ch_move_from_uci(&p, "a7a8n"), "ch_move_find: a knight");
}

/* ---- the rules that are easy to get wrong -------------------------------------------------------------------------------- */

static int has_move(const struct ch_pos *p, const char *uci)
{
	return ch_move_from_uci(p, uci) != 0;
}

static void test_special_rules(void)
{
	struct ch_pos p;

	/* en passant that would open the rank to a rook is illegal */
	set(&p, "8/8/8/KPp4r/8/8/8/7k w - c6 0 1");
	CHECK(!has_move(&p, "b5c6"), "en passant along a pinned rank was allowed");
	CHECK(has_move(&p, "b5b6"), "the pawn can still step");
	/* ... and an ordinary one is fine, and takes the right pawn */
	set(&p, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
	ch_move m = ch_move_from_uci(&p, "e5d6");
	CHECK(m != 0, "en passant not generated");
	struct ch_undo u;
	ch_make(&p, m, &u);
	CHECK(p.sq[35] == 0 && p.sq[43] == CH_PIECE(CH_WHITE, CH_PAWN) && p.half == 0, "en passant did not remove the pawn");
	ch_unmake(&p, m, &u);
	CHECK(p.sq[35] == CH_PIECE(CH_BLACK, CH_PAWN) && p.sq[36] == CH_PIECE(CH_WHITE, CH_PAWN) && p.sq[43] == 0, "en passant not undone");
	/* the en passant right lasts one move only */
	set(&p, START);
	ch_make(&p, ch_move_from_uci(&p, "e2e4"), &u);
	CHECK(p.ep == 20, "ep after e2e4 (%d)", p.ep);
	ch_make(&p, ch_move_from_uci(&p, "g8f6"), &u);
	CHECK(p.ep == -1, "ep must be gone");

	/* castling: not through, out of or into check; not over a piece; the rights go with the pieces */
	set(&p, "4k3/8/8/8/8/8/5r2/R3K2R w KQ - 0 1");
	CHECK(!has_move(&p, "e1g1") && has_move(&p, "e1c1"), "castling through an attacked square");
	set(&p, "4k3/8/8/8/8/8/4r3/R3K2R w KQ - 0 1");
	CHECK(!has_move(&p, "e1g1") && !has_move(&p, "e1c1"), "castling out of check");
	set(&p, "4k3/8/8/8/8/8/6r1/R3K2R w KQ - 0 1");
	CHECK(!has_move(&p, "e1g1") && has_move(&p, "e1c1"), "castling into check");
	set(&p, "4k3/8/8/8/8/8/1r6/R3K2R w KQ - 0 1");
	CHECK(has_move(&p, "e1g1") && has_move(&p, "e1c1"), "an attacked b1 does not stop the long castling (b1 is the rook's square)");
	set(&p, "4k3/8/8/8/8/8/8/RN2K2R w KQ - 0 1");
	CHECK(has_move(&p, "e1g1") && !has_move(&p, "e1c1"), "castling over a knight");
	set(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
	ch_make(&p, ch_move_from_uci(&p, "e1g1"), &u);
	CHECK(p.sq[6] == CH_PIECE(CH_WHITE, CH_KING) && p.sq[5] == CH_PIECE(CH_WHITE, CH_ROOK) && p.sq[7] == 0 && p.castle == (CH_BK | CH_BQ),
	      "short castling result");
	set(&p, "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
	ch_make(&p, ch_move_from_uci(&p, "e8c8"), &u);
	CHECK(p.sq[58] == CH_PIECE(CH_BLACK, CH_KING) && p.sq[59] == CH_PIECE(CH_BLACK, CH_ROOK) && p.sq[56] == 0 && p.castle == (CH_WK | CH_WQ),
	      "long castling result");
	set(&p, "r3k2r/8/8/8/8/8/6B1/R3K2R w KQkq - 0 1");
	ch_make(&p, ch_move_from_uci(&p, "g2a8"), &u);
	char fen[128];
	ch_pos_fen(&p, fen, sizeof fen);
	CHECK(!strcmp(fen, "B3k2r/8/8/8/8/8/8/R3K2R b KQk - 0 1"), "taking a rook takes the right: %s", fen);
	set(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
	ch_make(&p, ch_move_from_uci(&p, "h1h2"), &u);
	CHECK(p.castle == (CH_WQ | CH_BK | CH_BQ), "a rook that moved loses its right");
	ch_make(&p, ch_move_from_uci(&p, "e8e7"), &u);
	CHECK(p.castle == CH_WQ, "a king that moved loses both");

	/* promotion */
	set(&p, "4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_legal(&p, mv), promos = 0;
	for (int i = 0; i < n; i++)
		promos += CH_PROMO(mv[i]) != 0;
	CHECK(promos == 4, "four promotions (%d)", promos);
	ch_make(&p, ch_move_from_uci(&p, "a7a8n"), &u);
	CHECK(p.sq[56] == CH_PIECE(CH_WHITE, CH_KNIGHT), "a promotion to a knight");
	set(&p, "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
	ch_move capture_promo = ch_move_from_uci(&p, "a7b8r");
	ch_make(&p, capture_promo, &u);
	CHECK(p.sq[57] == CH_PIECE(CH_WHITE, CH_ROOK) && p.sq[48] == 0, "capture and promote");
	ch_unmake(&p, capture_promo, &u);
	CHECK(p.sq[57] == CH_PIECE(CH_BLACK, CH_ROOK) && p.sq[48] == CH_PIECE(CH_WHITE, CH_PAWN), "capture and promote undone");

	/* a check must be answered; a pinned piece stays on its line */
	set(&p, "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1");
	n = ch_gen_legal(&p, mv);
	CHECK(n == 3 && has_move(&p, "e1e2") && has_move(&p, "e1d1") && has_move(&p, "e1f1"), "answers to a check (%d moves)", n);
	set(&p, "4k3/4r3/8/8/8/8/4N3/4K3 w - - 0 1");
	CHECK(!has_move(&p, "e2c3") && !has_move(&p, "e2g3"), "a pinned knight moved");

	/* the hash: the same position by another move order, the en passant file counts only when it matters */
	struct ch_pos a, b;
	set(&a, START);
	set(&b, START);
	const char *o1[] = { "g1f3", "g8f6", "b1c3", "b8c6" }, *o2[] = { "b1c3", "b8c6", "g1f3", "g8f6" };
	for (int i = 0; i < 4; i++) {
		ch_make(&a, ch_move_from_uci(&a, o1[i]), &u);
		ch_make(&b, ch_move_from_uci(&b, o2[i]), &u);
	}
	CHECK(a.key == b.key && pos_equal(&a, &b), "a transposition has another hash");
	set(&a, START);
	ch_make(&a, ch_move_from_uci(&a, "e2e4"), &u);
	set(&b, "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
	CHECK(a.key == b.key, "an en passant square nobody can use must not change the hash");
	set(&a, "rnbqkbnr/pppp1ppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
	set(&b, "rnbqkbnr/pppp1ppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
	CHECK(a.key != b.key, "an en passant capture that can be made must change the hash");
}

/* ---- games ------------------------------------------------------------------------------------------------------------------ */

static int play_san(struct ch_game *g, const char *list)
{
	char buf[256];
	snprintf(buf, sizeof buf, "%s", list);
	for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
		ch_move m = ch_move_from_san(&g->pos, tok);
		if (!m || ch_game_play(g, m) < 0) {
			printf("could not play %s\n", tok);
			return -1;
		}
	}
	return 0;
}

static enum ch_status status_of_fen(const char *fen)
{
	struct ch_game g;
	const char *err;
	if (ch_game_init(&g, fen, &err) < 0) {
		printf("FEN refused: %s: %s\n", fen, err);
		return (enum ch_status)-1;
	}
	enum ch_status s = ch_game_status(&g);
	ch_game_free(&g);
	return s;
}

static void test_games(void)
{
	struct ch_game g;
	CHECK(ch_game_init(&g, NULL, NULL) == 0 && ch_game_status(&g) == CH_ONGOING, "a new game");

	/* mates */
	CHECK(play_san(&g, "f3 e5 g4 Qh4#") == 0 && ch_game_status(&g) == CH_CHECKMATE && g.pos.side == CH_WHITE, "fool's mate");
	CHECK(!strcmp(g.ply[3].san, "Qh4#") && !strcmp(g.ply[0].san, "f3"), "the SAN written while playing: %s", g.ply[3].san);
	CHECK(ch_game_play(&g, ch_move_from_uci(&g.pos, "a2a3")) < 0, "no move after the mate");
	while (ch_game_undo(&g) == 0) {}
	CHECK(g.n == 0 && ch_game_status(&g) == CH_ONGOING && ch_game_repetitions(&g) == 1, "all taken back");
	CHECK(ch_game_undo(&g) < 0, "nothing to take back");
	char fen[128];
	ch_pos_fen(&g.pos, fen, sizeof fen);
	CHECK(!strcmp(fen, START), "back at the start: %s", fen);
	ch_game_free(&g);

	ch_game_init(&g, NULL, NULL);
	CHECK(play_san(&g, "e4 e5 Bc4 Nc6 Qh5 Nf6 Qxf7#") == 0 && ch_game_status(&g) == CH_CHECKMATE, "scholar's mate");
	CHECK(ch_game_play(&g, 0) < 0, "no move is no move");
	ch_game_free(&g);

	/* drawn: stalemate, bare kings, bishops of one colour, the fifty moves */
	CHECK(status_of_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1") == CH_STALEMATE, "stalemate");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/8/8 w - - 0 1") == CH_DRAW_MATERIAL, "K against K");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/8/N7 w - - 0 1") == CH_DRAW_MATERIAL, "K and a knight against K");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/8/B1b5 w - - 0 1") == CH_DRAW_MATERIAL, "bishops on the same colour");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/8/Bb6 w - - 0 1") == CH_ONGOING, "bishops on different colours can still mate");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/8/NN6 w - - 0 1") == CH_ONGOING, "two knights");
	CHECK(status_of_fen("8/8/4k3/8/8/3K4/P7/8 w - - 0 1") == CH_ONGOING, "a pawn is enough");
	CHECK(status_of_fen("4k3/8/8/8/8/8/R7/4K3 w - - 100 80") == CH_DRAW_FIFTY, "fifty moves");
	CHECK(status_of_fen("4k3/8/8/8/8/8/R7/4K3 w - - 99 80") == CH_ONGOING, "49 and a half moves");

	ch_game_init(&g, "4k3/8/8/8/8/8/R7/4K3 w - - 99 80", NULL);
	CHECK(play_san(&g, "Ra3") == 0 && g.pos.half == 100 && ch_game_status(&g) == CH_DRAW_FIFTY, "the 100th ply makes the draw");
	ch_game_free(&g);
	ch_game_init(&g, "6k1/5ppp/8/8/8/8/8/R3K3 w - - 99 80", NULL);
	CHECK(play_san(&g, "Ra8#") == 0 && ch_game_status(&g) == CH_CHECKMATE, "a mate on the 100th ply is a mate, not a draw");
	ch_game_free(&g);

	/* three times the same position */
	ch_game_init(&g, NULL, NULL);
	play_san(&g, "Nf3 Nf6 Ng1 Ng8");
	CHECK(ch_game_repetitions(&g) == 2 && ch_game_status(&g) == CH_ONGOING, "twice is not enough (%d)", ch_game_repetitions(&g));
	play_san(&g, "Nf3 Nf6 Ng1 Ng8");
	CHECK(ch_game_repetitions(&g) == 3 && ch_game_status(&g) == CH_DRAW_REPETITION, "three times is a draw (%d)", ch_game_repetitions(&g));
	ch_game_undo(&g);
	CHECK(ch_game_status(&g) == CH_ONGOING, "taking back a move ends the repetition");
	ch_game_free(&g);

	/* a move that cannot be repeated (a pawn move) resets the count */
	ch_game_init(&g, NULL, NULL);
	play_san(&g, "Nf3 Nf6 Ng1 Ng8 e4 e5 Nf3 Nf6 Ng1 Ng8");
	CHECK(ch_game_repetitions(&g) == 2, "pawn moves cut the history (%d)", ch_game_repetitions(&g));
	ch_game_free(&g);

	/* a long game grows the arrays */
	ch_game_init(&g, NULL, NULL);
	for (int i = 0; i < 300; i++)
		play_san(&g, i % 2 ? "Ng1 Ng8" : "Nf3 Nf6");
	CHECK(g.n == 600 && g.cap >= 600, "long game (%d plies)", g.n);
	while (ch_game_undo(&g) == 0) {}
	ch_pos_fen(&g.pos, fen, sizeof fen);
	CHECK(!strcmp(fen, START), "the long game undone: %s", fen);
	ch_game_free(&g);

	/* a start from a FEN */
	CHECK(ch_game_init(&g, "not a fen", NULL) < 0, "a bad FEN starts no game");
	const char *err = NULL;
	CHECK(ch_game_init(&g, "8/8/8/8/8/8/8/8 w - - 0 1", &err) < 0 && err, "no kings");
}

/* ---- the opening book -------------------------------------------------------------------------------------------- */

static void test_book(void)
{
	struct chbook *b = chbook_new();
	CHECK(b != NULL, "the book was not made");
	if (!b)
		return;
	/* every move of every line is legal (a typo in a line shows here: the rest of that line is dropped) */
	CHECK(chbook_errors(b) == 0, "%d lines hold a move that is not legal; the first: %s", chbook_errors(b), chbook_error_text(b));
	printf("book: %d lines, %d positions\n", chbook_lines(b), chbook_positions(b));
	CHECK(chbook_lines(b) >= 60 && chbook_positions(b) >= 500, "the book is small: %d lines, %d positions", chbook_lines(b), chbook_positions(b));

	struct ch_pos p;
	ch_pos_start(&p);
	ch_move mv[10];
	unsigned w[10];
	int n = chbook_moves(b, &p, mv, w, 10);
	CHECK(n >= 5, "the first moves of the book: %d", n);
	unsigned we4 = 0, wd4 = 0, wb3 = 0, total = 0;
	for (int i = 0; i < n; i++) {
		char u[8];
		ch_move_uci(mv[i], u);
		we4 += !strcmp(u, "e2e4") ? w[i] : 0;
		wd4 += !strcmp(u, "d2d4") ? w[i] : 0;
		wb3 += !strcmp(u, "b2b3") ? w[i] : 0;
		total += w[i];
	}
	CHECK(we4 > 0 && wd4 > 0 && wb3 > 0 && we4 > wb3 * 3 && wd4 > wb3 * 3, "e4 %u, d4 %u, b3 %u", we4, wd4, wb3);

	/* the moves chosen are the book's, by weight, and the same for the same seed */
	int seen_e4 = 0, seen_d4 = 0, other = 0;
	for (uint32_t seed = 1; seed <= 600; seed++) {
		ch_move m = chbook_pick(b, &p, seed, 40);
		CHECK(m != 0 && ch_move_find(&p, CH_FROM(m), CH_TO(m), 0) == m, "a book move that is not legal");
		CHECK(chbook_pick(b, &p, seed, 40) == m, "the same seed gives another move");
		char u[8];
		ch_move_uci(m, u);
		seen_e4 += !strcmp(u, "e2e4");
		seen_d4 += !strcmp(u, "d2d4");
		other += strcmp(u, "e2e4") && strcmp(u, "d2d4");
	}
	CHECK(seen_e4 > 150 && seen_d4 > 100 && other > 30, "the choice by weight: e4 %d, d4 %d, other %d of 600", seen_e4, seen_d4, other);
	CHECK(chbook_pick(b, &p, 1, 0) == 0, "max_ply 0: no book");

	/* names: the opening is named from the move that makes it, and deeper lines have more specific names */
	static const struct { const char *san, *name; } names[] = {
		{ "e4", "King's Pawn Opening" },
		{ "e4 c5", "Sicilian Defence" },
		{ "e4 e5 Nf3 Nc6 Bb5", "Ruy Lopez" },
		{ "e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6 O-O Be7", "Ruy Lopez" },
		{ "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6", "Sicilian Defence: Najdorf Variation" },
		{ "d4 Nf6 c4 e6 Nc3 Bb4", "Nimzo-Indian Defence" },
		{ "e4 e6 d4 d5 Nc3 Bb4", "French Defence: Winawer Variation" },
		{ "d4 d5 c4 e6", "Queen's Gambit Declined" },
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		struct ch_pos q;
		ch_pos_start(&q);
		char line[160];
		snprintf(line, sizeof line, "%s", names[i].san);
		for (char *save = NULL, *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
			struct ch_undo u;
			ch_move m = ch_move_from_san(&q, tok);
			CHECK(m != 0, "%s: %s is not legal", names[i].san, tok);
			if (!m)
				break;
			ch_make(&q, m, &u);
		}
		const char *got = chbook_name(b, q.key);
		CHECK(got && !strcmp(got, names[i].name), "the name after \"%s\" is \"%s\", expected \"%s\"", names[i].san, got ? got : "(none)", names[i].name);
	}

	/* a transposition meets in one position: 1.d4 Nf6 2.c4 e6 and 1.c4 e6 2.d4 Nf6 */
	struct ch_pos a, c;
	ch_pos_start(&a);
	ch_pos_start(&c);
	const char *oa[] = { "d4", "Nf6", "c4", "e6" }, *oc[] = { "c4", "e6", "d4", "Nf6" };
	for (int i = 0; i < 4; i++) {
		struct ch_undo u;
		ch_make(&a, ch_move_from_san(&a, oa[i]), &u);
		ch_make(&c, ch_move_from_san(&c, oc[i]), &u);
	}
	CHECK(a.key == c.key, "the transposition has another hash");
	CHECK(chbook_pick(b, &a, 3, 40) != 0, "the transposition is in the book");

	/* a position that is no opening: nothing, and no name */
	CHECK(set(&p, "4k3/8/8/8/8/8/8/4K3 w - - 0 1") == 0 && chbook_pick(b, &p, 1, 40) == 0 && chbook_name(b, p.key) == NULL, "an endgame is in the book");
	set(&p, "rnbqkbnr/pppppppp/8/8/8/5P2/PPPPP1PP/RNBQKBNR b KQkq - 0 1");
	CHECK(chbook_pick(b, &p, 1, 40) == 0, "1.f3 is not in the book");

	/* every position of every line: the book has a legal move there (until the end of a line) */
	long visited = 0, bad = 0;
	for (int round = 0; round < 6; round++) {
		struct ch_pos q;
		ch_pos_start(&q);
		for (int ply = 0; ply < 30; ply++) {
			ch_move m = chbook_pick(b, &q, (uint32_t)(round * 7 + ply * 31 + 1), 40);
			if (!m)
				break;
			visited++;
			if (ch_move_find(&q, CH_FROM(m), CH_TO(m), CH_PROMO(m)) != m)
				bad++;
			struct ch_undo u;
			ch_make(&q, m, &u);
		}
	}
	CHECK(bad == 0 && visited > 40, "walking the book: %ld bad moves in %ld", bad, visited);
	printf("book: six walks through the book made %ld moves\n", visited);
	chbook_free(b);
}

/* ---- main ---------------------------------------------------------------------------------------------------------------------- */

static void divide_line(ch_move m, uint64_t n, void *ud)
{
	(void)ud;
	char uci[8];
	ch_move_uci(m, uci);
	printf("%s: %llu\n", uci, (unsigned long long)n);
}

int main(int argc, char **argv)
{
	ch_init();
	if (argc >= 4 && !strcmp(argv[1], "--divide")) {
		struct ch_pos p;
		if (set(&p, argv[2]) < 0)
			return 2;
		uint64_t total = ch_perft_divide(&p, atoi(argv[3]), divide_line, NULL);
		printf("\nnodes %llu\n", (unsigned long long)total);
		return 0;
	}
	int all = argc >= 2 && !strcmp(argv[1], "--all"), deep = all || (argc >= 2 && !strcmp(argv[1], "--deep"));

	test_fen();
	test_special_rules();
	test_san();
	test_games();
	test_book();
	test_random_games();
	test_perft(deep, all);

	printf("%d checks, %d failed\n", g_checks, g_fail);
	if (!g_fail)
		printf("chess-test: ok\n");
	return g_fail ? 1 : 0;
}

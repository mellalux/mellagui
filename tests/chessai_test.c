/* chessai_test.c - the search and the evaluation of client/chessai.c.
 *
 *   build/chessai-test                      the tests (about half a minute)
 *   build/chessai-test --match A B N        N games of one setting against another, colours swapped; a setting is
 *                                           dN (depth N), mN (N ms a move) or LN (strength level N); the result says
 *                                           how many points A got and what Elo difference that is about
 *   build/chessai-test --bench              nodes a second on a few positions
 *
 * What is tested, and how, so that a pass means something:
 *  - the evaluation is the same for a position and its mirror image (colours and board turned): a table that is
 *    the wrong way up fails at once;
 *  - the search in its PLAIN form (alpha-beta, table, ordering, nothing that guesses) gives exactly the value of a
 *    separately written plain minimax, and the move it chooses has that value;
 *  - mates: positions are found in random endings and a brute force mate finder (no use of the search) says what the
 *    shortest forced mate is; the real search must find a mate in that many moves, and the move it plays must be one
 *    that the brute force finder accepts;
 *  - the ends of a search: a stop from another thread, a time limit, the fifty move rule, a position with one move.
 */
#include "chessai.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
	g_checks++; \
	if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;
static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return (uint32_t)(g_rng >> 11);
}

#define START "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

static int set(struct ch_pos *p, const char *fen)
{
	const char *err = NULL;
	if (ch_pos_set_fen(p, fen, &err) < 0) {
		printf("FEN refused: %s: %s\n", fen, err);
		g_fail++;
		return -1;
	}
	return 0;
}

static void san_of(const struct ch_pos *p, ch_move m, char *out)
{
	if (!m)
		strcpy(out, "(none)");
	else
		ch_move_san(p, m, out);
}

/* a random game: a list of positions that came up (every `step` plies), captures preferred so that material thins out */
static void random_positions(struct ch_pos *out, int cap, int *count, int games, int maxply, int step, int capture_bias,
                             int max_pieces)
{
	*count = 0;
	for (int g = 0; g < games && *count < cap; g++) {
		struct ch_pos p;
		ch_pos_start(&p);
		for (int ply = 0; ply < maxply && *count < cap; ply++) {
			ch_move mv[CH_MAX_MOVES], caps[CH_MAX_MOVES];
			int n = ch_gen_legal(&p, mv), nc = 0;
			if (!n)
				break;
			for (int i = 0; i < n; i++)
				if (ch_move_is_capture(&p, mv[i]) || CH_PROMO(mv[i]))
					caps[nc++] = mv[i];
			ch_move m = (nc && (int)(rnd() % 100) < capture_bias) ? caps[rnd() % (uint32_t)nc] : mv[rnd() % (uint32_t)n];
			struct ch_undo u;
			ch_make(&p, m, &u);
			int pieces = 0;
			for (int s = 0; s < 64; s++)
				pieces += p.sq[s] != 0;
			if (ply % step == 0 && pieces <= max_pieces && ch_gen_legal(&p, mv) > 0)
				out[(*count)++] = p;
		}
	}
}

/* ----------------------------------------------------------------------------------------------- the evaluation */

static void mirror(const struct ch_pos *p, struct ch_pos *q)
{
	memset(q, 0, sizeof *q);
	for (int s = 0; s < 64; s++) {
		uint8_t pc = p->sq[s];
		int ms = ((7 - (s >> 3)) << 3) | (s & 7);
		q->sq[ms] = pc ? CH_PIECE(CH_COLOR(pc) ^ 1, CH_TYPE(pc)) : 0;
	}
	q->side = p->side ^ 1;
	q->castle = (uint8_t)(((p->castle & 3) << 2) | ((p->castle >> 2) & 3));
	q->ep = p->ep < 0 ? -1 : (int8_t)(((7 - (p->ep >> 3)) << 3) | (p->ep & 7));
	q->ksq[0] = (uint8_t)(((7 - (p->ksq[1] >> 3)) << 3) | (p->ksq[1] & 7));
	q->ksq[1] = (uint8_t)(((7 - (p->ksq[0] >> 3)) << 3) | (p->ksq[0] & 7));
	q->half = p->half;
	q->full = p->full;
	q->key = ch_pos_compute_key(q);
}

static void test_eval(void)
{
	struct ch_pos p, q;
	set(&p, START);
	CHECK(chai_eval(&p) == 8, "the start is worth only the move: %d", chai_eval(&p));

	static struct ch_pos list[1500];
	int n;
	random_positions(list, 1500, &n, 120, 140, 3, 40, 32);
	CHECK(n > 500, "random positions: %d", n);
	int bad = 0;
	for (int i = 0; i < n; i++) {
		mirror(&list[i], &q);
		int a = chai_eval(&list[i]), b = chai_eval(&q);
		if (a != b && bad++ < 3) {
			char f[128];
			ch_pos_fen(&list[i], f, sizeof f);
			printf("   eval %d, mirrored %d: %s\n", a, b, f);
		}
	}
	CHECK(bad == 0, "the evaluation is not the same for a position and its mirror image (%d of %d)", bad, n);

	/* material and the draw rule */
	set(&p, "4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
	CHECK(chai_eval(&p) > 800, "a queen up is worth about a queen: %d", chai_eval(&p));
	set(&p, "4k3/8/8/8/8/8/8/3QK3 b - - 0 1");
	CHECK(chai_eval(&p) < -800, "... and for the side without it: %d", chai_eval(&p));
	set(&p, "4k3/8/8/8/8/8/8/4KB2 w - - 0 1");
	CHECK(chai_eval(&p) == 0, "K and a bishop against K cannot win: %d", chai_eval(&p));
	set(&p, "4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
	int rook = chai_eval(&p);
	set(&p, "4k3/p7/8/8/8/8/8/R3K3 w - - 0 1");
	CHECK(rook > chai_eval(&p), "a pawn for the other side is worth something");
	/* the king belongs in the middle in the end game and in the corner in the middle game */
	set(&p, "4k3/8/8/8/3K4/8/8/7R w - - 0 1");
	int central = chai_eval(&p);
	set(&p, "4k3/8/8/8/8/8/8/K6R w - - 0 1");
	CHECK(central > chai_eval(&p), "the king in the centre of an ending (%d against %d)", central, chai_eval(&p));
}

/* ----------------------------------------------------------------------------------------- null move on the rules */

static void test_null_move(void)
{
	static struct ch_pos list[300];
	int n;
	random_positions(list, 300, &n, 40, 100, 4, 30, 32);
	for (int i = 0; i < n; i++) {
		struct ch_pos p = list[i], before = p;
		if (ch_in_check(&p))
			continue;
		struct ch_undo u;
		ch_make_null(&p, &u);
		CHECK(p.side != before.side && p.ep == -1 && p.key == ch_pos_compute_key(&p), "the null move's key / side / ep");
		ch_unmake_null(&p, &u);
		CHECK(!memcmp(p.sq, before.sq, 64) && p.side == before.side && p.ep == before.ep && p.key == before.key &&
		      p.half == before.half && p.full == before.full && p.castle == before.castle, "the null move was not undone");
	}
}

/* ------------------------------------------------------------------------------ plain minimax to compare with */

static int ref_search(struct ch_pos *p, int depth, int ply)
{
	if (depth <= 0)
		return chai_eval(p);
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_pseudo(p, mv, 0), best = -32000, legal = 0, us = p->side;
	for (int i = 0; i < n; i++) {
		struct ch_undo u;
		ch_make(p, mv[i], &u);
		if (!ch_attacked(p, p->ksq[us], us ^ 1)) {
			legal++;
			int v = -ref_search(p, depth - 1, ply + 1);
			if (v > best)
				best = v;
		}
		ch_unmake(p, mv[i], &u);
	}
	if (!legal)
		return ch_in_check(p) ? -CHAI_MATE + ply : 0;
	return best;
}

static int ref_move_value(const struct ch_pos *pos, ch_move m, int depth)
{
	struct ch_pos p = *pos;
	struct ch_undo u;
	ch_make(&p, m, &u);
	return -ref_search(&p, depth - 1, 1);
}

static void test_plain_search(struct chai *ai)
{
	static struct ch_pos list[400];
	int n;
	random_positions(list, 400, &n, 150, 120, 5, 30, 32);
	/* and the special positions, where the mates and the castling are */
	static const char *extra[] = {
		"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
		"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
		"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
		"6k1/5ppp/8/8/8/8/8/R3K3 w - - 0 1", "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1",
	};
	int tested = 0;
	for (int d = 1; d <= 4; d++) {
		int limit = d <= 2 ? 60 : d == 3 ? 40 : 12;
		for (int i = 0; i < limit + 5; i++) {
			struct ch_pos p;
			if (i >= limit) {
				if (set(&p, extra[i - limit]) < 0)
					continue;
				if (d == 4 && i - limit > 2)
					continue;
			} else if (i < n) {
				p = list[(i * 7 + d) % n];
			} else {
				continue;
			}
			ch_move mv[CH_MAX_MOVES];
			int cnt = ch_gen_legal(&p, mv);
			if (!cnt)
				continue;
			int want = -32000;
			for (int j = 0; j < cnt; j++) {
				int v = ref_move_value(&p, mv[j], d);
				if (v > want)
					want = v;
			}
			struct chai_limits lim = { .depth = d, .flags = CHAI_PLAIN };
			struct chai_result r;
			chai_clear(ai);
			chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
			char fen[128];
			ch_pos_fen(&p, fen, sizeof fen);
			CHECK(r.score == want, "plain search depth %d: %d, minimax %d  (%s)", d, r.score, want, fen);
			CHECK(r.best && ref_move_value(&p, r.best, d) == want, "plain search depth %d: the move does not have the value  (%s)", d, fen);
			tested++;
		}
	}
	printf("plain search = minimax in %d positions (depth 1 to 4)\n", tested);
}

/* ------------------------------------------------------------------------------------------ the mate finder */

/* can the side to move force a mate within n of its own moves?  (no search, no evaluation: the rules only) */
static int mate_in(const struct ch_pos *pos, int n, ch_move *first)
{
	struct ch_pos p = *pos;
	ch_move mv[CH_MAX_MOVES];
	int cnt = ch_gen_legal(&p, mv);
	for (int i = 0; i < cnt; i++) {
		struct ch_undo u;
		ch_make(&p, mv[i], &u);
		ch_move rep[CH_MAX_MOVES];
		int rn = ch_gen_legal(&p, rep), ok;
		if (rn == 0) {
			ok = ch_in_check(&p);
		} else if (n == 1) {
			ok = 0;
		} else {
			ok = 1;
			for (int j = 0; j < rn && ok; j++) {
				struct ch_undo u2;
				ch_make(&p, rep[j], &u2);
				ok = mate_in(&p, n - 1, NULL);
				ch_unmake(&p, rep[j], &u2);
			}
		}
		ch_unmake(&p, mv[i], &u);
		if (ok) {
			if (first)
				*first = mv[i];
			return 1;
		}
	}
	return 0;
}

static int move_keeps_mate(const struct ch_pos *pos, ch_move m, int n)     /* after m the mate is still there within n */
{
	struct ch_pos p = *pos;
	struct ch_undo u;
	ch_make(&p, m, &u);
	ch_move rep[CH_MAX_MOVES];
	int rn = ch_gen_legal(&p, rep);
	if (rn == 0)
		return ch_in_check(&p);
	if (n == 1)
		return 0;
	for (int j = 0; j < rn; j++) {
		struct ch_undo u2;
		ch_make(&p, rep[j], &u2);
		int ok = mate_in(&p, n - 1, NULL);
		ch_unmake(&p, rep[j], &u2);
		if (!ok)
			return 0;
	}
	return 1;
}

static void test_mates(struct chai *ai)
{
	/* positions with few pieces, where one side has the force: found in random games that prefer captures */
	static struct ch_pos list[4000];
	int n;
	random_positions(list, 4000, &n, 2500, 160, 1, 85, 6);
	printf("mate search: %d candidate positions with at most 6 pieces\n", n);

	int found[4] = { 0, 0, 0, 0 }, want[4] = { 0, 14, 10, 3 };
	struct ch_pos set_[4][16];
	for (int i = 0; i < n; i++) {
		struct ch_pos *p = &list[i];
		/* the side with the move must have a rook or queen, else mates are rare and the finder slow */
		int heavy = 0, other_heavy = 0;
		for (int s = 0; s < 64; s++) {
			int t = CH_TYPE(p->sq[s]);
			if (p->sq[s] && (t == CH_ROOK || t == CH_QUEEN)) {
				if (CH_COLOR(p->sq[s]) == p->side)
					heavy++;
				else
					other_heavy++;
			}
		}
		if (!heavy || other_heavy)
			continue;
		int maxk = found[3] < want[3] ? 3 : found[2] < want[2] ? 2 : 1, shortest = 0;
		for (int k = 1; k <= maxk && !shortest; k++) {
			if (k == 3) {
				int pieces = 0;
				for (int s = 0; s < 64; s++)
					pieces += p->sq[s] != 0;
				if (pieces > 5)
					break;
			}
			if (mate_in(p, k, NULL))
				shortest = k;
		}
		if (shortest && found[shortest] < want[shortest])
			set_[shortest][found[shortest]++] = *p;
		if (found[1] >= want[1] && found[2] >= want[2] && found[3] >= want[3])
			break;
	}
	for (int k = 1; k <= 3; k++) {
		CHECK(found[k] >= want[k] / 2, "only %d positions with a mate in %d were found", found[k], k);
		printf("  mate in %d: %d positions\n", k, found[k]);
	}

	for (int k = 1; k <= 3; k++)
		for (int i = 0; i < found[k]; i++) {
			struct ch_pos *p = &set_[k][i];
			struct chai_limits lim = { .depth = 2 * k + 2 };
			struct chai_result r;
			chai_clear(ai);
			chai_search(ai, p, NULL, 0, &lim, &r, NULL, NULL);
			char fen[128], san[16];
			ch_pos_fen(p, fen, sizeof fen);
			san_of(p, r.best, san);
			CHECK(chai_score_is_mate(r.score) && chai_mate_in(r.score) == k, "mate in %d not seen: score %d, mate_in %d, move %s  (%s)", k,
			      r.score, chai_mate_in(r.score), san, fen);
			CHECK(r.best && move_keeps_mate(p, r.best, k), "mate in %d: the move %s does not keep the mate  (%s)", k, san, fen);
		}
}

/* ------------------------------------------------------------------------------------------------ real positions */

static ch_move best_of(struct chai *ai, const char *fen, int depth, int *score)
{
	struct ch_pos p;
	if (set(&p, fen) < 0)
		return 0;
	struct chai_limits lim = { .depth = depth };
	struct chai_result r;
	chai_clear(ai);
	chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
	if (score)
		*score = r.score;
	return r.best;
}

static void expect_move(struct chai *ai, const char *fen, const char *uci, int depth)
{
	ch_move m = best_of(ai, fen, depth, NULL);
	char got[8] = "none";
	if (m)
		ch_move_uci(m, got);
	CHECK(!strcmp(got, uci), "in %s the engine plays %s, expected %s", fen, got, uci);
}

static void test_positions(struct chai *ai)
{
	/* an undefended queen is taken; a piece that is defended is not given for a pawn */
	expect_move(ai, "4k3/8/8/3q4/8/2N5/8/4K3 w - - 0 1", "c3d5", 4);
	expect_move(ai, "6k1/5ppp/8/8/8/8/8/R3K3 w - - 0 1", "a1a8", 1);
	expect_move(ai, "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", "h5f7", 2);
	/* an opening position is about level (a search that blunders, or an evaluation that is lopsided, shows here) */
	{
		int score;
		const char *fen = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3";   /* the Italian game */
		ch_move m = best_of(ai, fen, 4, &score);
		struct ch_pos p;
		set(&p, fen);
		char san[16];
		san_of(&p, m, san);
		CHECK(score > -150 && score < 150, "a quiet position is about equal: %d after %s", score, san);
	}
	/* a mate that is a stalemate trap: the queen must not stalemate the king */
	{
		struct ch_pos p;
		set(&p, "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1");
		struct chai_limits lim = { .depth = 6 };
		struct chai_result r;
		chai_clear(ai);
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		struct ch_undo u;
		ch_make(&p, r.best, &u);
		ch_move rep[CH_MAX_MOVES];
		int rn = ch_gen_legal(&p, rep);
		CHECK(rn > 0 || ch_in_check(&p), "the queen stalemated the king");
		CHECK(chai_score_is_mate(r.score), "a mate was not seen: %d", r.score);
	}
	/* the fifty move rule: every move that is not a pawn move or a capture makes it a draw, and a rook is no pawn */
	{
		int score;
		best_of(ai, "4k3/8/8/8/8/8/R7/4K3 w - - 99 80", 3, &score);
		CHECK(score == 0, "after 99 plies without a capture every rook move is a draw: %d", score);
		best_of(ai, "4k3/8/8/8/8/8/R7/4K3 w - - 0 80", 3, &score);
		CHECK(score > 300, "a rook up with the clock at 0: %d", score);
		best_of(ai, "6k1/5ppp/8/8/8/8/8/R3K3 w - - 99 80", 2, &score);
		CHECK(chai_score_is_mate(score), "a mate on the 100th ply is a mate: %d", score);
	}
	/* none: no move and the right score; one: that one */
	{
		struct ch_pos p;
		struct chai_limits lim = { .depth = 8 };
		struct chai_result r;
		set(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");   /* stalemate */
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		CHECK(r.best == 0 && r.score == 0, "stalemate: no move, score 0 (%d)", r.score);
		set(&p, "R5k1/5ppp/8/8/8/8/8/4K3 b - - 0 1");   /* mated */
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		CHECK(r.best == 0 && r.score == -CHAI_MATE, "mated: no move, the mate score (%d)", r.score);
		set(&p, "7k/8/8/8/8/8/1q6/K7 w - - 0 1");        /* the queen checks and only Kxb2 is legal */
		ch_move mv[CH_MAX_MOVES];
		CHECK(ch_gen_legal(&p, mv) == 1, "one legal move");
		double t0 = now_ms();
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		CHECK(r.best == mv[0] && now_ms() - t0 < 100, "the only move is played at once (%.0f ms)", now_ms() - t0);
	}
}

/* ------------------------------------------------------------------------------------ the ends of a search */

static void *stopper(void *arg)
{
	usleep(150 * 1000);
	chai_stop((struct chai *)arg);
	return NULL;
}

static void test_limits(struct chai *ai)
{
	struct ch_pos p;
	set(&p, START);
	struct chai_result r;

	/* a stop from another thread */
	pthread_t th;
	struct chai_limits lim = { 0 };
	pthread_create(&th, NULL, stopper, ai);
	double t0 = now_ms();
	chai_clear(ai);
	chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
	double dt = now_ms() - t0;
	pthread_join(th, NULL);
	CHECK(dt > 100 && dt < 500, "chai_stop ended the search after %.0f ms (asked at 150)", dt);
	CHECK(r.best && ch_move_find(&p, CH_FROM(r.best), CH_TO(r.best), 0) == r.best, "the stopped search has a legal move");
	CHECK(r.depth >= 3, "depth reached when stopped: %d", r.depth);

	/* a time limit */
	for (int ms = 50; ms <= 400; ms *= 2) {
		lim = (struct chai_limits){ .time_ms = ms };
		t0 = now_ms();
		chai_clear(ai);
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		dt = now_ms() - t0;
		CHECK(dt < ms * 1.3 + 15, "%d ms asked, %.0f used", ms, dt);
		CHECK(r.best != 0 && r.depth >= 1, "a move within %d ms", ms);
	}
	/* a node limit */
	lim = (struct chai_limits){ .nodes = 20000 };
	chai_clear(ai);
	chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
	CHECK(r.nodes >= 20000 && r.nodes < 30000 && r.best, "node limit: %lld", (long long)r.nodes);
	/* a depth limit, and the same search twice gives the same result */
	struct chai_result r2;
	lim = (struct chai_limits){ .depth = 5 };
	chai_clear(ai);
	chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
	chai_clear(ai);
	chai_search(ai, &p, NULL, 0, &lim, &r2, NULL, NULL);
	CHECK(r.depth == 5 && r.best == r2.best && r.score == r2.score && r.nodes == r2.nodes, "a search repeats: %lld / %lld nodes", (long long)r.nodes, (long long)r2.nodes);
	CHECK(r.npv >= 3 && r.pv[0] == r.best, "the principal variation starts with the move (%d long)", r.npv);
	/* the principal variation is a line of legal moves */
	struct ch_pos q = p;
	int legal = 1;
	for (int i = 0; i < r.npv; i++) {
		ch_move m = ch_move_find(&q, CH_FROM(r.pv[i]), CH_TO(r.pv[i]), CH_PROMO(r.pv[i]));
		if (m != r.pv[i]) {
			legal = 0;
			break;
		}
		struct ch_undo u;
		ch_make(&q, m, &u);
	}
	CHECK(legal, "the principal variation is not a line of legal moves");

	/* noise: a weaker setting still plays legal moves, and not always the same one */
	int seen[8], nseen = 0;
	for (uint32_t seed = 1; seed <= 12; seed++) {
		lim = (struct chai_limits){ .depth = 1, .noise = 250, .seed = seed };
		chai_clear(ai);
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		CHECK(r.best && ch_move_find(&p, CH_FROM(r.best), CH_TO(r.best), 0) == r.best, "legal move with noise");
		int dup = 0;
		for (int i = 0; i < nseen; i++)
			dup |= seen[i] == r.best;
		if (!dup && nseen < 8)
			seen[nseen++] = r.best;
	}
	CHECK(nseen >= 3, "with error 250 the first move varied between %d moves in 12 tries", nseen);

	/* the levels */
	CHECK(chai_level_count() >= 5, "levels");
	for (int l = 1; l <= chai_level_count(); l++) {
		struct chai_limits a, b;
		chai_level(l, &a);
		CHECK(a.depth > 0 || a.time_ms > 0, "level %d has a limit", l);
		if (l > 1) {
			chai_level(l - 1, &b);
			CHECK(a.noise <= b.noise, "the error does not grow with the level (%d)", l);
		}
		CHECK(strcmp(chai_level_name(l), "?") != 0, "a name for level %d", l);
	}

	/* through a game: the history is used (an engine that has the history does not crash and plays legally) */
	struct ch_game g;
	ch_game_init(&g, NULL, NULL);
	const char *line[] = { "Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nf6", "Ng1" };
	for (size_t i = 0; i < sizeof line / sizeof line[0]; i++)
		ch_game_play(&g, ch_move_from_san(&g.pos, line[i]));
	lim = (struct chai_limits){ .depth = 4 };
	chai_clear(ai);
	chai_search_game(ai, &g, &lim, &r, NULL, NULL);
	CHECK(r.best && ch_move_find(&g.pos, CH_FROM(r.best), CH_TO(r.best), 0) == r.best, "a move in a game with history");
	ch_game_free(&g);
}

/* ----------------------------------------------------------------------------------------------- a match */

static int parse_setting(const char *s, struct chai_limits *lim)
{
	memset(lim, 0, sizeof *lim);
	if (s[0] == 'd')
		lim->depth = atoi(s + 1);
	else if (s[0] == 'm')
		lim->time_ms = atoi(s + 1);
	else if (s[0] == 'L')
		chai_level(atoi(s + 1), lim);
	else
		return -1;
	return 0;
}

/* returns 1 = A won, 0 = draw, -1 = B won */
static int play_game(struct chai *a, struct chai *b, const struct chai_limits *la, const struct chai_limits *lb, int a_white,
                     const ch_move *opening, int nopen, int *plies)
{
	struct ch_game g;
	ch_game_init(&g, NULL, NULL);
	chai_clear(a);
	chai_clear(b);
	for (int i = 0; i < nopen; i++)
		ch_game_play(&g, opening[i]);
	int result = 0;
	for (;;) {
		enum ch_status st = ch_game_status(&g);
		if (st != CH_ONGOING) {
			if (st == CH_CHECKMATE) {
				int loser_white = g.pos.side == CH_WHITE;
				result = (loser_white == a_white) ? -1 : 1;
			}
			break;
		}
		if (g.n >= 300)
			break;
		int a_moves = (g.pos.side == CH_WHITE) == a_white;
		struct chai_result r;
		struct chai_limits lim = a_moves ? *la : *lb;
		lim.seed = (uint32_t)g.n * 7919u + (a_moves ? 1u : 2u);
		chai_search_game(a_moves ? a : b, &g, &lim, &r, NULL, NULL);
		if (!r.best || ch_game_play(&g, r.best) < 0) {
			printf("   the engine made no legal move!\n");
			break;
		}
	}
	*plies = g.n;
	ch_game_free(&g);
	return result;
}

static int run_match(const char *sa, const char *sb, int games)
{
	struct chai_limits la, lb;
	if (parse_setting(sa, &la) < 0 || parse_setting(sb, &lb) < 0) {
		printf("a setting is dN, mN or LN\n");
		return 2;
	}
	struct chai *a = chai_new(32), *b = chai_new(32);
	int w = 0, d = 0, l = 0;
	double t0 = now_ms();
	for (int i = 0; i < games; i++) {
		ch_move opening[6];
		int nopen = 0;
		struct ch_pos p;
		ch_pos_start(&p);
		g_rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(i / 2 + 1) * 0x2545F4914F6CDD1DULL;   /* a pair shares its opening */
		for (int k = 0; k < 4; k++) {
			ch_move mv[CH_MAX_MOVES];
			int n = ch_gen_legal(&p, mv);
			ch_move m = mv[rnd() % (uint32_t)n];
			struct ch_undo u;
			ch_make(&p, m, &u);
			opening[nopen++] = m;
		}
		int plies;
		int r = play_game(a, b, &la, &lb, i % 2 == 0, opening, nopen, &plies);
		w += r > 0;
		d += r == 0;
		l += r < 0;
		printf("game %d: %s as %s, %s after %d plies   (%d - %d - %d)\n", i + 1, sa, i % 2 == 0 ? "white" : "black",
		       r > 0 ? "won" : r < 0 ? "lost" : "drawn", plies, w, d, l);
		fflush(stdout);
	}
	double s = (w + d * 0.5) / games;
	double elo = s <= 0 ? -999 : s >= 1 ? 999 : -400.0 * log10(1.0 / s - 1.0);
	double var = (w * pow(1 - s, 2) + d * pow(0.5 - s, 2) + l * pow(s, 2)) / games;
	double err = 1.96 * sqrt(var / games);
	double lo = s - err, hi = s + err;
	double elo_lo = lo <= 0 ? -999 : -400.0 * log10(1.0 / lo - 1.0), elo_hi = hi >= 1 ? 999 : -400.0 * log10(1.0 / hi - 1.0);
	printf("\n%s against %s: +%d =%d -%d  (%.1f %%)  Elo difference %+.0f  (95%% between %+.0f and %+.0f)  in %.0f s\n", sa, sb, w, d, l,
	       s * 100, elo, elo_lo, elo_hi, (now_ms() - t0) / 1000);
	chai_free(a);
	chai_free(b);
	return 0;
}

static int run_bench(void)
{
	static const char *fens[] = {
		START,
		"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
		"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
		"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
		"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
	};
	struct chai *ai = chai_new(64);
	int64_t nodes = 0;
	double t0 = now_ms();
	for (size_t i = 0; i < sizeof fens / sizeof fens[0]; i++) {
		struct ch_pos p;
		set(&p, fens[i]);
		struct chai_limits lim = { .time_ms = 2000 };
		struct chai_result r;
		chai_clear(ai);
		chai_search(ai, &p, NULL, 0, &lim, &r, NULL, NULL);
		char san[16];
		san_of(&p, r.best, san);
		printf("depth %2d  %lld nodes  %5d ms  %s  score %d\n", r.depth, (long long)r.nodes, r.time_ms, san, r.score);
		nodes += r.nodes;
	}
	printf("%.2f million nodes a second\n", (double)nodes / (now_ms() - t0) / 1000.0);
	chai_free(ai);
	return 0;
}

int main(int argc, char **argv)
{
	ch_init();
	if (argc >= 5 && !strcmp(argv[1], "--match"))
		return run_match(argv[2], argv[3], atoi(argv[4]));
	if (argc >= 2 && !strcmp(argv[1], "--bench"))
		return run_bench();

	struct chai *ai = chai_new(32);
	test_null_move();
	test_eval();
	test_plain_search(ai);
	test_mates(ai);
	test_positions(ai);
	test_limits(ai);
	chai_free(ai);

	printf("%d checks, %d failed\n", g_checks, g_fail);
	if (!g_fail)
		printf("chessai-test: ok\n");
	return g_fail ? 1 : 0;
}

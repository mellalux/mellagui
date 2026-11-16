/* chessai.c - the search and the evaluation; the contract is in chessai.h. */
#include "chessai.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INF 32000

/* ======================================================================================================== evaluation */

/* The piece square tables of Tomasz Michniewski's "simplified evaluation function", written as a board seen from
 * White: the first row is the eighth rank.  (A piece of Black reads them the other way up.) */
static const int8_t T_PAWN[64] = {
	  0,  0,  0,  0,  0,  0,  0,  0,
	 50, 50, 50, 50, 50, 50, 50, 50,
	 10, 10, 20, 30, 30, 20, 10, 10,
	  5,  5, 10, 25, 25, 10,  5,  5,
	  0,  0,  0, 20, 20,  0,  0,  0,
	  5, -5,-10,  0,  0,-10, -5,  5,
	  5, 10, 10,-20,-20, 10, 10,  5,
	  0,  0,  0,  0,  0,  0,  0,  0 };
static const int8_t T_KNIGHT[64] = {
	-50,-40,-30,-30,-30,-30,-40,-50,
	-40,-20,  0,  0,  0,  0,-20,-40,
	-30,  0, 10, 15, 15, 10,  0,-30,
	-30,  5, 15, 20, 20, 15,  5,-30,
	-30,  0, 15, 20, 20, 15,  0,-30,
	-30,  5, 10, 15, 15, 10,  5,-30,
	-40,-20,  0,  5,  5,  0,-20,-40,
	-50,-40,-30,-30,-30,-30,-40,-50 };
static const int8_t T_BISHOP[64] = {
	-20,-10,-10,-10,-10,-10,-10,-20,
	-10,  0,  0,  0,  0,  0,  0,-10,
	-10,  0,  5, 10, 10,  5,  0,-10,
	-10,  5,  5, 10, 10,  5,  5,-10,
	-10,  0, 10, 10, 10, 10,  0,-10,
	-10, 10, 10, 10, 10, 10, 10,-10,
	-10,  5,  0,  0,  0,  0,  5,-10,
	-20,-10,-10,-10,-10,-10,-10,-20 };
static const int8_t T_ROOK[64] = {
	  0,  0,  0,  0,  0,  0,  0,  0,
	  5, 10, 10, 10, 10, 10, 10,  5,
	 -5,  0,  0,  0,  0,  0,  0, -5,
	 -5,  0,  0,  0,  0,  0,  0, -5,
	 -5,  0,  0,  0,  0,  0,  0, -5,
	 -5,  0,  0,  0,  0,  0,  0, -5,
	 -5,  0,  0,  0,  0,  0,  0, -5,
	  0,  0,  0,  5,  5,  0,  0,  0 };
static const int8_t T_QUEEN[64] = {
	-20,-10,-10, -5, -5,-10,-10,-20,
	-10,  0,  0,  0,  0,  0,  0,-10,
	-10,  0,  5,  5,  5,  5,  0,-10,
	 -5,  0,  5,  5,  5,  5,  0, -5,
	  0,  0,  5,  5,  5,  5,  0, -5,
	-10,  5,  5,  5,  5,  5,  0,-10,
	-10,  0,  5,  0,  0,  0,  0,-10,
	-20,-10,-10, -5, -5,-10,-10,-20 };
static const int8_t T_KING_MID[64] = {
	-30,-40,-40,-50,-50,-40,-40,-30,
	-30,-40,-40,-50,-50,-40,-40,-30,
	-30,-40,-40,-50,-50,-40,-40,-30,
	-30,-40,-40,-50,-50,-40,-40,-30,
	-20,-30,-30,-40,-40,-30,-30,-20,
	-10,-20,-20,-20,-20,-20,-20,-10,
	 20, 20,  0,  0,  0,  0, 20, 20,
	 20, 30, 10,  0,  0, 10, 30, 20 };
static const int8_t T_KING_END[64] = {
	-50,-40,-30,-20,-20,-30,-40,-50,
	-30,-20,-10,  0,  0,-10,-20,-30,
	-30,-10, 20, 30, 30, 20,-10,-30,
	-30,-10, 30, 40, 40, 30,-10,-30,
	-30,-10, 30, 40, 40, 30,-10,-30,
	-30,-10, 20, 30, 30, 20,-10,-30,
	-30,-30,  0,  0,  0,  0,-30,-30,
	-50,-30,-30,-30,-30,-30,-30,-50 };

static const int VAL_MG[7] = { 0, 100, 320, 330, 500, 900, 0 };
static const int VAL_EG[7] = { 0, 110, 310, 330, 520, 920, 0 };
static const int PHASE[7]  = { 0, 0, 1, 1, 2, 4, 0 };
static const int PASSED_MG[8] = { 0, 0, 5, 10, 20, 35, 60, 0 };
static const int PASSED_EG[8] = { 0, 0, 10, 20, 40, 70, 120, 0 };

static int g_pst[16][64];        /* by piece, by square: the table of the middle game (king too) */
static int g_king_end[2][64];    /* the king's table for the end game, by colour */
static int g_eval_ready;

static void eval_init(void)
{
	if (g_eval_ready)
		return;
	const int8_t *tab[7] = { NULL, T_PAWN, T_KNIGHT, T_BISHOP, T_ROOK, T_QUEEN, T_KING_MID };
	for (int t = 1; t <= 6; t++)
		for (int s = 0; s < 64; s++) {
			g_pst[CH_PIECE(CH_WHITE, t)][s] = tab[t][s ^ 56];
			g_pst[CH_PIECE(CH_BLACK, t)][s] = tab[t][s];
		}
	for (int s = 0; s < 64; s++) {
		g_king_end[CH_WHITE][s] = T_KING_END[s ^ 56];
		g_king_end[CH_BLACK][s] = T_KING_END[s];
	}
	g_eval_ready = 1;
}

int chai_eval(const struct ch_pos *p)
{
	eval_init();
	int mg[2] = { 0, 0 }, eg[2] = { 0, 0 }, phase = 0;
	int bishops[2] = { 0, 0 }, rooks[2][2], nrook[2] = { 0, 0 };
	int fc[2][8] = { { 0 } }, minr[2][8], maxr[2][8];
	int psq[2][8], np[2] = { 0, 0 }, nonpawn = 0;
	for (int c = 0; c < 2; c++)
		for (int f = 0; f < 8; f++) {
			minr[c][f] = 8;
			maxr[c][f] = -1;
		}

	for (int s = 0; s < 64; s++) {
		uint8_t pc = p->sq[s];
		if (!pc)
			continue;
		int c = CH_COLOR(pc), t = CH_TYPE(pc);
		mg[c] += VAL_MG[t] + g_pst[pc][s];
		eg[c] += VAL_EG[t] + (t == CH_KING ? g_king_end[c][s] : g_pst[pc][s]);
		phase += PHASE[t];
		if (t == CH_PAWN) {
			int f = s & 7, r = s >> 3;
			fc[c][f]++;
			if (r < minr[c][f]) minr[c][f] = r;
			if (r > maxr[c][f]) maxr[c][f] = r;
			if (np[c] < 8)
				psq[c][np[c]++] = s;
		} else if (t != CH_KING) {
			nonpawn++;
			if (t == CH_BISHOP)
				bishops[c]++;
			else if (t == CH_ROOK && nrook[c] < 2)
				rooks[c][nrook[c]++] = s;
		}
	}
	/* nobody can mate: a draw, whatever the tables say */
	if (np[0] + np[1] == 0 && nonpawn <= 2 && ch_insufficient_material(p))
		return 0;

	for (int c = 0; c < 2; c++) {
		int them = c ^ 1;
		if (bishops[c] >= 2) {
			mg[c] += 30;
			eg[c] += 40;
		}
		for (int i = 0; i < np[c]; i++) {
			int s = psq[c][i], f = s & 7, r = s >> 3;
			int rel = c == CH_WHITE ? r : 7 - r;
			if (fc[c][f] > 1) {                                   /* doubled: every pawn but the first costs */
				mg[c] -= 5;
				eg[c] -= 10;
			}
			int left = f > 0 ? fc[c][f - 1] : 0, right = f < 7 ? fc[c][f + 1] : 0;
			if (!left && !right) {                                 /* isolated */
				mg[c] -= 12;
				eg[c] -= 15;
			}
			int passed = 1;
			for (int ff = f - 1; ff <= f + 1 && passed; ff++) {
				if (ff < 0 || ff > 7)
					continue;
				if (c == CH_WHITE ? maxr[them][ff] > r : minr[them][ff] < r)
					passed = 0;
			}
			if (passed) {
				mg[c] += PASSED_MG[rel];
				eg[c] += PASSED_EG[rel];
			}
		}
		for (int i = 0; i < nrook[c]; i++) {
			int f = rooks[c][i] & 7;
			if (!fc[c][f]) {
				int bonus = fc[them][f] ? 8 : 15;                 /* a half open file, an open one */
				mg[c] += bonus;
				eg[c] += bonus / 2;
			}
		}
		/* the king's shelter: the pawns in front of it (the middle game only, the weights fade with the pieces) */
		int ks = p->ksq[c], kf = ks & 7, kr = ks >> 3, home = c == CH_WHITE ? kr <= 1 : kr >= 6;
		if (home) {
			int dir = c == CH_WHITE ? 1 : -1;
			uint8_t pawn = CH_PIECE(c, CH_PAWN);
			for (int f = kf - 1; f <= kf + 1; f++) {
				if (f < 0 || f > 7)
					continue;
				int r1 = kr + dir, r2 = kr + 2 * dir;
				if (r1 >= 0 && r1 < 8 && p->sq[r1 * 8 + f] == pawn)
					mg[c] += 10;
				else if (r2 >= 0 && r2 < 8 && p->sq[r2 * 8 + f] == pawn)
					mg[c] += 5;
				else
					mg[c] -= 8;
			}
		}
	}

	if (phase > 24)
		phase = 24;
	int score = ((mg[0] - mg[1]) * phase + (eg[0] - eg[1]) * (24 - phase)) / 24;
	if (p->side == CH_BLACK)
		score = -score;
	return score + 8;                                                  /* the move is worth something */
}

int chai_score_is_mate(int score)
{
	return score >= CHAI_MATE_MAX || score <= -CHAI_MATE_MAX;
}

int chai_mate_in(int score)
{
	if (score >= CHAI_MATE_MAX)
		return (CHAI_MATE - score + 1) / 2;
	if (score <= -CHAI_MATE_MAX)
		return -((CHAI_MATE + score + 1) / 2);
	return 0;
}

/* ============================================================================================================ search */

enum { TT_NONE = 0, TT_EXACT, TT_LOWER, TT_UPPER };

struct tt_e {
	uint64_t key;
	ch_move  move;
	int16_t  score;
	int8_t   depth;
	uint8_t  flag;
	uint8_t  gen;
};

struct pvline {
	ch_move m[CHAI_MAXPLY];
	int     n;
};

#define KEYS_MAX (128 + CHAI_MAXPLY + 4)

struct chai {
	struct tt_e *tt;
	size_t       mask;
	uint8_t      gen;
	int          stop;            /* set by chai_stop, from any thread */

	/* the search that runs */
	const struct chai_limits *lim;
	int64_t      nodes;
	struct timespec t0;
	int          aborted;
	int          plain;
	int          noise;
	uint32_t     seed;
	uint64_t     keys[KEYS_MAX];  /* the positions of the game and of the path */
	int          kbase;           /* keys[kbase + ply] = the key at ply (0 = the root) */
	ch_move      killer[CHAI_MAXPLY][2];
	int16_t      history[2][64][64];
};

static int lmr_ready;
static int lmr_table[64][64];

struct chai *chai_new(int tt_mb)
{
	ch_init();
	eval_init();
	struct chai *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	size_t n = 1;
	size_t want = (size_t)(tt_mb < 1 ? 1 : tt_mb) * 1024 * 1024 / sizeof(struct tt_e);
	while (n * 2 <= want)
		n *= 2;
	c->tt = calloc(n, sizeof *c->tt);
	if (!c->tt) {
		free(c);
		return NULL;
	}
	c->mask = n - 1;
	if (!lmr_ready) {
		for (int d = 1; d < 64; d++)
			for (int m = 1; m < 64; m++)
				lmr_table[d][m] = (int)(0.75 + log((double)d) * log((double)m) / 2.25);
		lmr_ready = 1;
	}
	return c;
}

void chai_free(struct chai *c)
{
	if (!c)
		return;
	free(c->tt);
	free(c);
}

void chai_clear(struct chai *c)
{
	memset(c->tt, 0, (c->mask + 1) * sizeof *c->tt);
	memset(c->killer, 0, sizeof c->killer);
	memset(c->history, 0, sizeof c->history);
	c->gen = 0;
}

void chai_stop(struct chai *c)
{
	__atomic_store_n(&c->stop, 1, __ATOMIC_RELAXED);
}

static double elapsed_ms(const struct chai *c)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)(t.tv_sec - c->t0.tv_sec) * 1000.0 + (double)(t.tv_nsec - c->t0.tv_nsec) / 1e6;
}

static void check_abort(struct chai *c)
{
	if (c->aborted)
		return;
	if (__atomic_load_n(&c->stop, __ATOMIC_RELAXED) || (c->lim->nodes && c->nodes >= c->lim->nodes) ||
	    (c->lim->time_ms && elapsed_ms(c) >= c->lim->time_ms))
		c->aborted = 1;
}

/* the evaluation with the made up error of the weaker levels */
static int eval_node(const struct chai *c, const struct ch_pos *p)
{
	int e = chai_eval(p);
	if (c->noise > 0) {
		uint64_t h = (p->key ^ ((uint64_t)c->seed * 0x9e3779b97f4a7c15ULL)) * 0xff51afd7ed558ccdULL;
		h ^= h >> 33;
		e += (int)(h % (uint64_t)(2 * c->noise + 1)) - c->noise;
	}
	return e;
}

static int to_tt(int score, int ply)
{
	return score >= CHAI_MATE_MAX ? score + ply : score <= -CHAI_MATE_MAX ? score - ply : score;
}

static int from_tt(int score, int ply)
{
	return score >= CHAI_MATE_MAX ? score - ply : score <= -CHAI_MATE_MAX ? score + ply : score;
}

static const int MVV[7] = { 0, 100, 320, 330, 500, 900, 0 };

static int move_score(const struct chai *c, const struct ch_pos *p, ch_move m, ch_move ttm, int ply)
{
	if (m == ttm)
		return 1000000;
	int from = CH_FROM(m), to = CH_TO(m), type = CH_TYPE(p->sq[from]);
	if (ch_move_is_capture(p, m)) {
		int victim = p->sq[to] ? CH_TYPE(p->sq[to]) : CH_PAWN;
		return 200000 + MVV[victim] * 16 - type;
	}
	if (CH_PROMO(m))
		return CH_PROMO(m) == CH_QUEEN ? 190000 : 1000;
	if (m == c->killer[ply][0])
		return 90000;
	if (m == c->killer[ply][1])
		return 89000;
	return c->history[p->side][from][to];
}

static void pv_set(struct pvline *dst, ch_move m, const struct pvline *child)
{
	dst->m[0] = m;
	int n = child->n < CHAI_MAXPLY - 1 ? child->n : CHAI_MAXPLY - 1;
	memcpy(dst->m + 1, child->m, (size_t)n * sizeof(ch_move));
	dst->n = n + 1;
}

static int has_pieces(const struct ch_pos *p, int side)
{
	for (int s = 0; s < 64; s++) {
		uint8_t pc = p->sq[s];
		if (pc && CH_COLOR(pc) == side && CH_TYPE(pc) >= CH_KNIGHT && CH_TYPE(pc) <= CH_QUEEN)
			return 1;
	}
	return 0;
}

static int qsearch(struct chai *c, struct ch_pos *p, int alpha, int beta, int ply, int qply)
{
	c->nodes++;
	if ((c->nodes & 1023) == 0)
		check_abort(c);
	if (c->aborted)
		return 0;
	if (ply >= CHAI_MAXPLY - 1)
		return eval_node(c, p);

	int us = p->side, in_check = qply < 8 && ch_in_check(p);
	int best = -INF, se = 0;
	if (!in_check) {
		se = best = eval_node(c, p);
		if (best >= beta)
			return best;
		if (best > alpha)
			alpha = best;
	}

	ch_move mv[CH_MAX_MOVES];
	int sc[CH_MAX_MOVES];
	int n = ch_gen_pseudo(p, mv, !in_check);
	for (int i = 0; i < n; i++)
		sc[i] = move_score(c, p, mv[i], 0, ply);
	int legal = 0;
	for (int i = 0; i < n; i++) {
		int k = i;
		for (int j = i + 1; j < n; j++)
			if (sc[j] > sc[k])
				k = j;
		ch_move m = mv[k];
		mv[k] = mv[i];
		sc[k] = sc[i];

		if (!in_check && !CH_PROMO(m)) {                           /* a capture that cannot lift the score to alpha */
			int victim = p->sq[CH_TO(m)] ? CH_TYPE(p->sq[CH_TO(m)]) : CH_PAWN;
			if (!c->plain && se + MVV[victim] + 200 <= alpha)
				continue;
		}
		struct ch_undo u;
		ch_make(p, m, &u);
		if (ch_attacked(p, p->ksq[us], us ^ 1)) {
			ch_unmake(p, m, &u);
			continue;
		}
		legal++;
		int v = -qsearch(c, p, -beta, -alpha, ply + 1, qply + 1);
		ch_unmake(p, m, &u);
		if (c->aborted)
			return 0;
		if (v > best) {
			best = v;
			if (v > alpha) {
				alpha = v;
				if (alpha >= beta)
					break;
			}
		}
	}
	if (in_check && !legal)
		return -CHAI_MATE + ply;
	return best;
}

static int search(struct chai *c, struct ch_pos *p, int depth, int alpha, int beta, int ply, struct pvline *pv, int allow_null)
{
	pv->n = 0;
	c->nodes++;
	if ((c->nodes & 1023) == 0)
		check_abort(c);
	if (c->aborted)
		return 0;

	const int plain = c->plain, is_pv = beta - alpha > 1, us = p->side;
	int in_check = ch_in_check(p);

	if (ply > 0) {
		if (!plain) {
			/* a position that came before on this path or in the game is counted as a draw (it can be repeated) */
			for (int i = 2; i <= p->half && c->kbase + ply - i >= 0; i += 2)
				if (c->keys[c->kbase + ply - i] == p->key)
					return 0;
			if (p->half >= 100) {
				ch_move any[CH_MAX_MOVES];
				if (!in_check || ch_gen_legal(p, any))
					return 0;
			}
		}
		int lo = -CHAI_MATE + ply, hi = CHAI_MATE - ply - 1;        /* no mate can be nearer than this */
		if (alpha < lo) alpha = lo;
		if (beta > hi) beta = hi;
		if (alpha >= beta)
			return alpha;
	}
	if (ply >= CHAI_MAXPLY - 1)
		return eval_node(c, p);
	if (in_check && !plain)
		depth++;
	if (depth <= 0)
		return plain ? eval_node(c, p) : qsearch(c, p, alpha, beta, ply, 0);

	/* the table */
	struct tt_e *e = &c->tt[p->key & c->mask];
	ch_move ttm = 0;
	if (e->flag != TT_NONE && e->key == p->key) {
		ttm = e->move;
		/* (the plain search takes only an equal depth: a deeper value would not be the plain minimax of that depth) */
		if (ply > 0 && !is_pv && (plain ? e->depth == depth : e->depth >= depth)) {
			int v = from_tt(e->score, ply);
			if (e->flag == TT_EXACT || (e->flag == TT_LOWER && v >= beta) || (e->flag == TT_UPPER && v <= alpha))
				return v;
		}
	}

	int se = in_check ? 0 : eval_node(c, p);
	if (!plain && !is_pv && !in_check) {
		/* reverse futility: so far above beta that a few plies will not change it */
		if (depth <= 3 && se - 120 * depth >= beta && beta > -CHAI_MATE_MAX)
			return se - 120 * depth;
		/* null move: if passing still beats beta, a real move will */
		if (allow_null && depth >= 3 && se >= beta && has_pieces(p, us)) {
			int r = 2 + (depth >= 6) + (se - beta > 200);
			struct ch_undo nu;
			struct pvline child;
			ch_make_null(p, &nu);
			c->keys[c->kbase + ply + 1] = p->key;
			int v = -search(c, p, depth - 1 - r, -beta, -beta + 1, ply + 1, &child, 0);
			ch_unmake_null(p, &nu);
			if (c->aborted)
				return 0;
			if (v >= beta)
				return v >= CHAI_MATE_MAX ? beta : v;
		}
	}

	ch_move mv[CH_MAX_MOVES];
	int sc[CH_MAX_MOVES];
	int n = ch_gen_pseudo(p, mv, 0);
	for (int i = 0; i < n; i++)
		sc[i] = move_score(c, p, mv[i], ttm, ply);

	int best = -INF, alpha0 = alpha, legal = 0;
	ch_move bestm = 0;
	struct pvline child;
	for (int i = 0; i < n; i++) {
		int k = i;
		for (int j = i + 1; j < n; j++)
			if (sc[j] > sc[k])
				k = j;
		ch_move m = mv[k];
		mv[k] = mv[i];
		sc[k] = sc[i];

		int capture = ch_move_is_capture(p, m), quiet = !capture && !CH_PROMO(m);
		struct ch_undo u;
		ch_make(p, m, &u);
		if (ch_attacked(p, p->ksq[us], us ^ 1)) {
			ch_unmake(p, m, &u);
			continue;
		}
		legal++;
		c->keys[c->kbase + ply + 1] = p->key;

		int gives_check = 0;
		if (!plain && quiet)
			gives_check = ch_in_check(p);
		/* futility: a quiet move near the leaves when the position is far below alpha */
		if (!plain && !is_pv && !in_check && quiet && !gives_check && legal > 1 && depth <= 2 &&
		    se + 150 * depth <= alpha && alpha > -CHAI_MATE_MAX) {
			ch_unmake(p, m, &u);
			continue;
		}

		int v, nd = depth - 1;
		if (legal == 1) {
			v = -search(c, p, nd, -beta, -alpha, ply + 1, &child, 1);
		} else {
			int r = 0;
			if (!plain && depth >= 3 && legal > 3 && quiet && !in_check && !gives_check &&
			    m != c->killer[ply][0] && m != c->killer[ply][1]) {
				r = lmr_table[depth < 64 ? depth : 63][legal < 64 ? legal : 63];
				if (is_pv && r > 0)
					r--;
				if (r > nd - 1)
					r = nd > 1 ? nd - 1 : 0;
			}
			v = -search(c, p, nd - r, -alpha - 1, -alpha, ply + 1, &child, 1);
			if (v > alpha && r > 0)
				v = -search(c, p, nd, -alpha - 1, -alpha, ply + 1, &child, 1);
			if (v > alpha && v < beta)
				v = -search(c, p, nd, -beta, -alpha, ply + 1, &child, 1);
		}
		ch_unmake(p, m, &u);
		if (c->aborted)
			return 0;

		if (v > best) {
			best = v;
			bestm = m;
			if (v > alpha) {
				alpha = v;
				pv_set(pv, m, &child);
				if (alpha >= beta) {
					if (quiet) {
						if (c->killer[ply][0] != m) {
							c->killer[ply][1] = c->killer[ply][0];
							c->killer[ply][0] = m;
						}
						int bonus = depth * depth;
						if (bonus > 400)
							bonus = 400;
						int16_t *h = &c->history[us][CH_FROM(m)][CH_TO(m)];
						*h = (int16_t)(*h + bonus - *h * bonus / 16384);
					}
					break;
				}
			}
		}
	}

	if (!legal)
		return in_check ? -CHAI_MATE + ply : 0;

	if (!c->aborted) {
		int flag = best >= beta ? TT_LOWER : best > alpha0 ? TT_EXACT : TT_UPPER;
		int replace = e->key != p->key ? (e->gen != c->gen || e->flag == TT_NONE || depth >= e->depth)
		                               : (depth >= e->depth || flag == TT_EXACT);
		if (replace) {
			ch_move keep = e->key == p->key ? e->move : 0;
			e->move = (bestm && flag != TT_UPPER) ? bestm : keep;      /* a fail low has no move worth remembering */
			e->key = p->key;
			e->score = (int16_t)to_tt(best, ply);
			e->depth = (int8_t)depth;
			e->flag = (uint8_t)flag;
			e->gen = c->gen;
		}
	}
	return best;
}

/* ========================================================================================================== the root */

int chai_search(struct chai *c, const struct ch_pos *pos, const uint64_t *hist, int nhist,
                const struct chai_limits *lim, struct chai_result *res, chai_info_fn info, void *ud)
{
	memset(res, 0, sizeof *res);
	c->lim = lim;
	c->plain = (lim->flags & CHAI_PLAIN) != 0;
	c->noise = c->plain ? 0 : lim->noise;
	c->seed = lim->seed;
	c->nodes = 0;
	c->aborted = 0;
	c->gen++;
	__atomic_store_n(&c->stop, 0, __ATOMIC_RELAXED);
	memset(c->killer, 0, sizeof c->killer);
	memset(c->history, 0, sizeof c->history);
	clock_gettime(CLOCK_MONOTONIC, &c->t0);

	struct ch_pos p = *pos;
	/* the keys before the root, as many as the repetition rule can look at */
	int keep = 0;
	if (hist && nhist > 1) {
		keep = nhist - 1;
		if (keep > 128)
			keep = 128;
		for (int i = 0; i < keep; i++)
			c->keys[i] = hist[nhist - 1 - keep + i];
	}
	c->kbase = keep;
	c->keys[c->kbase] = p.key;

	ch_move roots[CH_MAX_MOVES];
	int nroot = ch_gen_legal(&p, roots);
	if (nroot == 0) {
		res->score = ch_in_check(&p) ? -CHAI_MATE : 0;
		return 0;
	}
	/* a first order: the table's idea of the position, else the usual ordering */
	{
		struct tt_e *e = &c->tt[p.key & c->mask];
		ch_move ttm = (e->flag != TT_NONE && e->key == p.key) ? e->move : 0;
		int sc[CH_MAX_MOVES];
		for (int i = 0; i < nroot; i++)
			sc[i] = move_score(c, &p, roots[i], ttm, 0);
		for (int i = 0; i < nroot; i++)
			for (int j = i + 1; j < nroot; j++)
				if (sc[j] > sc[i]) {
					int ts = sc[i]; sc[i] = sc[j]; sc[j] = ts;
					ch_move tm = roots[i]; roots[i] = roots[j]; roots[j] = tm;
				}
	}
	res->best = roots[0];

	int maxd = lim->depth > 0 && lim->depth < CHAI_MAXPLY - 8 ? lim->depth : CHAI_MAXPLY - 8;
	int us = p.side;
	for (int depth = 1; depth <= maxd; depth++) {
		int alpha = -INF, iter_best = -1, iter_score = -INF;
		struct pvline pv, child;
		pv.n = 0;
		for (int i = 0; i < nroot; i++) {
			struct ch_undo u;
			ch_make(&p, roots[i], &u);
			c->keys[c->kbase + 1] = p.key;
			int v;
			if (i == 0) {
				v = -search(c, &p, depth - 1, -INF, -alpha, 1, &child, 1);
			} else {
				v = -search(c, &p, depth - 1, -alpha - 1, -alpha, 1, &child, 1);
				if (v > alpha && !c->aborted)
					v = -search(c, &p, depth - 1, -INF, -alpha, 1, &child, 1);
			}
			ch_unmake(&p, roots[i], &u);
			if (c->aborted)
				break;
			if (i == 0 || v > alpha) {
				if (i == 0 || v > iter_score) {
					iter_best = i;
					iter_score = v;
					pv_set(&pv, roots[i], &child);
					if (v > alpha)
						alpha = v;
				}
			}
		}
		(void)us;
		if (iter_best >= 0 && (!c->aborted || depth > 1 || res->depth == 0)) {
			/* the move that was best goes first next time; the others keep their order */
			ch_move bm = roots[iter_best];
			for (int j = iter_best; j > 0; j--)
				roots[j] = roots[j - 1];
			roots[0] = bm;
			res->best = bm;
			res->ponder = pv.n > 1 ? pv.m[1] : 0;
			res->score = iter_score;
			res->npv = pv.n;
			memcpy(res->pv, pv.m, (size_t)pv.n * sizeof(ch_move));
			if (!c->aborted)
				res->depth = depth;
			res->nodes = c->nodes;
			res->time_ms = (int)elapsed_ms(c);
			if (info && !c->aborted)
				info(res, ud);
		}
		if (c->aborted)
			break;
		if (nroot == 1)
			break;                                                    /* nothing to choose */
		if (chai_score_is_mate(res->score) && CHAI_MATE - abs(res->score) <= depth)
			break;                                                    /* the mate is shown to be the shortest */
		if (lim->time_ms && elapsed_ms(c) > lim->time_ms * 0.45)
			break;                                                    /* the next depth would not finish */
	}
	res->nodes = c->nodes;
	res->time_ms = (int)elapsed_ms(c);
	return res->best ? 0 : -1;
}

int chai_search_game(struct chai *c, const struct ch_game *g, const struct chai_limits *lim,
                     struct chai_result *res, chai_info_fn info, void *ud)
{
	return chai_search(c, &g->pos, g->hist, g->n + 1, lim, res, info, ud);
}

/* ========================================================================================================== levels */

struct level {
	const char *name;
	int depth, ms, noise;
};

static const struct level k_levels[] = {
	{ "Beginner",    1,    0, 220 },
	{ "Casual",      2,    0, 140 },
	{ "Easy",        3,    0,  90 },
	{ "Moderate",    4,    0,  50 },
	{ "Fair",        5,  300,  20 },
	{ "Good",        0,  200,   0 },
	{ "Strong",      0,  800,   0 },
	{ "Very strong", 0, 2500,   0 },
	{ "Maximum",     0, 8000,   0 },
};

int chai_level_count(void)
{
	return (int)(sizeof k_levels / sizeof k_levels[0]);
}

void chai_level(int level, struct chai_limits *lim)
{
	if (level < 1)
		level = 1;
	if (level > chai_level_count())
		level = chai_level_count();
	const struct level *l = &k_levels[level - 1];
	memset(lim, 0, sizeof *lim);
	lim->depth = l->depth;
	lim->time_ms = l->ms;
	lim->noise = l->noise;
}

const char *chai_level_name(int level)
{
	if (level < 1 || level > chai_level_count())
		return "?";
	return k_levels[level - 1].name;
}

/* chess.c - the rules of chess; the contract is in chess.h. */
#include "chess.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define FILE_OF(s) ((s) & 7)
#define RANK_OF(s) ((s) >> 3)

/* ---- tables ------------------------------------------------------------------------------------------------ */

/* directions 0..3 are the rook's, 4..7 the bishop's */
static const int8_t dfile[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
static const int8_t drank[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int8_t dstep[8] = { 8, -8, 1, -1, 9, 7, -7, -9 };

static int      g_ready;
static uint8_t  rlen[64][8];          /* squares from `sq` to the edge in direction d */
static uint8_t  kn_to[64][8], kn_n[64];
static uint8_t  kg_to[64][8], kg_n[64];
static uint8_t  castle_mask[64];       /* the rights that stay when a piece leaves or arrives at a square */
static uint64_t z_piece[16][64], z_side, z_castle[16], z_ep[8];

static uint64_t splitmix(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

void ch_init(void)
{
	if (g_ready)
		return;
	static const int8_t knf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 }, knr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
	for (int s = 0; s < 64; s++) {
		int f = FILE_OF(s), r = RANK_OF(s);
		for (int d = 0; d < 8; d++) {
			int n = 0, ff = f, rr = r;
			for (;;) {
				ff += dfile[d];
				rr += drank[d];
				if (ff < 0 || ff > 7 || rr < 0 || rr > 7)
					break;
				n++;
			}
			rlen[s][d] = (uint8_t)n;
			if (n > 0)
				kg_to[s][kg_n[s]++] = (uint8_t)(s + dstep[d]);
		}
		for (int k = 0; k < 8; k++) {
			int ff = f + knf[k], rr = r + knr[k];
			if (ff >= 0 && ff < 8 && rr >= 0 && rr < 8)
				kn_to[s][kn_n[s]++] = (uint8_t)(rr * 8 + ff);
		}
		castle_mask[s] = 15;
	}
	castle_mask[0] = (uint8_t)(15 & ~CH_WQ);
	castle_mask[7] = (uint8_t)(15 & ~CH_WK);
	castle_mask[4] = (uint8_t)(15 & ~(CH_WK | CH_WQ));
	castle_mask[56] = (uint8_t)(15 & ~CH_BQ);
	castle_mask[63] = (uint8_t)(15 & ~CH_BK);
	castle_mask[60] = (uint8_t)(15 & ~(CH_BK | CH_BQ));

	uint64_t seed = 0x6d656c6c61636865ULL;   /* fixed: the same keys in every run, so a failing test repeats */
	for (int p = 0; p < 16; p++)
		for (int s = 0; s < 64; s++)
			z_piece[p][s] = splitmix(&seed);
	z_side = splitmix(&seed);
	for (int i = 0; i < 16; i++)
		z_castle[i] = splitmix(&seed);
	for (int i = 0; i < 8; i++)
		z_ep[i] = splitmix(&seed);
	g_ready = 1;
}

/* ---- attacks ----------------------------------------------------------------------------------------------- */

int ch_attacked(const struct ch_pos *p, int sq, int by)
{
	/* a pawn of colour `by` attacks the squares one rank ahead of it, one file to each side */
	int pr = RANK_OF(sq) - (by == CH_WHITE ? 1 : -1);
	if (pr >= 0 && pr < 8) {
		int f = FILE_OF(sq);
		uint8_t pawn = CH_PIECE(by, CH_PAWN);
		if (f > 0 && p->sq[pr * 8 + f - 1] == pawn)
			return 1;
		if (f < 7 && p->sq[pr * 8 + f + 1] == pawn)
			return 1;
	}
	uint8_t kn = CH_PIECE(by, CH_KNIGHT), kg = CH_PIECE(by, CH_KING);
	for (int i = 0; i < kn_n[sq]; i++)
		if (p->sq[kn_to[sq][i]] == kn)
			return 1;
	for (int i = 0; i < kg_n[sq]; i++)
		if (p->sq[kg_to[sq][i]] == kg)
			return 1;
	uint8_t rook = CH_PIECE(by, CH_ROOK), bishop = CH_PIECE(by, CH_BISHOP), queen = CH_PIECE(by, CH_QUEEN);
	for (int d = 0; d < 8; d++) {
		uint8_t want = d < 4 ? rook : bishop;
		int s = sq;
		for (int i = 0; i < rlen[sq][d]; i++) {
			s += dstep[d];
			uint8_t t = p->sq[s];
			if (!t)
				continue;
			if (t == want || t == queen)
				return 1;
			break;
		}
	}
	return 0;
}

int ch_in_check(const struct ch_pos *p)
{
	return ch_attacked(p, p->ksq[p->side], p->side ^ 1);
}

/* ---- hash --------------------------------------------------------------------------------------------------- */

/* The en passant file counts only while a pawn of the side to move stands next to the pawn that moved: two positions
 * that differ only by a capture nobody can make are the same position for the repetition rule.  (A pawn that is
 * pinned and so cannot really capture still makes the key differ: a repetition missed only in that very rare case.) */
static uint64_t ep_key(const struct ch_pos *p)
{
	if (p->ep < 0)
		return 0;
	int f = FILE_OF(p->ep);
	int pr = p->side == CH_WHITE ? RANK_OF(p->ep) - 1 : RANK_OF(p->ep) + 1;   /* the rank the capturing pawn stands on */
	uint8_t pawn = CH_PIECE(p->side, CH_PAWN);
	if (pr < 0 || pr > 7)
		return 0;
	if ((f > 0 && p->sq[pr * 8 + f - 1] == pawn) || (f < 7 && p->sq[pr * 8 + f + 1] == pawn))
		return z_ep[f];
	return 0;
}

uint64_t ch_pos_compute_key(const struct ch_pos *p)
{
	ch_init();
	uint64_t k = 0;
	for (int s = 0; s < 64; s++)
		if (p->sq[s])
			k ^= z_piece[p->sq[s]][s];
	if (p->side == CH_BLACK)
		k ^= z_side;
	k ^= z_castle[p->castle];
	k ^= ep_key(p);
	return k;
}

/* ---- positions ---------------------------------------------------------------------------------------------- */

void ch_pos_start(struct ch_pos *p)
{
	ch_init();
	const char *err;
	ch_pos_set_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &err);
}

static int fen_fail(const char **err, const char *why)
{
	if (err)
		*err = why;
	return -1;
}

int ch_pos_set_fen(struct ch_pos *p, const char *fen, const char **err)
{
	ch_init();
	memset(p, 0, sizeof *p);
	p->ep = -1;
	if (!fen)
		return fen_fail(err, "no FEN");
	const char *s = fen;
	while (*s == ' ')
		s++;

	int file = 0, rank = 7, kings[2] = { 0, 0 };
	for (; *s && *s != ' '; s++) {
		char c = *s;
		if (c == '/') {
			if (file != 8 || rank == 0)
				return fen_fail(err, "a rank does not have eight squares");
			rank--;
			file = 0;
		} else if (c >= '1' && c <= '8') {
			file += c - '0';
			if (file > 8)
				return fen_fail(err, "a rank has more than eight squares");
		} else {
			static const char letters[] = "pnbrqk";
			const char *at = strchr(letters, tolower((unsigned char)c));
			if (!at || !*at)
				return fen_fail(err, "an unknown piece letter");
			if (file >= 8)
				return fen_fail(err, "a rank has more than eight squares");
			int type = (int)(at - letters) + 1, colour = isupper((unsigned char)c) ? CH_WHITE : CH_BLACK;
			if (type == CH_PAWN && (rank == 0 || rank == 7))
				return fen_fail(err, "a pawn on the first or last rank");
			int sq = rank * 8 + file;
			p->sq[sq] = CH_PIECE(colour, type);
			if (type == CH_KING) {
				p->ksq[colour] = (uint8_t)sq;
				kings[colour]++;
			}
			file++;
		}
	}
	if (rank != 0 || file != 8)
		return fen_fail(err, "the board does not have eight ranks of eight squares");
	if (kings[0] != 1 || kings[1] != 1)
		return fen_fail(err, "each side needs exactly one king");

	while (*s == ' ')
		s++;
	if (*s == 'w')
		p->side = CH_WHITE;
	else if (*s == 'b')
		p->side = CH_BLACK;
	else
		return fen_fail(err, "the side to move must be w or b");
	s++;
	if (*s && *s != ' ')
		return fen_fail(err, "the side to move must be w or b");
	while (*s == ' ')
		s++;

	if (*s == '-') {
		s++;
	} else {
		int any = 0;
		for (; *s && *s != ' '; s++) {
			int bit = *s == 'K' ? CH_WK : *s == 'Q' ? CH_WQ : *s == 'k' ? CH_BK : *s == 'q' ? CH_BQ : 0;
			if (!bit || (p->castle & bit))
				return fen_fail(err, "a bad castling field");
			p->castle |= bit;
			any = 1;
		}
		if (!any)
			return fen_fail(err, "a bad castling field");
	}
	/* a right needs its king and rook at home */
	if (p->sq[4] != CH_PIECE(CH_WHITE, CH_KING))
		p->castle &= (uint8_t)~(CH_WK | CH_WQ);
	if (p->sq[60] != CH_PIECE(CH_BLACK, CH_KING))
		p->castle &= (uint8_t)~(CH_BK | CH_BQ);
	if (p->sq[7] != CH_PIECE(CH_WHITE, CH_ROOK))
		p->castle &= (uint8_t)~CH_WK;
	if (p->sq[0] != CH_PIECE(CH_WHITE, CH_ROOK))
		p->castle &= (uint8_t)~CH_WQ;
	if (p->sq[63] != CH_PIECE(CH_BLACK, CH_ROOK))
		p->castle &= (uint8_t)~CH_BK;
	if (p->sq[56] != CH_PIECE(CH_BLACK, CH_ROOK))
		p->castle &= (uint8_t)~CH_BQ;
	while (*s == ' ')
		s++;

	if (*s == '-') {
		s++;
	} else if (s[0] >= 'a' && s[0] <= 'h' && (s[1] == '3' || s[1] == '6')) {
		int sq = (s[1] - '1') * 8 + (s[0] - 'a');
		/* white to move: the black pawn went e7-e5, so the square is on the 6th rank, the pawn stands behind it */
		int want_rank = p->side == CH_WHITE ? 5 : 2;
		int pawn_sq = p->side == CH_WHITE ? sq - 8 : sq + 8, from_sq = p->side == CH_WHITE ? sq + 8 : sq - 8;
		if (RANK_OF(sq) != want_rank || p->sq[sq] || p->sq[from_sq] ||
		    p->sq[pawn_sq] != CH_PIECE(p->side ^ 1, CH_PAWN))
			return fen_fail(err, "a bad en passant square");
		p->ep = (int8_t)sq;
		s += 2;
	} else {
		return fen_fail(err, "a bad en passant square");
	}
	while (*s == ' ')
		s++;

	p->half = 0;
	p->full = 1;
	if (*s) {
		char *end;
		long h = strtol(s, &end, 10);
		if (end == s || h < 0 || h > 65535)
			return fen_fail(err, "a bad halfmove clock");
		p->half = (uint16_t)h;
		s = end;
		while (*s == ' ')
			s++;
		if (*s) {
			long f = strtol(s, &end, 10);
			if (end == s || f < 0 || f > 65535)
				return fen_fail(err, "a bad move number");
			p->full = (uint16_t)(f ? f : 1);
			s = end;
			while (*s == ' ')
				s++;
			if (*s)
				return fen_fail(err, "there is more after the move number");
		}
	}
	if (ch_attacked(p, p->ksq[p->side ^ 1], p->side))
		return fen_fail(err, "the side that does not move is in check");
	p->key = ch_pos_compute_key(p);
	return 0;
}

void ch_pos_fen(const struct ch_pos *p, char *out, size_t cap)
{
	char buf[128];
	int n = 0;
	for (int r = 7; r >= 0; r--) {
		int empty = 0;
		for (int f = 0; f < 8; f++) {
			uint8_t pc = p->sq[r * 8 + f];
			if (!pc) {
				empty++;
				continue;
			}
			if (empty)
				buf[n++] = (char)('0' + empty);
			empty = 0;
			char c = "?pnbrqk"[CH_TYPE(pc)];
			buf[n++] = CH_COLOR(pc) == CH_WHITE ? (char)toupper(c) : c;
		}
		if (empty)
			buf[n++] = (char)('0' + empty);
		if (r)
			buf[n++] = '/';
	}
	buf[n++] = ' ';
	buf[n++] = p->side == CH_WHITE ? 'w' : 'b';
	buf[n++] = ' ';
	if (!p->castle) {
		buf[n++] = '-';
	} else {
		if (p->castle & CH_WK) buf[n++] = 'K';
		if (p->castle & CH_WQ) buf[n++] = 'Q';
		if (p->castle & CH_BK) buf[n++] = 'k';
		if (p->castle & CH_BQ) buf[n++] = 'q';
	}
	buf[n++] = ' ';
	if (p->ep < 0) {
		buf[n++] = '-';
	} else {
		buf[n++] = (char)('a' + FILE_OF(p->ep));
		buf[n++] = (char)('1' + RANK_OF(p->ep));
	}
	n += snprintf(buf + n, sizeof buf - (size_t)n, " %u %u", p->half, p->full);
	if (cap) {
		size_t k = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
		memcpy(out, buf, k);
		out[k] = 0;
	}
}

/* ---- move generation ----------------------------------------------------------------------------------------- */

static void add(ch_move *out, int *n, int from, int to, int promo)
{
	out[(*n)++] = CH_MOVE(from, to, promo);
}

static void add_pawn(ch_move *out, int *n, int from, int to)
{
	int r = RANK_OF(to);
	if (r == 7 || r == 0) {
		add(out, n, from, to, CH_QUEEN);
		add(out, n, from, to, CH_ROOK);
		add(out, n, from, to, CH_BISHOP);
		add(out, n, from, to, CH_KNIGHT);
	} else {
		add(out, n, from, to, 0);
	}
}

int ch_gen_pseudo(const struct ch_pos *p, ch_move *out, int caps)
{
	ch_init();
	int n = 0, us = p->side, them = us ^ 1;
	for (int from = 0; from < 64; from++) {
		uint8_t pc = p->sq[from];
		if (!pc || CH_COLOR(pc) != us)
			continue;
		switch (CH_TYPE(pc)) {
		case CH_PAWN: {
			int dir = us == CH_WHITE ? 8 : -8, f = FILE_OF(from), r = RANK_OF(from);
			int start = us == CH_WHITE ? 1 : 6, last = us == CH_WHITE ? 6 : 1;   /* rank before promoting */
			int to = from + dir;
			if (!p->sq[to]) {
				if (!caps || r == last)
					add_pawn(out, &n, from, to);
				if (!caps && r == start && !p->sq[to + dir])
					add(out, &n, from, to + dir, 0);
			}
			for (int df = -1; df <= 1; df += 2) {
				if (f + df < 0 || f + df > 7)
					continue;
				int t = to + df;
				if (p->sq[t] && CH_COLOR(p->sq[t]) == them)
					add_pawn(out, &n, from, t);
				else if (t == p->ep)
					add(out, &n, from, t, 0);
			}
			break;
		}
		case CH_KNIGHT:
			for (int i = 0; i < kn_n[from]; i++) {
				int t = kn_to[from][i];
				if (!p->sq[t] ? !caps : CH_COLOR(p->sq[t]) == them)
					add(out, &n, from, t, 0);
			}
			break;
		case CH_KING:
			for (int i = 0; i < kg_n[from]; i++) {
				int t = kg_to[from][i];
				if (!p->sq[t] ? !caps : CH_COLOR(p->sq[t]) == them)
					add(out, &n, from, t, 0);
			}
			if (!caps && from == (us == CH_WHITE ? 4 : 60)) {
				int base = us == CH_WHITE ? 0 : 56, ks = us == CH_WHITE ? CH_WK : CH_BK, qs = us == CH_WHITE ? CH_WQ : CH_BQ;
				if ((p->castle & ks) && !p->sq[base + 5] && !p->sq[base + 6] &&
				    !ch_attacked(p, base + 4, them) && !ch_attacked(p, base + 5, them))
					add(out, &n, from, base + 6, 0);
				if ((p->castle & qs) && !p->sq[base + 1] && !p->sq[base + 2] && !p->sq[base + 3] &&
				    !ch_attacked(p, base + 4, them) && !ch_attacked(p, base + 3, them))
					add(out, &n, from, base + 2, 0);
			}
			break;
		default: {
			int d0 = 0, d1 = 8;
			if (CH_TYPE(pc) == CH_BISHOP)
				d0 = 4;
			else if (CH_TYPE(pc) == CH_ROOK)
				d1 = 4;
			for (int d = d0; d < d1; d++) {
				int t = from;
				for (int i = 0; i < rlen[from][d]; i++) {
					t += dstep[d];
					if (!p->sq[t]) {
						if (!caps)
							add(out, &n, from, t, 0);
						continue;
					}
					if (CH_COLOR(p->sq[t]) == them)
						add(out, &n, from, t, 0);
					break;
				}
			}
			break;
		}
		}
	}
	return n;
}

/* ---- making a move ---------------------------------------------------------------------------------------------- */

void ch_make(struct ch_pos *p, ch_move m, struct ch_undo *u)
{
	int from = CH_FROM(m), to = CH_TO(m), promo = CH_PROMO(m), us = p->side;
	uint8_t pc = p->sq[from], type = CH_TYPE(pc), cap = p->sq[to];

	u->move = m;
	u->castle = p->castle;
	u->ep = p->ep;
	u->half = p->half;
	u->key = p->key;

	uint64_t k = p->key ^ ep_key(p) ^ z_castle[p->castle];
	k ^= z_piece[pc][from];
	p->sq[from] = 0;
	if (cap)
		k ^= z_piece[cap][to];
	if (type == CH_PAWN && to == p->ep && !cap) {        /* en passant: the pawn that is taken stands beside the mover */
		int vs = us == CH_WHITE ? to - 8 : to + 8;
		cap = p->sq[vs];
		k ^= z_piece[cap][vs];
		p->sq[vs] = 0;
	}
	u->captured = cap;

	uint8_t put = promo ? CH_PIECE(us, promo) : pc;
	p->sq[to] = put;
	k ^= z_piece[put][to];

	if (type == CH_KING) {
		p->ksq[us] = (uint8_t)to;
		if (to - from == 2 || from - to == 2) {          /* castling: the rook jumps over the king */
			int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
			uint8_t rook = p->sq[rf];
			p->sq[rf] = 0;
			p->sq[rt] = rook;
			k ^= z_piece[rook][rf] ^ z_piece[rook][rt];
		}
	}

	p->castle &= (uint8_t)(castle_mask[from] & castle_mask[to]);
	p->half = (type == CH_PAWN || cap) ? 0 : (uint16_t)(p->half + 1);
	if (us == CH_BLACK)
		p->full++;
	p->ep = (type == CH_PAWN && (to - from == 16 || from - to == 16)) ? (int8_t)((from + to) / 2) : -1;
	p->side = (uint8_t)(us ^ 1);
	k ^= z_side ^ z_castle[p->castle];
	p->key = k;
	p->key ^= ep_key(p);
}

void ch_unmake(struct ch_pos *p, ch_move m, const struct ch_undo *u)
{
	int from = CH_FROM(m), to = CH_TO(m), us = p->side ^ 1;
	uint8_t pc = p->sq[to];                                  /* the moved piece; a promoted pawn is a pawn again */
	if (CH_PROMO(m))
		pc = CH_PIECE(us, CH_PAWN);

	p->sq[from] = pc;
	p->sq[to] = 0;
	if (u->captured) {
		if (CH_TYPE(pc) == CH_PAWN && to == u->ep && FILE_OF(from) != FILE_OF(to))
			p->sq[us == CH_WHITE ? to - 8 : to + 8] = u->captured;
		else
			p->sq[to] = u->captured;
	}
	if (CH_TYPE(pc) == CH_KING) {
		p->ksq[us] = (uint8_t)from;
		if (to - from == 2 || from - to == 2) {
			int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
			p->sq[rf] = p->sq[rt];
			p->sq[rt] = 0;
		}
	}
	if (us == CH_BLACK)
		p->full--;
	p->side = (uint8_t)us;
	p->castle = u->castle;
	p->ep = u->ep;
	p->half = u->half;
	p->key = u->key;
}

void ch_make_null(struct ch_pos *p, struct ch_undo *u)
{
	u->move = 0;
	u->captured = 0;
	u->castle = p->castle;
	u->ep = p->ep;
	u->half = p->half;
	u->key = p->key;
	p->key ^= ep_key(p) ^ z_side;
	p->ep = -1;
	p->half++;
	if (p->side == CH_BLACK)
		p->full++;
	p->side ^= 1;
}

void ch_unmake_null(struct ch_pos *p, const struct ch_undo *u)
{
	p->side ^= 1;
	if (p->side == CH_BLACK)
		p->full--;
	p->castle = u->castle;
	p->ep = u->ep;
	p->half = u->half;
	p->key = u->key;
}

int ch_gen_legal(const struct ch_pos *p, ch_move *out)
{
	struct ch_pos c = *p;
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_pseudo(&c, mv, 0), k = 0, us = c.side;
	for (int i = 0; i < n; i++) {
		struct ch_undo u;
		ch_make(&c, mv[i], &u);
		if (!ch_attacked(&c, c.ksq[us], us ^ 1))
			out[k++] = mv[i];
		ch_unmake(&c, mv[i], &u);
	}
	return k;
}

int ch_move_is_capture(const struct ch_pos *p, ch_move m)
{
	if (p->sq[CH_TO(m)])
		return 1;
	return CH_TYPE(p->sq[CH_FROM(m)]) == CH_PAWN && CH_TO(m) == p->ep;
}

ch_move ch_move_find(const struct ch_pos *p, int from, int to, int promo)
{
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_legal(p, mv);
	for (int i = 0; i < n; i++) {
		if (CH_FROM(mv[i]) != from || CH_TO(mv[i]) != to)
			continue;
		int want = CH_PROMO(mv[i]) ? (promo ? promo : CH_QUEEN) : 0;
		if (CH_PROMO(mv[i]) == want)
			return mv[i];
	}
	return 0;
}

/* ---- text ---------------------------------------------------------------------------------------------------------- */

void ch_move_uci(ch_move m, char *out)
{
	out[0] = (char)('a' + FILE_OF(CH_FROM(m)));
	out[1] = (char)('1' + RANK_OF(CH_FROM(m)));
	out[2] = (char)('a' + FILE_OF(CH_TO(m)));
	out[3] = (char)('1' + RANK_OF(CH_TO(m)));
	out[4] = CH_PROMO(m) ? "?pnbrqk"[CH_PROMO(m)] : 0;
	out[CH_PROMO(m) ? 5 : 4] = 0;
}

ch_move ch_move_from_uci(const struct ch_pos *p, const char *s)
{
	size_t len = s ? strlen(s) : 0;
	if (len < 4 || len > 5 || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8' || s[2] < 'a' || s[2] > 'h' ||
	    s[3] < '1' || s[3] > '8')
		return 0;
	int promo = 0;
	if (len == 5) {
		const char *at = strchr("nbrq", tolower((unsigned char)s[4]));
		if (!at || !*at)
			return 0;
		promo = CH_KNIGHT + (int)(at - "nbrq");
	}
	int from = (s[1] - '1') * 8 + (s[0] - 'a'), to = (s[3] - '1') * 8 + (s[2] - 'a');
	ch_move m = ch_move_find(p, from, to, promo);
	/* "e7e8" with no letter would be taken as a queen by ch_move_find; as text it must name the piece */
	if (m && CH_PROMO(m) && !promo)
		return 0;
	return m;
}

void ch_move_san(const struct ch_pos *p, ch_move m, char *out)
{
	int from = CH_FROM(m), to = CH_TO(m), n = 0;
	uint8_t pc = p->sq[from];
	int type = CH_TYPE(pc);

	if (type == CH_KING && (to - from == 2 || from - to == 2)) {
		n = (int)snprintf(out, 16, "%s", to > from ? "O-O" : "O-O-O");
	} else {
		int capture = ch_move_is_capture(p, m);
		if (type != CH_PAWN) {
			out[n++] = "?PNBRQK"[type];
			ch_move mv[CH_MAX_MOVES];
			int cnt = ch_gen_legal(p, mv), other = 0, same_file = 0, same_rank = 0;
			for (int i = 0; i < cnt; i++) {
				int f = CH_FROM(mv[i]);
				if (CH_TO(mv[i]) != to || f == from || p->sq[f] != pc)
					continue;
				other++;
				same_file += FILE_OF(f) == FILE_OF(from);
				same_rank += RANK_OF(f) == RANK_OF(from);
			}
			if (other) {
				if (!same_file)
					out[n++] = (char)('a' + FILE_OF(from));
				else if (!same_rank)
					out[n++] = (char)('1' + RANK_OF(from));
				else {
					out[n++] = (char)('a' + FILE_OF(from));
					out[n++] = (char)('1' + RANK_OF(from));
				}
			}
		} else if (capture) {
			out[n++] = (char)('a' + FILE_OF(from));
		}
		if (capture)
			out[n++] = 'x';
		out[n++] = (char)('a' + FILE_OF(to));
		out[n++] = (char)('1' + RANK_OF(to));
		if (CH_PROMO(m)) {
			out[n++] = '=';
			out[n++] = "?PNBRQK"[CH_PROMO(m)];
		}
		out[n] = 0;
	}
	/* + or # */
	struct ch_pos c = *p;
	struct ch_undo u;
	ch_make(&c, m, &u);
	if (ch_in_check(&c)) {
		ch_move mv[CH_MAX_MOVES];
		out[n++] = ch_gen_legal(&c, mv) ? '+' : '#';
		out[n] = 0;
	}
}

ch_move ch_move_from_san(const struct ch_pos *p, const char *s)
{
	char want[24];
	size_t len = s ? strlen(s) : 0;
	if (len == 0 || len >= sizeof want)
		return 0;
	memcpy(want, s, len + 1);
	while (len && strchr("+#!?", want[len - 1]))
		want[--len] = 0;
	for (size_t i = 0; i < len; i++)
		if (want[i] == '0')
			want[i] = 'O';

	/* everything but castling ends in the square it goes to (before "=Q"): only the moves there need their SAN */
	int to = -1;
	if (want[0] != 'O') {
		size_t end = len;
		if (end >= 2 && want[end - 2] == '=')
			end -= 2;
		if (end < 2 || want[end - 2] < 'a' || want[end - 2] > 'h' || want[end - 1] < '1' || want[end - 1] > '8')
			return 0;
		to = (want[end - 1] - '1') * 8 + (want[end - 2] - 'a');
	}
	ch_move mv[CH_MAX_MOVES];
	int cnt = ch_gen_legal(p, mv);
	for (int i = 0; i < cnt; i++) {
		if (to >= 0 && CH_TO(mv[i]) != to)
			continue;
		char san[16];
		ch_move_san(p, mv[i], san);
		size_t l = strlen(san);
		while (l && strchr("+#", san[l - 1]))
			san[--l] = 0;
		if (!strcmp(san, want))
			return mv[i];
	}
	return 0;
}

/* ---- perft --------------------------------------------------------------------------------------------------------- */

uint64_t ch_perft(struct ch_pos *p, int depth)
{
	if (depth <= 0)
		return 1;
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_pseudo(p, mv, 0), us = p->side;
	uint64_t total = 0;
	for (int i = 0; i < n; i++) {
		struct ch_undo u;
		ch_make(p, mv[i], &u);
		if (!ch_attacked(p, p->ksq[us], us ^ 1))
			total += depth == 1 ? 1 : ch_perft(p, depth - 1);
		ch_unmake(p, mv[i], &u);
	}
	return total;
}

uint64_t ch_perft_divide(struct ch_pos *p, int depth, void (*each)(ch_move, uint64_t, void *), void *ud)
{
	ch_move mv[CH_MAX_MOVES];
	int n = ch_gen_legal(p, mv);
	uint64_t total = 0;
	for (int i = 0; i < n; i++) {
		struct ch_undo u;
		ch_make(p, mv[i], &u);
		uint64_t c = depth <= 1 ? 1 : ch_perft(p, depth - 1);
		ch_unmake(p, mv[i], &u);
		if (each)
			each(mv[i], c, ud);
		total += c;
	}
	return total;
}

/* ---- the end of a game ------------------------------------------------------------------------------------------------ */

int ch_insufficient_material(const struct ch_pos *p)
{
	int minors = 0, knights = 0, light = 0, dark = 0;
	for (int s = 0; s < 64; s++) {
		int t = CH_TYPE(p->sq[s]);
		if (t == CH_PAWN || t == CH_ROOK || t == CH_QUEEN)
			return 0;
		if (t == CH_KNIGHT) {
			knights++;
			minors++;
		} else if (t == CH_BISHOP) {
			minors++;
			if (((s & 7) + (s >> 3)) & 1)
				light++;
			else
				dark++;
		}
	}
	if (minors <= 1)
		return 1;                       /* K against K, or one minor piece on the board */
	return knights == 0 && (light == 0 || dark == 0);   /* only bishops, all on one colour of squares */
}

const char *ch_status_name(enum ch_status s)
{
	switch (s) {
	case CH_ONGOING:          return "ongoing";
	case CH_CHECKMATE:        return "checkmate";
	case CH_STALEMATE:        return "stalemate";
	case CH_DRAW_MATERIAL:    return "draw by insufficient material";
	case CH_DRAW_REPETITION:  return "draw by repetition";
	case CH_DRAW_FIFTY:       return "draw by the fifty move rule";
	}
	return "?";
}

/* ---- a game ------------------------------------------------------------------------------------------------------------ */

int ch_game_init(struct ch_game *g, const char *fen, const char **err)
{
	memset(g, 0, sizeof *g);
	if (fen) {
		if (ch_pos_set_fen(&g->start, fen, err) < 0)
			return -1;
	} else {
		ch_pos_start(&g->start);
	}
	g->pos = g->start;
	g->cap = 256;
	g->ply = calloc((size_t)g->cap, sizeof *g->ply);
	g->hist = calloc((size_t)g->cap + 1, sizeof *g->hist);
	if (!g->ply || !g->hist) {
		ch_game_free(g);
		return fen_fail(err, "out of memory");
	}
	g->hist[0] = g->pos.key;
	return 0;
}

void ch_game_free(struct ch_game *g)
{
	free(g->ply);
	free(g->hist);
	memset(g, 0, sizeof *g);
}

int ch_game_play(struct ch_game *g, ch_move m)
{
	ch_move mv[CH_MAX_MOVES];
	int cnt = ch_gen_legal(&g->pos, mv), ok = 0;
	for (int i = 0; i < cnt && !ok; i++)
		ok = mv[i] == m;
	if (!ok)
		return -1;
	if (g->n == g->cap) {
		int cap = g->cap * 2;
		struct ch_ply *ply = realloc(g->ply, (size_t)cap * sizeof *ply);
		if (!ply)
			return -1;
		g->ply = ply;
		uint64_t *hist = realloc(g->hist, ((size_t)cap + 1) * sizeof *hist);
		if (!hist)
			return -1;
		g->hist = hist;
		g->cap = cap;
	}
	struct ch_ply *pl = &g->ply[g->n];
	pl->move = m;
	ch_move_san(&g->pos, m, pl->san);
	ch_make(&g->pos, m, &pl->undo);
	g->n++;
	g->hist[g->n] = g->pos.key;
	return 0;
}

int ch_game_undo(struct ch_game *g)
{
	if (g->n == 0)
		return -1;
	g->n--;
	ch_unmake(&g->pos, g->ply[g->n].move, &g->ply[g->n].undo);
	return 0;
}

int ch_game_repetitions(const struct ch_game *g)
{
	int count = 1, lim = g->n - g->pos.half;
	if (lim < 0)
		lim = 0;
	for (int i = g->n - 2; i >= lim; i -= 2)
		if (g->hist[i] == g->hist[g->n])
			count++;
	return count;
}

enum ch_status ch_game_status(const struct ch_game *g)
{
	ch_move mv[CH_MAX_MOVES];
	if (ch_gen_legal(&g->pos, mv) == 0)
		return ch_in_check(&g->pos) ? CH_CHECKMATE : CH_STALEMATE;
	if (ch_insufficient_material(&g->pos))
		return CH_DRAW_MATERIAL;
	if (ch_game_repetitions(g) >= 3)
		return CH_DRAW_REPETITION;
	if (g->pos.half >= 100)
		return CH_DRAW_FIFTY;
	return CH_ONGOING;
}

/* chessai.h - the search and the evaluation of the chess game (stage 2; the rules are chess.h).
 *
 * `chai_search` is synchronous: the window runs it in a thread and ends it with `chai_stop` (the only call that may
 * come from another thread).  One `struct chai` holds the transposition table and the move ordering tables; it is
 * for one search at a time.
 *
 * Scores are centipawns from the point of view of the side to move; a mate is +-(CHAI_MATE - plies to the mate).
 *
 * What it does: negamax with alpha-beta in the principal variation form, iterative deepening, a transposition table,
 * a quiescence search, check extensions, null move pruning, late move reductions, futility and reverse futility
 * pruning, move ordering by the table move / captures (most valuable victim first) / killers / history, a
 * static evaluation of material, piece square tables (the middle game and the end game blended by the pieces left),
 * bishop pair, pawn structure, passed pawns, rooks on open files and the shelter of the king.
 * Not done: opening book, endgame tables, pondering, several threads, aspiration windows, static exchange evaluation.
 */
#ifndef MELLAGUI_CHESSAI_H
#define MELLAGUI_CHESSAI_H

#include "chess.h"

#define CHAI_MATE       30000
#define CHAI_MATE_MAX   (CHAI_MATE - 256)       /* a score beyond this is a mate */
#define CHAI_MAXPLY     96

struct chai;

/* What to stop at: the first limit reached ends the search (after the first full depth, which always finishes). */
struct chai_limits {
	int      depth;       /* plies; 0 = as deep as the time allows */
	int      time_ms;     /* 0 = no time limit */
	int64_t  nodes;       /* 0 = no limit */
	int      noise;       /* centipawns of made up error in the evaluation (weaker play); 0 = none */
	uint32_t seed;        /* the error is a function of the position and this, so a search repeats */
	int      flags;       /* CHAI_PLAIN */
};
/* For the tests: alpha-beta, the table and the move ordering only - no quiescence, no extensions, no pruning, no
 * repetition or fifty move rule - so that the value is exactly that of a plain minimax of the same depth. */
#define CHAI_PLAIN 1

struct chai_result {
	ch_move  best;        /* 0 = the side to move has no legal move */
	ch_move  ponder;      /* the reply the search expects, or 0 */
	int      score;       /* of the last depth that was finished */
	int      depth;       /* the last depth that was finished */
	int64_t  nodes;
	int      time_ms;
	int      npv;
	ch_move  pv[CHAI_MAXPLY];
};
typedef void (*chai_info_fn)(const struct chai_result *r, void *ud);   /* after every finished depth */

struct chai *chai_new(int tt_mb);                    /* tt_mb megabytes of table (rounded down to a power of two) */
void         chai_free(struct chai *c);
void         chai_clear(struct chai *c);             /* forget the table and the ordering: a new game */

/* `hist` = the keys of the positions of the game so far, hist[nhist - 1] being the position `pos` itself (what
 * ch_game.hist holds); they make a repetition seen.  NULL / 0 = no history.  Returns 0, or -1 when stopped before
 * a first move was chosen (cannot happen while the side to move has a legal move). */
int          chai_search(struct chai *c, const struct ch_pos *pos, const uint64_t *hist, int nhist,
                         const struct chai_limits *lim, struct chai_result *res, chai_info_fn info, void *ud);
int          chai_search_game(struct chai *c, const struct ch_game *g, const struct chai_limits *lim,
                              struct chai_result *res, chai_info_fn info, void *ud);
void         chai_stop(struct chai *c);              /* thread safe: the running search ends soon with what it has */

int          chai_eval(const struct ch_pos *p);      /* the static evaluation, from the side to move */
int          chai_score_is_mate(int score);
int          chai_mate_in(int score);                /* moves (not plies) to the mate, negative = being mated */

/* Strength levels 1 .. chai_level_count(): a depth, a time and an amount of made up error. */
int          chai_level_count(void);
void         chai_level(int level, struct chai_limits *lim);
const char  *chai_level_name(int level);

#endif

/* chessbook.h - a small opening book for the chess game: lines written down in SAN, built into a table of positions.
 *
 * The lines are in chessbook.c (about seventy of the main openings, up to 10 - 16 plies deep, written from the author's memory, NOT taken from
 * a database: the names and the choice of lines are a layman's; every move is checked against the rules at start-up, see chbook_errors).
 * A position can be reached by different move orders: the table is keyed by the position's hash (ch_pos.key), so transpositions meet.
 *
 *   chbook_pick(b, pos, seed, max_ply)    a move of the book for the position, chosen by weight (how many lines go that way) with `seed`; 0 = not in
 *                                         the book, or the game is deeper than max_ply plies
 *   chbook_name(b, key)                   the opening's name for a position, e.g. "Sicilian Defence: Najdorf Variation", or NULL
 *   chbook_moves(b, pos, ...)             all the book moves of a position with their weights (the tests, a hint)
 */
#ifndef MELLAGUI_CHESSBOOK_H
#define MELLAGUI_CHESSBOOK_H

#include "chess.h"

struct chbook;

struct chbook *chbook_new(void);
void           chbook_free(struct chbook *b);

int            chbook_lines(const struct chbook *b);        /* how many lines were read */
int            chbook_positions(const struct chbook *b);    /* how many different positions the table holds */
int            chbook_errors(const struct chbook *b);       /* lines that held a move that is not legal (the rest of such a line is left out) */
const char    *chbook_error_text(const struct chbook *b);   /* the first of them: "name: move" */

/* The number of plies played before this position (0 at the start). */
int            chbook_ply(const struct ch_pos *p);

ch_move        chbook_pick(const struct chbook *b, const struct ch_pos *p, uint32_t seed, int max_ply);
int            chbook_moves(const struct chbook *b, const struct ch_pos *p, ch_move *out, unsigned *weight, int cap);
const char    *chbook_name(const struct chbook *b, uint64_t key);

#endif

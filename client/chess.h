/* chess.h - the rules of chess as a plain engine: no window, no clock, no I/O.
 *
 * Used by mellagui-chess (the window), later by its search; tested by tests/chess_test.c (perft against the known
 * numbers, FEN / SAN / UCI round trips, the draw rules).
 *
 * Squares are 0..63, a1 = 0, h1 = 7, a8 = 56, h8 = 63 (file = sq & 7, rank = sq >> 3).
 * A piece is `type | colour << 3`: white 1..6, black 9..14, 0 = empty.
 * A move is 16 bits: from | to << 6 | promotion type << 12.  Castling is the king's two step (e1g1), an en passant
 * capture is the pawn's diagonal step onto the empty en passant square; nothing else is stored, `ch_make` works the
 * rest out from the position.  0 is "no move" (a1a1 is no move).
 *
 * Threads: `ch_init()` builds the tables once; every entry that makes a position calls it, so a position made on the
 * main thread first makes the rest safe for a search thread.  A `struct ch_pos` is a plain value: copy it to search.
 */
#ifndef MELLAGUI_CHESS_H
#define MELLAGUI_CHESS_H

#include <stdint.h>
#include <stddef.h>

enum { CH_NONE = 0, CH_PAWN, CH_KNIGHT, CH_BISHOP, CH_ROOK, CH_QUEEN, CH_KING };
enum { CH_WHITE = 0, CH_BLACK = 1 };
#define CH_PIECE(colour, type) ((uint8_t)((type) | ((colour) << 3)))
#define CH_TYPE(p)             ((p) & 7)
#define CH_COLOR(p)            ((p) >> 3)

/* castling rights */
enum { CH_WK = 1, CH_WQ = 2, CH_BK = 4, CH_BQ = 8 };

typedef uint16_t ch_move;
#define CH_MOVE(from, to, promo) ((ch_move)((from) | ((to) << 6) | ((promo) << 12)))
#define CH_FROM(m)  ((m) & 63)
#define CH_TO(m)    (((m) >> 6) & 63)
#define CH_PROMO(m) (((m) >> 12) & 7)

#define CH_MAX_MOVES 256   /* more than any position has (the record is 218) */

struct ch_pos {
	uint8_t  sq[64];
	uint8_t  side;     /* CH_WHITE / CH_BLACK: who moves */
	uint8_t  castle;   /* CH_WK | CH_WQ | CH_BK | CH_BQ that are still allowed */
	int8_t   ep;       /* the square a pawn just passed over with a double step, or -1 */
	uint8_t  ksq[2];   /* where the kings are */
	uint16_t half;     /* halfmove clock: plies since a pawn move or a capture */
	uint16_t full;     /* the move number, 1 at the start, up after Black's move */
	uint64_t key;      /* Zobrist hash (the en passant square counts only when a pawn could take it) */
};

/* what ch_make changed and cannot work out backwards */
struct ch_undo {
	ch_move  move;
	uint8_t  captured;
	uint8_t  castle;
	int8_t   ep;
	uint16_t half;
	uint64_t key;
};

void ch_init(void);

/* ---- positions ---- */
void        ch_pos_start(struct ch_pos *p);
/* Reads a FEN (the last two numbers may be left out).  0 = ok, else -1 and *err (when not NULL) says what is wrong:
 * a bad piece / rank / colour / castling / en passant field, no or two kings, a pawn on the first or last rank, the
 * side that does not move being in check.  Castling rights without the king and the rook at home are dropped. */
int         ch_pos_set_fen(struct ch_pos *p, const char *fen, const char **err);
void        ch_pos_fen(const struct ch_pos *p, char *out, size_t cap);   /* about 90 bytes are enough */
uint64_t    ch_pos_compute_key(const struct ch_pos *p);                  /* from scratch (the tests compare it with `key`) */

int         ch_attacked(const struct ch_pos *p, int sq, int by_colour);
int         ch_in_check(const struct ch_pos *p);                         /* is the side to move in check */

/* ---- moves ---- */
/* Pseudo legal moves (they may leave the own king in check); `captures_only` = captures, en passant and every
 * promotion (what a quiescence search wants).  Returns the number written to out[CH_MAX_MOVES]. */
int         ch_gen_pseudo(const struct ch_pos *p, ch_move *out, int captures_only);
int         ch_gen_legal(const struct ch_pos *p, ch_move *out);
void        ch_make(struct ch_pos *p, ch_move m, struct ch_undo *u);     /* m must be at least pseudo legal */
void        ch_unmake(struct ch_pos *p, ch_move m, const struct ch_undo *u);
/* A null move for the search: the side to move passes (the en passant right goes).  Not a legal move: only for a
 * search that is not in check.  Undone with ch_unmake_null. */
void        ch_make_null(struct ch_pos *p, struct ch_undo *u);
void        ch_unmake_null(struct ch_pos *p, const struct ch_undo *u);
/* the legal move that goes from -> to (promo = 0 or a piece type), or 0.  For a click in the window. */
ch_move     ch_move_find(const struct ch_pos *p, int from, int to, int promo);
int         ch_move_is_capture(const struct ch_pos *p, ch_move m);

void        ch_move_uci(ch_move m, char *out);                           /* "e2e4", "e7e8q" (6 bytes) */
ch_move     ch_move_from_uci(const struct ch_pos *p, const char *s);     /* the legal move, or 0 */
/* Standard algebraic: "Nf3", "exd5", "O-O", "Qh4xe1#", "R1a3", "e8=Q+" (16 bytes are enough). */
void        ch_move_san(const struct ch_pos *p, ch_move m, char *out);
/* Reads SAN of a legal move; a trailing + # ! ? is ignored, 0-0 counts as O-O.  0 = no such legal move. */
ch_move     ch_move_from_san(const struct ch_pos *p, const char *s);

/* ---- counting (the check of the move generator) ---- */
uint64_t    ch_perft(struct ch_pos *p, int depth);
/* the number below each legal move of the position, one call per move */
uint64_t    ch_perft_divide(struct ch_pos *p, int depth, void (*each)(ch_move m, uint64_t n, void *ud), void *ud);

/* ---- the end of a game ---- */
enum ch_status {
	CH_ONGOING = 0,
	CH_CHECKMATE,        /* the side to move has lost */
	CH_STALEMATE,        /* draw */
	CH_DRAW_MATERIAL,    /* draw: nobody can mate (K, K+minor, bishops all on one colour of squares) */
	CH_DRAW_REPETITION,  /* draw: the position came up three times (counted as automatic, not claimed) */
	CH_DRAW_FIFTY        /* draw: 50 moves without a pawn move or a capture (automatic, not claimed) */
};
int         ch_insufficient_material(const struct ch_pos *p);
const char *ch_status_name(enum ch_status s);

/* A game: a position and everything that was played, so moves can be taken back and repetitions counted. */
struct ch_ply {
	ch_move        move;
	char           san[16];   /* written when it was played */
	struct ch_undo undo;
};
struct ch_game {
	struct ch_pos start;
	struct ch_pos pos;
	struct ch_ply *ply;       /* n played moves */
	uint64_t      *hist;      /* hist[i] = the key before ply i, hist[n] = now */
	int            n, cap;
};

int         ch_game_init(struct ch_game *g, const char *fen, const char **err);   /* fen NULL = the start */
void        ch_game_free(struct ch_game *g);
int         ch_game_play(struct ch_game *g, ch_move m);       /* 0 = played, -1 = not a legal move */
int         ch_game_undo(struct ch_game *g);                  /* 0 = one move taken back, -1 = nothing to take back */
enum ch_status ch_game_status(const struct ch_game *g);
int         ch_game_repetitions(const struct ch_game *g);     /* how many times the position now has occurred (1 = new) */

#endif

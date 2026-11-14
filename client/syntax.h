/*
 * syntax.h: syntax highlighting for the editor. A small hand-written lexer, one line at a time: the language comes from the file's name
 * (or its #! line), the colour classes from a table of keywords per language, and the only thing carried from one line to the next is a
 * small state number (inside a block comment, a Python triple-quoted string, a JavaScript template string, a Markdown code fence, an HTML tag).
 * The editor keeps that state per line (it is recomputed from the line that was edited downwards), so only the lines on screen are lexed
 * for painting and a long file costs nothing until it is scrolled to.
 *
 *   const struct syn_lang *l = syn_find("main.c", "#include <stdio.h>");        NULL: no colours for this file
 *   uint8_t cls[len];  state = syn_line(l, line, len, state_in, cls);           cls[i] = SYN_* of byte i of the line
 */
#ifndef LM_SYNTAX_H
#define LM_SYNTAX_H

#include <stddef.h>
#include <stdint.h>

enum {
    SYN_PLAIN = 0, SYN_COMMENT, SYN_STRING, SYN_NUMBER, SYN_KEYWORD, SYN_TYPE, SYN_FUNC, SYN_PREPROC,
    SYN_ADDED, SYN_REMOVED,             /* diff lines */
    SYN_N
};

struct syn_lang;

/* The language of a file by its name (and the first line, for scripts with a #! line), NULL when there is none. */
const struct syn_lang *syn_find(const char *path, const char *first_line);
const char *syn_name(const struct syn_lang *l);

/* Lexes one line (`len` bytes, no line end). `state` is what the line before it left (0 at the top of the file); returns the state the next line
 * starts in. `cls` (len bytes) gets the class of every byte, or NULL when only the state is wanted. */
int syn_line(const struct syn_lang *l, const char *s, size_t len, int state, uint8_t *cls);

#endif

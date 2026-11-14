/* mdview - Markdown shown as a page (the editor's preview, `mellagui-edit FILE.md`).
 *
 * md_parse() reads the text into blocks, md_layout() sets them out for a column of a given width into a list of drawing items (text runs, rectangles,
 * pictures) in document coordinates, md_paint() draws the part that is in view. The three steps are separate so that the layout can be tested without a
 * window (mellagui-edit --selftest) and that scrolling only paints.
 *
 * What is understood: # headings (and === / --- underlined ones), paragraphs, **bold**, *emphasis* (shown in a tinted colour: there is no italic face),
 * ~~strikethrough~~, `code`, [links](url), <autolinks> and bare http(s):// addresses, ![images](local-file) (PNG / JPEG / PPM next to the document; others
 * are shown as a line of text), bullet and numbered lists with nesting, [ ] / [x] task lists, > quotes, fenced and indented code, --- rules, pipe tables with
 * alignment, backslash escapes, hard line breaks. Not understood: HTML, reference links, footnotes, nested block quotes inside lists.
 *
 * Text is set in labels (proportional when the font allows it; the editor's own text is cells), so layout and painting switch lm_font_ui_cells() to
 * labels while they run and put the program's own mode back.
 */
#ifndef MELLAGUI_MDVIEW_H
#define MELLAGUI_MDVIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct wlwin;
struct md_doc;

struct md_doc *md_parse(const char *text, size_t n);
void md_free(struct md_doc *d);
/* Lay the page out in a column `width` pixels wide. `base_dir` is where relative picture files are looked for (NULL = none are loaded). */
void md_layout(struct md_doc *d, int width, const char *base_dir);
int md_height(const struct md_doc *d);                                  /* of the laid out page, in pixels */
int md_width(const struct md_doc *d);                                   /* the width it was laid out for */
/* Draw the part of the page that shows in the box (x, y, w, h) of the window when the page is scrolled down by `scroll` pixels. */
void md_paint(struct wlwin *w, const struct md_doc *d, int x, int y, int w_box, int h_box, int scroll);
/* The address of the link at page position (px, py), NULL when there is none. */
const char *md_link_at(const struct md_doc *d, int px, int py);

/* Selecting text on the page (positions are page coordinates: x from the column's left edge, y from the top of the page). */
void md_sel_begin(struct md_doc *d, int px, int py);                    /* a press: the anchor and the caret go there, nothing is selected yet */
void md_sel_drag(struct md_doc *d, int px, int py);                     /* the pointer moves with the button down */
void md_sel_word(struct md_doc *d, int px, int py);                     /* a double click */
void md_sel_line(struct md_doc *d, int px, int py);                     /* a triple click */
void md_sel_all(struct md_doc *d);
void md_sel_clear(struct md_doc *d);
bool md_sel_active(const struct md_doc *d);                             /* something (not an empty range) is selected */
char *md_sel_text(const struct md_doc *d);                              /* a malloc'd copy of the selected text, NULL when nothing is selected */

enum md_stat { MD_N_BLOCKS, MD_N_HEADINGS, MD_N_LINKS, MD_N_ITEMS, MD_N_CODE, MD_N_TABLES, MD_N_IMAGES };
int md_count(const struct md_doc *d, enum md_stat what);
/* For the tests: one line per text run / rectangle (`text x y w flags scale "..."`), then `ok height=H`. */
void md_dump(const struct md_doc *d, FILE *f);

#endif

#ifndef MELLAGUI_PDFREAD_H
#define MELLAGUI_PDFREAD_H

/*
 * The text of a PDF for the file manager's preview, with no library: the objects are found by scanning the file (no xref table is trusted, so damaged and
 * incrementally updated files read too), streams are unpacked (Flate with the PNG / TIFF predictors, ASCII85, ASCIIHex, LZW, RunLength), object streams are
 * opened, the page tree is walked, and the content streams are interpreted for text only: BT / ET, Tf, Td, TD, Tm, T*, ', ", Tj, TJ and the graphics state
 * matrix (q, Q, cm). Letters are turned into Unicode through the font's /ToUnicode map, else through its /Encoding (WinAnsi, MacRoman, Standard, /Differences
 * with the Adobe glyph names of Latin text); line breaks come from the vertical position, blanks from the horizontal gaps. What is NOT read: encrypted files,
 * fonts with neither a ToUnicode map nor a plain encoding (their letters are left out), pictures, forms (XObjects), annotations, reading order beyond the
 * order of the content stream. Every length and count is limited, so a damaged or hostile file gives less text or an error, not a crash.
 */

#include <stddef.h>

struct pdf_info {
    char title[200], author[160], subject[200], producer[120], created[40];
    char version[8];
    int pages;                          /* the number of pages the file says (or that were found) */
    int pages_read;                     /* how many of them were turned into text */
};

/* 0 = ok (*text is malloc'd UTF-8, lines separated by \n, pages by a line of "=== Page N ==="); -1 = not a PDF; -2 = a PDF that cannot be read (err). */
int pdf_text(const char *path, int max_pages, size_t max_chars, struct pdf_info *info, char **text, char *err, size_t errn);

#endif

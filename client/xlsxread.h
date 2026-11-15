#ifndef MELLAGUI_XLSXREAD_H
#define MELLAGUI_XLSXREAD_H

/*
 * A read-only reader of Excel workbooks for the file manager's preview, with no library: the .xlsx family (.xlsx .xlsm .xltx .xltm: Office Open XML) and the
 * old .xls (Excel 97-2003: BIFF8 records in an OLE2 compound file) the OpenDocument .ods / .ots (ZIP with content.xml) and Apple Numbers (.numbers), plus word processor documents and presentations as Markdown (xl_document); see the end of xlsxread.c.
 * An .xlsx workbook is a ZIP
 * file of XML parts, so this reads the ZIP's central directory (stored and deflated entries; no ZIP64, no encryption), the sheet list (xl/workbook.xml and
 * its relationships), the styles (only to tell dates and percentages from plain numbers), the shared strings and one sheet's cells. Shown: text (shared,
 * inline, formula results), numbers (shortest form that keeps 15 digits), booleans, errors, dates and times (by the cell's number format and the workbook's
 * date system), percentages; a formula with no stored result shows as `=formula`. The first rows of a sheet are all that is read: its XML is inflated only
 * as far as `max_rows` rows need, so a sheet of a million rows costs the same as one of a thousand. NOT understood: Excel 5 / 95 (BIFF5) and password protected .xls files (an error line says so), .xlsb, flat .fods, number
 * formats other than date / time / percent (a currency shows as the plain number), merged cells, hidden rows and columns, charts, pictures. Every length that
 * comes from the file is checked and the sizes are capped, so a damaged file gives an error, not a crash.
 */

#include <stddef.h>

struct xl_book {
    const char *kind;                   /* "Excel" or "OpenDocument" */
    int nsheets;
    char **sheet_name;                  /* the visible worksheets, in the workbook's order */
    int sheet;                          /* the one in `cell` */
    int ncols, nrows;                   /* column 0 holds the row number, the others are A, B, C ... */
    char **head;                        /* ncols column titles: "", "A", "B", ... */
    char ***cell;                       /* cell[row][col], ncols each; "" for an empty one */
    int last_row;                       /* the last row the sheet says it has (its <dimension>), 0 when unknown */
    int more_rows;                      /* 1: rows were left out (the limit) */
    int more_cols;                      /* 1: columns were left out (the limit) */
};

/* 0 = ok (sheet number `sheet`, counted from 0, up to `max_rows` rows and `max_cols` columns of data); -1 = not a workbook; -2 = a workbook this reader
 * cannot read (the reason in err). */
int xl_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn);
void xl_free(struct xl_book *b);

/* A word processor document or a presentation (.docx .odt .pptx .odp and their templates) as Markdown: headings, paragraphs, bold and emphasis, lists, tables,
 * slides ("## Slide N"). 0 = ok (*md is malloc'd, `info` says what it was); -1 = not one of these; -2 = one that cannot be read (err). */
int xl_document(const char *path, char **md, size_t *mdlen, char *info, size_t infon, char *err, size_t errn);

/* The picture a Numbers package carries of itself (malloc'd JPEG / PNG bytes), or NULL. */
unsigned char *xl_thumbnail(const char *path, size_t *len);
/* Prints the parts and the objects of a Numbers file as text (mellagui-files --numbers-dump); 0 = ok. */
int xl_numbers_dump(const char *path);

#endif

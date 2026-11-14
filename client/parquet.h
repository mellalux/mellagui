#ifndef MELLAGUI_PARQUET_H
#define MELLAGUI_PARQUET_H

/*
 * A read-only Parquet reader for the file manager's preview (no libparquet here): the first rows of a file as text cells. Reads the Thrift compact footer
 * and the pages of the column chunks with pread (the file is never loaded whole). Understood: every physical type (boolean, int32, int64, int96, float,
 * double, byte array, fixed length byte array), the logical meaning of strings, dates and timestamps (millis / micros / nanos, int96), optional columns
 * (nulls are empty cells), struct fields as columns named `a.b`, data pages v1 and v2, PLAIN, PLAIN_DICTIONARY / RLE_DICTIONARY and RLE encodings, the
 * RLE / bit-packed hybrid, and the codecs UNCOMPRESSED, SNAPPY and GZIP. NOT understood: ZSTD, LZ4, Brotli and LZO pages ("compressed with ZSTD: not
 * read"), the DELTA_* and BYTE_STREAM_SPLIT encodings (the cells say so), lists and maps (a cell `<list>`), decimals (the raw number). Every length that
 * comes from the file is checked, the sizes are capped, so a damaged file gives an error, not a crash.
 */

#include <stddef.h>

struct pq_table {
    int ncols, nrows;
    char **name;                        /* ncols column names */
    char ***cell;                       /* cell[row][col]; "" for a null */
    long long total_rows;               /* what the file says it has */
    int row_groups;
    char created_by[96];
};

/* 0 = ok (a table with up to `max_rows` rows); -1 = not a Parquet file; -2 = a file this reader cannot read (the reason in err). */
int pq_read(const char *path, int max_rows, struct pq_table *t, char *err, size_t errn);
void pq_free(struct pq_table *t);

/* The snappy decompressor on its own (tests): `out` must have room for the length the data says; returns that length, or -1. */
long pq_snappy(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap);

#endif

#ifndef MELLAGUI_SQLITEREAD_H
#define MELLAGUI_SQLITEREAD_H

/*
 * A read-only reader of SQLite 3 database files (there is no libsqlite here): enough of the file format to list the tables and read the first rows of a
 * rowid table. Reads pages on demand with pread (the file is never loaded whole). Understood: the 100 byte header, table b-trees (interior pages 0x05, leaf
 * pages 0x0d) with their overflow chains, the record format (all serial types), UTF-8 text. NOT: UTF-16 databases (sq_open says -2), WITHOUT ROWID tables
 * and indexes (their pages are other kinds: a scan of one visits no rows), the write-ahead log (a database that is open in WAL mode shows what was last
 * checkpointed), encrypted files. Every length that comes from the file is checked against the page, and a scan stops at a page budget, so a damaged or
 * hostile file ends the scan, it does not crash.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sq {
    int fd;
    uint32_t psize, usable, npages;
};

enum { SQ_NULL, SQ_INT, SQ_FLOAT, SQ_TEXT, SQ_BLOB };

struct sq_val {
    int type;
    int64_t i;
    double f;
    const uint8_t *p;                   /* SQ_TEXT / SQ_BLOB: the bytes (inside the record) */
    size_t n;
};

/* 0 = open; -1 = not a SQLite file (or unreadable); -2 = text is UTF-16 (not read). */
int sq_open(struct sq *q, const char *path);
void sq_close(struct sq *q);

/* The callback gets every row of the table b-tree at `root` (page number) in order: its rowid and its record (valid only during the call).
 * Returns false to stop. The scan ends after `max_rows` rows; returns the number of rows given. */
typedef bool (*sq_row_cb)(void *user, int64_t rowid, const uint8_t *rec, size_t len);
int sq_scan(struct sq *q, uint32_t root, int max_rows, sq_row_cb cb, void *user);

/* A record's values (at most `max`); returns how many it has (even beyond max), -1 when it is damaged. */
int sq_record(const uint8_t *rec, size_t len, struct sq_val *v, int max);

#endif

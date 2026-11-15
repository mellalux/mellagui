#ifndef MELLAGUI_ARCHREAD_H
#define MELLAGUI_ARCHREAD_H

/*
 * A read-only lister of archives for the file manager's preview (no library): ZIP (and everything that is one: jar, apk, docx, odt, epub ...; ZIP64, names in
 * UTF-8 or the old DOS code page 437, the DOS date and time), TAR (ustar, GNU long names, pax headers, links; read by seeking from header to header) and
 * gzip (a single file, or a TAR inside: its first 64 MB are unpacked, so what lies further is not listed). Only the names, sizes and times are read, never
 * the contents. Every length from the file is checked; a damaged archive gives what could be read and a note, or an error, not a crash.
 */

#include <stddef.h>

struct arc_list {
    char kind[24];                      /* "ZIP", "TAR", "TAR.GZ", "GZIP" */
    int n;                              /* the entries kept (at most `max_entries`) */
    char **name;                        /* a directory ends in '/', a link reads "name -> target" */
    char **mtime;                       /* "YYYY-MM-DD HH:MM" or "" */
    long long *size;                    /* unpacked size, -1 when unknown (a directory has none) */
    long long *packed;                  /* packed size, -1 when the format has none (TAR) */
    long long total;                    /* the entries there are (counted past `n`) */
    long long total_size, total_packed; /* sums over all of them */
    int has_packed;                     /* the format has packed sizes (ZIP, GZIP) */
    char note[120];                     /* a remark to show under the list ("" for none) */
};

/* 0 = an archive was listed; -1 = not an archive of these kinds; -2 = one that could not be read (err). */
int arc_read(const char *path, int max_entries, struct arc_list *a, char *err, size_t errn);
void arc_free(struct arc_list *a);
/* The first `cap` bytes of a gzip / xz / bzip2 / zstd stream (told by its first bytes), malloc'd; *got = the bytes, *complete = the whole stream. NULL when it
 * is none of them or does not decode (*why may say so). */
unsigned char *arc_unpack(const unsigned char *in, size_t n, size_t cap, long *got, int *complete, const char **why);
/* A stored name as readable UTF-8 (malloc'd): `dos` = it may be in the DOS code page 437 (a ZIP name without the UTF-8 flag). */
char *arc_clean_name(const unsigned char *p, size_t n, int dos);

#endif

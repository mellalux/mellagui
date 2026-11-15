#ifndef MELLAGUI_ARCIO_H
#define MELLAGUI_ARCIO_H

/*
 * The archiver's engine (mellagui-archive), no library: open an archive, list it, test it, extract all or some of its entries, and make new ones.
 *
 * Open / extract: ZIP (stored and deflated entries, ZIP64, UTF-8 and DOS code page names, Unix modes, symlinks), TAR (ustar, GNU long names, pax, links),
 * TAR in gzip / xz / bzip2 / zstd, a single compressed file, ar, Debian packages (the data archive), RPM (the cpio payload), 7z (LZMA / LZMA2 / Copy / Deflate /
 * BZip2 with the Delta and BCJ filters; each folder - a solid block - is unpacked into memory whole; PPMd, BCJ2 and encrypted archives are refused with a message).
 * Everything that is not a plain TAR is unpacked into memory first (at most AIO_MAX_UNPACKED bytes), a ZIP entry is unpacked in memory one at a time
 * (at most AIO_MAX_ENTRY).
 *
 * Extraction never writes outside the destination folder: names with ".." are refused, a leading "/" is dropped, and every folder on the way is opened
 * without following links (a link planted by the archive itself cannot redirect later files). Existing files are kept unless `overwrite` is set. File
 * modes (without set-id bits) and times are restored; ownership is not.
 *
 * Create: ZIP (deflate, ZIP64 when needed), TAR, TAR.GZ from files and folders (links are stored as links).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIO_MAX_UNPACKED (1024ull << 20)
#define AIO_MAX_ENTRY (1024ull << 20)

enum aio_type { AIO_FILE, AIO_DIR, AIO_SYMLINK, AIO_HARDLINK, AIO_OTHER };

struct aio_entry {
    char *path;                         /* readable UTF-8, as stored ("a/b/c", a folder ends in '/') */
    char *link;                         /* the target of a link */
    int type;
    uint32_t mode;                      /* permission bits, 0 = not stored */
    int64_t mtime;                      /* Unix time, 0 = not stored */
    int64_t size;                       /* unpacked size, -1 unknown */
    int64_t packed;                     /* packed size, -1 = not known per entry */
    uint8_t *raw_path;                  /* the name's bytes as stored (used to extract) */
    size_t raw_len;
    uint32_t crc;
    int has_crc;
    uint64_t off, csize;                /* where the data lies (file offset, or offset in the unpacked stream) */
    int method;
};

struct aio {
    char kind[24];                      /* "ZIP", "TAR.GZ", "DEB", "RPM", "7Z" ... */
    char note[160];
    int n;
    struct aio_entry *e;
    int extractable;                    /* 0: listing only (7z) */
    char why_not[120];
    int mode;                           /* internal: how the data is read */
    int fd;
    uint64_t fsize;
    uint8_t *mem;
    size_t memn;
    int64_t total_size;                 /* sum of the sizes of the files */
    void *sz;                           /* internal: the 7z folders and the one that is unpacked now */
};

struct aio_progress {
    volatile uint64_t done, total;      /* bytes of file data */
    volatile int cancel;                /* set by the caller to stop */
    volatile int files_done;
    char current[260];                  /* the name being worked on (read with aio_progress_current) */
    void *lock;
};

struct aio_report {
    int extracted, dirs, links, skipped, failed;
    uint64_t bytes;
    int nerr;
    char err[6][260];                   /* the first problems */
};

struct aio_opts {
    bool overwrite;                     /* replace files that exist (default: keep them and count them as skipped) */
    bool test_only;                     /* read everything and check the CRCs, write nothing */
};

void aio_progress_init(struct aio_progress *p);
void aio_progress_free(struct aio_progress *p);
void aio_progress_current(struct aio_progress *p, char *out, size_t n);

/* 0 = opened; -1 = not an archive; -2 = unreadable (err). */
int aio_open(const char *path, struct aio **a, char *err, size_t errn);
void aio_close(struct aio *a);

/* Extracts the entries sel[0..nsel) (indexes into a->e; NULL = all) into dest (made when missing). 0 = done (see the report for single failures),
 * -1 = nothing could be done (err), -2 = cancelled. */
int aio_extract(struct aio *a, const int *sel, int nsel, const char *dest, const struct aio_opts *o, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn);

enum { AIO_ZIP, AIO_TAR, AIO_TARGZ };

/* Writes `out` (never over an existing file unless overwrite) from the files and folders `inputs`. 0 = done, -1 = failed (err), -2 = cancelled
 * (the unfinished file is removed). `level` 1..9 for the deflate formats. */
int aio_create(const char *out, int format, const char *const *inputs, int n, int level, bool overwrite, struct aio_progress *p, struct aio_report *rep, char *err, size_t errn);

/* The format an output name asks for (.zip, .tar, .tar.gz / .tgz), or -1. */
int aio_format_of(const char *name);

#endif

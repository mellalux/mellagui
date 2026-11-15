#ifndef MELLAGUI_DECOMP_H
#define MELLAGUI_DECOMP_H

/*
 * Decompressors for the archive lister (client/archread.c), written for it, no library: bzip2, LZMA (the raw stream a 7z folder holds), LZMA2 and the xz
 * container. All of them work into a buffer of `cap` bytes and stop when it is full, so the first part of a big file is enough to list the tar inside.
 * Checksums (the bzip2 and xz block checks) are NOT verified; every length is checked, so damaged input gives a short result or -1, not a crash.
 *
 * Return: the bytes written (0..cap), or -1 when nothing could be decoded. `*complete` (when given) says whether the whole stream was decoded (0: the
 * buffer filled up first, or the data is cut / damaged after what was written).
 */

#include <stddef.h>
#include <stdint.h>

long lm_bzip2_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete);
/* One xz file (several streams and blocks, the LZMA2 filter alone; a filter before it - BCJ, delta - gives -1 and `why`). */
long lm_xz_prefix(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete, const char **why);
/* A raw LZMA stream (a 7z folder's): props = the 5 bytes (lc/lp/pb byte and the dictionary size), `usize` = how much to make. */
long lm_lzma_raw(const uint8_t *in, size_t inlen, const uint8_t props[5], uint8_t *out, size_t usize);
/* A raw LZMA2 stream (a 7z folder's), props = the dictionary size byte. */
long lm_lzma2_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t cap, int *complete);

#endif

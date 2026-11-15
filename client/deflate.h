#ifndef MELLAGUI_DEFLATE_H
#define MELLAGUI_DEFLATE_H

/*
 * A deflate compressor (RFC 1951), written for the archiver (ZIP and tar.gz files), no library: LZ77 with hash chains and lazy matching, dynamic / fixed Huffman
 * or stored blocks, whichever is smallest for the block. It works as a stream: data is written in any pieces, the last 32 KB of what went before is kept as the
 * window, and the compressed bytes go to a sink function as they are made. Levels 1 (fast) .. 9 (small); 6 is the usual.
 */

#include <stddef.h>
#include <stdint.h>

struct lm_deflater;

/* Receives compressed bytes; returns 0, or non-zero to stop (a write error): the next lm_deflate_write then fails. */
typedef int (*lm_deflate_sink)(void *ctx, const uint8_t *p, size_t n);

struct lm_deflater *lm_deflate_new(int level, lm_deflate_sink sink, void *ctx);
/* 0 = ok, -1 = the sink refused something earlier or memory ran out. */
int lm_deflate_write(struct lm_deflater *d, const uint8_t *p, size_t n);
/* Ends the stream (the last block) and frees the compressor; 0 = ok. `d` may be freed without this by lm_deflate_free. */
int lm_deflate_finish(struct lm_deflater *d);
void lm_deflate_free(struct lm_deflater *d);

/* The whole buffer in one go; returns a malloc'd raw deflate stream (*outn bytes), or NULL. */
uint8_t *lm_deflate_buf(const uint8_t *in, size_t n, int level, size_t *outn);

/* CRC-32 (the zip / gzip / png one): continue with the previous result (0 to start). */
uint32_t lm_crc32(uint32_t crc, const uint8_t *p, size_t n);

#endif

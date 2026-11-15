#ifndef MELLAGUI_ZSTDDEC_H
#define MELLAGUI_ZSTDDEC_H

/*
 * A Zstandard decompressor (RFC 8878), written for the file manager's Parquet preview: whole frames from memory into a buffer. Understood: raw, RLE and
 * compressed blocks, Huffman-coded literals (one or four streams, treeless reuse), the three FSE sequence tables (predefined, RLE, FSE-compressed,
 * repeat), repeat offsets, several frames and skippable frames; the content checksum is skipped, not checked. NOT: dictionaries (a frame that names one is
 * refused). Every length is checked against the input and the output buffer, offsets against what has been written, so damaged input gives -1.
 */

#include <stddef.h>
#include <stdint.h>

/* Returns the number of bytes written to `out` (at most `outcap`), -1 on error. */
long lm_zstd_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap);

#endif

#ifndef MELLAGUI_WEBP_H
#define MELLAGUI_WEBP_H

/*
 * A WebP decoder written for MellaGUI, no library: the RIFF container, VP8L (lossless: transforms, colour cache, LZ77, meta prefix codes), VP8 (lossy: one key
 * frame, intra prediction, the loop filter; the picture is made to be the one libwebp makes, including its "fancy" chroma upsampling) with an ALPH chunk (raw or
 * lossless, with the four filters), the extended VP8X format (the first frame of an animation is shown, on the canvas). NOT read: ICC profiles and EXIF (they
 * are skipped), the later frames of an animation, a VP8 frame that is no key frame. Every length from the file is checked.
 */

#include <stddef.h>
#include <stdint.h>

/* RGBA bytes (R, G, B, A, not premultiplied), w * h * 4, malloc'd; NULL with a message in err. */
uint8_t *lm_webp_decode(const uint8_t *data, size_t len, int *w, int *h, char *err, size_t errsz);
/* Does it look like a WebP file? */
int lm_webp_sniff(const uint8_t *data, size_t len);

#endif

#ifndef MELLAGUI_GIFFRAMES_H
#define MELLAGUI_GIFFRAMES_H

/*
 * Every picture of an animated GIF as a whole canvas (for the picture editor, which makes a layer of each): the frames are laid over each other the way a GIF
 * player does (the disposal methods "leave", "clear the frame's area" and "go back to what was there before" are followed; the background is see-through), the
 * result of each step is one frame of w x h pixels, straight ARGB. No library; the LZW decoder is the usual one, interlaced pictures are straightened.
 */

#include <stddef.h>
#include <stdint.h>

struct gif_anim {
    int w, h, n;
    uint32_t **px;                      /* n frames of w * h pixels */
    int *delay_ms;                      /* how long each is shown (10 ms steps) */
    int loops;                          /* 0 = for ever */
    int cut;                            /* frames that were left out because there were more than the caller wanted */
};

/* 0 = ok (at least one frame), else -1 with a message. At most `max_frames` frames are kept. */
int lm_gif_frames(const uint8_t *data, size_t len, int max_frames, struct gif_anim *out, char *err, size_t errsz);
void lm_gif_anim_free(struct gif_anim *g);

#endif

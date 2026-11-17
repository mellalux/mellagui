#ifndef MELLAGUI_FACEDET_H
#define MELLAGUI_FACEDET_H

/*
 * facedet - finds frontal faces in a picture, for the face effects of mellagui-cam (hat, sunglasses, moustache, clown).
 *
 * No library: this is the Viola-Jones detector (an integral image, windows of 24 x 24 on an image pyramid, a cascade of 25 stages of boosted stumps
 * of Haar features, neighbouring hits grouped) running the cascade of OpenCV's haarcascade_frontalface_default.xml - trained by Rainer Lienhart,
 * Intel licence, see client/facedet_data.h, which tools/gen_cascade.py makes from that file. It sees faces that look at the camera and are not
 * turned more than ~25 degrees or tilted much; glasses, a beard or a hat cost some hits; a dark room costs more. It is a detector of 2001,
 * not a neural network: it finds the faces of a webcam in good light and now and then a face that is not there.
 *
 * The picture is squeezed to at most ~320 pixels wide first (a camera picture of 640 x 480 costs about 15 ms, the cascade is thrown out early at
 * most places). Not thread safe in the sense that it keeps no state: every call allocates its own buffers, so calls from several threads are fine.
 */

#include <stdint.h>

#include "camfx.h"

/* The faces of an XRGB8888 picture (w * h), the biggest first: `out[0..n)` in the picture's own pixels. Returns n (0 .. max). */
int facedet_detect(const uint32_t *px, int w, int h, struct camfx_face *out, int max);

/* Does a face found before (`old`) and one found now (`cur`) look like the same face (the boxes overlap a lot)? */
int facedet_same(const struct camfx_face *old, const struct camfx_face *cur);

#endif

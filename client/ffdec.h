#ifndef MELLAGUI_FFDEC_H
#define MELLAGUI_FFDEC_H

/*
 * Optional decoding through ffmpeg for mellagui-view: videos that its own reader cannot show (H.264, fragmented files, anything) are
 * played from a pipe when ffmpeg is installed. Nothing depends on it.
 *
 * ffdec_info() asks `ffmpeg -i` about the file; ffdec_open() starts `ffmpeg -ss START -i FILE -vf fps=FPS -f rawvideo -pix_fmt bgr0 -`
 * and ffdec_read() collects the pictures (XRGB8888, width * height * 4 bytes, constant frame rate) without ever blocking, so the player
 * decides when to take the next one; while it does not read, ffmpeg waits on the full pipe (pause costs nothing).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ffdec;

struct ffdec_info {
    int w, h;
    uint64_t duration_ms;           /* 0 = unknown */
    char codec[24];                 /* "h264", "hevc" ... */
    bool has_audio;                 /* the file has a sound track */
};

/* false (message in err) when there is no ffmpeg or it finds no video stream in the file. */
bool ffdec_probe(const char *path, struct ffdec_info *info, char *err, size_t errsz);

/* w x h is the size of the pictures that come out; when it differs from the video's own size (src_w x src_h) ffmpeg scales them (much cheaper
 * than the player doing it for every picture). */
struct ffdec *ffdec_open(const char *path, int w, int h, int src_w, int src_h, int fps, uint64_t start_ms, char *err, size_t errsz);
/* Collects bytes of the next picture into buf (w * h * 4 bytes, *have counts what it holds). 1: complete (*have is reset for the next
 * one), 0: not yet, -1: the stream has ended (ffmpeg finished or failed). */
int ffdec_read(struct ffdec *d, void *buf, size_t *have);
/* Waits up to `ms` for more of ffmpeg's data (poll on the pipe), so that a picture that is due is not waited for by sleeping in small steps. */
void ffdec_wait(struct ffdec *d, int ms);
void ffdec_close(struct ffdec *d);

#endif

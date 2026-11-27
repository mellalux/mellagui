#ifndef MELLAGUI_CCTVREC_H
#define MELLAGUI_CCTVREC_H

/*
 * Snapshots and recordings of a network camera, each a job with a thread and a connection of its own to the camera's MAIN stream, so that they have the
 * full picture and go on whatever the window is showing.
 *
 * A snapshot takes the first key frame of the stream and lets ffmpeg make a JPEG of it. A recording passes the video on as the camera made it (no
 * re-encoding) into a fragmented MP4 with the sound as AAC; the times of the pictures are the times they arrived. A lost connection ends the file and
 * a new one is begun when the camera is back; a refused login ends the job (it is not tried again). Files are NAME-YYYYMMDD-HHMMSS.jpg / .mp4,
 * readable by their owner only, and nothing is ever written over.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dvrip.h"

struct cctv_target {
    const char *name, *host, *user, *password;
    int port;
};

enum { CCTV_SNAPSHOT, CCTV_RECORD };
enum { CCTV_STARTING, CCTV_RECORDING, CCTV_DONE, CCTV_FAILED };

struct cctv_status {
    int state;
    int64_t since_ms;                   /* CCTV_RECORDING: when the recording began (the monotonic clock) */
    char file[512];                     /* the file being written, or the last one written ("" = none) */
    char msg[200];                      /* CCTV_FAILED: why; else what is going on */
    unsigned files;                     /* files finished */
    unsigned serial;                    /* goes up with every change */
};

struct cctv_job;

/* `target`, `dir` and `ffmpeg` are copied. NULL when the thread cannot be started. The folder is made when it is not there. */
struct cctv_job *cctv_job_start(const struct cctv_target *target, int kind, const char *dir, const char *ffmpeg);
/* Ask it to end (a recording finishes its file); returns at once. */
void cctv_job_stop(struct cctv_job *j);
void cctv_job_status(struct cctv_job *j, struct cctv_status *out);
/* Stops it, waits for it and frees it. */
void cctv_job_free(struct cctv_job *j);

#endif

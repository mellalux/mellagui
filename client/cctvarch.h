#ifndef MELLAGUI_CCTVARCH_H
#define MELLAGUI_CCTVARCH_H

/*
 * The recordings on a camera's memory card: the list of a day, and one of them fetched into an MP4. Each is a job with a thread and a session of its
 * own. A fetched file is whole or not there: its parts are collected beside it, put together by ffmpeg (the video as it is, the sound as AAC; the
 * pictures are spread evenly over the length of the sound) and only then given its name. Nothing is written over.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cctvrec.h"
#include "dvrip.h"

#define CCTV_DAY_MAX 2000               /* files of one day that are listed */

struct cctv_arch;

/* The files of the day "YYYY-MM-DD" (the camera's clock). */
struct cctv_arch *cctv_list_start(const struct cctv_target *t, const char *day);
/* The file `f` into folder `dir` as BASE.mp4 (BASE-2.mp4 ... when there is one). */
struct cctv_arch *cctv_fetch_start(const struct cctv_target *t, const struct dv_file *f, const char *dir, const char *base, const char *ffmpeg);

struct cctv_arch_status {
    bool done, failed;
    char msg[200];                      /* why it failed */
    uint32_t got_kb, kb;                /* a fetch: how far */
    const struct dv_file *files;        /* a list, once done: valid until the job is freed */
    int n;
    char path[512];                     /* a fetch, once done: the file */
    unsigned serial;
};

void cctv_arch_status(struct cctv_arch *j, struct cctv_arch_status *out);
void cctv_arch_stop(struct cctv_arch *j);
/* Stops it, waits and frees. */
void cctv_arch_free(struct cctv_arch *j);

#endif

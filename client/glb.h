/*
 * glb: a reader for glTF binary files (.glb) with a skinned mesh, one animation and one texture - what the Matrix screensaver's cat needs - and the
 * skinning arithmetic. No library: the JSON is read with json.c, the PNG / JPEG texture with img.c; a WebP texture (what many exporters write: the
 * required extension EXT_texture_webp) is turned into a PNG once by `ffmpeg` and kept in $XDG_CACHE_HOME/mellagui/glb/, so without ffmpeg the model
 * has no texture (the caller paints it a flat colour). Every index, count and offset of the file is checked; a file that does not fit is refused with a
 * reason in `err`.
 *
 * Understood: one mesh (its first primitive, triangles, 16 / 32 bit or no indices), POSITION NORMAL TEXCOORD_0 JOINTS_0 WEIGHTS_0 of the usual
 * component types, one skin (joints, inverse bind matrices), the node hierarchy with TRS or matrix, the first animation (translation / rotation / scale
 * channels, LINEAR and STEP samplers), the first texture. Not understood: sparse accessors, morph targets, cameras, lights, Draco / meshopt.
 *
 * Space: the file's own (after the nodes above the skeleton: a model exported from Blender usually has an armature node that scales it by 0.01); the
 * caller scales it to metres (`glb.bmin` / `bmax` give the size of the rest pose).
 */
#ifndef LM_GLB_H
#define LM_GLB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct glb_node {
    int parent;                         /* -1 = a root */
    char name[40];
    float t[3], r[4], s[3];             /* the rest pose (translation, rotation as x y z w, scale) */
};

struct glb_chan {
    int node;
    int path;                           /* 0 translation, 1 rotation, 2 scale */
    int n;                              /* keys */
    bool step;
    float *time;                        /* n */
    float *val;                         /* n * 3 or n * 4 */
};

struct glb {
    int nv, nt;                         /* vertices, triangles */
    float *pos, *nrm, *uv;              /* the rest pose (bind pose): 3, 3 and 2 floats a vertex */
    uint8_t *jidx;                      /* 4 joint numbers a vertex */
    float *jwt;                         /* 4 weights a vertex (sum 1) */
    uint32_t *idx;                      /* 3 a triangle */
    float bmin[3], bmax[3];             /* the bounds of the rest pose, as skinned with the rest pose (model space) */

    int nn;
    struct glb_node *node;
    int *order;                         /* the nodes, parents before children */
    int nj;
    int *joint;                         /* joint number -> node */
    float *ibm;                         /* nj * 16 (column major, as in the file) */

    int nc;                             /* the first animation */
    struct glb_chan *chan;
    float anim_len;                     /* seconds */

    int tex_w, tex_h;
    uint32_t *tex;                      /* XRGB, or NULL */
    char tex_note[160];                 /* why there is no texture, or where it came from */

    int head_joint, foot_joint, hips_joint;     /* the joints called head, frontleg2 and Hips, or -1 */
    float stride;                       /* how far the body goes in one cycle of the animation (model units), 0 = not known */
    bool has_eyes;
    float eye[2][3];                    /* the eyes in the rest pose (model space): painted on the texture, so they are given, not found */
};

struct glb *glb_load(const char *path, char *err, size_t errsz);
void glb_free(struct glb *g);

/* What the file holds, one line each (mellagui-matrix --glb-info). */
void glb_describe(const struct glb *g, FILE *out);

/* The joint matrices (nj * 16 floats, column major, ready to multiply a rest-pose vertex with) for a pose: the rest pose blended with the animation at
 * `anim_time` seconds (looped) by `amount` 0..1, and the head joint turned by `head_rot` (a 3 x 3 rotation, row major, in model space, about the joint's
 * own place; NULL = not turned). */
void glb_pose(const struct glb *g, float anim_time, float amount, const float *head_rot, float *jm);

/* Skins the vertices [v0, v1) with those matrices: positions and normals (not normalised). Safe for disjoint ranges on several threads. */
void glb_skin(const struct glb *g, const float *jm, int v0, int v1, float *outpos, float *outnrm);

#endif

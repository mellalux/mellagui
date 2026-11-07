#include "gl.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ---- the few EGL / GLES2 declarations we need (no headers on the build machine) ----------------------------------------- */

typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLNativeDisplayType;
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef unsigned char GLubyte;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_EXTENSIONS 0x3055
#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_WINDOW_BIT 0x0004
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_RED_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3024
#define EGL_ALPHA_SIZE 0x3021
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLE_STRIP 0x0005
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_RENDERER 0x1F01
#define GL_EXTENSIONS 0x1F03
#define GL_NO_ERROR 0

struct egl_fns {
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
    EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*Terminate)(EGLDisplay);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*GetError)(void);
    const char *(*QueryString)(EGLDisplay, EGLint);
    void *(*GetProcAddress)(const char *);
    EGLBoolean (*GetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLSurface (*CreatePlatformWindowSurfaceEXT)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
};

/* libgbm: buffers the GPU can render into and KMS can scan out */
#define GBM_BO_USE_SCANOUT (1u << 0)
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_TRANSFER_READ (1u << 0)
#define GBM_FORMAT_XRGB8888 0x34325258u         /* 'X' 'R' '2' '4' */
union gbm_bo_handle { void *ptr; int32_t s32; uint32_t u32; int64_t s64; uint64_t u64; };
struct gbm_fns {
    void *(*create_device)(int);
    void (*device_destroy)(void *);
    void *(*surface_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*surface_destroy)(void *);
    void *(*surface_lock_front_buffer)(void *);
    void (*surface_release_buffer)(void *, void *);
    union gbm_bo_handle (*bo_get_handle)(void *);
    uint32_t (*bo_get_stride)(void *);
    uint32_t (*bo_get_width)(void *);
    uint32_t (*bo_get_height)(void *);
    void *(*bo_map)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **);
    void (*bo_unmap)(void *, void *);
};

struct lm_gl_window {
    void *gbm_surf;
    EGLSurface surf;
    int w, h;
};

struct gl_fns {
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (*UseProgram)(GLuint);
    void (*DeleteProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    GLint (*GetAttribLocation)(GLuint, const GLchar *);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform1f)(GLint, GLfloat);
    void (*Uniform2f)(GLint, GLfloat, GLfloat);
    void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void (*EnableVertexAttribArray)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*ActiveTexture)(GLenum);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*PixelStorei)(GLenum, GLint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*Flush)(void);
    void (*Finish)(void);
    GLenum (*GetError)(void);
    const GLubyte *(*GetString)(GLenum);
};

struct lm_gl {
    void *egl_lib, *gles_lib, *gbm_lib;
    struct gbm_fns gbm;
    void *gbm_dev;                      /* GBM platform: the device the windows are made on */
    EGLConfig win_cfg;
    EGLSurface cur_surf;                /* what is current (EGL_NO_SURFACE: surfaceless) */
    struct egl_fns egl;
    struct gl_fns gl;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog;
    GLint u_size, u_ysign, u_tex, u_color, a_pos, a_uv;
    GLuint white;                       /* 1x1 white texture for colour rectangles */
    bool current;                       /* the context is current on this thread */
    float ysign;                        /* +1 offscreen targets (row 0 at the bottom of GL), -1 window surfaces */
    int tw, th;                         /* size of the bound target */
    char renderer[96];
};

/* ---- loading ------------------------------------------------------------------------------------------------------- */

static void *sym(void *lib, const char *name)
{
    return dlsym(lib, name);
}

#define LOAD_EGL(n) do { l->egl.n = sym(l->egl_lib, "egl" #n); if (!l->egl.n) { why = "libEGL lacks egl" #n; goto fail; } } while (0)
#define LOAD_GL(n) do { l->gl.n = sym(l->gles_lib, "gl" #n); if (!l->gl.n) { why = "libGLESv2 lacks gl" #n; goto fail; } } while (0)

static const char *VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_size;\n"
    "uniform float u_ysign;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 n = a_pos / u_size * 2.0 - 1.0;\n"
    "    gl_Position = vec4(n.x, u_ysign * n.y, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

static const char *FS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv) * u_color;\n"
    "}\n";

static GLuint compile(struct lm_gl *l, GLenum type, const char *src, char *err, size_t errsz)
{
    GLuint s = l->gl.CreateShader(type);
    l->gl.ShaderSource(s, 1, &src, NULL);
    l->gl.CompileShader(s);
    GLint ok = 0;
    l->gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        l->gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
        snprintf(err, errsz, "shader: %s", log);
        l->gl.DeleteShader(s);
        return 0;
    }
    return s;
}

struct lm_gl *lm_gl_create(int gbm_fd, char *err, size_t errsz)
{
    struct lm_gl *l = calloc(1, sizeof(*l));
    const char *why = "out of memory";
    if (!l)
        goto fail;
    l->egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    l->gles_lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!l->egl_lib || !l->gles_lib) {
        why = "libEGL.so.1 / libGLESv2.so.2 not found";
        goto fail;
    }
    LOAD_EGL(GetDisplay); LOAD_EGL(Initialize); LOAD_EGL(Terminate); LOAD_EGL(BindAPI); LOAD_EGL(ChooseConfig);
    LOAD_EGL(CreateContext); LOAD_EGL(DestroyContext); LOAD_EGL(MakeCurrent); LOAD_EGL(GetError); LOAD_EGL(QueryString);
    LOAD_EGL(GetProcAddress); LOAD_EGL(GetConfigs); LOAD_EGL(GetConfigAttrib); LOAD_EGL(DestroySurface); LOAD_EGL(SwapBuffers);
    LOAD_EGL(SwapInterval);
    l->egl.CreatePlatformWindowSurfaceEXT = (void *)l->egl.GetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    l->egl.CreateWindowSurface = sym(l->egl_lib, "eglCreateWindowSurface");
    l->egl.GetPlatformDisplayEXT = (void *)l->egl.GetProcAddress("eglGetPlatformDisplayEXT");
    LOAD_GL(CreateShader); LOAD_GL(ShaderSource); LOAD_GL(CompileShader); LOAD_GL(GetShaderiv); LOAD_GL(GetShaderInfoLog);
    LOAD_GL(DeleteShader); LOAD_GL(CreateProgram); LOAD_GL(AttachShader); LOAD_GL(LinkProgram); LOAD_GL(GetProgramiv);
    LOAD_GL(GetProgramInfoLog); LOAD_GL(UseProgram); LOAD_GL(DeleteProgram); LOAD_GL(GetUniformLocation);
    LOAD_GL(GetAttribLocation); LOAD_GL(BindAttribLocation); LOAD_GL(Uniform1i); LOAD_GL(Uniform1f); LOAD_GL(Uniform2f);
    LOAD_GL(Uniform4f); LOAD_GL(EnableVertexAttribArray); LOAD_GL(VertexAttribPointer); LOAD_GL(DrawArrays);
    LOAD_GL(GenTextures); LOAD_GL(DeleteTextures); LOAD_GL(BindTexture); LOAD_GL(ActiveTexture); LOAD_GL(TexParameteri);
    LOAD_GL(TexImage2D); LOAD_GL(TexSubImage2D); LOAD_GL(PixelStorei); LOAD_GL(GenFramebuffers); LOAD_GL(DeleteFramebuffers);
    LOAD_GL(BindFramebuffer); LOAD_GL(FramebufferTexture2D); LOAD_GL(CheckFramebufferStatus); LOAD_GL(Viewport);
    LOAD_GL(Scissor); LOAD_GL(Enable); LOAD_GL(Disable); LOAD_GL(BlendFunc); LOAD_GL(ClearColor); LOAD_GL(Clear);
    LOAD_GL(ReadPixels); LOAD_GL(Flush); LOAD_GL(Finish); LOAD_GL(GetError); LOAD_GL(GetString);

    if (gbm_fd >= 0) {
        l->gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!l->gbm_lib) {
            why = "libgbm.so.1 not found";
            goto fail;
        }
#define LOAD_GBM(f, n) do { l->gbm.f = sym(l->gbm_lib, "gbm_" n); if (!l->gbm.f) { why = "libgbm lacks gbm_" n; goto fail; } } while (0)
        LOAD_GBM(create_device, "create_device"); LOAD_GBM(device_destroy, "device_destroy");
        LOAD_GBM(surface_create, "surface_create"); LOAD_GBM(surface_destroy, "surface_destroy");
        LOAD_GBM(surface_lock_front_buffer, "surface_lock_front_buffer"); LOAD_GBM(surface_release_buffer, "surface_release_buffer");
        LOAD_GBM(bo_get_handle, "bo_get_handle"); LOAD_GBM(bo_get_stride, "bo_get_stride");
        LOAD_GBM(bo_get_width, "bo_get_width"); LOAD_GBM(bo_get_height, "bo_get_height");
        LOAD_GBM(bo_map, "bo_map"); LOAD_GBM(bo_unmap, "bo_unmap");
        l->gbm_dev = l->gbm.create_device(gbm_fd);
        if (!l->gbm_dev || !l->egl.GetPlatformDisplayEXT || !l->egl.CreatePlatformWindowSurfaceEXT) {
            why = "no GBM device or EGL_EXT_platform_base";
            goto fail;
        }
        l->dpy = l->egl.GetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, l->gbm_dev, NULL);
    } else {
        if (l->egl.GetPlatformDisplayEXT)
            l->dpy = l->egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (!l->dpy)
            l->dpy = l->egl.GetDisplay(EGL_DEFAULT_DISPLAY);
    }
    EGLint major, minor;
    if (!l->dpy || !l->egl.Initialize(l->dpy, &major, &minor)) {
        why = "eglInitialize failed (no GPU / driver?)";
        l->dpy = NULL;
        goto fail;
    }
    if (!l->egl.BindAPI(EGL_OPENGL_ES_API)) {
        why = "no OpenGL ES API";
        goto fail;
    }
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (gbm_fd >= 0) {                                  /* a window config whose native format is XRGB8888 */
        const EGLint attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                                EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
        EGLConfig all[64];
        if (!l->egl.ChooseConfig(l->dpy, attr, all, 64, &ncfg) || ncfg < 1) {
            why = "no OpenGL ES 2 window config";
            goto fail;
        }
        for (int i = 0; i < ncfg; i++) {
            EGLint id = 0, alpha = 0;
            l->egl.GetConfigAttrib(l->dpy, all[i], EGL_NATIVE_VISUAL_ID, &id);
            l->egl.GetConfigAttrib(l->dpy, all[i], EGL_ALPHA_SIZE, &alpha);
            if ((uint32_t)id == GBM_FORMAT_XRGB8888 && alpha == 0) {
                cfg = all[i];
                break;
            }
        }
        if (!cfg) {
            why = "no EGL config for XRGB8888 GBM buffers";
            goto fail;
        }
        l->win_cfg = cfg;
    } else {
        const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
        if (!l->egl.ChooseConfig(l->dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
            why = "no OpenGL ES 2 config";
            goto fail;
        }
    }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    l->ctx = l->egl.CreateContext(l->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (l->ctx == EGL_NO_CONTEXT) {
        why = "cannot create an OpenGL ES 2 context";
        goto fail;
    }
    if (!l->egl.MakeCurrent(l->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, l->ctx)) {
        why = "cannot make the context current without a surface (EGL_KHR_surfaceless_context)";
        goto fail;
    }
    l->current = true;
    l->cur_surf = EGL_NO_SURFACE;
    const char *exts = (const char *)l->gl.GetString(GL_EXTENSIONS);
    if (!exts || !strstr(exts, "GL_EXT_texture_format_BGRA8888")) {
        why = "GL_EXT_texture_format_BGRA8888 is missing";
        goto fail;
    }
    snprintf(l->renderer, sizeof(l->renderer), "%s", (const char *)l->gl.GetString(GL_RENDERER));

    char serr[300];
    GLuint vs = compile(l, GL_VERTEX_SHADER, VS, serr, sizeof(serr));
    GLuint fs = vs ? compile(l, GL_FRAGMENT_SHADER, FS, serr, sizeof(serr)) : 0;
    if (!vs || !fs) {
        if (err && errsz)
            snprintf(err, errsz, "%s", serr);
        why = NULL;
        goto fail_msg;
    }
    l->prog = l->gl.CreateProgram();
    l->gl.AttachShader(l->prog, vs);
    l->gl.AttachShader(l->prog, fs);
    l->gl.BindAttribLocation(l->prog, 0, "a_pos");
    l->gl.BindAttribLocation(l->prog, 1, "a_uv");
    l->gl.LinkProgram(l->prog);
    l->gl.DeleteShader(vs);
    l->gl.DeleteShader(fs);
    GLint ok = 0;
    l->gl.GetProgramiv(l->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        why = "shader link failed";
        goto fail;
    }
    l->u_size = l->gl.GetUniformLocation(l->prog, "u_size");
    l->u_ysign = l->gl.GetUniformLocation(l->prog, "u_ysign");
    l->u_tex = l->gl.GetUniformLocation(l->prog, "u_tex");
    l->u_color = l->gl.GetUniformLocation(l->prog, "u_color");
    l->gl.UseProgram(l->prog);
    l->gl.Uniform1i(l->u_tex, 0);
    l->gl.EnableVertexAttribArray(0);
    l->gl.EnableVertexAttribArray(1);
    l->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const uint32_t white = 0xffffffffu;
    l->white = lm_gl_tex_create(l, 1, 1, &white);
    l->ysign = 1.0f;
    return l;

fail:
    if (err && errsz && why)
        snprintf(err, errsz, "%s", why);
fail_msg:
    lm_gl_destroy(l);
    return NULL;
}

void lm_gl_destroy(struct lm_gl *l)
{
    if (!l)
        return;
    if (l->dpy && l->egl.MakeCurrent) {
        if (l->white && l->current)
            lm_gl_tex_delete(l, l->white);
        if (l->prog)
            l->gl.DeleteProgram(l->prog);
        l->egl.MakeCurrent(l->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (l->ctx != EGL_NO_CONTEXT && l->ctx)
            l->egl.DestroyContext(l->dpy, l->ctx);
        l->egl.Terminate(l->dpy);
    }
    if (l->gbm_dev)
        l->gbm.device_destroy(l->gbm_dev);
    /* the libraries stay loaded: Mesa keeps its EGL display and driver state until the process ends, and unloading a GL
     * driver is not safe */
    free(l);
}

const char *lm_gl_renderer_name(const struct lm_gl *l)
{
    return l->renderer;
}

bool lm_gl_make_current(struct lm_gl *l)
{
    if (l->current)
        return true;
    l->current = l->egl.MakeCurrent(l->dpy, l->cur_surf, l->cur_surf, l->ctx);
    return l->current;
}

/* ---- textures ------------------------------------------------------------------------------------------------------- */

unsigned lm_gl_tex_create(struct lm_gl *l, int w, int h, const void *bgra)
{
    GLuint t = 0;
    l->gl.GenTextures(1, &t);
    if (!t)
        return 0;
    l->gl.BindTexture(GL_TEXTURE_2D, t);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    l->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
    return t;
}

void lm_gl_tex_update(struct lm_gl *l, unsigned tex, int x, int y, int w, int h, const void *src, int stride_px)
{
    l->gl.BindTexture(GL_TEXTURE_2D, tex);
    /* GLES2 has no GL_UNPACK_ROW_LENGTH: a rectangle of a wider image goes up one row at a time */
    const uint8_t *p = src;
    if (stride_px == w) {
        l->gl.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, p);
        return;
    }
    for (int r = 0; r < h; r++)
        l->gl.TexSubImage2D(GL_TEXTURE_2D, 0, x, y + r, w, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, p + (size_t)r * (size_t)stride_px * 4);
}

void lm_gl_tex_delete(struct lm_gl *l, unsigned tex)
{
    GLuint t = tex;
    if (t)
        l->gl.DeleteTextures(1, &t);
}

/* ---- offscreen target ----------------------------------------------------------------------------------------------- */

bool lm_gl_target_create(struct lm_gl *l, int w, int h, struct lm_gl_target *t)
{
    memset(t, 0, sizeof(*t));
    GLuint tex = 0, fbo = 0;
    l->gl.GenTextures(1, &tex);
    l->gl.BindTexture(GL_TEXTURE_2D, tex);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    l->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    l->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    l->gl.GenFramebuffers(1, &fbo);
    l->gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    l->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (l->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        l->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        l->gl.DeleteFramebuffers(1, &fbo);
        l->gl.DeleteTextures(1, &tex);
        return false;
    }
    t->fbo = fbo;
    t->tex = tex;
    t->w = w;
    t->h = h;
    return true;
}

void lm_gl_target_destroy(struct lm_gl *l, struct lm_gl_target *t)
{
    GLuint fbo = t->fbo, tex = t->tex;
    if (fbo)
        l->gl.DeleteFramebuffers(1, &fbo);
    if (tex)
        l->gl.DeleteTextures(1, &tex);
    memset(t, 0, sizeof(*t));
}

void lm_gl_bind_target(struct lm_gl *l, const struct lm_gl_target *t, int w, int h)
{
    l->gl.BindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo : 0);
    l->ysign = t ? 1.0f : -1.0f;
    l->tw = w;
    l->th = h;
    l->gl.Viewport(0, 0, w, h);
    l->gl.UseProgram(l->prog);
    l->gl.Uniform2f(l->u_size, (float)w, (float)h);
    l->gl.Uniform1f(l->u_ysign, l->ysign);
}

bool lm_gl_readback(struct lm_gl *l, const struct lm_gl_target *t, uint32_t *dst, size_t stride)
{
    size_t n = (size_t)t->w * (size_t)t->h;
    uint8_t *tmp = malloc(n * 4);
    if (!tmp)
        return false;
    l->gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    l->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    l->gl.ReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < t->h; y++) {
        uint32_t *row = (uint32_t *)(void *)((uint8_t *)dst + (size_t)y * stride);
        const uint8_t *s = tmp + (size_t)y * (size_t)t->w * 4;
        for (int x = 0; x < t->w; x++)
            row[x] = (uint32_t)s[x * 4] << 16 | (uint32_t)s[x * 4 + 1] << 8 | s[x * 4 + 2];
    }
    free(tmp);
    return true;
}

/* ---- window surfaces ------------------------------------------------------------------------------------------------- */

struct lm_gl_window *lm_gl_window_create(struct lm_gl *l, int w, int h, bool scanout)
{
    if (!l->gbm_dev)
        return NULL;
    struct lm_gl_window *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    win->gbm_surf = l->gbm.surface_create(l->gbm_dev, (uint32_t)w, (uint32_t)h, GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_RENDERING | (scanout ? GBM_BO_USE_SCANOUT : 0));
    if (!win->gbm_surf) {
        free(win);
        return NULL;
    }
    win->surf = l->egl.CreatePlatformWindowSurfaceEXT(l->dpy, l->win_cfg, win->gbm_surf, NULL);
    if (win->surf == EGL_NO_SURFACE) {
        l->gbm.surface_destroy(win->gbm_surf);
        free(win);
        return NULL;
    }
    win->w = w;
    win->h = h;
    return win;
}

void lm_gl_window_destroy(struct lm_gl *l, struct lm_gl_window *win)
{
    if (!win)
        return;
    if (l->cur_surf == win->surf) {                     /* back to surfaceless before the surface goes */
        l->egl.MakeCurrent(l->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, l->ctx);
        l->cur_surf = EGL_NO_SURFACE;
        l->current = true;
    }
    l->egl.DestroySurface(l->dpy, win->surf);
    l->gbm.surface_destroy(win->gbm_surf);
    free(win);
}

bool lm_gl_window_bind(struct lm_gl *l, struct lm_gl_window *win)
{
    if (!l->current || l->cur_surf != win->surf) {
        if (!l->egl.MakeCurrent(l->dpy, win->surf, win->surf, l->ctx)) {
            l->current = false;
            return false;
        }
        l->cur_surf = win->surf;
        l->current = true;
        l->egl.SwapInterval(l->dpy, 0);                 /* the page flip events pace the frames, not eglSwapBuffers */
    }
    lm_gl_bind_target(l, NULL, win->w, win->h);
    return true;
}

bool lm_gl_window_swap(struct lm_gl *l, struct lm_gl_window *win, struct lm_gl_bo *bo)
{
    if (!lm_gl_window_bind(l, win) || !l->egl.SwapBuffers(l->dpy, win->surf))
        return false;
    void *b = l->gbm.surface_lock_front_buffer(win->gbm_surf);
    if (!b)
        return false;
    bo->handle = l->gbm.bo_get_handle(b).u32;
    bo->stride = l->gbm.bo_get_stride(b);
    bo->w = (int)l->gbm.bo_get_width(b);
    bo->h = (int)l->gbm.bo_get_height(b);
    bo->priv = b;
    return true;
}

void lm_gl_window_release(struct lm_gl *l, struct lm_gl_window *win, struct lm_gl_bo *bo)
{
    if (bo->priv)
        l->gbm.surface_release_buffer(win->gbm_surf, bo->priv);
    bo->priv = NULL;
}

bool lm_gl_bo_read(struct lm_gl *l, const struct lm_gl_bo *bo, uint32_t *dst, size_t stride)
{
    uint32_t map_stride = 0;
    void *map_data = NULL;
    uint8_t *p = l->gbm.bo_map(bo->priv, 0, 0, (uint32_t)bo->w, (uint32_t)bo->h, GBM_BO_TRANSFER_READ, &map_stride, &map_data);
    if (!p)
        return false;
    for (int y = 0; y < bo->h; y++) {
        const uint32_t *s = (const uint32_t *)(const void *)(p + (size_t)y * map_stride);
        uint32_t *d = (uint32_t *)(void *)((uint8_t *)dst + (size_t)y * stride);
        for (int x = 0; x < bo->w; x++)
            d[x] = s[x] & 0x00ffffffu;
    }
    l->gbm.bo_unmap(bo->priv, map_data);
    return true;
}

/* ---- drawing -------------------------------------------------------------------------------------------------------- */

void lm_gl_clear(struct lm_gl *l, float r, float g, float b, float a)
{
    l->gl.Disable(GL_SCISSOR_TEST);
    l->gl.ClearColor(r, g, b, a);
    l->gl.Clear(GL_COLOR_BUFFER_BIT);
}

void lm_gl_scissor(struct lm_gl *l, bool on, int x, int y, int w, int h)
{
    if (!on) {
        l->gl.Disable(GL_SCISSOR_TEST);
        return;
    }
    l->gl.Enable(GL_SCISSOR_TEST);
    /* GL's scissor origin is the bottom left; offscreen targets are stored top row first from the bottom (ysign +1) */
    int gy = l->ysign > 0 ? y : l->th - (y + h);
    l->gl.Scissor(x, gy, w, h);
}

static void quad(struct lm_gl *l, int x, int y, int w, int h, float u0, float v0, float u1, float v1)
{
    const GLfloat pos[8] = { (float)x, (float)y, (float)(x + w), (float)y, (float)x, (float)(y + h), (float)(x + w), (float)(y + h) };
    const GLfloat uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    l->gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    l->gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    l->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void lm_gl_draw_tex(struct lm_gl *l, unsigned tex, int x, int y, int w, int h, float u0, float v0, float u1, float v1,
                    float alpha, bool opaque)
{
    if (opaque)
        l->gl.Disable(GL_BLEND);
    else {
        l->gl.Enable(GL_BLEND);
        l->gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    l->gl.ActiveTexture(GL_TEXTURE0);
    l->gl.BindTexture(GL_TEXTURE_2D, tex);
    l->gl.Uniform4f(l->u_color, alpha, alpha, alpha, alpha);
    quad(l, x, y, w, h, u0, v0, u1, v1);
}

void lm_gl_draw_rect(struct lm_gl *l, int x, int y, int w, int h, float r, float g, float b, float a)
{
    l->gl.Enable(GL_BLEND);
    l->gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    l->gl.ActiveTexture(GL_TEXTURE0);
    l->gl.BindTexture(GL_TEXTURE_2D, l->white);
    l->gl.Uniform4f(l->u_color, r, g, b, a);
    quad(l, x, y, w, h, 0, 0, 1, 1);
}

void lm_gl_flush(struct lm_gl *l) { l->gl.Flush(); }
void lm_gl_finish(struct lm_gl *l) { l->gl.Finish(); }

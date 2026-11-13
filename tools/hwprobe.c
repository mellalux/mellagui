/* hwprobe - what the display hardware says, read only (docs/HARDWARE.md). It opens the card without becoming DRM master, so it can run on another VT
 * or over ssh while mellagui owns the screen. For every connector: status, the current mode, the modes, DPMS; for every CRTC in use: the gamma
 * table's size and its end points (the night light scales that table: with it on, blue's last value is lower than red's).
 *
 *   build/hwprobe [/dev/dri/cardN]       the first card with connectors when none is named
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *conn_name(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    default: return "other";
    }
}

static int open_card(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd;
}

int main(int argc, char **argv)
{
    char path[64];
    int fd = -1;
    if (argc > 1) {
        snprintf(path, sizeof(path), "%s", argv[1]);
        fd = open_card(path);
    } else {
        for (int i = 0; i < 8 && fd < 0; i++) {
            snprintf(path, sizeof(path), "/dev/dri/card%d", i);
            int f = open_card(path);
            if (f < 0)
                continue;
            drmModeRes *r = drmModeGetResources(f);
            if (r && r->count_connectors > 0)
                fd = f;
            else
                close(f);
            if (r)
                drmModeFreeResources(r);
        }
    }
    if (fd < 0) {
        fprintf(stderr, "hwprobe: no card to open (%s); are you in the video group?\n", strerror(errno));
        return 1;
    }
    drmVersionPtr v = drmGetVersion(fd);
    if (v) {
        printf("card %s driver=%s %d.%d.%d\n", path, v->name, v->version_major, v->version_minor, v->version_patchlevel);
        drmFreeVersion(v);
    }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "hwprobe: no mode setting resources: %s\n", strerror(errno));
        return 1;
    }
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        printf("connector %s-%u %s", conn_name(c->connector_type), c->connector_type_id,
               c->connection == DRM_MODE_CONNECTED ? "connected" : c->connection == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown");
        if (c->connection == DRM_MODE_CONNECTED)
            printf(" %ux%u mm, %d modes", c->mmWidth, c->mmHeight, c->count_modes);
        for (int p = 0; p < c->count_props; p++) {
            drmModePropertyRes *pr = drmModeGetProperty(fd, c->props[p]);
            if (pr && !strcmp(pr->name, "DPMS"))
                printf(" dpms=%s", c->prop_values[p] == 0 ? "on" : c->prop_values[p] == 3 ? "off" : "standby/suspend");
            if (pr)
                drmModeFreeProperty(pr);
        }
        printf("\n");
        for (int m = 0; m < c->count_modes && m < 6; m++)
            printf("    mode %s %uHz%s\n", c->modes[m].name, c->modes[m].vrefresh, (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)" : "");
        drmModeFreeConnector(c);
    }
    for (int i = 0; i < res->count_crtcs; i++) {
        drmModeCrtc *cr = drmModeGetCrtc(fd, res->crtcs[i]);
        if (!cr)
            continue;
        printf("crtc %u %s", cr->crtc_id, cr->mode_valid ? cr->mode.name : "(off)");
        if (cr->mode_valid)
            printf(" %uHz fb=%u", cr->mode.vrefresh, cr->buffer_id);
        printf(" gamma_size=%d", cr->gamma_size);
        if (cr->mode_valid && cr->gamma_size > 1) {
            int n = cr->gamma_size;
            uint16_t *r = calloc((size_t)n, 2), *g = calloc((size_t)n, 2), *b = calloc((size_t)n, 2);
            if (r && g && b && drmModeCrtcGetGamma(fd, cr->crtc_id, (uint32_t)n, r, g, b) == 0) {
                printf(" last(r,g,b)=%u,%u,%u mid(r,g,b)=%u,%u,%u", r[n - 1], g[n - 1], b[n - 1], r[n / 2], g[n / 2], b[n / 2]);
                printf(" -> %s", (r[n - 1] == g[n - 1] && g[n - 1] == b[n - 1]) ? "neutral" : "TINTED (night light / colour change)");
            } else {
                printf(" (cannot read it: %s)", strerror(errno));
            }
            free(r); free(g); free(b);
        }
        printf("\n");
        drmModeFreeCrtc(cr);
    }
    drmModeFreeResources(res);
    close(fd);
    return 0;
}

/*
 * kmswake.c - One-shot: activate a CRTC/mode on a connector so that
 * kmssink (which requires an active encoder->crtc link) can start.
 *
 * Sets connector's preferred mode on the given CRTC with a black dumb
 * buffer on the given primary plane, then exits without restoring.
 * The kernel keeps crtc->enabled and encoder->crtc linkage; the fb is
 * reaped on close (screen goes black), which is exactly what we want.
 *
 * Build: gcc -O2 -o kmswake kmswake.c $(pkg-config --cflags --libs libdrm)
 * Usage: kmswake [card] [conn_id] [crtc_id] [plane_id]
 * Defaults: /dev/dri/card0 172 85 57
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(int argc, char **argv)
{
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
    int conn_id = argc > 2 ? atoi(argv[2]) : 172;
    int crtc_id = argc > 3 ? atoi(argv[3]) : 85;
    int plane_id = argc > 4 ? atoi(argv[4]) : 57;
    int fd, ret = 1;

    fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card"); return 1; }

    if (drmSetMaster(fd) != 0)
        perror("drmSetMaster (continuing anyway)");

    drmModeConnector *conn = drmModeGetConnector(fd, conn_id);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes < 1) {
        fprintf(stderr, "connector %d not connected or no modes\n", conn_id);
        goto out;
    }
    drmModeModeInfo mode = conn->modes[0]; /* preferred */
    printf("mode: %s %dx%d@%d\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);

    /* dumb buffer, filled black */
    struct drm_mode_create_dumb creq = {0};
    creq.width = mode.hdisplay;
    creq.height = mode.vdisplay;
    creq.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        perror("CREATE_DUMB"); goto out;
    }
    uint32_t fb;
    if (drmModeAddFB(fd, creq.width, creq.height, 24, 32, creq.pitch,
                     creq.handle, &fb) != 0) {
        perror("AddFB"); goto out;
    }
    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
        void *map = mmap(0, creq.size, PROT_WRITE, MAP_SHARED, fd, mreq.offset);
        if (map != MAP_FAILED) { memset(map, 0, creq.size); munmap(map, creq.size); }
    }

    if (drmModeSetCrtc(fd, crtc_id, fb, 0, 0, (uint32_t *)&conn_id, 1, &mode) != 0) {
        perror("SetCrtc"); goto out;
    }
    if (drmModeSetPlane(fd, plane_id, crtc_id, fb, 0,
                        0, 0, mode.hdisplay, mode.vdisplay,
                        0, 0, mode.hdisplay << 16, mode.vdisplay << 16) != 0) {
        perror("SetPlane"); goto out;
    }

    printf("crtc %d activated on connector %d with plane %d\n",
           crtc_id, conn_id, plane_id);
    ret = 0;
out:
    if (conn) drmModeFreeConnector(conn);
    close(fd);
    return ret;
}

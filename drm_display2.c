#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* 256-entry hue palette, precomputed once */
static uint32_t palette[256];

static void build_palette(void) {
    for (int i = 0; i < 256; i++) {
        int h = i * 6;          /* 0..1535 */
        int s = h % 256;
        uint8_t r, g, b;
        switch (h / 256) {
            case 0: r=255;    g=s;      b=0;      break;
            case 1: r=255-s;  g=255;    b=0;      break;
            case 2: r=0;      g=255;    b=s;      break;
            case 3: r=0;      g=255-s;  b=255;    break;
            case 4: r=s;      g=0;      b=255;    break;
            default:r=255;    g=0;      b=255-s;  break;
        }
        palette[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

static void paint_frame(uint32_t *buf, uint32_t w, uint32_t h,
                        uint32_t pitch_px, int frame) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t base = (uint8_t)(y + frame * 2);
        uint32_t *row = buf + y * pitch_px;
        for (uint32_t x = 0; x < w; x++)
            row[x] = palette[(uint8_t)(base + x)];
    }
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    if (drmSetMaster(fd) < 0)
        fprintf(stderr, "warn: drmSetMaster: %s\n", strerror(errno));

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); return 1; }

    /* Find first non-eDP connected connector */
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED &&
            c->connector_type != DRM_MODE_CONNECTOR_eDP &&
            c->count_modes > 0)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no external connector\n"); return 1; }

    drmModeModeInfo mode = conn->modes[0];
    printf("Connector %u  encoder %u  mode %dx%d@%d\n",
           conn->connector_id, conn->encoder_id,
           mode.hdisplay, mode.vdisplay, mode.vrefresh);

    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : 0;
    if (enc) drmModeFreeEncoder(enc);
    printf("CRTC %u\n", crtc_id);

    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) { perror("create_dumb"); return 1; }

    uint32_t fb_id;
    if (drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32,
                     creq.pitch, creq.handle, &fb_id)) { perror("AddFB"); return 1; }

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) { perror("map_dumb"); return 1; }

    uint32_t *fb = mmap(NULL, creq.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }

    build_palette();

    paint_frame(fb, mode.hdisplay, mode.vdisplay, creq.pitch / 4, 0);
    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    double start = now_s(), last_report = start;
    int frame = 0, reported = 0;
    while (now_s() - start < 3.0) {
        paint_frame(fb, mode.hdisplay, mode.vdisplay, creq.pitch / 4, frame++);
        double t = now_s();
        if (!reported && t - last_report >= 1.0) {
            printf("~%.0f fps (after 1s)\n", frame / (t - start));
            fflush(stdout);
            reported = 1;
        }
    }
    printf("%d frames in 3s = %.1f fps\n", frame, frame / 3.0);

    drmModeSetCrtc(fd, crtc_id, 0, 0, 0, NULL, 0, NULL);
    munmap(fb, creq.size);
    drmModeRmFB(fd, fb_id);
    struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}

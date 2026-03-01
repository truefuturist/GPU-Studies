/* Raw DRM ioctls — no libdrm */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void paint(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch_px, int frame) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = fb + y * pitch_px;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t t = (uint8_t)(x * 255 / w + frame);
            /* three channels offset by 120 degrees of the 256-cycle */
            row[x] = (uint32_t)(t)         << 16  /* R */
                   | (uint32_t)(t + 85u)   <<  8  /* G */
                   | (uint32_t)(t + 170u);         /* B */
        }
    }
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    /* --- get connector/encoder/crtc IDs --- */
    uint32_t conn_ids[8]={0}, crtc_ids[8]={0}, fb_ids[8]={0}, enc_ids[32]={0};
    struct drm_mode_card_res res = {
        .fb_id_ptr        = (uint64_t)(uintptr_t)fb_ids,
        .crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids,
        .connector_id_ptr = (uint64_t)(uintptr_t)conn_ids,
        .encoder_id_ptr   = (uint64_t)(uintptr_t)enc_ids,
        .count_fbs=8, .count_crtcs=8, .count_connectors=8, .count_encoders=32,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return 1; }

    uint32_t conn_id = 0, enc_id = 0;
    struct drm_mode_modeinfo mode = {0};

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_modeinfo modes[32];
        uint32_t encoders[8];
        struct drm_mode_get_connector c = {
            .connector_id   = conn_ids[i],
            .modes_ptr      = (uint64_t)(uintptr_t)modes,
            .count_modes    = 32,
            .encoders_ptr   = (uint64_t)(uintptr_t)encoders,
            .count_encoders = 8,
        };
        ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        /* DRM_MODE_CONNECTOR_eDP == 14 */
        if (c.connection == 1 && c.connector_type != 14 && c.count_modes) {
            conn_id = conn_ids[i];
            enc_id  = c.encoder_id;
            mode    = modes[0];
            break;
        }
    }
    if (!conn_id) { fputs("no connector\n", stderr); return 1; }

    struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
    ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc);
    uint32_t crtc_id = enc.crtc_id;

    printf("conn %u  enc %u  crtc %u  %ux%u@%u\n",
           conn_id, enc_id, crtc_id, mode.hdisplay, mode.vdisplay, mode.vrefresh);

    /* --- allocate dumb buffer --- */
    struct drm_mode_create_dumb db = {
        .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32
    };
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &db);

    /* --- register framebuffer --- */
    struct drm_mode_fb_cmd fb_cmd = {
        .width  = mode.hdisplay, .height = mode.vdisplay,
        .pitch  = db.pitch, .bpp = 32, .depth = 24,
        .handle = db.handle,
    };
    ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd);

    /* --- mmap --- */
    struct drm_mode_map_dumb md = { .handle = db.handle };
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint32_t *fb = mmap(NULL, db.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);

    /* --- set crtc --- */
    struct drm_mode_crtc sc = {
        .crtc_id        = crtc_id,
        .fb_id          = fb_cmd.fb_id,
        .set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id,
        .count_connectors   = 1,
        .mode           = mode,
        .mode_valid     = 1,
    };
    ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc);

    /* --- animate --- */
    double start = now(); int frame = 0;
    while (now() - start < 3.0)
        paint(fb, mode.hdisplay, mode.vdisplay, db.pitch / 4, frame++);
    printf("%d frames = %.0f fps\n", frame, frame / 3.0);

    /* --- cleanup --- */
    struct drm_mode_crtc off = { .crtc_id = crtc_id };
    ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &off);
    munmap(fb, db.size);
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_cmd.fb_id);
    struct drm_mode_destroy_dumb dd = { .handle = db.handle };
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    return 0;
}

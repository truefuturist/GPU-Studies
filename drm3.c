/*
 * Writes animation directly through the GPU's PCI aperture.
 *
 * DRM is used only for the one-time modeset (create buffer, register fb,
 * set CRTC).  After that we:
 *   1. Read the PLANE_SURF display register via mmap of PCI BAR0 (MMIO).
 *   2. mmap PCI BAR2 (GTT aperture) at the address PLANE_SURF points to.
 *   3. Write pixels directly into the scanout buffer through the aperture.
 *
 * No libdrm.  Animation path touches no DRM code at all.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* Intel Gen12 (Alder Lake) primary-plane surface address registers.
   PLANE_SURF holds the GTT address the display engine scans out from. */
#define PLANE_SURF_A  0x7019C   /* pipe A, plane 1 */
#define PLANE_SURF_B  0x7119C   /* pipe B */
#define PLANE_SURF_C  0x7219C   /* pipe C */
#define PLANE_SURF_D  0x7319C   /* pipe D */

#define PCI     "/sys/bus/pci/devices/0000:00:02.0"
#define MMIO_SZ (16u << 20)     /* BAR0: 16 MB of registers */
#define GTT_SZ  (256u << 20)    /* BAR2: 256 MB aperture    */

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void paint(uint32_t *fb, uint32_t w, uint32_t h,
                  uint32_t pitch_px, int frame) {
    for (uint32_t y = 0; y < h; y++) {
        /* one color per row, scrolls vertically */
        uint8_t v = (uint8_t)(y + frame);
        uint32_t c = (uint32_t)v << 16 | (uint32_t)(v+85u) << 8 | (uint32_t)(v+170u);
        uint32_t *row = fb + y * pitch_px;
        for (uint32_t x = 0; x < w; x++) row[x] = c;
    }
}

int main(void) {
    /* ── DRM: modeset only ───────────────────────────────────────────── */
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

    uint32_t conn_ids[8]={}, crtc_ids[8]={}, fb_ids[8]={}, enc_ids[32]={};
    struct drm_mode_card_res res = {
        .fb_id_ptr=(uint64_t)(uintptr_t)fb_ids,
        .crtc_id_ptr=(uint64_t)(uintptr_t)crtc_ids,
        .connector_id_ptr=(uint64_t)(uintptr_t)conn_ids,
        .encoder_id_ptr=(uint64_t)(uintptr_t)enc_ids,
        .count_fbs=8,.count_crtcs=8,.count_connectors=8,.count_encoders=32,
    };
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    uint32_t conn_id=0, enc_id=0;
    struct drm_mode_modeinfo mode={};
    for (uint32_t i=0; i<res.count_connectors; i++) {
        struct drm_mode_modeinfo modes[32]; uint32_t encs[8];
        struct drm_mode_get_connector c={
            .connector_id=conn_ids[i],
            .modes_ptr=(uint64_t)(uintptr_t)modes,.count_modes=32,
            .encoders_ptr=(uint64_t)(uintptr_t)encs,.count_encoders=8,
        };
        ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        if (c.connection==1 && c.connector_type!=14 && c.count_modes) {
            conn_id=conn_ids[i]; enc_id=c.encoder_id; mode=modes[0]; break;
        }
    }

    struct drm_mode_get_encoder enc={.encoder_id=enc_id};
    ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc);
    uint32_t crtc_id = enc.crtc_id;

    struct drm_mode_create_dumb db={
        .width=mode.hdisplay,.height=mode.vdisplay,.bpp=32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &db);

    struct drm_mode_fb_cmd fb_cmd={
        .width=mode.hdisplay,.height=mode.vdisplay,
        .pitch=db.pitch,.bpp=32,.depth=24,.handle=db.handle};
    ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd);

    struct drm_mode_crtc sc={
        .crtc_id=crtc_id,.fb_id=fb_cmd.fb_id,
        .set_connectors_ptr=(uint64_t)(uintptr_t)&conn_id,.count_connectors=1,
        .mode=mode,.mode_valid=1};
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("SETCRTC"); return 1; }
    printf("conn %u  crtc %u  %ux%u\n",
           conn_id, crtc_id, mode.hdisplay, mode.vdisplay);

    /* ── MMIO: read hardware register to find scanout address ────────── */
    int mmio_fd = open(PCI "/resource0", O_RDONLY);
    if (mmio_fd < 0) { perror("resource0"); return 1; }
    volatile uint32_t *mmio = mmap(NULL, MMIO_SZ, PROT_READ,
                                   MAP_SHARED, mmio_fd, 0);
    if (mmio == MAP_FAILED) { perror("mmap mmio"); return 1; }

    /* Find which pipe is scanning our buffer by matching PLANE_SURF */
    uint32_t surfs[4] = {
        mmio[PLANE_SURF_A/4], mmio[PLANE_SURF_B/4],
        mmio[PLANE_SURF_C/4], mmio[PLANE_SURF_D/4],
    };
    printf("PLANE_SURF  A:%08x  B:%08x  C:%08x  D:%08x\n",
           surfs[0], surfs[1], surfs[2], surfs[3]);

    /* GTT address = PLANE_SURF with lower 12 bits masked off */
    uint64_t gtt_addr = 0;
    for (int i=0; i<4; i++)
        if (surfs[i] & ~0xFFFu) { gtt_addr = surfs[i] & ~0xFFFu; }
    printf("Using GTT addr: 0x%lx\n", gtt_addr);

    /* ── GTT aperture: map framebuffer via PCI BAR2 directly ─────────── */
    int gtt_fd = open(PCI "/resource2", O_RDWR);
    if (gtt_fd < 0) { perror("resource2"); return 1; }
    uint32_t *fb = mmap(NULL, db.size, PROT_READ|PROT_WRITE,
                        MAP_SHARED, gtt_fd, gtt_addr);
    if (fb == MAP_FAILED) { perror("mmap gtt"); return 1; }
    printf("Mapped framebuffer at GTT offset 0x%lx via BAR2\n", gtt_addr);

    /* ── animate — no DRM from here on ──────────────────────────────── */
    double start = now(); int frame=0;
    while (now()-start < 3.0)
        paint(fb, mode.hdisplay, mode.vdisplay, db.pitch/4, frame++);
    printf("%d frames = %.0f fps\n", frame, frame/3.0);

    /* cleanup */
    struct drm_mode_crtc off={.crtc_id=crtc_id};
    ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &off);
    munmap(fb, db.size); munmap((void*)mmio, MMIO_SZ);
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_cmd.fb_id);
    struct drm_mode_destroy_dumb dd={.handle=db.handle};
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    return 0;
}

/*
 * paint.ko — kernel module that writes directly to the Intel GPU scanout buffer.
 *
 * No userspace.  No DRM calls.  No sysfs resource files.
 * We use the kernel's own PCI infrastructure (pci_get_device, pci_iomap)
 * to map BAR0 (MMIO registers) and BAR2 (GTT aperture), read the
 * PLANE_SURF register to locate the active scanout buffer, and write a
 * gradient animation directly from kernel space.
 *
 * The modeset is assumed to be already active (GNOME or our drm3 program
 * will have set the CRTC).  We just overwrite the pixels the display
 * engine is already scanning out from.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/io.h>

MODULE_LICENSE("GPL");

static ulong gtt_off = 0;
module_param(gtt_off, ulong, 0);
MODULE_PARM_DESC(gtt_off, "GTT aperture offset of scanout buffer (from drm_setup)");

/* Intel Iris Xe (Alder Lake) PCI ID */
#define INTEL_VID   0x8086
#define ADL_DID     0x46a6   /* Alder Lake-P GT2 */

/* Gen12 primary-plane surface address registers (pipe A/B/C/D, plane 1) */
#define PLANE_SURF_A  0x7019C
#define PLANE_SURF_B  0x7119C
#define PLANE_SURF_C  0x7219C
#define PLANE_SURF_D  0x7319C

/* Display width/height — must match whatever the active mode is */
#define W 1920
#define H 1080

static int __init paint_init(void)
{
    struct pci_dev *dev;
    void __iomem *mmio;
    void __iomem *gtt;
    resource_size_t gtt_base, gtt_len;
    u32 surf[4];
    ulong local_gtt_off;   /* avoid shadowing the module param */
    u32 __iomem *fb;
    int frame, x, y;

    dev = pci_get_device(INTEL_VID, ADL_DID, NULL);
    if (!dev) { pr_err("paint: Intel GPU not found\n"); return -ENODEV; }

    /* BAR0 = MMIO registers (16 MB) */
    mmio = pci_iomap(dev, 0, 0);
    if (!mmio) { pr_err("paint: iomap BAR0 failed\n"); goto out_dev; }

    /* BAR2 = GTT aperture (256 MB) */
    gtt_base = pci_resource_start(dev, 2);   /* kept for log only */
    gtt_len  = pci_resource_len(dev, 2);
    (void)gtt_base; (void)gtt_len;
    gtt = pci_iomap(dev, 2, 0);
    if (!gtt) { pr_err("paint: iomap BAR2 failed\n"); goto out_mmio; }

    /* Read PLANE_SURF for all four pipes */
    surf[0] = readl(mmio + PLANE_SURF_A);
    surf[1] = readl(mmio + PLANE_SURF_B);
    surf[2] = readl(mmio + PLANE_SURF_C);
    surf[3] = readl(mmio + PLANE_SURF_D);
    pr_info("paint: PLANE_SURF A:%08x B:%08x C:%08x D:%08x\n",
            surf[0], surf[1], surf[2], surf[3]);

    /* Use module param if supplied, else fall back to last active pipe */
    local_gtt_off = gtt_off;
    if (!local_gtt_off) {
        for (int i = 0; i < 4; i++)
            if (surf[i] & ~0xFFFu) local_gtt_off = surf[i] & ~0xFFFu;
    }
    if (!local_gtt_off) {
        pr_err("paint: no scanout buffer — pass gtt_off=<addr>\n");
        goto out_gtt;
    }
    pr_info("paint: scanout at GTT offset 0x%lx, writing via BAR2\n", local_gtt_off);

    /* Framebuffer pointer inside the GTT aperture */
    fb = gtt + local_gtt_off;

    /* Animate for 3 seconds, as fast as the CPU can write */
    {
        ktime_t end = ktime_add_ms(ktime_get(), 3000);
        frame = 0;
        while (ktime_before(ktime_get(), end)) {
            for (y = 0; y < H; y++) {
                u8  v = (u8)(y + frame * 3);
                u32 c = ((u32)v << 16) | ((u32)(v+85) << 8) | (u32)(v+170);
                u32 __iomem *row = fb + y * W;
                for (x = 0; x < W; x++)
                    writel(c, row + x);
            }
            frame++;
            cond_resched();
        }
    }
    pr_info("paint: %d frames in 3s\n", frame);

out_gtt:
    pci_iounmap(dev, gtt);
out_mmio:
    pci_iounmap(dev, mmio);
out_dev:
    pci_dev_put(dev);
    return 0;
}

static void __exit paint_exit(void) {}

module_init(paint_init);
module_exit(paint_exit);

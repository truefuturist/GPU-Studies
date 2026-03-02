/*
 * paint2.ko — self-contained display takeover, zero DRM involvement.
 *
 * Previous level (paint.ko): DRM ioctls for CRTC setup, BAR2 for animation.
 * This level: no DRM at all.
 *
 *   1. alloc_page() × N_PAGES, vmap'd with pgprot_writecombine.
 *      WC bypasses LLC so display-engine DMA reads fresh pixels from DRAM.
 *      (Plain vzalloc gives WB cached memory; display DMA bypasses LLC → stale.)
 *   2. Write GGTT PTEs directly at BAR0+0x800000 (phys | 1 — IOMMU pass-through
 *      confirmed, same format as i915).
 *   3. Switch PLANE_CTL to linear mode (clear tiling bits[12:10] and bit 15).
 *      Set PLANE_STRIDE to 120 (×64B = 7680B = 1920px).
 *   4. Write pixels in row-major order (WC writes bypass cache, visible to GPU).
 *   5. Flip PLANE_SURF_B → our buffer.
 *   6. Animate, then restore PLANE_CTL, PLANE_STRIDE, and PLANE_SURF.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");

#define INTEL_VID   0x8086
#define ADL_DID     0x46a6

#define GGTT_BASE       0x800000    /* GGTT page tables: BAR0 + 8 MB        */
#define PLANE_CTL_B     0x71180     /* pipe B plane 1 control               */
#define PLANE_STRIDE_B  0x71188     /* pipe B plane 1 stride                */
#define PLANE_SURF_B    0x7119C     /* pipe B plane 1 surface address       */
#define OUR_GTT_ADDR    0x10000000u /* 256 MB into GGTT, above i915 allocs  */

/* PLANE_CTL fields we modify:
 *   bits[12:10] = tiling mode (non-zero = tiled, 0 = linear)
 *   bit 15      = Render Decompression (needs CCS aux surface; we have none) */
#define PLANE_CTL_TILED         0x1C00u
#define PLANE_CTL_RENDER_DECOMP 0x8000u

#define W           1920
#define H           1080
#define OUR_STRIDE  120         /* linear: 120 × 64B = 7680B = 1920px × 4B */
#define N_PAGES     ((size_t)(H) * (OUR_STRIDE) * 64 / PAGE_SIZE)   /* 2025 */

/* Geometry test: colored 100×100 squares at corners and center,
 * 3px cyan border, gray crosshairs through center. */
static void paint_test(u32 *fb)
{
    int y, x;

    /* Background */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            fb[y * W + x] = 0x202020;

    /* 3px border: cyan */
    for (y = 0; y < H; y++)
        for (x = 0; x < 3; x++) {
            fb[y * W + x]       = 0x00FFFF;
            fb[y * W + W-1-x]   = 0x00FFFF;
        }
    for (x = 0; x < W; x++)
        for (y = 0; y < 3; y++) {
            fb[y * W + x]       = 0x00FFFF;
            fb[(H-1-y) * W + x] = 0x00FFFF;
        }

    /* Top-left: red */
    for (y = 0; y < 100; y++)
        for (x = 0; x < 100; x++)
            fb[y * W + x] = 0xFF0000;

    /* Top-right: green */
    for (y = 0; y < 100; y++)
        for (x = W-100; x < W; x++)
            fb[y * W + x] = 0x00FF00;

    /* Bottom-left: blue */
    for (y = H-100; y < H; y++)
        for (x = 0; x < 100; x++)
            fb[y * W + x] = 0x0000FF;

    /* Bottom-right: magenta */
    for (y = H-100; y < H; y++)
        for (x = W-100; x < W; x++)
            fb[y * W + x] = 0xFF00FF;

    /* Center 100×100: white */
    for (y = H/2-50; y < H/2+50; y++)
        for (x = W/2-50; x < W/2+50; x++)
            fb[y * W + x] = 0xFFFFFF;

    /* Crosshairs: gray horizontal and vertical lines through center */
    for (x = 0; x < W; x++)
        fb[(H/2) * W + x] = 0x606060;
    for (y = 0; y < H; y++)
        fb[y * W + W/2] = 0x606060;
}

static int __init p2_init(void)
{
    struct pci_dev  *dev;
    void __iomem    *mmio, *ggtt;
    struct page    **pages = NULL;
    u8              *fb    = NULL;
    u32              orig_surf, orig_ctl, orig_stride;
    int              frame, ret = 0;
    ktime_t          end;

    dev = pci_get_device(INTEL_VID, ADL_DID, NULL);
    if (!dev) { pr_err("p2: GPU not found\n"); return -ENODEV; }

    mmio = pci_iomap(dev, 0, 0);
    if (!mmio) { pr_err("p2: BAR0 iomap failed\n"); ret = -ENOMEM; goto err_dev; }
    ggtt = mmio + GGTT_BASE;

    orig_surf   = readl(mmio + PLANE_SURF_B);
    orig_ctl    = readl(mmio + PLANE_CTL_B);
    orig_stride = readl(mmio + PLANE_STRIDE_B);
    pr_info("p2: PLANE_CTL=0x%08x PLANE_STRIDE=0x%08x PLANE_SURF=0x%08x\n",
            orig_ctl, orig_stride, orig_surf);

    pages = kmalloc_array(N_PAGES, sizeof(*pages), GFP_KERNEL);
    if (!pages) { pr_err("p2: kmalloc pages[] failed\n"); ret = -ENOMEM; goto err_mmio; }

    for (int i = 0; i < (int)N_PAGES; i++) {
        pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pages[i]) {
            pr_err("p2: alloc_page %d failed\n", i);
            for (int j = 0; j < i; j++) __free_page(pages[j]);
            ret = -ENOMEM;
            goto err_pages;
        }
    }

    /* vmap with WC: CPU writes bypass LLC → DRAM, visible to display DMA */
    fb = vmap(pages, N_PAGES, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
    if (!fb) {
        pr_err("p2: vmap failed\n");
        for (int i = 0; i < (int)N_PAGES; i++) __free_page(pages[i]);
        ret = -ENOMEM;
        goto err_pages;
    }
    pr_info("p2: fb=%p (WC vmap, %zu pages)\n", fb, N_PAGES);

    /* Write GGTT PTEs: phys | 1 for each page */
    for (int i = 0; i < (int)N_PAGES; i++)
        writeq(page_to_phys(pages[i]) | 1u,
               ggtt + (OUR_GTT_ADDR / PAGE_SIZE + i) * 8);
    readl(ggtt);
    pr_info("p2: wrote %zu GGTT entries at GPU addr 0x%x\n", N_PAGES, OUR_GTT_ADDR);

    /* Paint test pattern before flip */
    paint_test((u32 *)fb);

    /* Switch to linear mode, set stride, flip surface.
     * PLANE_SURF write latches PLANE_CTL and PLANE_STRIDE on next vblank. */
    writel(orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP), mmio + PLANE_CTL_B);
    writel(OUR_STRIDE, mmio + PLANE_STRIDE_B);
    writel(OUR_GTT_ADDR, mmio + PLANE_SURF_B);
    pr_info("p2: flipped SURF 0x%08x->0x%08x CTL 0x%08x->0x%08x STRIDE %u->%u\n",
            orig_surf, OUR_GTT_ADDR,
            orig_ctl, orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP),
            orig_stride, OUR_STRIDE);

    /* Hold for 5s (static pattern) */
    end = ktime_add_ms(ktime_get(), 5000);
    for (frame = 0; ktime_before(ktime_get(), end); frame++)
        cond_resched();
    pr_info("p2: geometry test held 5s\n");

    /* Restore */
    writel(orig_ctl,    mmio + PLANE_CTL_B);
    writel(orig_stride, mmio + PLANE_STRIDE_B);
    writel(orig_surf,   mmio + PLANE_SURF_B);
    readl(mmio + PLANE_SURF_B);

    /* Clear GGTT entries */
    for (int i = 0; i < (int)N_PAGES; i++)
        writeq(0, ggtt + (OUR_GTT_ADDR / PAGE_SIZE + i) * 8);
    readl(ggtt);

    vunmap(fb);
    for (int i = 0; i < (int)N_PAGES; i++) __free_page(pages[i]);
err_pages:
    kfree(pages);
err_mmio:
    pci_iounmap(dev, mmio);
err_dev:
    pci_dev_put(dev);
    return ret;
}

static void __exit p2_exit(void) {}
module_init(p2_init);
module_exit(p2_exit);

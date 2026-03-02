/*
 * paint3 — Level 6: userspace display takeover, no kernel module.
 *
 * Does exactly what paint2.ko did, using only userspace Linux APIs.
 * Each Linux-specific section is marked with its EFI replacement.
 *
 * Linux-specific:
 *   BAR0 access       — mmap /sys/bus/pci/devices/.../resource0
 *   Physical addrs    — mlock + /proc/self/pagemap
 *   Cache coherency   — clflush (unprivileged x86 insn, same in EFI)
 *
 * EFI translation:
 *   BAR0 access       — read BAR from PCI ECAM, use physical addr directly
 *   Physical addrs    — EFI AllocatePages() returns physical addr directly
 *   Everything else   — identical
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <immintrin.h>  /* _mm_clflush, _mm_sfence — unprivileged on x86 */

/* ── hardware constants (same in EFI) ────────────────────────────────────── */
#define GGTT_BASE           0x800000u
#define PLANE_CTL_B         0x71180u
#define PLANE_STRIDE_B      0x71188u
#define PLANE_SURF_B        0x7119Cu
#define OUR_GTT_ADDR        0x10000000u
#define PLANE_CTL_TILED         0x1C00u
#define PLANE_CTL_RENDER_DECOMP 0x8000u

#define W           1920
#define H           1080
#define OUR_STRIDE  120         /* linear: 120 × 64B = 7680B = 1920px × 4B */
#define PAGE_SIZE   4096u
#define N_PAGES     ((size_t)(H) * (OUR_STRIDE) * 64 / PAGE_SIZE)   /* 2025 */
#define FB_SIZE     ((size_t)(H) * (OUR_STRIDE) * 64)
#define MMIO_SIZE   (16u * 1024 * 1024)   /* BAR0 = 16 MB */

/* ── MMIO accessors: volatile pointer writes, same pattern as EFI ─────────── */
static inline uint32_t mmio_r32(volatile uint8_t *b, uint32_t off)
    { return *(volatile uint32_t *)(b + off); }
static inline void mmio_w32(volatile uint8_t *b, uint32_t off, uint32_t v)
    { *(volatile uint32_t *)(b + off) = v; }
static inline void mmio_w64(volatile uint8_t *b, uint64_t off, uint64_t v)
    { *(volatile uint64_t *)(b + off) = v; }

/* ── Linux-specific: physical address of a pinned virtual page ────────────── *
 * EFI replacement: AllocatePages(AllocateAnyPages, EfiLoaderData, N, &phys)  *
 *   — physical addresses are returned directly, no lookup needed.             */
static uint64_t virt_to_phys(void *vaddr)
{
    static int fd = -1;
    if (fd < 0 && (fd = open("/proc/self/pagemap", O_RDONLY)) < 0)
        { perror("pagemap"); exit(1); }
    uint64_t entry;
    uint64_t idx = (uintptr_t)vaddr / PAGE_SIZE;
    if (pread(fd, &entry, 8, idx * 8) != 8)
        { perror("pread pagemap"); exit(1); }
    if (!(entry >> 63))
        { fprintf(stderr, "page not present: %p\n", vaddr); exit(1); }
    return (entry & ((1ULL << 55) - 1)) * PAGE_SIZE;
}

/* ── cache flush: unprivileged x86 instruction, identical in EFI ─────────── */
static void clflush_range(const void *p, size_t n)
{
    const char *cp = (const char *)((uintptr_t)p & ~(uintptr_t)63);
    for (; cp < (const char *)p + n; cp += 64)
        _mm_clflush(cp);
    _mm_sfence();
}

/* ── pixel painting (same in EFI) ─────────────────────────────────────────── */
static void paint_test(uint32_t *fb)
{
    /* background */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y * W + x] = 0x202020;

    /* 3px cyan border */
    for (int y = 0; y < H; y++)
        for (int b = 0; b < 3; b++) {
            fb[y * W + b]       = 0x00FFFF;
            fb[y * W + W-1-b]   = 0x00FFFF;
        }
    for (int x = 0; x < W; x++)
        for (int b = 0; b < 3; b++) {
            fb[b * W + x]           = 0x00FFFF;
            fb[(H-1-b) * W + x]     = 0x00FFFF;
        }

    /* corner squares: TL=red, TR=green, BL=blue, BR=magenta */
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 100; x++) {
            fb[ y         * W +  x       ] = 0xFF0000;
            fb[ y         * W + (W-100+x)] = 0x00FF00;
            fb[(H-100+y)  * W +  x       ] = 0x0000FF;
            fb[(H-100+y)  * W + (W-100+x)] = 0xFF00FF;
        }

    /* center 100×100: white */
    for (int y = H/2-50; y < H/2+50; y++)
        for (int x = W/2-50; x < W/2+50; x++)
            fb[y * W + x] = 0xFFFFFF;

    /* crosshairs: gray */
    for (int x = 0; x < W; x++) fb[(H/2) * W + x] = 0x606060;
    for (int y = 0; y < H; y++) fb[ y    * W + W/2] = 0x606060;
}

int main(void)
{
    /* ── Linux-specific: map BAR0 via sysfs ─────────────────────────────── *
     * EFI replacement: phys_bar0 = read PCI config ECAM BAR0 register;     *
     *   volatile uint8_t *mmio = (volatile uint8_t *)(uintptr_t)phys_bar0; *
     * No fd, no mmap — the physical address is used directly.               */
    int bar_fd = open("/sys/bus/pci/devices/0000:00:02.0/resource0",
                      O_RDWR | O_SYNC);
    if (bar_fd < 0) { perror("open BAR0"); return 1; }
    volatile uint8_t *mmio = mmap(NULL, MMIO_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, bar_fd, 0);
    if (mmio == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    volatile uint8_t *ggtt = mmio + GGTT_BASE;

    uint32_t orig_ctl    = mmio_r32(mmio, PLANE_CTL_B);
    uint32_t orig_stride = mmio_r32(mmio, PLANE_STRIDE_B);
    uint32_t orig_surf   = mmio_r32(mmio, PLANE_SURF_B);
    printf("PLANE_CTL=0x%08x PLANE_STRIDE=0x%08x PLANE_SURF=0x%08x\n",
           orig_ctl, orig_stride, orig_surf);

    /* ── Linux-specific: allocate and pin framebuffer ───────────────────── *
     * EFI replacement: EFI_STATUS s = AllocatePages(                        *
     *     AllocateAnyPages, EfiLoaderData, N_PAGES, &phys_base);            *
     *   uint8_t *fb = (uint8_t *)(uintptr_t)phys_base;  ← already physical */
    uint8_t *fb = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (fb == MAP_FAILED) { perror("mmap fb"); return 1; }
    if (mlock(fb, FB_SIZE)) { perror("mlock"); return 1; }

    /* ── write GGTT PTEs (same in EFI, using phys_base + i*PAGE_SIZE) ───── */
    for (int i = 0; i < (int)N_PAGES; i++) {
        uint64_t phys = virt_to_phys(fb + i * PAGE_SIZE);
        mmio_w64(ggtt, (uint64_t)(OUR_GTT_ADDR / PAGE_SIZE + i) * 8, phys | 1);
    }
    mmio_r32(mmio, 0);  /* flush posted writes */
    printf("wrote %zu GGTT entries at GPU addr 0x%x\n", N_PAGES, OUR_GTT_ADDR);

    /* paint then flush cache so display DMA reads fresh pixels from DRAM */
    paint_test((uint32_t *)fb);
    clflush_range(fb, FB_SIZE);

    /* ── flip plane (same in EFI) ─────────────────────────────────────────── */
    mmio_w32(mmio, PLANE_CTL_B,
             orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP));
    mmio_w32(mmio, PLANE_STRIDE_B, OUR_STRIDE);
    mmio_w32(mmio, PLANE_SURF_B,   OUR_GTT_ADDR);
    printf("flipped SURF 0x%08x->0x%08x CTL 0x%08x->0x%08x STRIDE %u->%u\n",
           orig_surf, OUR_GTT_ADDR,
           orig_ctl, orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP),
           orig_stride, OUR_STRIDE);

    /* hold 5s */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do { clock_gettime(CLOCK_MONOTONIC, &t1); }
    while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 5000);

    /* ── restore (same in EFI) ────────────────────────────────────────────── */
    mmio_w32(mmio, PLANE_CTL_B,    orig_ctl);
    mmio_w32(mmio, PLANE_STRIDE_B, orig_stride);
    mmio_w32(mmio, PLANE_SURF_B,   orig_surf);
    mmio_r32(mmio, PLANE_SURF_B);

    /* clear GGTT */
    for (int i = 0; i < (int)N_PAGES; i++)
        mmio_w64(ggtt, (uint64_t)(OUR_GTT_ADDR / PAGE_SIZE + i) * 8, 0);
    mmio_r32(mmio, 0);

    munmap(fb, FB_SIZE);
    munmap((void *)mmio, MMIO_SIZE);
    close(bar_fd);
    return 0;
}

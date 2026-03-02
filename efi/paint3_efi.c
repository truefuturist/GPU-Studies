/*
 * paint3_efi — Level 7: bare-metal EFI display takeover.
 *
 * All output goes to screen AND to /EFI/paint/log.txt on the ESP,
 * readable from Linux at /boot/efi/EFI/paint/log.txt after reboot.
 */
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

/* ── log buffer: written to ESP on exit, readable from Linux ─────────────── */
#define LOG_MAX 8192
static CHAR8 log_buf[LOG_MAX];
static UINTN log_len = 0;

/* Append CHAR16 string to ASCII log buffer */
static void log_str(const CHAR16 *s)
{
    for (; *s && log_len < LOG_MAX - 1; s++)
        log_buf[log_len++] = (*s < 128) ? (CHAR8)*s : '?';
}

/* Print to screen and append to log buffer */
#define LOG(fmt, ...) do { \
    CHAR16 _b[512]; \
    SPrint(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    Print(L"%s", _b); \
    log_str(_b); \
} while (0)

/* Write log_buf to EFI\paint\log.txt on the same volume we booted from */
static void log_flush(EFI_HANDLE ImageHandle)
{
    EFI_LOADED_IMAGE        *li   = NULL;
    EFI_FILE_HANDLE          root = NULL;
    EFI_FILE_HANDLE          f    = NULL;

    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3,
            ImageHandle, &LoadedImageProtocol, (void **)&li)) || !li)
        return;

    root = LibOpenRoot(li->DeviceHandle);
    if (!root) return;

    EFI_STATUS s = uefi_call_wrapper(root->Open, 5, root, &f,
        L"EFI\\paint\\log.txt",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        (UINT64)0);
    if (!EFI_ERROR(s) && f) {
        UINTN sz = log_len;
        uefi_call_wrapper(f->Write, 3, f, &sz, log_buf);
        uefi_call_wrapper(f->Close, 1, f);
    }
    uefi_call_wrapper(root->Close, 1, root);
}

/* ── hardware constants ───────────────────────────────────────────────────── */
#define INTEL_VID   0x8086
#define ADL_DID     0x46a6

#define GGTT_BASE           0x800000u
#define PLANE_CTL_B         0x71180u
#define PLANE_STRIDE_B      0x71188u
#define PLANE_SURF_B        0x7119Cu
#define OUR_GTT_ADDR        0x10000000u
#define PLANE_CTL_TILED         0x1C00u
#define PLANE_CTL_RENDER_DECOMP 0x8000u

#define W           1920
#define H           1080
#define OUR_STRIDE  120
#define PAGE_SZ     4096u
#define N_PAGES     ((UINTN)(H) * (OUR_STRIDE) * 64 / PAGE_SZ)   /* 2025 */

/* ── MMIO accessors ───────────────────────────────────────────────────────── */
static inline UINT32 mmio_r32(volatile UINT8 *b, UINT32 off)
    { return *(volatile UINT32 *)(b + off); }
static inline void mmio_w32(volatile UINT8 *b, UINT32 off, UINT32 v)
    { *(volatile UINT32 *)(b + off) = v; }
static inline void mmio_w64(volatile UINT8 *b, UINT64 off, UINT64 v)
    { *(volatile UINT64 *)(b + off) = v; }

/* ── cache flush ──────────────────────────────────────────────────────────── */
static void clflush_range(const void *p, UINTN n)
{
    const char *cp = (const char *)((UINTN)p & ~(UINTN)63);
    for (; cp < (const char *)p + n; cp += 64)
        __asm__ volatile("clflush (%0)" :: "r"(cp) : "memory");
    __asm__ volatile("sfence" ::: "memory");
}

/* ── GOP stamp: colored corners on firmware framebuffer (proof of life) ───── */
static void gop_stamp(EFI_SYSTEM_TABLE *ST)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS s = uefi_call_wrapper(ST->BootServices->LocateProtocol, 3,
        &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(s) || !gop) { LOG(L"GOP: not found\n"); return; }

    UINT32  w  = gop->Mode->Info->HorizontalResolution;
    UINT32  h  = gop->Mode->Info->VerticalResolution;
    UINT32  sp = gop->Mode->Info->PixelsPerScanLine;
    UINT32 *fb = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    LOG(L"GOP: %dx%d  stride_px=%d  fb_phys=0x%lx\n",
        w, h, sp, (UINT64)gop->Mode->FrameBufferBase);

    /* 50px corner squares so we can see the app ran on screen */
    for (UINT32 y = 0; y < 50; y++)
        for (UINT32 x = 0; x < 50; x++) {
            fb[ y      * sp +  x      ] = 0xFF0000;  /* TL red    */
            fb[ y      * sp + (w-1-x) ] = 0x00FF00;  /* TR green  */
            fb[(h-1-y) * sp +  x      ] = 0x0000FF;  /* BL blue   */
            fb[(h-1-y) * sp + (w-1-x) ] = 0xFF00FF;  /* BR magenta*/
        }
}

/* ── pipe dump: read PLANE_CTL/STRIDE/SURF for pipes A, B, C ─────────────── */
static void diag_pipes(volatile UINT8 *mmio)
{
    static const struct {
        const CHAR16 *name;
        UINT32 ctl, stride, surf;
    } pipes[] = {
        { L"A", 0x70180, 0x70188, 0x7019C },
        { L"B", 0x71180, 0x71188, 0x7119C },
        { L"C", 0x72180, 0x72188, 0x7219C },
    };
    for (int i = 0; i < 3; i++) {
        UINT32 ctl    = mmio_r32(mmio, pipes[i].ctl);
        UINT32 stride = mmio_r32(mmio, pipes[i].stride);
        UINT32 surf   = mmio_r32(mmio, pipes[i].surf);
        LOG(L"Pipe %s: CTL=0x%08x %s  STRIDE=0x%04x  SURF=0x%08x\n",
            pipes[i].name, ctl,
            (ctl & (1u << 31)) ? L"[ON] " : L"[off]",
            stride, surf);
    }
}

/* ── pixel painting: identical to paint3.c ────────────────────────────────── */
static void paint_test(UINT32 *fb)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y * W + x] = 0x202020;

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

    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 100; x++) {
            fb[ y        * W +  x       ] = 0xFF0000;
            fb[ y        * W + (W-100+x)] = 0x00FF00;
            fb[(H-100+y) * W +  x       ] = 0x0000FF;
            fb[(H-100+y) * W + (W-100+x)] = 0xFF00FF;
        }

    for (int y = H/2-50; y < H/2+50; y++)
        for (int x = W/2-50; x < W/2+50; x++)
            fb[y * W + x] = 0xFFFFFF;

    for (int x = 0; x < W; x++) fb[(H/2) * W + x] = 0x606060;
    for (int y = 0; y < H; y++) fb[ y    * W + W/2] = 0x606060;
}

/* ── find GPU BAR0 via EFI_PCI_IO_PROTOCOL ───────────────────────────────── */
static EFI_STATUS find_gpu_bar0(EFI_SYSTEM_TABLE *ST, UINT64 *bar0_out)
{
    EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;

    EFI_STATUS s = uefi_call_wrapper(ST->BootServices->LocateHandleBuffer, 5,
        ByProtocol, &pci_guid, NULL, &count, &handles);
    if (EFI_ERROR(s)) return s;

    for (UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *pciio = NULL;
        uefi_call_wrapper(ST->BootServices->HandleProtocol, 3,
            handles[i], &pci_guid, (void **)&pciio);
        if (!pciio) continue;

        UINT16 vid = 0, did = 0;
        uefi_call_wrapper(pciio->Pci.Read, 5, pciio, EfiPciIoWidthUint16, 0x00, 1, &vid);
        uefi_call_wrapper(pciio->Pci.Read, 5, pciio, EfiPciIoWidthUint16, 0x02, 1, &did);
        if (vid != INTEL_VID || did != ADL_DID) continue;

        UINT32 bar_lo = 0, bar_hi = 0;
        uefi_call_wrapper(pciio->Pci.Read, 5, pciio, EfiPciIoWidthUint32, 0x10, 1, &bar_lo);
        uefi_call_wrapper(pciio->Pci.Read, 5, pciio, EfiPciIoWidthUint32, 0x14, 1, &bar_hi);
        *bar0_out = ((UINT64)bar_hi << 32) | (bar_lo & ~(UINT32)0xF);

        uefi_call_wrapper(ST->BootServices->FreePool, 1, handles);
        return EFI_SUCCESS;
    }
    uefi_call_wrapper(ST->BootServices->FreePool, 1, handles);
    return EFI_NOT_FOUND;
}

/* ── entry point ──────────────────────────────────────────────────────────── */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    LOG(L"=== paint3_efi ===\n");

    /* stage 1: GOP stamp + info */
    gop_stamp(SystemTable);

    /* stage 2: find GPU, dump pipes */
    UINT64 bar0_phys = 0;
    EFI_STATUS s = find_gpu_bar0(SystemTable, &bar0_phys);
    if (EFI_ERROR(s)) {
        LOG(L"GPU not found: %r\n", s);
        log_flush(ImageHandle);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 10000000);
        return s;
    }
    LOG(L"GPU BAR0 phys: 0x%lx\n", bar0_phys);

    volatile UINT8 *mmio = (volatile UINT8 *)(UINTN)bar0_phys;
    volatile UINT8 *ggtt = mmio + GGTT_BASE;

    diag_pipes(mmio);

    UINT32 orig_ctl    = mmio_r32(mmio, PLANE_CTL_B);
    UINT32 orig_stride = mmio_r32(mmio, PLANE_STRIDE_B);
    UINT32 orig_surf   = mmio_r32(mmio, PLANE_SURF_B);

    /* stage 3: direct GGTT flip on pipe B if active */
    if (!(orig_ctl & (1u << 31))) {
        LOG(L"Pipe B [off] — skipping direct flip\n");
        log_flush(ImageHandle);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 10000000);
        return EFI_SUCCESS;
    }

    EFI_PHYSICAL_ADDRESS fb_phys = 0;
    s = uefi_call_wrapper(SystemTable->BootServices->AllocatePages, 4,
        AllocateAnyPages, EfiLoaderData, N_PAGES, &fb_phys);
    if (EFI_ERROR(s)) {
        LOG(L"AllocatePages failed: %r\n", s);
        log_flush(ImageHandle);
        return s;
    }
    UINT8 *fb = (UINT8 *)(UINTN)fb_phys;
    SetMem(fb, N_PAGES * PAGE_SZ, 0);
    LOG(L"fb phys: 0x%lx  pages: %d\n", fb_phys, (int)N_PAGES);

    for (UINTN i = 0; i < N_PAGES; i++)
        mmio_w64(ggtt, (UINT64)(OUR_GTT_ADDR / PAGE_SZ + i) * 8,
                 (fb_phys + i * PAGE_SZ) | 1u);
    mmio_r32(mmio, 0);
    LOG(L"wrote %d GGTT entries at GPU addr 0x%x\n", (int)N_PAGES, OUR_GTT_ADDR);

    paint_test((UINT32 *)fb);
    clflush_range(fb, N_PAGES * PAGE_SZ);

    mmio_w32(mmio, PLANE_CTL_B,
             orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP));
    mmio_w32(mmio, PLANE_STRIDE_B, OUR_STRIDE);
    mmio_w32(mmio, PLANE_SURF_B,   OUR_GTT_ADDR);
    LOG(L"flipped: SURF 0x%08x->0x%08x  CTL 0x%08x->0x%08x  STRIDE %d->%d\n",
        orig_surf, OUR_GTT_ADDR,
        orig_ctl, orig_ctl & ~(PLANE_CTL_TILED | PLANE_CTL_RENDER_DECOMP),
        (int)orig_stride, OUR_STRIDE);

    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);

    mmio_w32(mmio, PLANE_CTL_B,    orig_ctl);
    mmio_w32(mmio, PLANE_STRIDE_B, orig_stride);
    mmio_w32(mmio, PLANE_SURF_B,   orig_surf);
    mmio_r32(mmio, PLANE_SURF_B);

    for (UINTN i = 0; i < N_PAGES; i++)
        mmio_w64(ggtt, (UINT64)(OUR_GTT_ADDR / PAGE_SZ + i) * 8, 0);
    mmio_r32(mmio, 0);

    uefi_call_wrapper(SystemTable->BootServices->FreePages, 2, fb_phys, N_PAGES);
    LOG(L"done\n");

    /* flush log before waiting for key */
    log_flush(ImageHandle);

    LOG(L"press any key\n");
    EFI_INPUT_KEY key;
    while (uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2,
                             SystemTable->ConIn, &key) == EFI_NOT_READY)
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 100000);

    return EFI_SUCCESS;
}

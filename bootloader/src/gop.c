#include "gop.h"
#include "serial.h"

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static Framebuffer framebuffer;

static void serial_write_str(BootInfo *boot_info, const char *s)
{
    if (boot_info)
        serial_write_all(boot_info, s);
}

static void serial_hex(BootInfo *boot_info, uint64_t v)
{
    if (boot_info)
        serial_write_hex64_all(boot_info, v);
}

static void serial_mode_line(BootInfo *boot_info, const char *prefix, UINT32 mode, UINT32 w, UINT32 h, EFI_STATUS st)
{
    serial_write_str(boot_info, prefix);
    serial_write_str(boot_info, " mode=0x");
    serial_hex(boot_info, (uint64_t)mode);
    serial_write_str(boot_info, " W=0x");
    serial_hex(boot_info, (uint64_t)w);
    serial_write_str(boot_info, " H=0x");
    serial_hex(boot_info, (uint64_t)h);
    serial_write_str(boot_info, " ST=0x");
    serial_hex(boot_info, (uint64_t)st);
    serial_write_str(boot_info, "\n");
}

EFI_STATUS locate_gop(EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    return uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3, &GopGuid, NULL, (void **)&gop);
}

typedef struct
{
    UINT32 mode;
    UINT32 w;
    UINT32 h;
    UINT64 area;
} Candidate;

static void swap_candidate(Candidate *a, Candidate *b)
{
    Candidate t = *a;
    *a = *b;
    *b = t;
}

static void gop_clear_screen_black()
{
    if (!gop || !gop->Mode || !gop->Mode->Info)
        return;

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black;
    black.Blue = 0;
    black.Green = 0;
    black.Red = 0;
    black.Reserved = 0;

    uefi_call_wrapper(
        gop->Blt,
        10,
        gop,
        &black,
        EfiBltVideoFill,
        0, 0,
        0, 0,
        gop->Mode->Info->HorizontalResolution,
        gop->Mode->Info->VerticalResolution,
        0);
}

Framebuffer *get_framebuffer(BootInfo *boot_info)
{
    if (gop == NULL)
    {
        if (locate_gop(SystemTable) != EFI_SUCCESS)
        {
            serial_write_str(boot_info, "[UEFI][GOP] ERROR: LocateProtocol(GOP) failed.\n");
            return NULL;
        }
    }

    if (gop->Mode == NULL || gop->Mode->Info == NULL)
    {
        serial_write_str(boot_info, "[UEFI][GOP] ERROR: gop->Mode or gop->Mode->Info is NULL.\n");
        return NULL;
    }

    UINT32 native_mode = gop->Mode->Mode;
    UINT32 max_modes = gop->Mode->MaxMode;

    serial_write_str(boot_info, "[UEFI][GOP] Enumerating modes...\n");
    serial_write_str(boot_info, "[UEFI][GOP] Native mode=0x");
    serial_hex(boot_info, (uint64_t)native_mode);
    serial_write_str(boot_info, " W=0x");
    serial_hex(boot_info, (uint64_t)gop->Mode->Info->HorizontalResolution);
    serial_write_str(boot_info, " H=0x");
    serial_hex(boot_info, (uint64_t)gop->Mode->Info->VerticalResolution);
    serial_write_str(boot_info, "\n");

    Candidate cand[256];
    UINT32 cand_count = 0;

    for (UINT32 i = 0; i < max_modes && cand_count < 256; i++)
    {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN size_of_info = 0;

        EFI_STATUS st = uefi_call_wrapper(gop->QueryMode, 4, gop, i, &size_of_info, &info);
        if (st != EFI_SUCCESS || info == NULL)
        {
            serial_mode_line(boot_info, "[UEFI][GOP] Query FAIL", i, 0, 0, st);
            continue;
        }

        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        UINT64 area = (UINT64)w * (UINT64)h;

        cand[cand_count].mode = i;
        cand[cand_count].w = w;
        cand[cand_count].h = h;
        cand[cand_count].area = area;
        cand_count++;

        serial_mode_line(boot_info, "[UEFI][GOP] Query OK  ", i, w, h, st);
    }

    if (cand_count == 0)
    {
        serial_write_str(boot_info, "[UEFI][GOP] ERROR: No valid modes from QueryMode.\n");
        return NULL;
    }

    for (UINT32 i = 0; i < cand_count; i++)
    {
        for (UINT32 j = i + 1; j < cand_count; j++)
        {
            Candidate *A = &cand[i];
            Candidate *B = &cand[j];

            int better =
                (B->area > A->area) ||
                (B->area == A->area && B->w > A->w) ||
                (B->area == A->area && B->w == A->w && B->h > A->h);

            if (better)
                swap_candidate(A, B);
        }
    }

    UINT32 chosen_mode = gop->Mode->Mode;
    int set_ok = 0;

    for (UINT32 k = 0; k < cand_count; k++)
    {
        UINT32 try_mode = cand[k].mode;

        if (try_mode == gop->Mode->Mode)
        {
            chosen_mode = try_mode;
            set_ok = 1;
            break;
        }

        EFI_STATUS st = uefi_call_wrapper(gop->SetMode, 2, gop, try_mode);
        if (st == EFI_SUCCESS)
        {

            UINT32 cur_mode = gop->Mode->Mode;
            UINT32 cur_w = gop->Mode->Info ? gop->Mode->Info->HorizontalResolution : 0;
            UINT32 cur_h = gop->Mode->Info ? gop->Mode->Info->VerticalResolution : 0;

            if (cur_mode == try_mode && cur_w == cand[k].w && cur_h == cand[k].h)
            {
                chosen_mode = try_mode;
                set_ok = 1;

                gop_clear_screen_black();

                serial_mode_line(boot_info, "[UEFI][GOP] Set OK   ", try_mode, cur_w, cur_h, st);
                break;
            }
            else
            {

                serial_write_str(boot_info, "[UEFI][GOP] SetMode returned SUCCESS but mode did not change!\n");
                serial_write_str(boot_info, "[UEFI][GOP] Expected mode=0x");
                serial_hex(boot_info, (uint64_t)try_mode);
                serial_write_str(boot_info, " W=0x");
                serial_hex(boot_info, (uint64_t)cand[k].w);
                serial_write_str(boot_info, " H=0x");
                serial_hex(boot_info, (uint64_t)cand[k].h);
                serial_write_str(boot_info, "\n");

                serial_write_str(boot_info, "[UEFI][GOP] Current  mode=0x");
                serial_hex(boot_info, (uint64_t)cur_mode);
                serial_write_str(boot_info, " W=0x");
                serial_hex(boot_info, (uint64_t)cur_w);
                serial_write_str(boot_info, " H=0x");
                serial_hex(boot_info, (uint64_t)cur_h);
                serial_write_str(boot_info, "\n");
            }
        }
        else
        {
            serial_mode_line(boot_info, "[UEFI][GOP] Set FAIL ", try_mode, cand[k].w, cand[k].h, st);
        }
    }

    if (!set_ok)
    {
        serial_write_str(boot_info, "[UEFI][GOP] WARNING: all SetMode attempts failed, staying native.\n");
        chosen_mode = native_mode;
    }

    (void)chosen_mode;

    framebuffer.BaseAddress = (void *)gop->Mode->FrameBufferBase;
    framebuffer.BufferSize = gop->Mode->FrameBufferSize;
    framebuffer.Width = gop->Mode->Info->HorizontalResolution;
    framebuffer.Height = gop->Mode->Info->VerticalResolution;
    framebuffer.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;

    serial_write_str(boot_info, "[UEFI][GOP] FINAL mode=0x");
    serial_hex(boot_info, (uint64_t)gop->Mode->Mode);
    serial_write_str(boot_info, " W=0x");
    serial_hex(boot_info, (uint64_t)framebuffer.Width);
    serial_write_str(boot_info, " H=0x");
    serial_hex(boot_info, (uint64_t)framebuffer.Height);
    serial_write_str(boot_info, " PPSL=0x");
    serial_hex(boot_info, (uint64_t)framebuffer.PixelsPerScanLine);
    serial_write_str(boot_info, " FB=0x");
    serial_hex(boot_info, (uint64_t)framebuffer.BaseAddress);
    serial_write_str(boot_info, " Size=0x");
    serial_hex(boot_info, (uint64_t)framebuffer.BufferSize);
    serial_write_str(boot_info, "\n");

    return &framebuffer;
}

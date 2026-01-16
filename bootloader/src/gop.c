#include "gop.h"

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static Framebuffer framebuffer;

EFI_STATUS locate_gop(EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3, &GopGuid, NULL, (void **)&gop);
    return status;
}

Framebuffer *get_framebuffer()
{
    if (gop == NULL)
    {
        if (locate_gop(SystemTable) != EFI_SUCCESS)
            return NULL;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN size_of_info, num_modes, native_mode;

    uefi_call_wrapper(gop->QueryMode, 4, gop, gop->Mode == NULL ? 0 : gop->Mode->Mode, &size_of_info, &info);
    num_modes = gop->Mode->MaxMode;
    native_mode = gop->Mode->Mode;

    UINT32 best_width = 0;
    UINT32 best_height = 0;
    UINT32 best_mode = native_mode;

    for (UINT32 i = 0; i < num_modes; i++)
    {
        uefi_call_wrapper(gop->QueryMode, 4, gop, i, &size_of_info, &info);

        if (info->HorizontalResolution > best_width)
        {
            best_width = info->HorizontalResolution;
            best_height = info->VerticalResolution;
            best_mode = i;
        }
    }

    if (best_mode != native_mode)
    {
        uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);

        uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    }
    else
    {
        Print(L"[*] Using Default Video Mode: %dx%d\n", gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution);
    }

    framebuffer.BaseAddress = (void *)gop->Mode->FrameBufferBase;
    framebuffer.BufferSize = gop->Mode->FrameBufferSize;
    framebuffer.Width = gop->Mode->Info->HorizontalResolution;
    framebuffer.Height = gop->Mode->Info->VerticalResolution;
    framebuffer.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;

    return &framebuffer;
}
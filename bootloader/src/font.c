#include "font.h"
#include "file.h"

static int is_valid_psf(Psf1_Header *header)
{

    if (header->magic[0] != 0x36 || header->magic[1] != 0x04)
    {
        return 0;
    }
    return 1;
}

Psf1_Font *load_psf_font(EFI_FILE *directory, CHAR16 *path)
{
    EFI_STATUS status;

    EFI_FILE *font_file = load_file(directory, path);
    if (font_file == NULL)
        return NULL;

    Psf1_Header *header;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(Psf1_Header), (void **)&header);

    UINTN size = sizeof(Psf1_Header);
    uefi_call_wrapper(font_file->Read, 3, font_file, &size, header);

    if (!is_valid_psf(header))
    {
        Print(L"[-] Error: Invalid PSF Font Magic.\n");
        uefi_call_wrapper(font_file->Close, 1, font_file);
        return NULL;
    }

    UINTN glyph_buffer_size;
    if (header->mode == PSF1_MODE512)
    {
        glyph_buffer_size = header->charsize * 512;
    }
    else
    {
        glyph_buffer_size = header->charsize * 256;
    }

    void *glyph_buffer;

    uefi_call_wrapper(font_file->SetPosition, 2, font_file, sizeof(Psf1_Header));

    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, glyph_buffer_size, &glyph_buffer);
    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Failed to allocate font buffer.\n");
        return NULL;
    }

    uefi_call_wrapper(font_file->Read, 3, font_file, &glyph_buffer_size, glyph_buffer);

    uefi_call_wrapper(font_file->Close, 1, font_file);

    Psf1_Font *font_pack;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(Psf1_Font), (void **)&font_pack);

    font_pack->psf_header = header;
    font_pack->glyph_buffer = glyph_buffer;

    return font_pack;
}
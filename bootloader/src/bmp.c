#include "bmp.h"
#include "file.h"

#pragma pack(1)
typedef struct
{
    UINT16 bfType;
    UINT32 bfSize;
    UINT16 bfReserved1;
    UINT16 bfReserved2;
    UINT32 bfOffBits;
} BmpFileHeader;

typedef struct
{
    UINT32 biSize;
    INT32 biWidth;
    INT32 biHeight;
    UINT16 biPlanes;
    UINT16 biBitCount;
    UINT32 biCompression;
    UINT32 biSizeImage;
    INT32 biXPelsPerMeter;
    INT32 biYPelsPerMeter;
    UINT32 biClrUsed;
    UINT32 biClrImportant;
} BmpInfoHeader;
#pragma pack()

SimpleImage *load_bmp_image(EFI_FILE *directory, CHAR16 *path)
{
    EFI_STATUS status;

    EFI_FILE *file = load_file(directory, path);
    if (file == NULL)
        return NULL;

    BmpFileHeader fileHeader;
    BmpInfoHeader infoHeader;
    UINTN size;

    size = sizeof(BmpFileHeader);
    uefi_call_wrapper(file->Read, 3, file, &size, &fileHeader);

    size = sizeof(BmpInfoHeader);
    uefi_call_wrapper(file->Read, 3, file, &size, &infoHeader);

    if (fileHeader.bfType != 0x4D42)
    {
        Print(L"[-] Error: Invalid BMP Magic\n");
        uefi_call_wrapper(file->Close, 1, file);
        return NULL;
    }

    SimpleImage *img;
    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(SimpleImage), (void **)&img);

    img->Width = infoHeader.biWidth;
    img->Height = (infoHeader.biHeight > 0) ? infoHeader.biHeight : -infoHeader.biHeight;
    img->Size = infoHeader.biSizeImage;

    if (img->Size == 0)
    {

        img->Size = img->Width * img->Height * (infoHeader.biBitCount / 8);
    }

    uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, img->Size, (void **)&img->PixelBuffer);

    uefi_call_wrapper(file->SetPosition, 2, file, fileHeader.bfOffBits);

    UINTN sizeToRead = img->Size;
    uefi_call_wrapper(file->Read, 3, file, &sizeToRead, img->PixelBuffer);

    uefi_call_wrapper(file->Close, 1, file);

    return img;
}
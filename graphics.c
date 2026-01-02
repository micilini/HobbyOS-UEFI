#include "graphics.h"
#include "filesystem.h" 
#include "utils.h" 

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gGop = NULL;


#pragma pack(1)
typedef struct {
    UINT16 Type;
    UINT32 Size;
    UINT16 Reserved1;
    UINT16 Reserved2;
    UINT32 Offset;  
} BMP_FILE_HEADER;

typedef struct {
    UINT32 Size;
    INT32  Width;
    INT32  Height;
    UINT16 Planes;
    UINT16 BitCount; 
    UINT32 Compression;
    UINT32 SizeImage;
    INT32  XPelsPerMeter;
    INT32  YPelsPerMeter;
    UINT32 ClrUsed;
    UINT32 ClrImportant;
} BMP_INFO_HEADER;
#pragma pack()


EFI_STATUS InitGOP(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    Status = uefi_call_wrapper(SystemTable->BootServices->LocateProtocol, 3, 
        &GopGuid, NULL, (void**)&gGop);

    if (EFI_ERROR(Status)) {
        Print(L"Error: GOP (Graphics) not supported.\n");
        return Status;
    }
    return EFI_SUCCESS;
}

HOBBY_IMAGE* LoadBMP(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName) {
    EFI_FILE_PROTOCOL *Root = fs_get_root();
    if (!Root) return NULL;

    EFI_FILE_PROTOCOL *File;
    EFI_STATUS Status = uefi_call_wrapper(Root->Open, 5, Root, &File, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"Splash: Image %s not found.\n", FileName);
        return NULL;
    }

    UINT8 HeaderBuffer[54];
    UINTN HeaderSize = 54;
    uefi_call_wrapper(File->Read, 3, File, &HeaderSize, HeaderBuffer);

    BMP_FILE_HEADER *FileHeader = (BMP_FILE_HEADER*)HeaderBuffer;
    BMP_INFO_HEADER *InfoHeader = (BMP_INFO_HEADER*)(HeaderBuffer + 14);

    if (FileHeader->Type != 0x4D42) {
        Print(L"Error: %s is not a valid BMP image.\n", FileName);
        uefi_call_wrapper(File->Close, 1, File);
        return NULL;
    }

    if (InfoHeader->Compression != 0) {
        Print(L"Error: BMP compress (Type %d). Use BMP without compressions.\n", InfoHeader->Compression);
        uefi_call_wrapper(File->Close, 1, File);
        return NULL;
    }

    if (InfoHeader->BitCount != 24 && InfoHeader->BitCount != 32) {
        Print(L"Erro: BMP should be 24 or 32 bits.\n");
        uefi_call_wrapper(File->Close, 1, File);
        return NULL;
    }

    HOBBY_IMAGE *Img;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, sizeof(HOBBY_IMAGE), (void**)&Img);
    Img->Width = (UINT32)(InfoHeader->Width > 0 ? InfoHeader->Width : -InfoHeader->Width);
    Img->Height = (UINT32)(InfoHeader->Height > 0 ? InfoHeader->Height : -InfoHeader->Height);

    UINTN NumPixels = Img->Width * Img->Height;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, 
        NumPixels * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL), (void**)&Img->Pixels);

    uefi_call_wrapper(File->SetPosition, 2, File, FileHeader->Offset);

    UINTN BytesPerPixel = InfoHeader->BitCount / 8;
    UINTN Padding = (4 - ((Img->Width * BytesPerPixel) % 4)) % 4;
    
    UINTN RowSize = (Img->Width * BytesPerPixel) + Padding;
    UINT8 *RowBuffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, RowSize, (void**)&RowBuffer);

    for (INTN y = Img->Height - 1; y >= 0; y--) {
        UINTN ReadSize = RowSize;
        uefi_call_wrapper(File->Read, 3, File, &ReadSize, RowBuffer);

        for (UINTN x = 0; x < Img->Width; x++) {
            UINTN FileIndex = x * BytesPerPixel;
            UINTN ArrayIndex = y * Img->Width + x;

            Img->Pixels[ArrayIndex].Blue  = RowBuffer[FileIndex];
            Img->Pixels[ArrayIndex].Green = RowBuffer[FileIndex + 1];
            Img->Pixels[ArrayIndex].Red   = RowBuffer[FileIndex + 2];
            Img->Pixels[ArrayIndex].Reserved = 0;
        }
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, RowBuffer);
    uefi_call_wrapper(File->Close, 1, File);

    return Img;
}

void FreeImage(EFI_SYSTEM_TABLE *SystemTable, HOBBY_IMAGE* Image) {
    if (Image) {
        if (Image->Pixels) uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Image->Pixels);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Image);
    }
}

void DrawWithOpacity(HOBBY_IMAGE* Img, UINTN X, UINTN Y, UINTN Opacity) {
    if (!gGop) return;

    if (Opacity >= 100) {
        uefi_call_wrapper(gGop->Blt, 10, gGop, Img->Pixels, EfiBltBufferToVideo, 
            0, 0, X, Y, Img->Width, Img->Height, 0);
        return;
    }
}

void ShowSplashScreen(EFI_SYSTEM_TABLE *SystemTable, HOBBY_IMAGE* Img) {
    if (!gGop || !Img) return;

    UINTN ScreenW = gGop->Mode->Info->HorizontalResolution;
    UINTN ScreenH = gGop->Mode->Info->VerticalResolution;

    if (Img->Width > ScreenW || Img->Height > ScreenH) {
        Print(L"Erro: Image (%dx%d) bigger than the screen (%dx%d)!\n", 
              Img->Width, Img->Height, ScreenW, ScreenH);
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 4000000);
        return;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0, 0, 0, 0};
    uefi_call_wrapper(gGop->Blt, 10, gGop, &Black, EfiBltVideoFill, 
        0, 0, 0, 0, ScreenW, ScreenH, 0);

    UINTN X = (ScreenW - Img->Width) / 2;
    UINTN Y = (ScreenH - Img->Height) / 2;

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *FadeBuffer;
    UINTN BufferSize = Img->Width * Img->Height * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BufferSize, (void**)&FadeBuffer);

    for (int op = 0; op <= 100; op += 2) {
        
        for (UINTN i = 0; i < Img->Width * Img->Height; i++) {
            FadeBuffer[i].Red   = (Img->Pixels[i].Red   * op) / 100;
            FadeBuffer[i].Green = (Img->Pixels[i].Green * op) / 100;
            FadeBuffer[i].Blue  = (Img->Pixels[i].Blue  * op) / 100;
            FadeBuffer[i].Reserved = 0;
        }

        uefi_call_wrapper(gGop->Blt, 10, gGop, FadeBuffer, EfiBltBufferToVideo, 
            0, 0, X, Y, Img->Width, Img->Height, 0);
            
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 20000);
    }

    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 2000000);

    for (int op = 100; op >= 0; op -= 2) {
        
        for (UINTN i = 0; i < Img->Width * Img->Height; i++) {
            FadeBuffer[i].Red   = (Img->Pixels[i].Red   * op) / 100;
            FadeBuffer[i].Green = (Img->Pixels[i].Green * op) / 100;
            FadeBuffer[i].Blue  = (Img->Pixels[i].Blue  * op) / 100;
            FadeBuffer[i].Reserved = 0;
        }

        uefi_call_wrapper(gGop->Blt, 10, gGop, FadeBuffer, EfiBltBufferToVideo, 
            0, 0, X, Y, Img->Width, Img->Height, 0);
            
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 10000);
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FadeBuffer);

    uefi_call_wrapper(gGop->Blt, 10, gGop, &Black, EfiBltVideoFill, 
        0, 0, 0, 0, ScreenW, ScreenH, 0);
}

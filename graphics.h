#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <efi.h>
#include <efilib.h>

typedef struct {
    UINT32 Width;
    UINT32 Height;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
} HOBBY_IMAGE;

EFI_STATUS InitGOP(EFI_SYSTEM_TABLE *SystemTable);

HOBBY_IMAGE* LoadBMP(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);

void ShowSplashScreen(EFI_SYSTEM_TABLE *SystemTable, HOBBY_IMAGE* Image);

void FreeImage(EFI_SYSTEM_TABLE *SystemTable, HOBBY_IMAGE* Image);

#endif
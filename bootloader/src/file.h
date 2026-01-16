#ifndef FILE_H
#define FILE_H

#include "boot.h"

EFI_FILE *open_root_volume(EFI_HANDLE ImageHandle);

EFI_FILE *load_file(EFI_FILE *Directory, CHAR16 *Path);

#endif
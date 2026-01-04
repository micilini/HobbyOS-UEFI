#ifndef BMP_H
#define BMP_H

#include "boot.h"
#include "../../shared/protocol.h"

SimpleImage* load_bmp_image(EFI_FILE* directory, CHAR16* path);

#endif
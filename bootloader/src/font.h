#ifndef FONT_H
#define FONT_H

#include "boot.h"

Psf1_Font *load_psf_font(EFI_FILE *directory, CHAR16 *path);

#endif
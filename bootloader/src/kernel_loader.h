#ifndef KERNEL_LOADER_H
#define KERNEL_LOADER_H

#include "boot.h"

void* load_elf_kernel(EFI_FILE* kernel_file);

#endif
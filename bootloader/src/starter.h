#ifndef STARTER_H
#define STARTER_H

#include "boot.h"
#include "font.h"

void start_kernel(EFI_HANDLE ImageHandle, void *entry_point, BootInfo *prepared_boot_info);

#endif

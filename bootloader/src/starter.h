#ifndef STARTER_H
#define STARTER_H

#include "boot.h"
#include "font.h" 

void start_kernel(EFI_HANDLE ImageHandle, void* entry_point, Framebuffer* fb, Psf1_Font* font, void* logo, void* rsdp, MemoryMap* mem_map);

#endif
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#include "mem_map.h"

#define PSF1_MAGIC 0x0436
#define PSF1_MODE512 0x01

typedef struct
{
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} Psf1_Header;

typedef struct
{
    Psf1_Header *psf_header;
    void *glyph_buffer;
} Psf1_Font;

typedef struct
{
    uint32_t Width;
    uint32_t Height;
    uint32_t Size;
    void *PixelBuffer;
} SimpleImage;

typedef struct
{
    void *BaseAddress;
    size_t BufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
} Framebuffer;

typedef struct
{
    Framebuffer *framebuffer;
    Psf1_Font *font;
    SimpleImage *logo;
    void *rsdp;
    MemoryMap *memory_map;
} BootInfo;

#endif
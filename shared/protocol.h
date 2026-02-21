#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#include "mem_map.h"

#define PSF1_MAGIC 0x0436
#define PSF1_MODE512 0x01

#define HOBBYOS_MAX_SERIAL_PORTS 32

#define HOBBYOS_SERIAL_KIND_NONE 0
#define HOBBYOS_SERIAL_KIND_16550_IO 1

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
    uint16_t io_base;
    uint8_t kind;
    uint8_t reserved;
    uint32_t reserved2;
} HobbyOSSerialPort;

typedef struct
{
    uint32_t count;
    HobbyOSSerialPort ports[HOBBYOS_MAX_SERIAL_PORTS];
} HobbyOSSerialInfo;

typedef struct
{
    Framebuffer *framebuffer;
    Psf1_Font *font;
    SimpleImage *logo;
    void *rsdp;
    MemoryMap *memory_map;
    HobbyOSSerialInfo serial;
} BootInfo;

#endif
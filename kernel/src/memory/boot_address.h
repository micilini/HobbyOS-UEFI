#ifndef BOOT_ADDRESS_H
#define BOOT_ADDRESS_H
#include "physical_translation.h"
#include "../../../shared/protocol.h"

typedef struct {
    uint64_t handoff, framebuffer_descriptor, framebuffer, framebuffer_bytes;
    uint64_t map_descriptor, map, map_bytes, map_stride;
    uint64_t font_descriptor, font_header, glyphs, glyph_bytes;
    uint64_t logo_descriptor, pixels, pixel_bytes, rsdp, rsdp_bytes;
} boot_physical_objects_t;

/* Called once at C entry, before reading any handoff field. The returned view
 * belongs to the kernel. Source handoff objects remain unmodified and retained. */
bool physical_boot_adapt(uint64_t handoff, BootInfo **view);
bool physical_boot_activate(void);
const boot_physical_objects_t *physical_boot_objects(void);
#ifdef HOBBYOS_SELFTEST
const BootInfo *physical_boot_view(void);
#endif
#endif

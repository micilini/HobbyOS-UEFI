#include "boot_address.h"
#include "physical_internal.h"
#include "mmio_mapping.h"
#include "firmware_mapping.h"
#include "../acpi/acpi.h"
#include "../libc/memory.h"

static BootInfo g_view;
static MemoryMap g_map;
static uint8_t g_map_bytes[PHYSICAL_DESCRIPTOR_CAPACITY * 256u];
static Framebuffer g_framebuffer;
static Psf1_Font g_font;
static Psf1_Header g_font_header;
static SimpleImage g_logo;
static boot_physical_objects_t g_objects;
static bool g_attempted, g_complete;
static mapping_view_t g_framebuffer_view;
static mapping_view_t g_rsdp_view;
extern char _kernel_start[], _kernel_end[];

/* This seam substitutes only physical backing in hosted boot-object tests. */
#ifdef HOBBYOS_PHYSICAL_HOST
extern const void *physical_host_backing(uint64_t physical, size_t bytes);
static const void *backing(uint64_t physical, size_t bytes)
{ return physical_host_backing(physical, bytes); }
#else
static const void *backing(uint64_t physical, size_t bytes)
{ (void)bytes; return (const void *)(uintptr_t)physical; }
#endif

static bool early_copy(uint64_t physical, void *out, size_t bytes)
{
    if (g_physical_contract.ready || !out || !bytes || physical < 4096 ||
        physical >= PHYSICAL_ENTRY_LIMIT || bytes > PHYSICAL_ENTRY_LIMIT - physical) return false;
    const void *source = backing(physical, bytes);
    if (!source) return false;
    memcpy(out, source, bytes);
    return true;
}

static bool boot_pointer(uint64_t physical, size_t bytes, void **pointer)
{
    uint64_t virtual_address;
    if (!physical || !pointer || !physical_contract_translate(&g_physical_contract,
        physical, bytes, PHYSICAL_FIRMWARE, &virtual_address)) return false;
    const void *value = backing(virtual_address, bytes);
    if (!value) return false;
    *pointer = (void *)value;
    return true;
}

static bool boot_copy(uint64_t physical, void *out, size_t bytes)
{
    void *source;
    if (!boot_pointer(physical, bytes, &source)) return false;
    memcpy(out, source, bytes);
    return true;
}

bool physical_boot_adapt(uint64_t handoff, BootInfo **view)
{
    if (g_attempted || !view) return false;
    g_attempted = true;
    BootInfo raw;
    if (!early_copy(handoff, &raw, sizeof(raw)) || !raw.framebuffer || !raw.memory_map ||
        raw.serial.count > HOBBYOS_MAX_SERIAL_PORTS ||
        !early_copy((uintptr_t)raw.memory_map, &g_map, sizeof(g_map)) ||
        g_map.DescriptorSize < sizeof(EfiMemoryDescriptor) || g_map.DescriptorSize > 256 ||
        !g_map.Size || g_map.Size > sizeof(g_map_bytes) ||
        g_map.Size % g_map.DescriptorSize ||
        g_map.Size / g_map.DescriptorSize > PHYSICAL_DESCRIPTOR_CAPACITY ||
        !early_copy((uintptr_t)g_map.Base, g_map_bytes, g_map.Size)) return false;
    g_objects.handoff = handoff; g_objects.map_descriptor = (uintptr_t)raw.memory_map;
    g_objects.map = (uintptr_t)g_map.Base; g_objects.map_bytes = g_map.Size;
    g_objects.map_stride = g_map.DescriptorSize;
    g_map.Base = (EfiMemoryDescriptor *)g_map_bytes;
    if (!physical_translation_publish(&g_map, (uintptr_t)_kernel_start, (uintptr_t)_kernel_end)) return false;
    void *checked;
    if (!boot_pointer(handoff, sizeof(raw), &checked) ||
        !boot_pointer(g_objects.map_descriptor, sizeof(MemoryMap), &checked) ||
        !boot_pointer(g_objects.map, g_objects.map_bytes, &checked) ||
        !boot_copy((uintptr_t)raw.framebuffer, &g_framebuffer, sizeof(g_framebuffer))) return false;
    uint64_t end;
    if (!g_framebuffer.Width || !g_framebuffer.Height ||
        g_framebuffer.Width > g_framebuffer.PixelsPerScanLine ||
        (uint64_t)g_framebuffer.PixelsPerScanLine * g_framebuffer.Height > UINT64_MAX / 4 ||
        (uint64_t)g_framebuffer.PixelsPerScanLine * g_framebuffer.Height * 4 > g_framebuffer.BufferSize ||
        !g_framebuffer.BaseAddress || !physical_range_valid((uintptr_t)g_framebuffer.BaseAddress,
            g_framebuffer.BufferSize, &end) ||
        !physical_canonical_range((uintptr_t)g_framebuffer.BaseAddress, g_framebuffer.BufferSize)) return false;
    g_objects.framebuffer_descriptor = (uintptr_t)raw.framebuffer;
    g_objects.framebuffer = (uintptr_t)g_framebuffer.BaseAddress;
    g_objects.framebuffer_bytes = g_framebuffer.BufferSize;
    /* The framebuffer may exceed entry coverage; do not expose its CPU pointer
     * until init_paging has installed the existing framebuffer mappings. */
    g_framebuffer.BaseAddress = NULL;
    g_view = raw; g_view.framebuffer = &g_framebuffer; g_view.memory_map = &g_map;
    if (raw.font) {
        if (!boot_copy((uintptr_t)raw.font, &g_font, sizeof(g_font)) ||
            !boot_copy((uintptr_t)g_font.psf_header, &g_font_header, sizeof(g_font_header)) ||
            g_font_header.magic[0] != 0x36 || g_font_header.magic[1] != 0x04 || !g_font_header.charsize) return false;
        g_objects.font_descriptor = (uintptr_t)raw.font;
        g_objects.font_header = (uintptr_t)g_font.psf_header;
        g_objects.glyphs = (uintptr_t)g_font.glyph_buffer;
        g_objects.glyph_bytes = (g_font_header.mode & PSF1_MODE512 ? 512u : 256u) * g_font_header.charsize;
        if (!boot_pointer(g_objects.glyphs, g_objects.glyph_bytes, &g_font.glyph_buffer)) return false;
        g_font.psf_header = &g_font_header; g_view.font = &g_font;
    }
    if (raw.logo) {
        if (!boot_copy((uintptr_t)raw.logo, &g_logo, sizeof(g_logo))) return false;
        uint64_t pixels = (uint64_t)g_logo.Width * g_logo.Height;
        if (!pixels || pixels > UINT64_MAX / 4 || pixels * 3 > g_logo.Size) return false;
        g_objects.logo_descriptor = (uintptr_t)raw.logo;
        g_objects.pixels = (uintptr_t)g_logo.PixelBuffer; g_objects.pixel_bytes = g_logo.Size;
        if (!boot_pointer(g_objects.pixels, g_objects.pixel_bytes, &g_logo.PixelBuffer)) return false;
        g_view.logo = &g_logo;
    }
    if (!raw.rsdp) return false;
    AcpiRsdp rsdp = {0};
    if (!boot_copy((uintptr_t)raw.rsdp, &rsdp, 20)) return false;
    size_t rsdp_bytes = 20;
    if (rsdp.revision >= 2) {
        if (!boot_copy((uintptr_t)raw.rsdp, &rsdp, sizeof(rsdp)) || rsdp.length < sizeof(rsdp) ||
            rsdp.length > 4096) return false;
        rsdp_bytes = rsdp.length;
    }
    g_objects.rsdp = (uintptr_t)raw.rsdp; g_objects.rsdp_bytes = rsdp_bytes;
    if (!boot_pointer(g_objects.rsdp, rsdp_bytes, &g_view.rsdp)) return false;
    g_complete = true; *view = &g_view;
    return true;
}

bool physical_boot_activate(void)
{
    if (!g_complete) return false;
    /* Image-owned descriptor copies stay linked. Only retained backing borrows
     * change domains; firmware outside the RAM map has its own WB/RO owner. */
    if (g_objects.glyph_bytes && !physical_pointer_range(g_objects.glyphs,
        g_objects.glyph_bytes, PHYSICAL_RAM, &g_font.glyph_buffer)) return false;
    if (g_objects.pixel_bytes && !physical_pointer_range(g_objects.pixels,
        g_objects.pixel_bytes, PHYSICAL_RAM, &g_logo.PixelBuffer)) return false;
    uint64_t rsdp_pointer;
    if (physical_contract_translate(&g_physical_contract, g_objects.rsdp,
        g_objects.rsdp_bytes, PHYSICAL_FIRMWARE, &rsdp_pointer)) {
        if (!physical_pointer_range(g_objects.rsdp, g_objects.rsdp_bytes,
            PHYSICAL_FIRMWARE, &g_view.rsdp)) return false;
    } else {
        if (firmware_mapping_acquire(g_objects.rsdp, g_objects.rsdp_bytes, &g_rsdp_view) != MAPPING_OK) return false;
        g_view.rsdp = g_rsdp_view.cpu;
    }
    mmio_origin_t origin;
    if (!mmio_origin_resolve(MMIO_FRAMEBUFFER, g_objects.framebuffer_descriptor, &origin)) return false;
#if defined(HOBBYOS_SELFTEST) && defined(HOBBYOS_FRAMEBUFFER_REFERENCE)
    if (mmio_framebuffer_reference(&origin, &g_framebuffer_view) != MAPPING_OK)
#else
    if (mmio_mapping_acquire(&origin, CACHE_WRITE_COMBINING, &g_framebuffer_view) != MAPPING_OK)
#endif
        return false;
    g_framebuffer.BaseAddress = g_framebuffer_view.cpu;
    return true;
}

const boot_physical_objects_t *physical_boot_objects(void)
{ return g_complete ? &g_objects : NULL; }

#ifdef HOBBYOS_SELFTEST
const BootInfo *physical_boot_view(void)
{ return g_complete ? &g_view : NULL; }
#endif

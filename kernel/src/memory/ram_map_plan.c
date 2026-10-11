#include "ram_map_plan.h"
#include "../libc/memory.h"

static bool ram_type(uint32_t type)
{
    return (type >= EFI_LOADER_CODE && type <= EFI_CONVENTIONAL_MEMORY) ||
        type == EFI_ACPI_RECLAIM_MEMORY || type == EFI_ACPI_MEMORY_NVS;
}

bool ram_features_decode(uint32_t maximum, uint32_t edx, uint32_t address,
                         bool nx_policy, ram_features_t *out)
{
    unsigned width = address & 255;
    if (!out || maximum < UINT32_C(0x80000008) || width < 36 || width > 52 ||
        !(edx & (1u << 29)) || (nx_policy && !(edx & (1u << 20)))) return false;
    *out = (ram_features_t){maximum, edx, address, width, nx_policy,
                            (edx & (1u << 26)) != 0};
    return true;
}

static bool memory_type(unsigned type)
{ return type == 0 || type == 1 || type == 4 || type == 5 || type == 6; }

static bool supported_type(const ram_mtrr_inputs_t *m, unsigned type)
{ return memory_type(type) && (type != 1 || (m->capability & (UINT64_C(1) << 10))); }

/* A disabled range is irrelevant. An enabled mask must describe one aligned
 * power-of-two interval, including on CPUs narrower than the software limit. */
static bool mtrr_range(const ram_mtrr_inputs_t *m, unsigned i, unsigned bits,
                       ram_extent_t *range)
{
    uint64_t width = (UINT64_C(1) << bits) - 1;
    uint64_t address = width & ~UINT64_C(4095), mask = m->mask[i], base = m->base[i];
    if (!(mask & (UINT64_C(1) << 11))) { *range = (ram_extent_t){0}; return true; }
    if ((mask & ~(address | (UINT64_C(1) << 11))) ||
        (base & ~(address | UINT64_C(255))) || !supported_type(m, base & 255)) return false;
    uint64_t size = ((~mask) & address) + 4096;
    uint64_t first = base & address;
    if (!size || (size & (size - 1)) || (first & (size - 1)) ||
        (mask & address) != (address & ~(size - 1)) || size > width - first + 1) return false;
    *range = (ram_extent_t){first, first + size};
    return true;
}

static bool cache_inputs(const ram_mtrr_inputs_t *m, unsigned bits)
{
    if (!m || !m->available || m->count > RAM_MTRR_VARIABLES ||
        m->count != (m->capability & 255) ||
        (m->defaults & ~UINT64_C(0xcff)) || !supported_type(m, m->defaults & 255)) return false;
    if ((m->defaults & (UINT64_C(1) << 10)) && !(m->capability & (UINT64_C(1) << 8))) return false;
    if ((m->defaults & UINT64_C(0xc00)) == UINT64_C(0xc00))
        for (unsigned i = 0; i < 11; ++i)
            for (unsigned j = 0; j < 8; ++j)
                if (!supported_type(m, (m->fixed[i] >> (8 * j)) & 255)) return false;
    for (unsigned i = 0; i < m->count; ++i) {
        ram_extent_t r;
        if (!mtrr_range(m, i, bits, &r)) return false;
    }
    return true;
}

/* Boundaries alone do not define an overlapping type combination. Fixed
 * MTRRs take precedence below one MiB; above it only the architectural UC,
 * identical-type and WB/WT precedence cases admit RAM construction. */
static bool cache_interval_supported(const ram_mtrr_inputs_t *m, unsigned bits,
                                     uint64_t first, uint64_t end)
{
    if (!(m->defaults & UINT64_C(0x800))) return true;
    if ((m->defaults & UINT64_C(0x400)) && first < UINT64_C(0x100000)) {
        if (end <= UINT64_C(0x100000)) return true;
        first = UINT64_C(0x100000);
    }
    unsigned types = 0;
    for (unsigned i = 0; i < m->count; ++i) {
        ram_extent_t range;
        if (!mtrr_range(m, i, bits, &range)) return false;
        if (range.first <= first && first < range.end)
            types |= 1u << (m->base[i] & 255);
    }
    return !types || !(types & (types - 1)) || (types & 1) ||
        types == ((1u << 4) | (1u << 6));
}

static bool intersects(uint64_t first, uint64_t end, ram_extent_t r)
{ return r.first < end && first < r.end; }

static uint64_t boundary(uint64_t cursor, uint64_t end, uint64_t point)
{ return point > cursor && point < end ? point : end; }

bool ram_map_plan_create(ram_map_plan_t *p, const physical_contract_t *c,
                        const ram_features_t *f, const ram_mtrr_inputs_t *m,
                        uint64_t fb, uint64_t fb_bytes)
{
    if (!p || p->ready || !c || !c->ready || !f || f->physical_bits < 36 ||
        f->physical_bits > 52 || !cache_inputs(m, f->physical_bits)) return false;
    uint64_t fb_end = 0;
    if (fb_bytes && (!physical_range_valid(fb, fb_bytes, &fb_end) ||
                    fb_end > PHYSICAL_ADDRESS_LIMIT - 4095)) return false;
    memset(p, 0, sizeof(*p));
    p->features = *f;
    p->image_first = c->image_first - KERNEL_IMAGE_OFFSET;
    p->image_end = (c->image_end - KERNEL_IMAGE_OFFSET + 4095) & ~UINT64_C(4095);
    if (fb_bytes) p->framebuffer = (ram_extent_t){fb & ~UINT64_C(4095),
                                                (fb_end + 4095) & ~UINT64_C(4095)};
    if (intersects(p->image_first, p->image_end, p->framebuffer)) return false;
    if (m->defaults & (UINT64_C(1) << 11))
        for (unsigned i = 0; i < m->count; ++i) {
            ram_extent_t r;
            if (!mtrr_range(m, i, f->physical_bits, &r)) return false;
            if (r.end) p->cache_ranges[p->cache_count++] = r;
        }
    for (unsigned i = 0; i < c->count; ++i) {
        const physical_region_t *r = &c->regions[i];
        if (!ram_type(r->type)) continue;
        if (r->end > RAM_DIRECT_BYTES || r->end > (UINT64_C(1) << f->physical_bits)) return false;
        uint64_t cursor = r->first;
        while (cursor < r->end) {
            if (cursor >= p->framebuffer.first && cursor < p->framebuffer.end) {
                cursor = p->framebuffer.end < r->end ? p->framebuffer.end : r->end;
                continue;
            }
            uint64_t end = boundary(cursor, r->end, UINT64_C(0x200000));
            end = boundary(cursor, end, p->image_first);
            end = boundary(cursor, end, p->image_end);
            end = boundary(cursor, end, p->framebuffer.first);
            for (unsigned j = 0; j < p->cache_count; ++j) {
                end = boundary(cursor, end, p->cache_ranges[j].first);
                end = boundary(cursor, end, p->cache_ranges[j].end);
            }
            if (!cache_interval_supported(m, f->physical_bits, cursor, end)) return false;
            if (p->count == RAM_MAP_INTERVALS) return false;
            unsigned shift = cursor < UINT64_C(0x200000) ||
                (cursor < p->image_end && end > p->image_first) ? 12 : (f->page1g ? 30 : 21);
            p->intervals[p->count++] = (ram_interval_t){cursor, end, r->attributes, r->type, shift};
            p->bytes += end - cursor;
            p->ceiling = end;
            cursor = end;
        }
    }
    if (!p->count) return false;
    p->ready = true;
    if (!ram_map_plan_verify(p, c, m, (ram_extent_t){fb_bytes ? fb & ~UINT64_C(4095) : 0, fb_bytes ? (fb_end + 4095) & ~UINT64_C(4095) : 0})) { p->ready = false; return false; }
    return true;
}

const ram_interval_t *ram_map_interval(const ram_map_plan_t *p, uint64_t physical)
{
    if (!p || !p->ready) return NULL;
    unsigned lo = 0, hi = p->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (p->intervals[mid].end <= physical) lo = mid + 1; else hi = mid;
    }
    return lo < p->count && p->intervals[lo].first <= physical ? &p->intervals[lo] : NULL;
}

bool ram_map_contains(const ram_map_plan_t *p, uint64_t first, uint64_t bytes)
{
    if (!p || !p->ready || !bytes || first >= RAM_DIRECT_BYTES || bytes > RAM_DIRECT_BYTES-first) return false;
    uint64_t end = first + bytes, cursor = first;
    const ram_interval_t *r = ram_map_interval(p, first);
    if (!r) return false;
    unsigned i = (unsigned)(r - p->intervals);
    while (cursor < end && i < p->count) {
        r = &p->intervals[i++];
        if (r->first > cursor || r->end <= cursor) return false;
        cursor = r->end < end ? r->end : end;
    }
    return cursor == end;
}

bool ram_map_pointer(const ram_map_plan_t *p, uint64_t first, uint64_t bytes, uint64_t *pointer)
{
    if (!pointer || !ram_map_contains(p, first, bytes)) return false;
    *pointer = RAM_DIRECT_BASE + first;
    return true;
}

bool ram_map_inverse(const ram_map_plan_t *p, uint64_t pointer, uint64_t bytes, uint64_t *physical)
{
    if (!physical || pointer < RAM_DIRECT_BASE || pointer >= RAM_DIRECT_END ||
        !ram_map_contains(p, pointer - RAM_DIRECT_BASE, bytes)) return false;
    *physical = pointer - RAM_DIRECT_BASE;
    return true;
}

unsigned ram_map_leaf_shift(const ram_map_plan_t *p, const ram_interval_t *r, uint64_t physical)
{
    if (!p || !r || physical < r->first || physical >= r->end) return 0;
    if (p->features.page1g && r->maximum_shift >= 30 && !(physical & UINT64_C(0x3fffffff)) &&
        r->end - physical >= UINT64_C(0x40000000)) return 30;
    if (r->maximum_shift >= 21 && !(physical & UINT64_C(0x1fffff)) &&
        r->end - physical >= UINT64_C(0x200000)) return 21;
    return 12;
}

/* Reconcile against each original descriptor, not a builder byte/count total.
 * Boundaries are checked again against raw enabled MTRR masks, independently
 * of the cached boundary list used by construction. */
bool ram_map_plan_verify(const ram_map_plan_t *p, const physical_contract_t *c,
                        const ram_mtrr_inputs_t *m, ram_extent_t authorized_framebuffer)
{
    if (!p || !p->ready || !c || !c->ready || !p->count || p->count > RAM_MAP_INTERVALS ||
        !cache_inputs(m, p->features.physical_bits) ||
        p->framebuffer.first != authorized_framebuffer.first ||
        p->framebuffer.end != authorized_framebuffer.end) return false;
    ram_features_t decoded;
    if (!ram_features_decode(p->features.extended_max,p->features.extended_edx,
        p->features.address_eax,p->features.nx,&decoded) ||
        decoded.physical_bits != p->features.physical_bits || decoded.page1g != p->features.page1g) return false;
    unsigned at = 0;
    uint64_t bytes = 0, last = 0;
    for (unsigned i = 0; i < c->count; ++i) {
        const physical_region_t *d = &c->regions[i];
        if (!ram_type(d->type)) continue;
        uint64_t cursor = d->first;
        while (cursor < d->end) {
            if (cursor >= p->framebuffer.first && cursor < p->framebuffer.end) {
                cursor = p->framebuffer.end < d->end ? p->framebuffer.end : d->end;
                continue;
            }
            if (at == p->count) return false;
            const ram_interval_t *r = &p->intervals[at++];
            if (r->first != cursor || r->end <= cursor || r->end > d->end ||
                r->first < last || ((r->first | r->end) & 4095) ||
                r->type != d->type || r->attributes != d->attributes ||
                r->end > RAM_DIRECT_BYTES || r->end > (UINT64_C(1) << p->features.physical_bits) ||
                intersects(r->first, r->end, p->framebuffer) ||
                !cache_interval_supported(m, p->features.physical_bits, r->first, r->end)) return false;
            bool small = r->first < UINT64_C(0x200000) ||
                (r->first < p->image_end && p->image_first < r->end);
            if (r->maximum_shift != (small ? 12 : (p->features.page1g ? 30 : 21))) return false;
            if (m->defaults & (UINT64_C(1) << 11))
                for (unsigned j = 0; j < m->count; ++j) {
                    ram_extent_t raw;
                    if (!mtrr_range(m, j, p->features.physical_bits, &raw) ||
                        (raw.first > r->first && raw.first < r->end) ||
                        (raw.end > r->first && raw.end < r->end)) return false;
                }
            bytes += r->end - r->first;
            last = cursor = r->end;
        }
    }
    return at == p->count && bytes == p->bytes && last == p->ceiling &&
        p->image_first == c->image_first - KERNEL_IMAGE_OFFSET &&
        p->image_end == ((c->image_end - KERNEL_IMAGE_OFFSET + 4095) & ~UINT64_C(4095)) &&
        ram_map_contains(p, p->image_first, p->image_end - p->image_first);
}

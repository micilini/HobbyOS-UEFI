#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/memory/page_table_decode.h"
#include "../kernel/src/memory/memory_observe_decode.h"

static uint64_t tables[4][512], saved[4][512];
static unsigned reads, denied, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "fixture line %u: %s\n", __LINE__, #x); return 1; } } while (0)
#define B(n) (UINT64_C(1) << (n))

static bool read_entry(void *unused, uint64_t table, uint16_t index, uint64_t *value)
{
    (void)unused;
    reads++;
    if (table < 0x1000 || table > 0x4000 || (table & 4095) || index > 511 ||
        denied == table / 4096) return false;
    *value = tables[table / 4096 - 1][index];
    return true;
}

static pt_controls_t controls(void)
{
    return (pt_controls_t){.cr0 = B(31) | B(16), .cr3 = 0x1018,
        .cr4 = B(5), .efer = B(10) | B(11), .pat = UINT64_C(0x0007040600070406),
        .physical_bits = 48, .efer_valid = true, .pat_supported = true,
        .pat_valid = true, .nx_supported = true, .page1g_supported = true};
}

static void mapping(uint64_t va, unsigned leaf, uint64_t raw)
{
    memset(tables, 0, sizeof(tables)); reads = 0; denied = 0;
    for (unsigned level = 4; level >= leaf; level--)
    {
        unsigned table = 4 - level, index = (va >> (12 + 9 * (level - 1))) & 511;
        tables[table][index] = level == leaf ? raw : (table + 2) * 4096 + 7;
    }
}

static void put64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(value >> (8 * i));
}

int main(void)
{
    pt_controls_t c = controls(); pt_result_t r; uint64_t value;
    CHECK(pt_parse_address("0", &value) && value == 0);
    CHECK(pt_parse_address("0xffffffffffffffff", &value) && value == UINT64_MAX);
    CHECK(pt_parse_address("18446744073709551615", &value) && value == UINT64_MAX);
    const char *bad[] = {"", "0x", "00", "010", "-1", "+1", " 1", "1 ", "1junk",
        "0x10000000000000000", "18446744073709551616", "0b100"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) CHECK(!pt_parse_address(bad[i], &value));
    static const uint64_t vas[] = {0, 0x123, 0x7fffffff, 0x80000000, 0x80000001,
        0xffffffff, UINT64_C(0x100000000), UINT64_C(0xffff800000000abc), UINT64_MAX};
    static const unsigned expected_pat[] = {6, 4, 7, 0, 6, 4, 7, 0};
    for (unsigned level = 1; level <= 3; level++)
    for (unsigned a = 0; a < sizeof(vas) / sizeof(*vas); a++)
    for (unsigned pat = 0; pat < 8; pat++)
    {
        uint64_t size = level == 1 ? 4096 : level == 2 ? 2097152 : 1073741824;
        uint64_t base = level == 3 ? UINT64_C(0x80000000) : UINT64_C(0x2000000);
        uint64_t raw = base | 7 | ((pat & 3) << 3) |
            ((pat & 4) ? B(level == 1 ? 7 : 12) : 0) | (level > 1 ? B(7) : 0);
        mapping(vas[a], level, raw); memcpy(saved, tables, sizeof(saved));
        page_table_decode(vas[a], &c, read_entry, 0, &r);
        CHECK(r.status == PT_MAPPED && r.page_size == size && r.leaf_level == level);
        CHECK(r.physical == base + vas[a] % size && r.offset == vas[a] % size);
        CHECK(r.pat_index == pat && r.pat_type_valid && r.pat_type == expected_pat[pat]);
        CHECK(r.count == 5 - level && reads == 5 - level);
        CHECK(!memcmp(tables, saved, sizeof(saved)));
        CHECK((r.entries[r.count - 1].applicable & PT_FIELD_PAT) != 0);
        CHECK(level != 1 || !(r.entries[r.count - 1].applicable & PT_FIELD_PS));
    }
    mapping(0, 1, 3); page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_MAPPED && r.physical == 0);
    for (unsigned level = 1; level <= 4; level++)
    {
        mapping(0, level, UINT64_MAX & ~UINT64_C(1));
        page_table_decode(0, &c, read_entry, 0, &r);
        CHECK(r.status == PT_NOT_PRESENT && r.stopped_level == level && reads == 5 - level);
        CHECK(r.entries[r.count - 1].raw == (UINT64_MAX & ~UINT64_C(1)));
    }
    for (unsigned level = 0; level < 4; level++)
    {
        mapping(0, 1, 7); tables[level][0] &= ~UINT64_C(6); tables[level][0] |= B(63);
        page_table_decode(0, &c, read_entry, 0, &r);
        CHECK(r.status == PT_MAPPED && !r.rw && !r.user && r.nx && r.supervisor_write_protected);
        c.cr0 &= ~B(16); page_table_decode(0, &c, read_entry, 0, &r);
        CHECK(r.status == PT_MAPPED && !r.supervisor_write_protected);
        c = controls(); c.efer &= ~B(11); page_table_decode(0, &c, read_entry, 0, &r);
        CHECK(r.status == PT_MALFORMED && r.stopped_level == 4 - level); c = controls();
    }
    mapping(0, 1, 3); denied = 3; page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_UNAVAILABLE && r.stopped_level == 2 && r.count == 2);
    denied = 0; c.cr4 |= B(12); page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_UNSUPPORTED_MODE); c = controls();
    page_table_decode(B(48), &c, read_entry, 0, &r); CHECK(r.status == PT_NONCANONICAL);
    c.cr4 |= B(12); page_table_decode(B(56), &c, read_entry, 0, &r); CHECK(r.status == PT_NONCANONICAL);
    c = controls(); c.efer_valid = false; page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_UNSUPPORTED_MODE); c = controls();
    c.pat_valid = false; page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_MAPPED && !r.pat_type_valid); c = controls();
    c.pat = 2; page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_MAPPED && !r.pat_type_valid && r.pat_type == 2); c.pat = 255;
    page_table_decode(0, &c, read_entry, 0, &r); CHECK(!r.pat_type_valid && r.pat_type == 255); c = controls();
    mapping(0, 4, 0x2087); page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_MALFORMED && r.count == 1);
    for (unsigned level = 2; level <= 3; level++)
    {
        mapping(123, level, 0x2083); page_table_decode(123, &c, read_entry, 0, &r);
        CHECK(r.status == PT_MALFORMED && r.stopped_level == level);
    }
    mapping(0, 3, 0x83); c.page1g_supported = false; page_table_decode(0, &c, read_entry, 0, &r);
    CHECK(r.status == PT_MALFORMED); c = controls();
    mapping(0, 1, B(48) | 3); page_table_decode(0, &c, read_entry, 0, &r); CHECK(r.status == PT_MALFORMED);
    c.physical_bits = 52; page_table_decode(0, &c, read_entry, 0, &r); CHECK(r.status == PT_MAPPED && r.physical == B(48));
    c.physical_bits = 53; page_table_decode(0, &c, read_entry, 0, &r); CHECK(r.status == PT_UNSUPPORTED_MODE);
    c = controls(); c.cr3 |= B(48); page_table_decode(0, &c, read_entry, 0, &r); CHECK(r.status == PT_MALFORMED);

    unsigned char map[128] = {0}; memory_descriptor_t d;
    put64(map, 0xfeed); put64(map + 8, B(40)); put64(map + 16, B(41)); put64(map + 24, 7); put64(map + 32, B(63) | 8);
    memset(map + 40, 0xa7, 24); put64(map + 64, 7); put64(map + 72, 0x1000); put64(map + 88, 2);
    CHECK(memory_descriptor_decode(map, sizeof(map), 64, 0, &d));
    CHECK(d.type == 0xfeed && d.physical == B(40) && d.end == B(40) + 7 * 4096 && d.virtual_address == B(41));
    CHECK(d.attributes == (B(63) | 8) && !strcmp(memory_efi_type(d.type), "Unknown"));
    CHECK(memory_descriptor_decode(map, sizeof(map), 64, 1, &d) && d.physical == 4096 && d.end == 12288);
    CHECK(!memory_descriptor_decode(map, sizeof(map), 64, 2, &d));
    CHECK(!memory_map_shape(1, 128, 0, &value)); CHECK(!memory_map_shape(1, 128, 8, &value));
    CHECK(!memory_map_shape(1, 127, 64, &value)); CHECK(!memory_map_shape(UINT64_MAX - 5, 128, 64, &value));
    put64(map + 24, UINT64_MAX); CHECK(memory_descriptor_decode(map, sizeof(map), 64, 0, &d) && !d.range_valid);
    put64(map + 24, 2); put64(map + 8, UINT64_MAX & ~UINT64_C(4095));
    CHECK(memory_descriptor_decode(map, sizeof(map), 64, 0, &d) && !d.range_valid);
    uint64_t start, end;
    CHECK(memory_interval_overlap(0, 10, 5, 20, &start, &end) && start == 5 && end == 10);
    CHECK(!memory_interval_overlap(0, 10, 10, 20, &start, &end));
    CHECK(!memory_interval_overlap(10, 0, 5, 20, &start, &end));
    for (unsigned bits = 32; bits <= 52; bits++)
    {
        memory_mtrr_range_t m;
        uint64_t mask = ((B(bits) - 1) & ~UINT64_C(0x1fffff)) | 0x800;
        memory_mtrr_decode(0x400006, mask, bits, true, true, &m);
        CHECK(m.valid && m.enabled && m.base == 0x400000 && m.end == 0x600000 && m.type == 6);
        memory_mtrr_decode(0x401006, mask, bits, true, true, &m); CHECK(!m.valid);
        memory_mtrr_decode(0x400006, mask ^ B(30), bits, true, true, &m); CHECK(!m.valid);
        memory_mtrr_decode(0x400006, mask & ~B(11), bits, true, true, &m); CHECK(m.valid && !m.enabled);
        memory_mtrr_decode(0x400006, mask, bits, false, true, &m); CHECK(!m.valid && !strcmp(m.reason, "unsupported"));
        memory_mtrr_decode(0x400006, mask, bits, true, false, &m); CHECK(!m.valid && !strcmp(m.reason, "read_failed"));
        memory_mtrr_decode(B(bits) | 6, mask, bits, true, true, &m); CHECK(!m.valid);
    }
    printf("[MEMORY_HOST] kind=hosted_fixtures checks=%u failures=0 decoder=shared_c\n", checks);
    return 0;
}

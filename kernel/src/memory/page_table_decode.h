#ifndef PAGE_TABLE_DECODE_H
#define PAGE_TABLE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PT_MAPPED, PT_NOT_PRESENT, PT_NONCANONICAL, PT_UNSUPPORTED_MODE,
    PT_MALFORMED, PT_UNAVAILABLE
} pt_status_t;

enum {
    PT_FIELD_P = 1u, PT_FIELD_RW = 2u, PT_FIELD_US = 4u,
    PT_FIELD_PWT = 8u, PT_FIELD_PCD = 16u, PT_FIELD_A = 32u,
    PT_FIELD_D = 64u, PT_FIELD_PS = 128u, PT_FIELD_PAT = 256u,
    PT_FIELD_G = 512u, PT_FIELD_NX = 1024u
};

typedef struct {
    uint64_t cr0, cr3, cr4, efer, pat;
    uint8_t physical_bits;
    bool efer_valid, pat_supported, pat_valid, nx_supported, page1g_supported;
} pt_controls_t;

typedef struct {
    uint64_t table, raw;
    uint16_t index, fields, applicable;
    uint8_t level;
} pt_entry_t;

typedef struct {
    uint64_t va, root, base, offset, physical, page_size;
    pt_entry_t entries[4];
    pt_status_t status;
    const char *reason;
    uint8_t count, stopped_level, leaf_level, pat_index, pat_type;
    bool rw, user, nx, supervisor_write_protected, pat_type_valid;
} pt_result_t;

/* The callback reads one aligned entry, never the queried VA or mapped data. */
typedef bool (*pt_read_entry_t)(void *context, uint64_t table,
                               uint16_t index, uint64_t *value);
void page_table_decode(uint64_t va, const pt_controls_t *controls,
                       pt_read_entry_t read_entry, void *context,
                       pt_result_t *out);
const char *pt_status_name(pt_status_t status);
const char *pt_memory_type_name(uint8_t type);
bool pt_canonical(uint64_t address, unsigned bits);
bool pt_parse_address(const char *text, uint64_t *out);

#endif

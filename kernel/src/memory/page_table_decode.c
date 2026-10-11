#include "page_table_decode.h"

#define BIT(n) (UINT64_C(1) << (n))
#define ADDRESS_BITS UINT64_C(0x000ffffffffff000)

bool pt_canonical(uint64_t address, unsigned bits)
{
    if (bits != 48 && bits != 57)
        return false;
    uint64_t upper = address >> bits;
    return upper == ((address & BIT(bits - 1)) ? (UINT64_MAX >> bits) : 0);
}

bool pt_parse_address(const char *text, uint64_t *out)
{
    if (!text || !out || !*text)
        return false;
    unsigned radix = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        radix = 16;
        text += 2;
    }
    else if (text[0] == '0' && text[1])
        return false;
    if (!*text)
        return false;
    uint64_t value = 0;
    for (; *text; text++)
    {
        unsigned digit;
        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a') + 10;
        else if (*text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A') + 10;
        else return false;
        if (digit >= radix || value > (UINT64_MAX - digit) / radix)
            return false;
        value = value * radix + digit;
    }
    *out = value;
    return true;
}

const char *pt_status_name(pt_status_t status)
{
    switch (status) {
    case PT_MAPPED: return "MAPPED";
    case PT_NOT_PRESENT: return "NOT_PRESENT";
    case PT_NONCANONICAL: return "NONCANONICAL";
    case PT_UNSUPPORTED_MODE: return "UNSUPPORTED_MODE";
    case PT_MALFORMED: return "MALFORMED";
    default: return "UNAVAILABLE";
    }
}

const char *pt_memory_type_name(uint8_t type)
{
    switch (type) {
    case 0: return "UC";
    case 1: return "WC";
    case 4: return "WT";
    case 5: return "WP";
    case 6: return "WB";
    case 7: return "UC_MINUS";
    default: return "RESERVED";
    }
}

void page_table_decode(uint64_t va, const pt_controls_t *c,
                       pt_read_entry_t reader, void *context, pt_result_t *r)
{
    *r = (pt_result_t){.va = va, .status = PT_UNSUPPORTED_MODE,
                         .reason = "controls_unavailable", .rw = true,
                         .user = true, .stopped_level = 4};
    if (!c || !reader)
        return;
    if (!pt_canonical(va, c->cr4 & BIT(12) ? 57 : 48))
    {
        r->status = PT_NONCANONICAL;
        r->reason = "active_canonical_width";
        return;
    }
    if (c->cr4 & BIT(12)) { r->reason = "la57_active"; return; }
    if (!c->efer_valid) return;
    if (!(c->cr0 & BIT(31)) || !(c->cr4 & BIT(5)) || !(c->efer & BIT(10)))
    { r->reason = "not_four_level_ia32e"; return; }
    if (c->physical_bits < 32 || c->physical_bits > 52)
    { r->reason = "physical_width"; return; }
    uint64_t physical_mask = (BIT(c->physical_bits) - 1) & ADDRESS_BITS;
    r->root = c->cr3 & physical_mask;
    r->status = PT_MALFORMED;
    if (c->cr3 & ~(physical_mask | UINT64_C(0xfff)))
    { r->reason = "cr3_reserved_address"; return; }
    if ((c->efer & BIT(11)) && !c->nx_supported)
    { r->reason = "nxe_without_capability"; return; }
    uint64_t table = r->root;
    for (unsigned level = 4; level; level--)
    {
        uint16_t index = (uint16_t)((va >> (12 + 9 * (level - 1))) & 511);
        uint64_t raw;
        r->stopped_level = (uint8_t)level;
        if (!reader(context, table, index, &raw))
        { r->status = PT_UNAVAILABLE; r->reason = "unsafe_table_access"; return; }
        pt_entry_t *entry = &r->entries[r->count++];
        *entry = (pt_entry_t){.table = table, .raw = raw, .index = index,
                               .level = (uint8_t)level, .applicable = PT_FIELD_P,
                               .fields = (uint16_t)(raw & 1)};
        if (!(raw & 1))
        { r->status = PT_NOT_PRESENT; r->reason = "present_clear"; return; }
        bool large = level > 1 && level < 4 && (raw & BIT(7));
        bool leaf = level == 1 || large;
        entry->applicable |= PT_FIELD_RW | PT_FIELD_US | PT_FIELD_PWT |
                             PT_FIELD_PCD | PT_FIELD_A | PT_FIELD_NX;
        entry->fields |= (uint16_t)(raw & UINT64_C(0x3e));
        if (raw & BIT(63)) entry->fields |= PT_FIELD_NX;
        if (level == 2 || level == 3)
        {
            entry->applicable |= PT_FIELD_PS;
            if (large) entry->fields |= PT_FIELD_PS;
        }
        if (leaf)
        {
            entry->applicable |= PT_FIELD_D | PT_FIELD_PAT | PT_FIELD_G;
            if (raw & BIT(6)) entry->fields |= PT_FIELD_D;
            if (raw & BIT(8)) entry->fields |= PT_FIELD_G;
            if (raw & BIT(large ? 12 : 7)) entry->fields |= PT_FIELD_PAT;
        }
        if ((raw & ADDRESS_BITS & ~physical_mask) ||
            ((raw & BIT(63)) && !(c->efer & BIT(11))))
        { r->reason = "reserved_address_or_nx"; return; }
        if (level == 4 && (raw & BIT(7)))
        { r->reason = "pml4_ps_reserved"; return; }
        if (level == 3 && large && !c->page1g_supported)
        { r->reason = "page1g_unsupported"; return; }
        r->rw = r->rw && (raw & BIT(1));
        r->user = r->user && (raw & BIT(2));
        r->nx = r->nx || (raw & BIT(63));
        if (!leaf) { table = raw & physical_mask; continue; }
        uint64_t size = BIT(12 + 9 * (level - 1));
        if (large && (raw & (size - 1) & ~UINT64_C(0x1fff)))
        { r->reason = "large_leaf_alignment_reserved"; return; }
        r->leaf_level = (uint8_t)level;
        r->page_size = size;
        r->base = raw & physical_mask & ~(size - 1);
        r->offset = va & (size - 1);
        r->physical = r->base | r->offset;
        r->supervisor_write_protected = !r->rw && (c->cr0 & BIT(16));
        r->pat_index = (uint8_t)(((entry->fields & PT_FIELD_PAT) ? 4 : 0) |
                                ((raw >> 3) & 3));
        if ((entry->fields & PT_FIELD_PAT) && !c->pat_supported)
        { r->reason = "pat_selector_unsupported"; return; }
        if (c->pat_supported && c->pat_valid)
        {
            r->pat_type = (uint8_t)(c->pat >> (r->pat_index * 8));
            r->pat_type_valid = r->pat_type == 0 || r->pat_type == 1 ||
                (r->pat_type >= 4 && r->pat_type <= 7);
        }
        r->status = PT_MAPPED;
        r->reason = "leaf";
        return;
    }
}

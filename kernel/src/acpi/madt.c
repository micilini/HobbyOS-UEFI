#include "madt.h"
#include <stddef.h>

static MadtTable* g_madt = NULL;

void init_madt() {
    
    g_madt = (MadtTable*)acpi_find_table(ACPI_SIG_MADT);
}


uint64_t get_lapic_base() {
    if (!g_madt) init_madt();
    if (!g_madt) return 0;
    
    return g_madt->lapic_address;
}


uint64_t get_ioapic_base() {
    if (!g_madt) init_madt();
    if (!g_madt) return 0;

    uint8_t* ptr = (uint8_t*)g_madt + sizeof(MadtTable);
    uint8_t* end = (uint8_t*)g_madt + g_madt->header.length;

    while (ptr < end) {
        MadtEntryHeader* entry = (MadtEntryHeader*)ptr;

        if (entry->type == MADT_TYPE_IO_APIC) {
            MadtIoApicEntry* ioapic = (MadtIoApicEntry*)ptr;
            return ioapic->ioapic_address;
        }

        
        ptr += entry->length;
    }

    return 0; 
}
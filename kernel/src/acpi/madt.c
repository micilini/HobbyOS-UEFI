#include "madt.h"
#include <stddef.h>
#include "../graphics/console.h" 

static MadtTable *g_madt = NULL;

void init_madt()
{
    g_madt = (MadtTable *)acpi_find_table(ACPI_SIG_MADT);
}

uint64_t get_lapic_base()
{
    if (!g_madt)
        init_madt();
    if (!g_madt)
        return 0;

    return g_madt->lapic_address;
}

uint64_t get_ioapic_base()
{
    if (!g_madt)
        init_madt();
    if (!g_madt)
        return 0;

    uint8_t *ptr = (uint8_t *)g_madt + sizeof(MadtTable);
    uint8_t *end = (uint8_t *)g_madt + g_madt->header.length;

    while (ptr < end)
    {
        if ((end - ptr) < (int)sizeof(MadtEntryHeader)) break;

        MadtEntryHeader *entry = (MadtEntryHeader *)ptr;

        if (entry->length < sizeof(MadtEntryHeader)) break;
        if (ptr + entry->length > end) break;

        if (entry->type == MADT_TYPE_IO_APIC)
        {
            if (entry->length < sizeof(MadtIoApicEntry)) break;
            MadtIoApicEntry *ioapic = (MadtIoApicEntry *)ptr;
            return ioapic->ioapic_address;
        }

        ptr += entry->length;
    }

    return 0;
}



uint32_t madt_get_cpu_count()
{
    if (!g_madt)
        init_madt();
    
    if (!g_madt)
        return 1;

    uint32_t count = 0;
    uint8_t *ptr = (uint8_t *)g_madt + sizeof(MadtTable);
    uint8_t *end = (uint8_t *)g_madt + g_madt->header.length;

    while (ptr < end)
    {
        MadtEntryHeader *entry = (MadtEntryHeader *)ptr;

        if ((end - ptr) < (int)sizeof(MadtEntryHeader)) break;
        if (entry->length < sizeof(MadtEntryHeader)) break;

        if (entry->type == MADT_TYPE_PROCESSOR_LOCAL_APIC)
        {
            MadtProcessorEntry *proc = (MadtProcessorEntry *)ptr;
            
            
            if (proc->flags & MADT_FLAG_ENABLED)
            {
                count++;
            }
            
            
        }

        ptr += entry->length;
    }

    return (count > 0) ? count : 1;
}

uint32_t madt_get_cpu_apic_ids(uint8_t *buffer, uint32_t max_count)
{
    if (!g_madt) return 0;
    if (!buffer) return 0;

    uint32_t count = 0;
    uint8_t *ptr = (uint8_t *)g_madt + sizeof(MadtTable);
    uint8_t *end = (uint8_t *)g_madt + g_madt->header.length;

    while (ptr < end && count < max_count)
    {
        MadtEntryHeader *entry = (MadtEntryHeader *)ptr;

        if ((end - ptr) < (int)sizeof(MadtEntryHeader)) break;
        
        if (entry->type == MADT_TYPE_PROCESSOR_LOCAL_APIC)
        {
            MadtProcessorEntry *proc = (MadtProcessorEntry *)ptr;
            
            if (proc->flags & MADT_FLAG_ENABLED)
            {
                buffer[count++] = proc->apic_id;
            }
        }

        ptr += entry->length;
    }
    
    return count;
}
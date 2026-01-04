#ifndef MADT_H
#define MADT_H

#include "acpi.h"


#define MADT_TYPE_PROCESSOR_LOCAL_APIC 0
#define MADT_TYPE_IO_APIC 1
#define MADT_TYPE_ISO 2 


typedef struct {
    AcpiSdtHeader header;
    uint32_t lapic_address; 
    uint32_t flags;
} __attribute__((packed)) MadtTable;


typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) MadtEntryHeader;


typedef struct {
    MadtEntryHeader header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags; 
} __attribute__((packed)) MadtProcessorEntry;


typedef struct {
    MadtEntryHeader header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address; 
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) MadtIoApicEntry;


void init_madt();
uint64_t get_lapic_base();
uint64_t get_ioapic_base();

#endif
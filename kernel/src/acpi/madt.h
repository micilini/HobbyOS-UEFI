#ifndef MADT_H
#define MADT_H

#include "acpi.h"
#include <stdbool.h>
#include <stdint.h>

#define MADT_TYPE_PROCESSOR_LOCAL_APIC 0u
#define MADT_TYPE_IO_APIC 1u
#define MADT_TYPE_ISO 2u
#define MADT_TYPE_LOCAL_APIC_NMI 4u
#define MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE 5u
#define MADT_TYPE_PROCESSOR_LOCAL_X2APIC 9u
#define MADT_TYPE_LOCAL_X2APIC_NMI 10u

#define MADT_FLAG_ENABLED (1u << 0)
#define MADT_FLAG_ONLINE_CAPABLE (1u << 1)
#define MADT_FLAG_PCAT_COMPAT (1u << 0)

typedef enum
{
    IRQ_POLARITY_CONFORMS = 0,
    IRQ_POLARITY_HIGH,
    IRQ_POLARITY_LOW
} irq_polarity_t;

typedef enum
{
    IRQ_TRIGGER_CONFORMS = 0,
    IRQ_TRIGGER_EDGE,
    IRQ_TRIGGER_LEVEL
} irq_trigger_t;

typedef struct
{
    AcpiSdtHeader header;
    uint32_t lapic_address;
    uint32_t flags;
} __attribute__((packed)) MadtTable;

typedef struct
{
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) MadtEntryHeader;

typedef struct
{
    MadtEntryHeader header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) MadtProcessorEntry;

typedef struct
{
    MadtEntryHeader header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) MadtIoApicEntry;

typedef struct
{
    MadtEntryHeader header;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) MadtIsoEntry;

typedef struct
{
    MadtEntryHeader header;
    uint8_t acpi_processor_id;
    uint16_t flags;
    uint8_t lint;
} __attribute__((packed)) MadtLocalApicNmiEntry;

typedef struct
{
    MadtEntryHeader header;
    uint16_t reserved;
    uint64_t lapic_address;
} __attribute__((packed)) MadtLocalApicAddressOverrideEntry;

typedef struct
{
    MadtEntryHeader header;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_uid;
} __attribute__((packed)) MadtProcessorX2ApicEntry;

typedef struct
{
    MadtEntryHeader header;
    uint16_t flags;
    uint32_t acpi_uid;
    uint8_t lint;
    uint8_t reserved[3];
} __attribute__((packed)) MadtLocalX2ApicNmiEntry;

typedef struct
{
    uint32_t acpi_id;
    uint32_t apic_id;
    uint32_t flags;
    uint8_t x2apic;
} madt_cpu_t;

typedef struct
{
    uint8_t id;
    uint64_t mmio_base;
    uint32_t gsi_base;
} madt_ioapic_t;

typedef struct
{
    uint8_t source_irq;
    uint32_t gsi;
    irq_polarity_t polarity;
    irq_trigger_t trigger;
    uint16_t raw_flags;
} madt_iso_t;

typedef struct
{
    uint32_t processor_id;
    uint16_t raw_flags;
    uint8_t lint;
    uint8_t x2apic;
} madt_nmi_t;

typedef struct
{
    uint64_t lapic_base;
    uint32_t flags;
    uint32_t cpu_count;
    uint32_t ioapic_count;
    uint32_t iso_count;
    uint32_t nmi_count;
    uint8_t pcat_compat;
    uint8_t lapic_override;
    uint8_t valid;
} madt_snapshot_t;

bool init_madt(void);
bool madt_is_valid(void);
bool madt_snapshot(madt_snapshot_t *out);
uint64_t get_lapic_base(void);
uint64_t get_ioapic_base(void);

uint32_t madt_get_cpu_count(void);
uint32_t madt_get_cpu_apic_ids(uint32_t *buffer, uint32_t max_count);
bool madt_cpu_at(uint32_t index, madt_cpu_t *out);

uint32_t madt_get_ioapic_count(void);
bool madt_ioapic_at(uint32_t index, madt_ioapic_t *out);
uint32_t madt_get_iso_count(void);
bool madt_iso_at(uint32_t index, madt_iso_t *out);
uint32_t madt_get_nmi_count(void);
bool madt_nmi_at(uint32_t index, madt_nmi_t *out);

bool madt_decode_interrupt_flags(uint16_t flags,
                                 irq_polarity_t *out_polarity,
                                 irq_trigger_t *out_trigger);
bool madt_resolve_isa_irq(uint8_t source_irq,
                          uint32_t *out_gsi,
                          irq_polarity_t *out_polarity,
                          irq_trigger_t *out_trigger);
bool madt_validation_selftest(void);

#endif

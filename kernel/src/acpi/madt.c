#include "madt.h"

#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../memory/heap.h"

#include <stddef.h>

typedef struct
{
    uint32_t cpus;
    uint32_t ioapics;
    uint32_t isos;
    uint32_t nmis;
    uint64_t lapic_base;
    uint8_t lapic_override;
} madt_counts_t;

typedef struct
{
    MadtTable *table;
    madt_cpu_t *cpus;
    madt_ioapic_t *ioapics;
    madt_iso_t *isos;
    madt_nmi_t *nmis;
    madt_snapshot_t snapshot;
    uint8_t initialized;
    uint8_t failed;
} madt_registry_t;

static madt_registry_t g_madt_registry;

static bool madt_signature_valid(const AcpiSdtHeader *header)
{
    return header && header->signature[0] == 'A' &&
           header->signature[1] == 'P' && header->signature[2] == 'I' &&
           header->signature[3] == 'C';
}

static bool madt_checksum_valid(const MadtTable *table)
{
    if (!table || table->header.length < sizeof(MadtTable))
        return false;
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < table->header.length; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

bool madt_decode_interrupt_flags(uint16_t flags,
                                 irq_polarity_t *out_polarity,
                                 irq_trigger_t *out_trigger)
{
    if (!out_polarity || !out_trigger || (flags & 0xFFF0u))
        return false;

    switch (flags & 0x3u)
    {
    case 0u: *out_polarity = IRQ_POLARITY_CONFORMS; break;
    case 1u: *out_polarity = IRQ_POLARITY_HIGH; break;
    case 3u: *out_polarity = IRQ_POLARITY_LOW; break;
    default: return false;
    }

    switch ((flags >> 2) & 0x3u)
    {
    case 0u: *out_trigger = IRQ_TRIGGER_CONFORMS; break;
    case 1u: *out_trigger = IRQ_TRIGGER_EDGE; break;
    case 3u: *out_trigger = IRQ_TRIGGER_LEVEL; break;
    default: return false;
    }
    return true;
}

static bool madt_entry_size_valid(uint8_t type, uint8_t length)
{
    switch (type)
    {
    case MADT_TYPE_PROCESSOR_LOCAL_APIC:
        return length >= sizeof(MadtProcessorEntry);
    case MADT_TYPE_IO_APIC:
        return length >= sizeof(MadtIoApicEntry);
    case MADT_TYPE_ISO:
        return length >= sizeof(MadtIsoEntry);
    case MADT_TYPE_LOCAL_APIC_NMI:
        return length >= sizeof(MadtLocalApicNmiEntry);
    case MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE:
        return length >= sizeof(MadtLocalApicAddressOverrideEntry);
    case MADT_TYPE_PROCESSOR_LOCAL_X2APIC:
        return length >= sizeof(MadtProcessorX2ApicEntry);
    case MADT_TYPE_LOCAL_X2APIC_NMI:
        return length >= sizeof(MadtLocalX2ApicNmiEntry);
    default:
        return true;
    }
}

static bool madt_scan(const MadtTable *table, madt_counts_t *out)
{
    if (!table || !out || !madt_signature_valid(&table->header) ||
        table->header.length < sizeof(MadtTable) ||
        !madt_checksum_valid(table) || !table->lapic_address)
        return false;

    memset(out, 0, sizeof(*out));
    out->lapic_base = table->lapic_address;
    const uint8_t *ptr = (const uint8_t *)table + sizeof(MadtTable);
    const uint8_t *end = (const uint8_t *)table + table->header.length;

    while (ptr < end)
    {
        if ((uint64_t)(end - ptr) < sizeof(MadtEntryHeader))
            return false;
        const MadtEntryHeader *entry = (const MadtEntryHeader *)ptr;
        if (entry->length < sizeof(MadtEntryHeader) ||
            (uint64_t)(end - ptr) < entry->length ||
            !madt_entry_size_valid(entry->type, entry->length))
            return false;

        switch (entry->type)
        {
        case MADT_TYPE_PROCESSOR_LOCAL_APIC:
            if (((const MadtProcessorEntry *)ptr)->flags & MADT_FLAG_ENABLED)
                out->cpus++;
            break;
        case MADT_TYPE_PROCESSOR_LOCAL_X2APIC:
            if (((const MadtProcessorX2ApicEntry *)ptr)->flags & MADT_FLAG_ENABLED)
                out->cpus++;
            break;
        case MADT_TYPE_IO_APIC:
            if (!((const MadtIoApicEntry *)ptr)->ioapic_address)
                return false;
            out->ioapics++;
            break;
        case MADT_TYPE_ISO:
        {
            const MadtIsoEntry *iso = (const MadtIsoEntry *)ptr;
            irq_polarity_t polarity;
            irq_trigger_t trigger;
            if (iso->bus != 0 || iso->source_irq >= 16 ||
                !madt_decode_interrupt_flags(iso->flags, &polarity, &trigger))
                return false;
            out->isos++;
            break;
        }
        case MADT_TYPE_LOCAL_APIC_NMI:
        {
            const MadtLocalApicNmiEntry *nmi =
                (const MadtLocalApicNmiEntry *)ptr;
            irq_polarity_t polarity;
            irq_trigger_t trigger;
            if (nmi->lint > 1 ||
                !madt_decode_interrupt_flags(nmi->flags, &polarity, &trigger))
                return false;
            out->nmis++;
            break;
        }
        case MADT_TYPE_LOCAL_X2APIC_NMI:
        {
            const MadtLocalX2ApicNmiEntry *nmi =
                (const MadtLocalX2ApicNmiEntry *)ptr;
            irq_polarity_t polarity;
            irq_trigger_t trigger;
            if (nmi->lint > 1 ||
                !madt_decode_interrupt_flags(nmi->flags, &polarity, &trigger))
                return false;
            out->nmis++;
            break;
        }
        case MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE:
        {
            uint64_t address =
                ((const MadtLocalApicAddressOverrideEntry *)ptr)->lapic_address;
            if (!address || (out->lapic_override && out->lapic_base != address))
                return false;
            out->lapic_base = address;
            out->lapic_override = 1;
            break;
        }
        default:
            break;
        }
        ptr += entry->length;
    }
    return ptr == end && out->cpus != 0;
}

static bool madt_cpu_duplicate(uint32_t count, uint32_t apic_id)
{
    for (uint32_t i = 0; i < count; i++)
        if (g_madt_registry.cpus[i].apic_id == apic_id)
            return true;
    return false;
}

static bool madt_iso_store(const MadtIsoEntry *entry)
{
    irq_polarity_t polarity;
    irq_trigger_t trigger;
    if (!madt_decode_interrupt_flags(entry->flags, &polarity, &trigger))
        return false;
    for (uint32_t i = 0; i < g_madt_registry.snapshot.iso_count; i++)
    {
        madt_iso_t *old = &g_madt_registry.isos[i];
        if (old->source_irq != entry->source_irq)
            continue;
        return old->gsi == entry->gsi && old->raw_flags == entry->flags;
    }
    madt_iso_t *iso =
        &g_madt_registry.isos[g_madt_registry.snapshot.iso_count++];
    iso->source_irq = entry->source_irq;
    iso->gsi = entry->gsi;
    iso->polarity = polarity;
    iso->trigger = trigger;
    iso->raw_flags = entry->flags;
    return true;
}

static bool madt_populate(const MadtTable *table)
{
    const uint8_t *ptr = (const uint8_t *)table + sizeof(MadtTable);
    const uint8_t *end = (const uint8_t *)table + table->header.length;
    uint32_t cpu_count = 0;
    uint32_t ioapic_count = 0;
    uint32_t nmi_count = 0;

    while (ptr < end)
    {
        const MadtEntryHeader *entry = (const MadtEntryHeader *)ptr;
        switch (entry->type)
        {
        case MADT_TYPE_PROCESSOR_LOCAL_APIC:
        {
            const MadtProcessorEntry *cpu = (const MadtProcessorEntry *)ptr;
            if (cpu->flags & MADT_FLAG_ENABLED)
            {
                if (madt_cpu_duplicate(cpu_count, cpu->apic_id))
                    return false;
                g_madt_registry.cpus[cpu_count++] = (madt_cpu_t){
                    .acpi_id = cpu->acpi_processor_id,
                    .apic_id = cpu->apic_id,
                    .flags = cpu->flags,
                    .x2apic = 0};
            }
            break;
        }
        case MADT_TYPE_PROCESSOR_LOCAL_X2APIC:
        {
            const MadtProcessorX2ApicEntry *cpu =
                (const MadtProcessorX2ApicEntry *)ptr;
            if (cpu->flags & MADT_FLAG_ENABLED)
            {
                if (madt_cpu_duplicate(cpu_count, cpu->x2apic_id))
                    return false;
                g_madt_registry.cpus[cpu_count++] = (madt_cpu_t){
                    .acpi_id = cpu->acpi_uid,
                    .apic_id = cpu->x2apic_id,
                    .flags = cpu->flags,
                    .x2apic = 1};
            }
            break;
        }
        case MADT_TYPE_IO_APIC:
        {
            const MadtIoApicEntry *io = (const MadtIoApicEntry *)ptr;
            g_madt_registry.ioapics[ioapic_count++] = (madt_ioapic_t){
                .id = io->ioapic_id,
                .mmio_base = io->ioapic_address,
                .gsi_base = io->global_system_interrupt_base};
            break;
        }
        case MADT_TYPE_ISO:
            if (!madt_iso_store((const MadtIsoEntry *)ptr))
                return false;
            break;
        case MADT_TYPE_LOCAL_APIC_NMI:
        {
            const MadtLocalApicNmiEntry *nmi =
                (const MadtLocalApicNmiEntry *)ptr;
            g_madt_registry.nmis[nmi_count++] = (madt_nmi_t){
                .processor_id = nmi->acpi_processor_id,
                .raw_flags = nmi->flags,
                .lint = nmi->lint,
                .x2apic = 0};
            break;
        }
        case MADT_TYPE_LOCAL_X2APIC_NMI:
        {
            const MadtLocalX2ApicNmiEntry *nmi =
                (const MadtLocalX2ApicNmiEntry *)ptr;
            g_madt_registry.nmis[nmi_count++] = (madt_nmi_t){
                .processor_id = nmi->acpi_uid,
                .raw_flags = nmi->flags,
                .lint = nmi->lint,
                .x2apic = 1};
            break;
        }
        default:
            break;
        }
        ptr += entry->length;
    }

    g_madt_registry.snapshot.cpu_count = cpu_count;
    g_madt_registry.snapshot.ioapic_count = ioapic_count;
    g_madt_registry.snapshot.nmi_count = nmi_count;
    return true;
}

static char *madt_append_u64(char *out, uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; }
    while (value);
    while (count) *out++ = digits[--count];
    return out;
}

static char *madt_append_text(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}

static void madt_serial_summary(void)
{
    char line[192];
    char *p = madt_append_text(line, "[IRQ][MADT] PASS cpus=");
    p = madt_append_u64(p, g_madt_registry.snapshot.cpu_count);
    p = madt_append_text(p, " ioapics=");
    p = madt_append_u64(p, g_madt_registry.snapshot.ioapic_count);
    p = madt_append_text(p, " iso=");
    p = madt_append_u64(p, g_madt_registry.snapshot.iso_count);
    p = madt_append_text(p, " nmi=");
    p = madt_append_u64(p, g_madt_registry.snapshot.nmi_count);
    p = madt_append_text(p, " pcat=");
    p = madt_append_u64(p, g_madt_registry.snapshot.pcat_compat);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

bool init_madt(void)
{
    if (g_madt_registry.initialized)
        return !g_madt_registry.failed;
    g_madt_registry.initialized = 1;

    MadtTable *table = (MadtTable *)acpi_find_table(ACPI_SIG_MADT);
    madt_counts_t counts;
    if (!madt_scan(table, &counts))
    {
        g_madt_registry.failed = 1;
        return false;
    }

    g_madt_registry.table = table;
    if (counts.cpus)
        g_madt_registry.cpus = kmalloc(sizeof(madt_cpu_t) * counts.cpus);
    if (counts.ioapics)
        g_madt_registry.ioapics =
            kmalloc(sizeof(madt_ioapic_t) * counts.ioapics);
    if (counts.isos)
        g_madt_registry.isos = kmalloc(sizeof(madt_iso_t) * counts.isos);
    if (counts.nmis)
        g_madt_registry.nmis = kmalloc(sizeof(madt_nmi_t) * counts.nmis);
    if (!g_madt_registry.cpus ||
        (counts.ioapics && !g_madt_registry.ioapics) ||
        (counts.isos && !g_madt_registry.isos) ||
        (counts.nmis && !g_madt_registry.nmis))
    {
        g_madt_registry.failed = 1;
        return false;
    }

    g_madt_registry.snapshot = (madt_snapshot_t){
        .lapic_base = counts.lapic_base,
        .flags = table->flags,
        .pcat_compat = (table->flags & MADT_FLAG_PCAT_COMPAT) != 0,
        .lapic_override = counts.lapic_override,
        .valid = 1};
    if (!madt_populate(table) ||
        g_madt_registry.snapshot.cpu_count != counts.cpus ||
        g_madt_registry.snapshot.ioapic_count != counts.ioapics ||
        g_madt_registry.snapshot.nmi_count != counts.nmis)
    {
        g_madt_registry.snapshot.valid = 0;
        g_madt_registry.failed = 1;
        return false;
    }
    madt_serial_summary();
    return true;
}

bool madt_is_valid(void)
{
    return g_madt_registry.initialized && !g_madt_registry.failed &&
           g_madt_registry.snapshot.valid;
}

bool madt_snapshot(madt_snapshot_t *out)
{
    if (!out || !madt_is_valid())
        return false;
    *out = g_madt_registry.snapshot;
    return true;
}

uint64_t get_lapic_base(void)
{
    return madt_is_valid() ? g_madt_registry.snapshot.lapic_base : 0;
}

uint64_t get_ioapic_base(void)
{
    return madt_is_valid() && g_madt_registry.snapshot.ioapic_count
               ? g_madt_registry.ioapics[0].mmio_base
               : 0;
}

uint32_t madt_get_cpu_count(void)
{
    return madt_is_valid() ? g_madt_registry.snapshot.cpu_count : 0;
}

uint32_t madt_get_cpu_apic_ids(uint32_t *buffer, uint32_t max_count)
{
    if (!buffer || !madt_is_valid())
        return 0;
    uint32_t count = g_madt_registry.snapshot.cpu_count;
    if (count > max_count) count = max_count;
    for (uint32_t i = 0; i < count; i++)
        buffer[i] = g_madt_registry.cpus[i].apic_id;
    return count;
}

bool madt_cpu_at(uint32_t index, madt_cpu_t *out)
{
    if (!out || !madt_is_valid() || index >= g_madt_registry.snapshot.cpu_count)
        return false;
    *out = g_madt_registry.cpus[index];
    return true;
}

uint32_t madt_get_ioapic_count(void)
{
    return madt_is_valid() ? g_madt_registry.snapshot.ioapic_count : 0;
}

bool madt_ioapic_at(uint32_t index, madt_ioapic_t *out)
{
    if (!out || !madt_is_valid() ||
        index >= g_madt_registry.snapshot.ioapic_count)
        return false;
    *out = g_madt_registry.ioapics[index];
    return true;
}

uint32_t madt_get_iso_count(void)
{
    return madt_is_valid() ? g_madt_registry.snapshot.iso_count : 0;
}

bool madt_iso_at(uint32_t index, madt_iso_t *out)
{
    if (!out || !madt_is_valid() || index >= g_madt_registry.snapshot.iso_count)
        return false;
    *out = g_madt_registry.isos[index];
    return true;
}

uint32_t madt_get_nmi_count(void)
{
    return madt_is_valid() ? g_madt_registry.snapshot.nmi_count : 0;
}

bool madt_nmi_at(uint32_t index, madt_nmi_t *out)
{
    if (!out || !madt_is_valid() || index >= g_madt_registry.snapshot.nmi_count)
        return false;
    *out = g_madt_registry.nmis[index];
    return true;
}

bool madt_resolve_isa_irq(uint8_t source_irq,
                          uint32_t *out_gsi,
                          irq_polarity_t *out_polarity,
                          irq_trigger_t *out_trigger)
{
    if (!out_gsi || !out_polarity || !out_trigger || source_irq >= 16 ||
        !madt_is_valid())
        return false;
    *out_gsi = source_irq;
    *out_polarity = IRQ_POLARITY_HIGH;
    *out_trigger = IRQ_TRIGGER_EDGE;
    for (uint32_t i = 0; i < g_madt_registry.snapshot.iso_count; i++)
    {
        const madt_iso_t *iso = &g_madt_registry.isos[i];
        if (iso->source_irq != source_irq)
            continue;
#ifdef HOBBYOS_IRQ_NEGATIVE_IGNORE_ISO
        return true;
#else
        *out_gsi = iso->gsi;
        *out_polarity = iso->polarity == IRQ_POLARITY_CONFORMS
                            ? IRQ_POLARITY_HIGH : iso->polarity;
        *out_trigger = iso->trigger == IRQ_TRIGGER_CONFORMS
                           ? IRQ_TRIGGER_EDGE : iso->trigger;
        return true;
#endif
    }
    return true;
}

static void madt_fixture_checksum(MadtTable *table)
{
    table->header.checksum = 0;
    uint8_t sum = 0;
    uint8_t *bytes = (uint8_t *)table;
    for (uint32_t i = 0; i < table->header.length; i++)
        sum = (uint8_t)(sum + bytes[i]);
    table->header.checksum = (uint8_t)(0u - sum);
}

bool madt_validation_selftest(void)
{
    struct
    {
        MadtTable table;
        MadtProcessorEntry cpu;
        MadtIoApicEntry ioapic;
        MadtIsoEntry iso;
    } fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.table.header.signature[0] = 'A';
    fixture.table.header.signature[1] = 'P';
    fixture.table.header.signature[2] = 'I';
    fixture.table.header.signature[3] = 'C';
    fixture.table.header.length = sizeof(fixture);
    fixture.table.lapic_address = 0xFEE00000u;
    fixture.table.flags = MADT_FLAG_PCAT_COMPAT;
    fixture.cpu.header.type = MADT_TYPE_PROCESSOR_LOCAL_APIC;
    fixture.cpu.header.length = sizeof(fixture.cpu);
    fixture.cpu.flags = MADT_FLAG_ENABLED;
    fixture.ioapic.header.type = MADT_TYPE_IO_APIC;
    fixture.ioapic.header.length = sizeof(fixture.ioapic);
    fixture.ioapic.ioapic_address = 0xFEC00000u;
    fixture.iso.header.type = MADT_TYPE_ISO;
    fixture.iso.header.length = sizeof(fixture.iso);
    fixture.iso.source_irq = 1;
    fixture.iso.gsi = 9;
    fixture.iso.flags = 0xFu;
    madt_fixture_checksum(&fixture.table);

    madt_counts_t counts;
    bool ok = madt_scan(&fixture.table, &counts) && counts.cpus == 1 &&
              counts.ioapics == 1 && counts.isos == 1;
    irq_polarity_t polarity;
    irq_trigger_t trigger;
    ok = ok && madt_decode_interrupt_flags(0, &polarity, &trigger) &&
         polarity == IRQ_POLARITY_CONFORMS &&
         trigger == IRQ_TRIGGER_CONFORMS;
    ok = ok && madt_decode_interrupt_flags(0xFu, &polarity, &trigger) &&
         polarity == IRQ_POLARITY_LOW && trigger == IRQ_TRIGGER_LEVEL;
    ok = ok && !madt_decode_interrupt_flags(2u, &polarity, &trigger) &&
         !madt_decode_interrupt_flags(8u, &polarity, &trigger);

    uint8_t saved_length = fixture.iso.header.length;
    fixture.iso.header.length = 1;
    madt_fixture_checksum(&fixture.table);
    ok = ok && !madt_scan(&fixture.table, &counts);
    fixture.iso.header.length = saved_length;
    madt_fixture_checksum(&fixture.table);
    fixture.table.header.length--;
    fixture.table.header.checksum = 0;
    ok = ok && !madt_scan(&fixture.table, &counts);
    return ok;
}

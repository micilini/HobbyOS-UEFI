#include "ioapic.h"
#include "../cpu/mmio.h"
#include "../acpi/madt.h"

static uint64_t g_ioapic_base = 0;

static void ioapic_write(uint8_t reg, uint32_t val) {
    if (!g_ioapic_base) return;
    
    mmio_write32((void*)(g_ioapic_base + IOREGSEL), reg);
    
    mmio_write32((void*)(g_ioapic_base + IOWIN), val);
}

static uint32_t ioapic_read(uint8_t reg) {
    if (!g_ioapic_base) return 0;
    mmio_write32((void*)(g_ioapic_base + IOREGSEL), reg);
    return mmio_read32((void*)(g_ioapic_base + IOWIN));
}

void ioapic_set_entry(uint8_t index, uint64_t data) {
    IoApicRedirEntry entry;
    *(uint64_t*)&entry = data; 

    
    
    
    ioapic_write(IOREDTBL + (index * 2), entry.lower);
    ioapic_write(IOREDTBL + (index * 2) + 1, entry.upper);
}


void ioapic_map_irq(uint8_t irq, uint8_t vector, uint8_t apic_id) {
    IoApicRedirEntry entry;
    *(uint64_t*)&entry = 0; 

    entry.vector = vector;   
    entry.dest_mode = 0;     
    entry.delv_mode = 0;     
    entry.mask = 0;          
    entry.destination = apic_id; 

    ioapic_set_entry(irq, *(uint64_t*)&entry);
}

void init_ioapic() {
    g_ioapic_base = get_ioapic_base();
    
    
    
    
}
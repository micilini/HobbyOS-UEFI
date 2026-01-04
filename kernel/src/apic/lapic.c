#include "lapic.h"
#include "../cpu/cpu.h"
#include "../cpu/mmio.h"
#include "../acpi/madt.h"

#define LAPIC_EOI_REG 0xFEE000B0


static uint64_t g_lapic_base = 0;

void lapic_write(uint32_t reg, uint32_t value) {
    if (g_lapic_base)
        mmio_write32((void*)(g_lapic_base + reg), value);
}

uint32_t lapic_read(uint32_t reg) {
    if (g_lapic_base)
        return mmio_read32((void*)(g_lapic_base + reg));
    return 0;
}

void lapic_eoi() {
    lapic_write(LAPIC_EOI, 0);
}

void enable_lapic() {
    
    uint64_t msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    msr |= (1 << 11); 
    cpu_write_msr(IA32_APIC_BASE_MSR, msr);

    
    
    lapic_write(LAPIC_SPURIOUS, 0x1FF); 
}

void init_lapic() {
    g_lapic_base = get_lapic_base();
    
    
    lapic_write(LAPIC_TPR, 0);

    enable_lapic();
}

void lapic_send_eoi() {
    
    *(volatile uint32_t*)(LAPIC_EOI_REG) = 0;
}
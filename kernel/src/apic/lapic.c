#include "lapic.h"
#include "../cpu/cpu.h"
#include "../cpu/mmio.h"
#include "../acpi/madt.h"
#include "../memory/paging.h"

static uint64_t g_lapic_base = 0;

static uint8_t g_x2apic = 0;

static inline uint32_t x2apic_msr_index(uint32_t lapic_reg_offset)
{

    return 0x800u + (lapic_reg_offset >> 4);
}

void lapic_write(uint32_t reg, uint32_t value)
{

    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    uint8_t x2apic = (apic_msr & (1ULL << 10)) ? 1 : 0;

    if (x2apic)
    {

        uint32_t msr = 0x800 + (reg >> 4);
        cpu_write_msr(msr, (uint64_t)value);
        return;
    }

    if (!g_lapic_base)
        return;
    mmio_write32((void *)(g_lapic_base + reg), value);
}

uint32_t lapic_read(uint32_t reg)
{
    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    uint8_t x2apic = (apic_msr & (1ULL << 10)) ? 1 : 0;

    if (x2apic)
    {
        uint32_t msr = 0x800 + (reg >> 4);
        return (uint32_t)cpu_read_msr(msr);
    }

    if (!g_lapic_base)
        return 0;
    return mmio_read32((void *)(g_lapic_base + reg));
}

uint32_t lapic_get_id(void)
{
    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    uint8_t x2apic = (apic_msr & (1ULL << 10)) ? 1 : 0;

    if (x2apic)
    {
        return lapic_read(LAPIC_ID);
    }

    uint32_t v = lapic_read(LAPIC_ID);
    return (v >> 24) & 0xFFu;
}

void lapic_eoi()
{
    lapic_write(LAPIC_EOI, 0);
}

void init_lapic()
{

    g_lapic_base = get_lapic_base();

    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    uint64_t msr_base = apic_msr & 0xFFFFF000ULL;
    if (!g_lapic_base)
        g_lapic_base = msr_base;

    apic_msr |= (1ULL << 11);
    cpu_write_msr(IA32_APIC_BASE_MSR, apic_msr);

    uint8_t x2apic = (apic_msr & (1ULL << 10)) ? 1 : 0;
    if (!x2apic && g_lapic_base)
    {

        uint64_t base_page = g_lapic_base & ~0xFFFULL;
        paging_map(base_page, base_page, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        paging_map(base_page + 0x1000, base_page + 0x1000, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

        __asm__ volatile("invlpg (%0)" ::"r"(base_page) : "memory");
        __asm__ volatile("invlpg (%0)" ::"r"(base_page + 0x1000) : "memory");
    }

    lapic_write(LAPIC_TPR, 0);

    lapic_write(LAPIC_SPURIOUS, 0xFF | (1u << 8));
}
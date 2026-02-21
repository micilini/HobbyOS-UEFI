#include "lapic.h"
#include "../cpu/cpu.h"
#include "../cpu/mmio.h"
#include "../acpi/madt.h"
#include "../memory/paging.h"
#include "../graphics/console.h"
#include "../drivers/serial.h"

static uint64_t g_lapic_base = 0;
static uint8_t g_x2apic = 0;

static inline uint32_t x2apic_msr_index(uint32_t lapic_reg_offset)
{
    return 0x800u + (lapic_reg_offset >> 4);
}

void lapic_write(uint32_t reg, uint32_t value)
{

    if (g_x2apic)
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
    if (g_x2apic)
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
    if (g_x2apic)
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

    if (!g_lapic_base)
        g_lapic_base = apic_msr & 0xFFFFF000ULL;

    uint32_t eax, ebx, ecx, edx;
    cpu_get_cpuid(1, &eax, &ebx, &ecx, &edx);
    uint8_t x2apic_supported = (ecx >> 21) & 1;

    apic_msr |= (1ULL << 11);

    if (x2apic_supported)
    {
        apic_msr |= (1ULL << 10);
    }

    cpu_write_msr(IA32_APIC_BASE_MSR, apic_msr);

    apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    g_x2apic = (apic_msr & (1ULL << 10)) ? 1 : 0;

    if (g_x2apic)
        serial_write_all("[LAPIC] System using X2APIC\n");
    else
        serial_write_all("[LAPIC] System using XAPIC\n");

    if (!g_x2apic && g_lapic_base)
    {
        uint64_t base_page = g_lapic_base & ~0xFFFULL;
        paging_map(base_page, base_page, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        paging_map(base_page + 0x1000, base_page + 0x1000, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        __asm__ volatile("invlpg (%0)" ::"r"(base_page) : "memory");
    }

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SPURIOUS, 0xFF | (1u << 8));
}

static void lapic_wait_icr_idle()
{

    if (g_x2apic)
        return;

    int timeout = 100000;
    while (lapic_read(LAPIC_ICR0) & APIC_DS_PENDING)
    {
        __asm__ volatile("pause");
        timeout--;
        if (timeout == 0)
        {
            console_write_debug("[LAPIC] Warning: ICR Idle Timeout!\n");
            break;
        }
    }
}

static void lapic_write_icr(uint32_t dest_apic_id, uint32_t vector, uint32_t flags)
{
    lapic_wait_icr_idle();

    if (g_x2apic)
    {

        uint64_t val = ((uint64_t)dest_apic_id << 32) | (uint64_t)(flags | vector);
        cpu_write_msr(0x830, val);
    }
    else
    {

        lapic_write(LAPIC_ICR1, dest_apic_id << 24);

        lapic_write(LAPIC_ICR0, flags | vector);
    }

    lapic_wait_icr_idle();
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{

    lapic_write_icr(apic_id, vector, APIC_DM_FIXED | APIC_LEVEL_ASSERT | APIC_TRIGGER_EDGE);
}

void lapic_send_init(uint32_t apic_id)
{

    lapic_write_icr(apic_id, 0, APIC_DM_INIT | APIC_LEVEL_ASSERT | APIC_TRIGGER_LEVEL);

    for (volatile int i = 0; i < 10000; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));

    if (!g_x2apic)
    {
        lapic_write_icr(apic_id, 0, APIC_DM_INIT | APIC_LEVEL_DEASSERT | APIC_TRIGGER_LEVEL);
    }

    for (volatile int i = 0; i < 10000; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

void lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page)
{

    uint8_t vector = (uint8_t)(trampoline_page & 0xFF);
    lapic_write_icr(apic_id, vector, APIC_DM_SIPI | APIC_LEVEL_ASSERT | APIC_TRIGGER_EDGE);

    for (volatile int i = 0; i < 200; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

void init_lapic_ap(void)
{

    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    apic_msr |= (1ULL << 11);

    if (g_x2apic)
    {
        apic_msr |= (1ULL << 10);
    }

    cpu_write_msr(IA32_APIC_BASE_MSR, apic_msr);

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SPURIOUS, 0xFF | (1u << 8));
}

void lapic_timer_set_periodic(uint32_t vector, uint32_t ticks)
{

    lapic_write(LAPIC_TDCR, 0x3);

    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_PERIODIC | vector);

    lapic_write(LAPIC_TICR, ticks);
}

void lapic_send_broadcast_halt(void)
{

    lapic_write_icr(0, 0, APIC_DEST_SHORTHAND_ALL_BUT_SELF | APIC_DM_NMI | APIC_LEVEL_ASSERT);
}
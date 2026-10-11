#include "smp_topology.h"
#include "smp_boot.h"
#include "../acpi/madt.h"
#include "../drivers/serial.h"
#include "../apic/lapic.h"
#include "../memory/heap.h"
#include "../cpu/tss.h"
#include "../memory/gdt.h"
#include "../libc/string.h"
#include "../memory/paging.h"
#include "../memory/physical_translation.h"
#include "../memory/kernel_stack.h"
#include "../core/panic.h"
#include <string.h>

#define PAGE_SIZE 4096
#define IST_STACK_SIZE (32 * 1024)

SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count = 0;
uint32_t g_bsp_apic_id = 0;
static cpu_slot_t g_bsp_slot = CPU_SLOT_INVALID;

static void *checked_cpu_pointer(void *pointer, size_t size)
{
    (void)physical_require_address(pointer, size);
    return pointer;
}

static void serial_print_hex(uint64_t n)
{
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

static void serial_print_dec(uint32_t n)
{
    if (n == 0)
    {
        serial_write_all("0");
        return;
    }
    char buffer[32];
    int i = 0;
    while (n > 0)
    {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--)
    {
        serial_putc_all(buffer[j]);
    }
}

bool smp_cpu_slot_from_apic_id(uint32_t apic_id, cpu_slot_t *out_slot)
{
    if (!out_slot)
        return false;
    *out_slot = CPU_SLOT_INVALID;
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
    {
        if (g_cpus[slot].apic_id == apic_id)
        {
            *out_slot = slot;
            return true;
        }
    }
    return false;
}

bool smp_current_cpu_slot(cpu_slot_t *out_slot)
{
    return smp_cpu_slot_from_apic_id(lapic_get_id(), out_slot);
}

SmpCpuInfo *smp_cpu_by_slot(cpu_slot_t slot)
{
    if (slot >= g_cpu_count || slot >= HOBBYOS_MAX_CPUS)
        return NULL;
    return &g_cpus[slot];
}

const SmpCpuInfo *smp_cpu_by_slot_const(cpu_slot_t slot)
{
    return smp_cpu_by_slot(slot);
}

cpu_slot_t smp_bsp_cpu_slot(void) { return g_bsp_slot; }

uint32_t smp_online_cpu_count(void)
{
    uint32_t count = 0;
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
        if (__atomic_load_n(&g_cpus[slot].state, __ATOMIC_ACQUIRE) == CPU_STATE_ONLINE)
            count++;
    return count;
}

uint32_t smp_failed_cpu_count(void)
{
    uint32_t count = 0;
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
        if (__atomic_load_n(&g_cpus[slot].state, __ATOMIC_ACQUIRE) == CPU_STATE_FAILED)
            count++;
    return count;
}

bool smp_mark_cpu_online(cpu_slot_t slot)
{
    SmpCpuInfo *cpu = smp_cpu_by_slot(slot);
    if (!cpu)
        return false;
    __atomic_store_n(&cpu->state, CPU_STATE_ONLINE, __ATOMIC_RELEASE);
    return true;
}

static char *append_text(char *dst, const char *text)
{
    while (*text) *dst++ = *text++;
    return dst;
}

static char *append_dec(char *dst, uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    if (value == 0) { *dst++ = '0'; return dst; }
    while (value && count < sizeof(digits))
    {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (count) *dst++ = digits[--count];
    return dst;
}

void smp_log_cpu_online(cpu_slot_t slot, const char *role)
{
    const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
    if (!cpu) return;
    char line[112];
    char *out = line;
    out = append_text(out, "[SMP][CPU] ONLINE slot=");
    out = append_dec(out, slot);
    out = append_text(out, " apic=");
    out = append_dec(out, cpu->apic_id);
    out = append_text(out, " role=");
    out = append_text(out, role ? role : "UNKNOWN");
    *out++ = '\n'; *out = '\0';
    serial_write_all(line);
}

bool smp_topology_init(void)
{
    serial_write_all("[SMP] Init Topology...\n");
    g_bsp_apic_id = lapic_get_id();
    g_bsp_slot = CPU_SLOT_INVALID;

    uint32_t madt_count = madt_get_cpu_count();
    if (madt_count == 0 || madt_count > HOBBYOS_MAX_CPUS)
    {
        serial_write_all("[SMP][CPU] ERROR code=INVALID_CPU_COUNT\n");
        return false;
    }

    uint32_t apic_ids[HOBBYOS_MAX_CPUS];
    g_cpu_count = madt_get_cpu_apic_ids(apic_ids, HOBBYOS_MAX_CPUS);
    if (g_cpu_count == 0 || g_cpu_count > HOBBYOS_MAX_CPUS || g_cpu_count != madt_count)
    {
        serial_write_all("[SMP][CPU] ERROR code=CPU_DISCOVERY_MISMATCH\n");
        return false;
    }

    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
    {
        SmpCpuInfo *cpu = &g_cpus[slot];
        cpu->slot = slot;
        cpu->apic_id = apic_ids[slot];
        madt_cpu_t madt_cpu;
        if (!madt_cpu_at(slot, &madt_cpu) || madt_cpu.apic_id != cpu->apic_id)
        {
            serial_write_all("[SMP][CPU] ERROR code=MADT_CPU_LOOKUP\n");
            return false;
        }
        cpu->acpi_id = madt_cpu.acpi_id;
        cpu->state = CPU_STATE_PREPARE;
        cpu->is_bsp = (cpu->apic_id == g_bsp_apic_id);
        for (cpu_slot_t prior = 0; prior < slot; prior++)
        {
            if (g_cpus[prior].apic_id == cpu->apic_id)
            {
                serial_write_all("[SMP][CPU] ERROR code=DUPLICATE_APIC\n");
                return false;
            }
        }
        if (cpu->is_bsp)
        {
            if (g_bsp_slot != CPU_SLOT_INVALID)
            {
                serial_write_all("[SMP][CPU] ERROR code=MULTIPLE_BSP\n");
                return false;
            }
            g_bsp_slot = slot;
        }
    }

    if (g_bsp_slot == CPU_SLOT_INVALID)
    {
        serial_write_all("[SMP][CPU] ERROR code=MISSING_BSP\n");
        return false;
    }

    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
    {
        cpu_slot_t roundtrip = CPU_SLOT_INVALID;
        if (g_cpus[slot].slot != slot ||
            !smp_cpu_slot_from_apic_id(g_cpus[slot].apic_id, &roundtrip) ||
            roundtrip != slot)
        {
            serial_write_all("[SMP][CPU] ERROR code=ROUNDTRIP\n");
            return false;
        }
    }
    cpu_slot_t unknown = CPU_SLOT_INVALID;
    if (smp_cpu_slot_from_apic_id(UINT32_MAX, &unknown))
    {
        serial_write_all("[SMP][CPU] ERROR code=UNKNOWN_APIC_LOOKUP\n");
        return false;
    }

    serial_write_all("[SMP] Topology Done.\n");
    return true;
}
void smp_prepare_cpu_structures()
{
    serial_write_all("[SMP] Allocating CPU Structs (w/ Mapping)...\n");
    _Static_assert(SMP_AP_STACK_BYTES==STACK_TASK_BYTES && IST_STACK_SIZE==STACK_IST_BYTES,
                   "CPU stack capacities must remain unchanged");

    for (uint32_t i = 0; i < g_cpu_count; i++)
    {
        serial_write_all("   -> Index ");
        serial_print_dec(i);

        if (g_cpus[i].is_bsp)
        {
            extern uint8_t bsp_stack_top[];
            if (!kernel_stack_bind_bsp(i,g_cpus[i].apic_id)) kpanic("BSP guarded stack ownership failed");
            g_cpus[i].stack_top=(uintptr_t)bsp_stack_top;
            g_cpus[i].tss_ptr=tss_get();
            serial_write_all(" [BSP] GUARDED\n");
            continue;
        }
        serial_write_all(" [AP] Allocating...\n");

        if (!kernel_stack_cpu(STACK_KIND_AP_BOOT,i,g_cpus[i].apic_id,&g_cpus[i].stack_top))
            kpanic("AP guarded boot stack allocation failed");

        serial_write_all("OK (Virt: ");
        serial_print_hex(g_cpus[i].stack_top);
        serial_write_all(")\n");

        serial_write_all("      [2] TSS... ");
        void *tss_pointer = kmalloc(sizeof(Tss64));
        if (!tss_pointer)
        {
            serial_write_all("FAIL (kmalloc)\n");
            continue;
        }

        Tss64 *tss_virt = (Tss64 *)checked_cpu_pointer(tss_pointer, sizeof(Tss64));

        serial_write_all("OK (Virt: ");
        serial_print_hex((uint64_t)tss_virt);
        serial_write_all(")\n");

        serial_write_all("      [2.1] Memset TSS... ");
        memset(tss_virt, 0, sizeof(Tss64));
        serial_write_all("OK\n");

        tss_virt->rsp0 = g_cpus[i].stack_top;
        g_cpus[i].tss_ptr = tss_virt;

        serial_write_all("      [2.2] IST... ");

        uint64_t ist[3];
        for (unsigned n=0;n<3;++n)
            if (!kernel_stack_cpu((stack_kind_t)(STACK_KIND_IST1+n),i,g_cpus[i].apic_id,&ist[n]))
                kpanic("AP guarded IST allocation failed");
        tss_virt->ist1=ist[0];tss_virt->ist2=ist[1];tss_virt->ist3=ist[2];

        serial_write_all("OK\n");

        serial_write_all("      [3] GDT... ");
        void *gdt_pointer = kmalloc(sizeof(GdtTable));
        if (!gdt_pointer)
        {
            serial_write_all("FAIL (kmalloc)\n");
            continue;
        }

        GdtTable *gdt_virt = (GdtTable *)checked_cpu_pointer(gdt_pointer, sizeof(GdtTable));

        extern GdtTable g_gdt;
        memcpy(gdt_virt, &g_gdt, sizeof(GdtTable));

        uint64_t tss_base = (uint64_t)tss_virt;
        uint32_t tss_limit = sizeof(Tss64) - 1;

        TssDesc64 *tss_desc = &gdt_virt->tss;
        tss_desc->limit_low = (uint16_t)(tss_limit & 0xFFFF);
        tss_desc->base_low = (uint16_t)(tss_base & 0xFFFF);
        tss_desc->base_mid1 = (uint8_t)((tss_base >> 16) & 0xFF);
        tss_desc->access = 0x89;
        tss_desc->gran = (uint8_t)((tss_limit >> 16) & 0x0F);
        tss_desc->base_mid2 = (uint8_t)((tss_base >> 24) & 0xFF);
        tss_desc->base_high = (uint32_t)((tss_base >> 32) & 0xFFFFFFFF);
        tss_desc->reserved = 0;

        g_cpus[i].gdt_ptr = gdt_virt;

        serial_write_all("OK (Virt: ");
        serial_print_hex((uint64_t)gdt_virt);
        serial_write_all(")\n");
    }
    serial_write_all("[SMP] All structs allocated & Mapped.\n");
}

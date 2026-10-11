#include "smp_boot.h"
#include "../core/clocksource.h"
#include "smp_topology.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../memory/gdt.h"
#include "../core/idt.h"
#include "../memory/paging.h"
#include "../memory/pmem.h"
#include "../memory/bootstrap_memory.h"
#include "../cpu/tss.h"
#include "../cpu/cpu.h"
#include "../cpu/control_policy.h"
#include "../core/interrupts.h"
#include "../core/irq_bootstrap.h"
#include "../core/scheduler.h"
#include "../core/build_profile.h"
#include "../core/panic.h"


extern uint64_t smp_trampoline_cr4;
extern uint64_t smp_trampoline_efer;
extern uint64_t smp_trampoline_cr0;
extern uint64_t smp_trampoline_pat;
extern uint32_t smp_trampoline_status;
extern uint32_t smp_trampoline_paging_features;
extern uint32_t smp_trampoline_physical_bits;
extern uint32_t smp_trampoline_x2apic_flag;
extern uint32_t smp_trampoline_base;
extern uint8_t smp_trampoline_gdtr;
extern uint8_t smp_trampoline_gdt_start;
extern uint8_t _trampoline_start[];
extern uint8_t _trampoline_end[];
extern uint64_t smp_trampoline_cr3;
extern uint64_t smp_trampoline_stack;
extern uint64_t smp_trampoline_entry_ptr;
extern void ap_kernel_entry_asm(void);

extern void gdt_flush(uint64_t gdtr_addr);

#include "../memory/physical_translation.h"

volatile bool g_ap_boot_done = false;

static void smp_print_dec(uint32_t n)
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

static void smp_print_hex(uint64_t n)
{
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

void ap_kernel_entry(uint64_t entry_rsp_mod16, uint64_t entry_df)
{
    uint32_t my_apic_id;
    if (!cpu_local_arch_id(&my_apic_id))
    {
        cpu_slot_t failed_slot;
        if (cpu_local_arch_id(&my_apic_id) &&
            smp_cpu_slot_from_apic_id(my_apic_id, &failed_slot))
        {
            SmpCpuInfo *failed = smp_cpu_by_slot(failed_slot);
            if (failed) failed->state = CPU_STATE_FAILED;
        }
        while (1) __asm__ volatile("cli; hlt");
    }
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_cpu_slot_from_apic_id(my_apic_id, &slot))
    {
        serial_write_all("[SMP][CPU] ERROR code=UNKNOWN_APIC apic=");
        smp_print_dec(my_apic_id);
        serial_write_all("\n");
        while (1)
            __asm__ volatile("cli; hlt");
    }
#if HOBBYOS_DEBUG_ASSERT
    build_profile_record_ap_entry(slot, entry_rsp_mod16, entry_df);
#else
    (void)entry_rsp_mod16;
    (void)entry_df;
#endif
    SmpCpuInfo *me = smp_cpu_by_slot(slot);
    if (!me || me->is_bsp || !me->gdt_ptr || !me->tss_ptr)
    {
        serial_write_all("[SMP][CPU] ERROR code=INVALID_AP_STRUCTURES\n");
        while (1)
            __asm__ volatile("cli; hlt");
    }

    me->state = CPU_STATE_STARTING;

    GdtPtr gdtr;
    gdtr.size = sizeof(GdtTable) - 1;
    gdtr.offset = (uint64_t)me->gdt_ptr;

    gdt_flush((uint64_t)&gdtr);
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" ::: "ax");
    idt_load();
    GdtPtr actual_gdt;
    IdtPtr actual_idt;
    bootstrap_departure_t departure={0};
    __asm__ volatile("mov %%cr3,%0; mov %%rsp,%1; sgdt %2; sidt %3; mov %%cs,%4; str %5"
        : "=r"(departure.previous_root),"=r"(departure.rsp),"=m"(actual_gdt),"=m"(actual_idt),
          "=r"(departure.cs),"=r"(departure.tr) :: "memory");
    const Tss64 *tss=me->tss_ptr;
    const GdtTable *gdt=me->gdt_ptr;
    uint64_t tss_base=(uint64_t)gdt->tss.base_low | ((uint64_t)gdt->tss.base_mid1<<16) |
        ((uint64_t)gdt->tss.base_mid2<<24) | ((uint64_t)gdt->tss.base_high<<32);
    departure.gdtr=actual_gdt.offset; departure.idtr=actual_idt.base; departure.tss=tss_base;
    departure.ist1=tss->ist1; departure.ist2=tss->ist2; departure.ist3=tss->ist3;
    departure.stack_first=me->stack_top-SMP_AP_STACK_BYTES; departure.stack_bytes=SMP_AP_STACK_BYTES;
    departure.generation=paging_bootstrap_lifetime()->generation;
    departure.apic_id=my_apic_id; departure.slot=slot;
    if (entry_rsp_mod16!=8 || entry_df || actual_gdt.offset!=(uintptr_t)gdt ||
        actual_gdt.size!=sizeof(GdtTable)-1 || actual_idt.limit!=IDT_ENTRIES*sizeof(IdtEntry)-1 ||
        tss_base!=(uintptr_t)tss || !paging_bootstrap_handoff_ready(&departure))
        kpanic("AP high descriptor handoff is not ready");
    paging_load_kernel_root();
    __asm__ volatile("mov %%cr3,%0; mov %%rsp,%1; sgdt %2; sidt %3; mov %%cs,%4; str %5"
        : "=r"(departure.root),"=r"(departure.rsp),"=m"(actual_gdt),"=m"(actual_idt),
          "=r"(departure.cs),"=r"(departure.tr) :: "memory");
    if (actual_gdt.offset!=departure.gdtr || actual_idt.base!=departure.idtr ||
        actual_gdt.size!=sizeof(GdtTable)-1 || actual_idt.limit!=IDT_ENTRIES*sizeof(IdtEntry)-1)
        kpanic("AP descriptors changed across the main-root switch");
    if (!cpu_policy_confirm_local(my_apic_id,paging_kernel_root_physical(),"AP") ||
        !paging_bootstrap_depart(&departure)) {
        me->state=CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }
    init_lapic_ap();
    serial_write_all("[AP] apic_id=0x");
    smp_print_hex(my_apic_id);
    serial_write_all("\n");
    if (scheduler_cpu_init_ap(slot) != SCHED_BOOT_OK)
    {
        serial_write_all("[SMP][CPU] ERROR code=SCHEDULER_AP_INIT apic=");
        smp_print_dec(my_apic_id);
        serial_write_all("\n");
        me->state = CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }

    clocksource_prepare_local();
    lapic_timer_calibration_t ap_cal;
    if (!lapic_timer_calibrate(1000, &ap_cal) ||
        !lapic_timer_prepare_periodic(INT_VECTOR_LAPIC_TIMER, &ap_cal))
    {
        serial_write_all("[TIMER][LAPIC_CAL] status=FAIL\n");
        me->state=CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }
    if (scheduler_cpu_mark_timer_ready(slot) != SCHED_BOOT_OK ||
        !scheduler_cpu_is_ready(slot) || !irq_bootstrap_cpu_prepare(slot))
    {
        serial_write_all("[SMP][CPU] ERROR code=TIMER_READY apic=");
        smp_print_dec(my_apic_id);
        serial_write_all("\n");
        me->state = CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }

    if (!smp_mark_cpu_online(slot))
    {
        serial_write_all("[SMP][CPU] ERROR code=ONLINE_TRANSITION\n");
        me->state = CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }
    smp_log_cpu_online(slot, "AP");

    irq_cpu_ready_result_t runtime_result =
        irq_bootstrap_cpu_run_runtime(slot);
    if (runtime_result != IRQ_CPU_READY_OK)
    {
        irq_bootstrap_log_cpu_runtime_error(slot, runtime_result);
        me->state = CPU_STATE_FAILED;
        while (1) __asm__ volatile("cli; hlt");
    }

    while (1)
    {
        __asm__ volatile("sti; hlt");

        if (scheduler_is_started() && interrupts_consume_reschedule())
            schedule_voluntary();
    }
}

void smp_boot_aps(void)
{
    serial_write_all("[SMP] Starting BOOTSTRAP sequence...\n");
    if (!g_cpu_count || g_cpu_count>HOBBYOS_MAX_CPUS) kpanic("AP bootstrap topology is invalid");
    if (g_cpu_count==1) { paging_bootstrap_not_needed(); return; }
    uint32_t ids[HOBBYOS_MAX_CPUS];
    uint64_t intended=0;
    for (unsigned i=0; i<g_cpu_count; ++i) {
        ids[i]=g_cpus[i].apic_id;
        if (!g_cpus[i].is_bsp) intended|=UINT64_C(1)<<i;
    }
    bootstrap_claim_t claim;
    uint64_t boot_cr3;
    if (!pmm_claim_bootstrap(&claim) || !paging_bootstrap_create(&claim,claim.generation,intended,ids,g_cpu_count,&boot_cr3))
        kpanic("AP bootstrap ownership or private root construction failed");

    serial_write_all("[SMP] Bootstrap CR3 created at ");
    smp_print_hex(boot_cr3);
    serial_write_all("\n");

    uint64_t trampoline_addr=claim.physical;

    uint64_t trampoline_len = (uint64_t)_trampoline_end - (uint64_t)_trampoline_start;
    if (trampoline_len > CPU_BOOT_PAGE_BYTES - CPU_BOOT_STACK_RESERVE)
        kpanic("AP bootstrap blob overlaps temporary stack");
    uint8_t *trampoline = physical_require_pointer(trampoline_addr, CPU_BOOT_PAGE_BYTES, PHYSICAL_RAM);
    memcpy(trampoline, _trampoline_start, trampoline_len);

    volatile uint64_t *ptr_cr3 = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_cr3 - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_cr4 = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_cr4 - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_efer = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_efer - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_cr0 = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_cr0 - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_pat = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_pat - (uint64_t)_trampoline_start));
    volatile uint32_t *ptr_status = (volatile uint32_t *)(trampoline + ((uint64_t)&smp_trampoline_status - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_stack = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_stack - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_entry = (volatile uint64_t *)(trampoline + ((uint64_t)&smp_trampoline_entry_ptr - (uint64_t)_trampoline_start));
    volatile uint32_t *ptr_x2flag = (volatile uint32_t *)(trampoline + ((uint64_t)&smp_trampoline_x2apic_flag - (uint64_t)_trampoline_start));
    volatile uint8_t *ptr_gdtr = (volatile uint8_t *)(trampoline + ((uint64_t)&smp_trampoline_gdtr - (uint64_t)_trampoline_start));

    const cpu_policy_t *policy = cpu_policy_get();
    if (!policy) kpanic("AP control policy is not published");
    uint64_t apic_msr = cpu_read_msr(0x1B);
    uint32_t x2apic_on = (apic_msr & (1ULL << 10)) ? 1 : 0;

    paging_boot_requirements_t paging_requirements;
    if (!paging_boot_requirements(&paging_requirements)) kpanic("AP paging requirements are not published");
    volatile uint32_t *ptr_paging_features = (volatile uint32_t *)(trampoline +
        ((uint64_t)&smp_trampoline_paging_features - (uint64_t)_trampoline_start));
    volatile uint32_t *ptr_physical_bits = (volatile uint32_t *)(trampoline +
        ((uint64_t)&smp_trampoline_physical_bits - (uint64_t)_trampoline_start));
    *ptr_paging_features = paging_requirements.extended_features;
    *ptr_physical_bits = paging_requirements.physical_bits;
    *ptr_cr3 = boot_cr3;
    *ptr_cr4 = policy->cr4;
    *ptr_efer = policy->efer_write;
    *ptr_cr0 = policy->cr0_final;
    *ptr_pat = policy->pat;
    *ptr_entry = (uint64_t)&ap_kernel_entry_asm;
    *ptr_x2flag = x2apic_on;

    uint32_t gdt_phys_addr = (uint32_t)(trampoline_addr + ((uint64_t)&smp_trampoline_gdt_start - (uint64_t)_trampoline_start));
    *(volatile uint32_t *)(ptr_gdtr + 2) = gdt_phys_addr;

    __asm__ volatile("mfence; wbinvd" ::: "memory");

    uint32_t sipi_vec = (uint32_t)(trampoline_addr >> 12);

    for (uint32_t i = 0; i < g_cpu_count; i++)
    {
        if (g_cpus[i].is_bsp)
            continue;

        SmpCpuInfo *cpu = &g_cpus[i];
        if (!paging_bootstrap_start(i,cpu->apic_id,claim.generation)) kpanic("AP start has no live bootstrap owner");
        cpu->state = CPU_STATE_PREPARE;

        if (cpu->stack_top < SMP_AP_STACK_BYTES)
            kpanic("AP stack extent is invalid");
        (void)physical_require_address(
            (const void *)(uintptr_t)(cpu->stack_top - SMP_AP_STACK_BYTES), SMP_AP_STACK_BYTES);
        *ptr_stack = cpu->stack_top;
        *ptr_status = CPU_BOOT_PENDING;

        __asm__ volatile("mfence" ::: "memory");

        lapic_delivery_result_t delivery = lapic_send_init(cpu->apic_id);
        if (delivery == LAPIC_DELIVERY_SUBMITTED)
            delivery = lapic_send_sipi(cpu->apic_id, sipi_vec);
        if (delivery != LAPIC_DELIVERY_SUBMITTED) {
            cpu->state = CPU_STATE_FAILED;
            char line[128];
            int n = ksnprintf(line, sizeof(line),
                "[SMP][CPU] ERROR code=AP_DELIVERY slot=%u apic=%u result=%u\n",
                i, cpu->apic_id, (unsigned)delivery);
            if (n > 0 && (size_t)n < sizeof(line)) serial_write_all(line);
            /* A triggered SIPI may still arrive; ONLINE cannot erase this error. */
            kpanic("AP checked IPI delivery failed");
        }

        int timeout = 10000000;
        bool online = false;

        while (timeout > 0)
        {
            if (*ptr_status > CPU_BOOT_APPLIED || cpu->state == CPU_STATE_FAILED)
                break;
            if (cpu->state == CPU_STATE_ONLINE)
            {
                online = true;
                break;
            }
            __asm__ volatile("pause");
            timeout--;
        }

        if (!online && *ptr_status <= CPU_BOOT_APPLIED && cpu->state != CPU_STATE_FAILED)
        {
            if (!paging_bootstrap_retry(i,cpu->apic_id,claim.generation))
                kpanic("AP retry outlived its bootstrap admission");
            delivery = lapic_send_sipi(cpu->apic_id, sipi_vec);
            if (delivery != LAPIC_DELIVERY_SUBMITTED) {
                cpu->state = CPU_STATE_FAILED;
                char line[128];
                int n = ksnprintf(line, sizeof(line),
                    "[SMP][CPU] ERROR code=AP_DELIVERY slot=%u apic=%u result=%u\n",
                    i, cpu->apic_id, (unsigned)delivery);
                if (n > 0 && (size_t)n < sizeof(line)) serial_write_all(line);
                kpanic("AP checked IPI delivery failed");
            }

            timeout = 10000000;
            while (timeout > 0)
            {
                if (*ptr_status > CPU_BOOT_APPLIED || cpu->state == CPU_STATE_FAILED)
                    break;
                if (cpu->state == CPU_STATE_ONLINE)
                {
                    online = true;
                    break;
                }
                __asm__ volatile("pause");
                timeout--;
            }

            if (!online)
            {
                cpu->state = CPU_STATE_FAILED;
                serial_write_all("[SMP][CPU] ERROR code=AP_BOOT_TIMEOUT apic=");
                smp_print_dec(cpu->apic_id);
                serial_write_all("\n");
            }
        }
        if (!online && (*ptr_status > CPU_BOOT_APPLIED || cpu->state == CPU_STATE_FAILED))
        {
            cpu->state = CPU_STATE_FAILED;
            char line[128];
            int n = ksnprintf(line, sizeof(line),
                "[CPU_STATE][AP_FAIL] slot=%u apic=%u reason=%u\n",
                i, cpu->apic_id, *ptr_status > CPU_BOOT_APPLIED ? *ptr_status : CPU_BOOT_READBACK);
            if (n > 0 && (size_t)n < sizeof(line)) serial_write_all(line);
            break;
        }
    }
    if (!paging_bootstrap_retire(ptr_cr3))
        kpanic("AP bootstrap retirement could not be proved");
    serial_write_all("[SMP] Boot sequence finished.\n");
}

#include "kernel_init.h"
#include "../memory/gdt.h"
#include "idt.h"
#include "../graphics/console.h"
#include "../graphics/graphics.h"
#include "../graphics/splash.h"
#include "../memory/pmem.h"
#include "../memory/paging.h"
#include "../memory/heap.h"
#include "../acpi/acpi.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../apic/ioapic.h"
#include "../apic/legacy_pic.h"
#include "../timer/hpet.h"
#include "clock.h"
#include "task_metrics.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/pci.h"
#include "../drivers/watchdog/intel_tco.h"
#include "../drivers/watchdog/acpi_wdat.h"
#include "../drivers/watchdog/acpi_wddt.h"
#include "../drivers/watchdog/acpi_wdrt.h"
#include "../drivers/serial.h"
#include "../core/input_router.h"
#include "modal_session.h"
#include "modal_ui.h"
#include "panic.h"
#include "timers.h"
#include "dpc.h"
#include "scheduler.h"
#include "interrupt_context.h"
#include "irq_bootstrap.h"
#include "selftest.h"
#include "runtime_ready.h"
#include "semaphore.h"
#include "../smp/smp_topology.h"
#include "../smp/smp_boot.h"
#include "list.h"
#include "queue.h"
#include "task.h"

#include <stdint.h>
#include <stddef.h>

static void kinit_debug(const char *msg)
{
    serial_write_all(msg);
}

static char *kinit_progress_append_text(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *kinit_progress_append_u64(char *out, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        *out++ = reverse[--count];
    return out;
}

static void kinit_progress(const char *stage)
{
    timer_clockevent_snapshot_t event = {0};
    (void)timer_clockevent_snapshot(&event);
    char line[176];
    char *p = kinit_progress_append_text(line, "[BOOT][PROGRESS] stage=");
    p = kinit_progress_append_text(p, stage);
    p = kinit_progress_append_text(p, " monotonic_ms=");
    p = kinit_progress_append_u64(p, timer_get_uptime_ms());
    p = kinit_progress_append_text(p, " clockevent_ticks=");
    p = kinit_progress_append_u64(p, event.total_ticks);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

void init_system_core(BootInfo *boot_info)
{
    irq_disable();
    runtime_ready_init();

    serial_init_from_bootinfo(boot_info);
    kinit_debug("[CORE] Serial init OK (dynamic ports from BootInfo)\n");

    kinit_debug("[CORE] Init GDT...\n");
    init_gdt();

    kinit_debug("[CORE] Init IDT...\n");
    init_idt();

    kinit_debug("[CORE] Init PMM...\n");
    init_pmm(boot_info->memory_map);

    kinit_debug("[CORE] Init Paging...\n");

    init_paging((uint64_t)boot_info->framebuffer->BaseAddress,
                (uint64_t)boot_info->framebuffer->BufferSize);

    kinit_debug("[CORE] Paging OK! Switched CR3.\n");

    kinit_debug("[CORE] Init Heap...\n");
    init_heap();

    kinit_debug("[CORE] Init Console...\n");
    console_init(boot_info);
    console_clear(CONSOLE_COLOR_BLACK);
    console_set_debug_enabled(1);

    void *test_heap = kmalloc(16);
    if (!test_heap)
        kpanic("CRITICAL: Heap Failed.");
    kfree(test_heap);

    kinit_debug("[CORE] Init ACPI...\n");
    init_acpi(boot_info->rsdp);

    acpi_list_tables_debug();
    acpi_enable_mode();

    intel_tco_disable();
    acpi_wdat_disable();
    acpi_wddt_disable();
    acpi_wdrt_disable();

    kinit_debug("[CORE] Init Interrupt Controller (MADT/APIC)...\n");
    if (!init_madt())
        kpanic("MADT validation failed");
    madt_snapshot_t madt;
    if (!madt_snapshot(&madt) ||
        !legacy_pic_quiesce(madt.pcat_compat != 0))
        kpanic("Legacy PIC quiescence failed");
    if (!init_lapic())
        kpanic("Local APIC quiescence failed");

    kinit_debug("[CORE] Init SMP Topology...\n");
    if (!smp_topology_init())
        kpanic("SMP topology validation failed");
    smp_prepare_cpu_structures();

    if (!irq_bootstrap_init() || !irq_bootstrap_controllers_quiescent())
        kpanic("Interrupt controller quiescence failed");

    init_hpet();
    if (!hpet_counter_access_selftest())
        kpanic("HPET counter access selftest failed");
    if (!clock_monotonic_init() || !clock_monotonic_selftest())
        kpanic("Monotonic HPET clock unavailable");
    if (!hpet_validation_selftest())
        kpanic("HPET validation selftest failed");
    if (!task_metrics_sampler_selftest() || !task_metrics_format_selftest() ||
        !task_metrics_long_selftest())
        kpanic("Task metrics selftest failed");
    if (!scheduler_bootstrap_selftest())
        kpanic("Scheduler pre-global guards failed");
    if (scheduler_global_init() != SCHED_BOOT_OK)
        kpanic("Scheduler global init failed");
    if (!scheduler_cancellation_selftest())
        kpanic("Cancellation guards failed");
    if (!madt_validation_selftest() || !legacy_pic_contract_selftest() ||
        !ioapic_model_selftest() || !lapic_timer_model_selftest() ||
        !hpet_timer0_model_selftest() || !irq_bootstrap_model_selftest() ||
        !timer_clockevent_model_selftest() ||
        !interrupt_context_selftest() ||
        !scheduler_preemption_gate_selftest() ||
        !graphics_binding_selftest() || !splash_model_selftest())
        kpanic("Interrupt bootstrap selftest failed");
    serial_write_all("[IRQ][SELFTEST] PASS\n");

    cpu_slot_t bsp_slot = smp_bsp_cpu_slot();
    if (bsp_slot == CPU_SLOT_INVALID || scheduler_cpu_init_bsp(bsp_slot) != SCHED_BOOT_OK)
        kpanic("Scheduler BSP init failed");

    keyboard_init();
    if (!irq_bootstrap_routes_prepared())
        kpanic("Interrupt route preparation failed");

    timer_init();
    timers_init();
    lapic_timer_calibration_t bsp_cal;
    if (!lapic_timer_calibrate(1000, &bsp_cal) ||
        !lapic_timer_prepare_periodic(INT_VECTOR_LAPIC_TIMER, &bsp_cal))
        kpanic("BSP LAPIC timer calibration failed");
    if (scheduler_cpu_mark_timer_ready(bsp_slot) != SCHED_BOOT_OK ||
        !scheduler_cpu_is_ready(bsp_slot) ||
        !irq_bootstrap_cpu_prepare(bsp_slot) ||
        !smp_mark_cpu_online(bsp_slot))
        kpanic("Scheduler BSP timer/online transition failed");
    smp_log_cpu_online(bsp_slot, "BSP");

    if (!scheduler_bootstrap_selftest())
        kpanic("Scheduler post-init guards failed");

    smp_boot_aps();
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CPUS_PREPARED ||
        smp_online_cpu_count() != g_cpu_count || smp_failed_cpu_count())
        kpanic("AP interrupt preparation barrier failed");

    kinit_debug("[CORE] Init Drivers & Scheduler...\n");
    dpc_init();


    input_router_init();
    modal_session_init();
    if (!modal_ui_init())
        kpanic("Modal UI runtime initialization failed");

    thread_create_named_with_class_flags(input_thread_entry, NULL, TASK_CLASS_INTERACTIVE, "input-thread", TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED);

    extern void shell_thread_entry(void *arg);
    thread_create_named_with_class_flags(shell_thread_entry, NULL, TASK_CLASS_INTERACTIVE, "shell-thread", TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED);


    extern void reaper_thread_entry(void *arg);
    thread_create_named_with_class_flags(reaper_thread_entry, NULL, TASK_CLASS_NORMAL, "reaper", TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED);

    if (!scheduler_validate_task_identity())
        kpanic("Task identity validation failed");

    serial_write_all("[BOOTTRACE][BSP] 1 before-scheduler-start\n");
    if (scheduler_start() != SCHED_BOOT_OK)
        kpanic("Scheduler start readiness validation failed");
    serial_write_all("[BOOTTRACE][BSP] 2 after-scheduler-start\n");

#ifdef HOBBYOS_IRQ_NEGATIVE_EARLY_PREEMPTION
    if (!scheduler_cpu_enable_irq_preemption(bsp_slot)) {
        serial_write_all(
            "[IRQ][NEGATIVE] PREEMPTION_BEFORE_HANDOFF_DETECTED\n");
        kpanic("Early IRQ preemption negative detected");
    }
#endif

    serial_write_all("[BOOTTRACE][BSP] 3 before-irq-enable\n");
    if (!irq_bootstrap_verify_bsp_lapic(250000u))
        kpanic("BSP LAPIC enter-return probe failed");
    if (!irq_bootstrap_verify_hpet_clocksource(4096u))
        kpanic("HPET clocksource probe failed");
    if (!timer_clockevent_configure_bsp_lapic(bsp_slot, 1000u) ||
        !timer_clockevent_activate() ||
        !irq_bootstrap_activate_clockevent())
        kpanic("BSP LAPIC runtime clockevent activation failed");
    if (!scheduler_bootstrap_handoff_current_cpu() ||
        !scheduler_cpu_enable_irq_preemption(bsp_slot) ||
        !irq_bootstrap_cpu_complete_runtime(bsp_slot))
        kpanic("BSP scheduler bootstrap handoff failed");
    if (!irq_bootstrap_release_cpus())
        kpanic("AP interrupt release failed");
    kinit_progress("CPU_RELEASE");
    irq_enable();
    serial_write_all("[BOOTTRACE][BSP] 4 after-irq-enable\n");
    if (!irq_bootstrap_wait_all_runtime_ready(10000000u))
        kpanic("CPU interrupt runtime readiness failed");

    console_begin_batch();
    serial_write_all("[BOOTTRACE][BSP] 5 before-pci-init\n");
    kinit_progress("PCI_BEGIN"); pci_init();
    runtime_ready_mark_pci_scan_complete();
    kinit_progress("PCI_SCAN_COMPLETE");

    console_end_batch();

    kinit_progress("CORE_COMPLETE");
    kinit_debug("[CORE] System Core Initialization Complete.\n");
}

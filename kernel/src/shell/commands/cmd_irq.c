#include "cmd_irq.h"

#include "../../apic/ioapic.h"
#include "../../apic/lapic.h"
#include "../../apic/legacy_pic.h"
#include "../../core/interrupt_context.h"
#include "../../core/irq_bootstrap.h"
#include "../../core/irq_stats.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../graphics/console.h"
#include "../../libc/string.h"
#include "../../timer/hpet.h"

static void both_text(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

static void serial_u64(uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(digits[--count]);
}

static void both_u64(uint64_t value)
{
    console_print_dec(value);
    serial_u64(value);
}

static void both_hex(uint64_t value)
{
    console_print_hex(value);
    serial_write_hex64_all(value);
}

static void field_u64(const char *name, uint64_t value)
{
    both_text(" ");
    both_text(name);
    both_text("=");
    both_u64(value);
}

static void field_hex(const char *name, uint64_t value)
{
    both_text(" ");
    both_text(name);
    both_text("=");
    both_hex(value);
}

static void field_text(const char *name, const char *value)
{
    both_text(" ");
    both_text(name);
    both_text("=");
    both_text(value);
}

static const char *bootstrap_state_name(irq_bootstrap_state_t state)
{
    switch (state)
    {
    case IRQ_BOOTSTRAP_OFF: return "OFF";
    case IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT:
        return "CONTROLLERS_QUIESCENT";
    case IRQ_BOOTSTRAP_ROUTES_PREPARED: return "ROUTES_PREPARED";
    case IRQ_BOOTSTRAP_CPUS_PREPARED: return "CPUS_PREPARED";
    case IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED: return "BSP_LAPIC_VERIFIED";
    case IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED:
        return "HPET_CLOCKSOURCE_VERIFIED";
    case IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE: return "CLOCKEVENT_ACTIVE";
    case IRQ_BOOTSTRAP_SERVICES_ACTIVE: return "SERVICES_ACTIVE";
    case IRQ_BOOTSTRAP_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

static const char *polarity_name(irq_polarity_t polarity)
{
    switch (polarity)
    {
    case IRQ_POLARITY_HIGH: return "high";
    case IRQ_POLARITY_LOW: return "low";
    default: return "conforms";
    }
}

static const char *trigger_name(irq_trigger_t trigger)
{
    switch (trigger)
    {
    case IRQ_TRIGGER_EDGE: return "edge";
    case IRQ_TRIGGER_LEVEL: return "level";
    default: return "conforms";
    }
}

static void irq_usage(void)
{
    both_text("Usage:\n");
    both_text("  irq              - show simple counters\n");
    both_text("  irq reset        - reset simple counters\n");
    both_text("  irq check        - validate interrupt runtime\n");
    both_text("  irq controllers  - show controller snapshots\n");
    both_text("  irq routes       - show prepared routes\n");
    both_text("  irq boot         - show bootstrap and CPU state\n");
    both_text("  irq help         - show this help\n");
    both_text("Alias: int\n");
}

static int irq_check(void)
{
    irq_bootstrap_snapshot_t boot;
    ioapic_registry_snapshot_t registry;
    timer_clockevent_snapshot_t event;
    hpet_runtime_snapshot_t hpet;
    uint64_t unexpected = 0;
    uint64_t imbalance = 0;
    cpu_slot_t unstable_slot = CPU_SLOT_INVALID;
    interrupt_cpu_snapshot_t unstable = {0};
    bool snapshots_ok = irq_bootstrap_snapshot(&boot) &&
                        ioapic_registry_snapshot(&registry) &&
                        timer_clockevent_snapshot(&event) &&
                        hpet_runtime_snapshot(&hpet);
    if (snapshots_ok)
    {
        for (cpu_slot_t slot = 0; slot < boot.cpus_expected; slot++)
        {
            interrupt_cpu_snapshot_t journal;
            if (!interrupt_context_snapshot_stable(
                    slot, &journal, INTERRUPT_CONTEXT_SNAPSHOT_ATTEMPTS))
            {
                unstable_slot = slot;
                (void)interrupt_context_snapshot(slot, &unstable);
                snapshots_ok = false;
                break;
            }
            unexpected += journal.unexpected;
            imbalance += journal.imbalance;
        }
    }
    bool pass = snapshots_ok && !unexpected && !imbalance &&
                irq_bootstrap_validate();
    both_text("[IRQ][CHECK] ");
    both_text(pass ? "PASS" : "FAIL");
    field_text("clocksource", snapshots_ok ? "HPET" : "UNKNOWN");
    field_text("clockevent", snapshots_ok &&
        event.source == TIMER_CLOCKEVENT_BSP_LAPIC ?
        "BSP_LAPIC" : "NONE");
    field_u64("period_us", snapshots_ok ? event.period_us : 0);
    field_text("hpet_timer0", snapshots_ok &&
        boot.hpet_timer0_quiescent ? "QUIESCENT" : "NOT_QUIESCENT");
    field_u64("non_bsp_ticks", snapshots_ok ? event.non_bsp_attempts : 0);
    field_u64("stray_hpet", snapshots_ok ? hpet.stray_irqs : 0);
    field_u64("cpus", snapshots_ok ? boot.cpus_runtime_ready : 0);
    both_text("/");
    both_u64(snapshots_ok ? boot.cpus_expected : 0);
    field_u64("controllers", snapshots_ok ? registry.controllers : 0);
    field_u64("routes", snapshots_ok ? registry.prepared_routes : 0);
    field_u64("unexpected", unexpected);
    field_u64("imbalance", imbalance);
    if (unstable_slot != CPU_SLOT_INVALID)
    {
        field_u64("unstable_slot", unstable_slot);
        field_u64("entered", unstable.entered_total);
        field_u64("returned", unstable.returned_total);
        field_u64("depth", unstable.depth);
        field_u64("epilogue", unstable.preempt_epilogue);
    }
    both_text("\n");
    return pass ? 0 : 1;
}

static int irq_controllers(void)
{
    legacy_pic_snapshot_t pic;
    ioapic_registry_snapshot_t registry;
    irq_bootstrap_snapshot_t boot;
    hpet_runtime_snapshot_t hpet;
    if (!legacy_pic_snapshot(&pic) ||
        !ioapic_registry_snapshot(&registry) ||
        !irq_bootstrap_snapshot(&boot) || !hpet_runtime_snapshot(&hpet))
    {
        both_text("[IRQ][CONTROLLERS] FAIL snapshot=0\n");
        return 1;
    }
    both_text("[IRQ][PIC]");
    field_hex("master", pic.master_imr);
    field_hex("slave", pic.slave_imr);
    field_u64("pcat", pic.pcat_declared);
    field_u64("defensive", pic.defensive_attempt);
    field_u64("quiescent", pic.quiescent);
    both_text("\n");

    for (uint32_t index = 0; index < registry.controllers; index++)
    {
        ioapic_controller_t controller;
        if (!ioapic_controller_at(index, &controller))
            return 1;
        both_text("[IRQ][IOAPIC]");
        field_u64("index", index);
        field_u64("id", controller.id);
        field_hex("base", controller.mmio_base);
        field_u64("gsi_first", controller.gsi_base);
        field_u64("gsi_last", controller.gsi_base +
                  controller.redirection_count - 1u);
        field_u64("entries", controller.redirection_count);
        field_u64("quiescent", controller.quiescent);
        both_text("\n");
    }

    for (cpu_slot_t slot = 0; slot < boot.cpus_expected; slot++)
    {
        lapic_quiescent_snapshot_t lapic;
        if (!lapic_quiescent_snapshot(slot, &lapic))
            return 1;
        both_text("[IRQ][LAPIC]");
        field_u64("slot", slot);
        field_u64("apic", lapic.apic_id);
        field_text("mode", lapic.x2apic ? "x2apic" : "xapic");
        field_u64("lvt_masked", lapic.masked_count);
        field_hex("lint0", lapic.lvt_lint0);
        field_hex("lint1", lapic.lvt_lint1);
        field_hex("esr", lapic.esr_after);
        both_text("\n");
    }

    both_text("[IRQ][HPET]");
    field_text("clocksource", hpet.main_counter_enabled ? "ACTIVE" : "OFF");
    field_text("timer0", boot.hpet_timer0_quiescent ?
        "QUIESCENT" : "NOT_QUIESCENT");
    field_u64("irq_enabled", hpet.timer0_interrupt_enabled);
    field_u64("periodic", hpet.timer0_periodic_enabled);
    field_u64("fsb", hpet.timer0_fsb_enabled);
    field_u64("route_prepared", hpet.timer0_route_prepared);
    field_u64("route_enabled", hpet.timer0_route_enabled);
    field_u64("pending", hpet.timer0_pending);
    field_u64("legacy", hpet.legacy_replacement_enabled);
    field_u64("stray", hpet.stray_irqs);
    both_text("\n");
    return 0;
}

static int irq_routes(void)
{
    uint32_t count = ioapic_route_count();
    for (uint32_t index = 0; index < count; index++)
    {
        interrupt_route_t route;
        if (!ioapic_route_at(index, &route))
            return 1;
        both_text("[IRQ][ROUTE]");
        field_text("owner", ioapic_route_owner_name(route.owner));
        field_u64("gsi", route.gsi);
        field_u64("controller", route.controller_index);
        field_u64("pin", route.pin);
        field_u64("vector", route.vector);
        field_u64("destination", route.destination_apic_id);
        field_text("polarity", polarity_name(route.polarity));
        field_text("trigger", trigger_name(route.trigger));
        field_u64("masked", route.masked);
        field_u64("enabled", route.masked ? 0u : 1u);
        both_text("\n");
    }
    both_text("[IRQ][ROUTES] PASS count=");
    both_u64(count);
    both_text("\n");
    return 0;
}

static int irq_boot(void)
{
    irq_bootstrap_snapshot_t boot;
    timer_clockevent_snapshot_t event;
    hpet_runtime_snapshot_t hpet;
    if (!irq_bootstrap_snapshot(&boot) ||
        !timer_clockevent_snapshot(&event) ||
        !hpet_runtime_snapshot(&hpet))
    {
        both_text("[IRQ][BOOT] FAIL snapshot=0\n");
        return 1;
    }
    both_text("[IRQ][BOOT]");
    field_text("state", bootstrap_state_name(boot.state));
    field_u64("cpus_prepared", boot.cpus_prepared);
    field_u64("cpus_verified", boot.cpus_verified);
    field_u64("runtime_ready", boot.cpus_runtime_ready);
    both_text("/");
    both_u64(boot.cpus_expected);
    field_u64("handoffs", boot.handoffs_complete);
    field_u64("preemption", boot.preemption_enabled);
    field_u64("failed", boot.failed_cpus);
    both_text("\n");

    both_text("[IRQ][BOOT_PROBES]");
    field_u64("bsp_probe_vector", boot.bsp_probe_vector);
    field_u64("bsp_probe_entered", boot.bsp_lapic_entered);
    field_u64("bsp_probe_returned", boot.bsp_lapic_returned);
    field_u64("hpet_clocksource_verified", boot.hpet_clocksource_verified);
    field_u64("hpet_samples", boot.hpet_probe_samples);
    field_u64("hpet_counter_before", boot.hpet_counter_before);
    field_u64("hpet_counter_after", boot.hpet_counter_after);
    field_u64("hpet_counter_delta", boot.hpet_counter_delta);
    field_u64("keyboard_gsi", boot.keyboard_gsi);
    both_text("\n");

    both_text("[IRQ][BOOT_CLOCK]");
    field_text("clocksource", "HPET");
    field_u64("hpet_clocksource_verified", boot.hpet_clocksource_verified);
    field_u64("hpet_timer0_quiescent", boot.hpet_timer0_quiescent);
    field_u64("hpet_timer0_irq_enabled", hpet.timer0_interrupt_enabled);
    field_u64("hpet_timer0_route_enabled", hpet.timer0_route_enabled);
    field_text("clockevent", event.source == TIMER_CLOCKEVENT_BSP_LAPIC ?
        "BSP_LAPIC" : "NONE");
    field_u64("clockevent_bsp_slot", event.bsp_slot);
    field_u64("clockevent_period_us", event.period_us);
    field_u64("clockevent_ticks", event.total_ticks);
    field_u64("clockevent_non_bsp", event.non_bsp_attempts);
    field_u64("clockevent_early", event.early_attempts);
    field_u64("clockevent_regressions", event.timestamp_regressions);
    field_u64("hpet_stray_irqs", hpet.stray_irqs);
    both_text("\n");

    for (cpu_slot_t slot = 0; slot < boot.cpus_expected; slot++)
    {
        irq_bootstrap_cpu_snapshot_t cpu;
        interrupt_cpu_snapshot_t journal;
        if (!irq_bootstrap_cpu_snapshot(slot, &cpu) ||
            !interrupt_context_snapshot_stable(slot, &journal, 128u))
            return 1;
        both_text("[IRQ][BOOT_CPU]");
        field_u64("slot", slot);
        field_u64("apic", cpu.apic_id);
        field_u64("first_vector", journal.first_vector);
        field_u64("entered", journal.entered_total);
        field_u64("returned", journal.returned_total);
        field_u64("depth", journal.depth);
        field_u64("unexpected", journal.unexpected);
        field_u64("handoff", cpu.handoff_complete);
        field_u64("preempt", cpu.preemption_enabled);
        field_u64("ready", cpu.runtime_ready);
        both_text("\n");
    }
    return 0;
}

int cmd_irq(int argc, char **argv)
{
    if (argc == 1)
    {
        irq_stats_dump();
        return 0;
    }
    if (argc != 2)
    {
        irq_usage();
        return 1;
    }
    if (strcmp(argv[1], "reset") == 0)
    {
        irq_stats_reset();
        both_text("[IRQ] Counters reset.\n");
        return 0;
    }
    if (strcmp(argv[1], "check") == 0)
        return irq_check();
    if (strcmp(argv[1], "controllers") == 0)
        return irq_controllers();
    if (strcmp(argv[1], "routes") == 0)
        return irq_routes();
    if (strcmp(argv[1], "boot") == 0)
        return irq_boot();
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "?") == 0)
    {
        irq_usage();
        return 0;
    }
    irq_usage();
    return 1;
}

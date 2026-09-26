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
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../memory/heap.h"
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

#define IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS 5000u
#define IRQ_DIAGNOSTIC_SNAPSHOT_POLL_ATTEMPTS 128u
#define IRQ_BOOT_RECORD_CAPACITY 768u

typedef enum
{
    IRQ_BOOT_SNAPSHOT_OK = 0,
    IRQ_BOOT_SNAPSHOT_UNAVAILABLE,
    IRQ_BOOT_SNAPSHOT_STRUCTURAL,
    IRQ_BOOT_SNAPSHOT_TIMEOUT
} irq_boot_snapshot_result_t;

typedef struct
{
    irq_bootstrap_cpu_snapshot_t cpu;
    interrupt_cpu_snapshot_t journal;
    interrupt_cpu_snapshot_t last;
} irq_boot_sample_t;

#if defined(HOBBYOS_IRQ_SNAPSHOT_HOST_TEST)
static size_t g_irq_boot_test_record_capacity = IRQ_BOOT_RECORD_CAPACITY;
#endif

static size_t irq_boot_record_capacity(void)
{
#if defined(HOBBYOS_IRQ_SNAPSHOT_HOST_TEST)
    return g_irq_boot_test_record_capacity < IRQ_BOOT_RECORD_CAPACITY ?
        g_irq_boot_test_record_capacity : IRQ_BOOT_RECORD_CAPACITY;
#else
    return IRQ_BOOT_RECORD_CAPACITY;
#endif
}

static bool irq_boot_publish_record(char *record, const char *format, ...)
{
    size_t capacity = irq_boot_record_capacity();
    va_list args;
    va_start(args, format);
    int length = kvsnprintf(record, capacity, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= capacity)
        return false;
    console_write(record);
    serial_write_all(record);
    return true;
}

static int irq_boot_format_error(void)
{
    both_text("[IRQ][BOOT_ERROR] reason=format\n");
    both_text("[IRQ][BOOT_RESULT] FAIL reason=format\n");
    return 1;
}

static const char *irq_diagnostic_snapshot_result_name(
    irq_boot_snapshot_result_t result)
{
    switch (result)
    {
    case IRQ_BOOT_SNAPSHOT_UNAVAILABLE: return "unavailable";
    case IRQ_BOOT_SNAPSHOT_STRUCTURAL: return "structural";
    case IRQ_BOOT_SNAPSHOT_TIMEOUT: return "timeout";
    default: return "ok";
    }
}

static irq_boot_snapshot_result_t irq_diagnostic_journal_snapshot(
    cpu_slot_t slot, uint64_t deadline, interrupt_cpu_snapshot_t *out,
    interrupt_cpu_snapshot_t *last, uint32_t *polls)
{
    for (;;)
    {
        (*polls)++;
        if (interrupt_context_snapshot_stable(
                slot, out, IRQ_DIAGNOSTIC_SNAPSHOT_POLL_ATTEMPTS))
            return IRQ_BOOT_SNAPSHOT_OK;
        if (!interrupt_context_snapshot(slot, last) || !last->initialized)
            return IRQ_BOOT_SNAPSHOT_UNAVAILABLE;
        if (last->underflow || last->mismatch ||
            last->returned_total > last->entered_total)
            return IRQ_BOOT_SNAPSHOT_STRUCTURAL;
        if (timer_get_uptime_ms() >= deadline)
            return IRQ_BOOT_SNAPSHOT_TIMEOUT;
        timer_sleep(1);
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
    irq_bootstrap_snapshot_t boot = {0};
    ioapic_registry_snapshot_t registry = {0};
    timer_clockevent_snapshot_t event = {0};
    hpet_runtime_snapshot_t hpet = {0};
    uint64_t unexpected = 0;
    uint64_t imbalance = 0;
    uint64_t snapshot_started = timer_get_uptime_ms();
    uint64_t snapshot_deadline = UINT64_MAX - snapshot_started <
        IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS ? UINT64_MAX :
        snapshot_started + IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS;
    uint32_t snapshot_polls = 0;
    uint32_t validation_polls = 0;
    cpu_slot_t failed_slot = CPU_SLOT_INVALID;
    interrupt_cpu_snapshot_t failed_snapshot = {0};
    const char *failure_reason = "snapshot-unavailable";
    bool structural_failure = false;
    bool pass = false;

    for (;;)
    {
        unexpected = 0;
        imbalance = 0;
        failed_slot = CPU_SLOT_INVALID;
        bool snapshots_ok = irq_bootstrap_snapshot(&boot) &&
                            ioapic_registry_snapshot(&registry) &&
                            timer_clockevent_snapshot(&event) &&
                            hpet_runtime_snapshot(&hpet);
        if (snapshots_ok &&
            (boot.state != IRQ_BOOTSTRAP_SERVICES_ACTIVE ||
             boot.cpus_runtime_ready != boot.cpus_expected ||
             boot.failed_cpus || boot.runtime_ready_abort ||
             !boot.hpet_timer0_quiescent ||
             event.source != TIMER_CLOCKEVENT_BSP_LAPIC ||
             event.period_us != 1000u || event.non_bsp_attempts ||
             event.timestamp_regressions || hpet.stray_irqs ||
             !registry.initialized || !registry.controllers ||
             registry.prepared_routes < 1u))
        {
            failure_reason = "runtime-contract";
            structural_failure = true;
        }
        for (cpu_slot_t slot = 0;
             snapshots_ok && !structural_failure &&
             slot < boot.cpus_expected; slot++)
        {
            interrupt_cpu_snapshot_t journal = {0};
            irq_boot_snapshot_result_t result =
                irq_diagnostic_journal_snapshot(
                    slot, snapshot_deadline, &journal, &failed_snapshot,
                    &snapshot_polls);
            if (result != IRQ_BOOT_SNAPSHOT_OK)
            {
                failed_slot = slot;
                failure_reason = irq_diagnostic_snapshot_result_name(result);
                snapshots_ok = false;
                break;
            }
            unexpected += journal.unexpected;
            imbalance += journal.imbalance;
            if (journal.unexpected || journal.imbalance || journal.underflow ||
                journal.mismatch ||
                journal.returned_total > journal.entered_total)
            {
                failed_slot = slot;
                failed_snapshot = journal;
                failure_reason = "journal-structural";
                structural_failure = true;
                break;
            }
        }
        if (structural_failure)
            break;
        if (snapshots_ok)
        {
            validation_polls++;
            if (irq_bootstrap_validate())
            {
                pass = true;
                failure_reason = "none";
                break;
            }
            failure_reason = "bootstrap-validation";
        }
        if (timer_get_uptime_ms() >= snapshot_deadline)
            break;
        timer_sleep(1);
    }

    both_text("[IRQ][CHECK] ");
    both_text(pass ? "PASS" : "FAIL");
    field_text("clocksource", hpet.available ? "HPET" : "UNKNOWN");
    field_text("clockevent", event.source == TIMER_CLOCKEVENT_BSP_LAPIC ?
        "BSP_LAPIC" : "NONE");
    field_u64("period_us", event.period_us);
    field_text("hpet_timer0",
        boot.hpet_timer0_quiescent ? "QUIESCENT" : "NOT_QUIESCENT");
    field_u64("non_bsp_ticks", event.non_bsp_attempts);
    field_u64("stray_hpet", hpet.stray_irqs);
    field_u64("cpus", boot.cpus_runtime_ready);
    both_text("/");
    both_u64(boot.cpus_expected);
    field_u64("controllers", registry.controllers);
    field_u64("routes", registry.prepared_routes);
    field_u64("unexpected", unexpected);
    field_u64("imbalance", imbalance);
    field_u64("snapshot_polls", snapshot_polls);
    field_u64("validation_polls", validation_polls);
    field_u64("elapsed_ms", timer_get_uptime_ms() - snapshot_started);
    field_text("reason", pass ? "ok" : failure_reason);
    if (failed_slot != CPU_SLOT_INVALID)
    {
        field_u64("failed_slot", failed_slot);
        field_u64("entered", failed_snapshot.entered_total);
        field_u64("returned", failed_snapshot.returned_total);
        field_u64("depth", failed_snapshot.depth);
        field_u64("epilogue", failed_snapshot.preempt_epilogue);
        field_u64("underflow", failed_snapshot.underflow);
        field_u64("mismatch", failed_snapshot.mismatch);
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
    irq_bootstrap_snapshot_t boot = {0};
    timer_clockevent_snapshot_t event = {0};
    hpet_runtime_snapshot_t hpet = {0};
    char record[IRQ_BOOT_RECORD_CAPACITY];
    if (!irq_bootstrap_snapshot(&boot) ||
        !timer_clockevent_snapshot(&event) ||
        !hpet_runtime_snapshot(&hpet))
    {
        both_text("[IRQ][BOOT] FAIL snapshot=0\n");
        both_text("[IRQ][BOOT_RESULT] FAIL snapshots=0 reason=snapshot-unavailable\n");
        return 1;
    }

    if (!boot.cpus_expected || boot.cpus_expected > HOBBYOS_MAX_CPUS)
    {
        if (!irq_boot_publish_record(
                record,
                "[IRQ][BOOT_ERROR] reason=topology-invalid cpus=%u max=%u\n",
                (unsigned int)boot.cpus_expected,
                (unsigned int)HOBBYOS_MAX_CPUS) ||
            !irq_boot_publish_record(
                record,
                "[IRQ][BOOT_RESULT] FAIL snapshots=0 reason=topology-invalid\n"))
            return irq_boot_format_error();
        return 1;
    }

    size_t samples_bytes = sizeof(irq_boot_sample_t) * boot.cpus_expected;
    irq_boot_sample_t *samples = (irq_boot_sample_t *)kmalloc(samples_bytes);
    if (!samples)
    {
        both_text("[IRQ][BOOT_ERROR] reason=storage-unavailable\n");
        both_text("[IRQ][BOOT_RESULT] FAIL snapshots=0 reason=storage-unavailable\n");
        return 1;
    }
    memset(samples, 0, samples_bytes);

    uint64_t snapshot_started = timer_get_uptime_ms();
    uint64_t snapshot_deadline = UINT64_MAX - snapshot_started <
        IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS ? UINT64_MAX :
        snapshot_started + IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS;
    uint32_t snapshot_polls = 0;
    uint32_t snapshots = 0;
    cpu_slot_t failed_slot = CPU_SLOT_INVALID;
    const char *failure_reason = "none";
    irq_boot_snapshot_result_t snapshot_result = IRQ_BOOT_SNAPSHOT_OK;
    for (cpu_slot_t slot = 0; slot < boot.cpus_expected; slot++)
    {
        irq_boot_sample_t *sample = &samples[slot];
        if (!irq_bootstrap_cpu_snapshot(slot, &sample->cpu))
        {
            failed_slot = slot;
            failure_reason = "bootstrap-unavailable";
            snapshot_result = IRQ_BOOT_SNAPSHOT_UNAVAILABLE;
            break;
        }
        if (slot && timer_get_uptime_ms() >= snapshot_deadline)
        {
            failed_slot = slot;
            failure_reason = "timeout";
            snapshot_result = IRQ_BOOT_SNAPSHOT_TIMEOUT;
            break;
        }
        snapshot_result = irq_diagnostic_journal_snapshot(
            slot, snapshot_deadline, &sample->journal, &sample->last,
            &snapshot_polls);
        if (snapshot_result != IRQ_BOOT_SNAPSHOT_OK)
        {
            failed_slot = slot;
            failure_reason =
                irq_diagnostic_snapshot_result_name(snapshot_result);
            break;
        }
        snapshots++;
    }
    uint64_t acquisition_finished = timer_get_uptime_ms();
    uint64_t acquisition_ms = acquisition_finished - snapshot_started;
    uint64_t publication_started = acquisition_finished;

    bool formatted = irq_boot_publish_record(
        record,
        "[IRQ][BOOT] state=%s cpus_prepared=%u cpus_verified=%u"
        " runtime_ready=%u/%u handoffs=%u preemption=%u failed=%u\n",
        bootstrap_state_name(boot.state), (unsigned int)boot.cpus_prepared,
        (unsigned int)boot.cpus_verified,
        (unsigned int)boot.cpus_runtime_ready,
        (unsigned int)boot.cpus_expected,
        (unsigned int)boot.handoffs_complete,
        (unsigned int)boot.preemption_enabled,
        (unsigned int)boot.failed_cpus);
    formatted = formatted && irq_boot_publish_record(
        record,
        "[IRQ][BOOT_PROBES] bsp_probe_vector=%u bsp_probe_entered=%llu"
        " bsp_probe_returned=%llu hpet_clocksource_verified=%u"
        " hpet_samples=%u hpet_counter_before=%llu"
        " hpet_counter_after=%llu hpet_counter_delta=%llu keyboard_gsi=%u\n",
        (unsigned int)boot.bsp_probe_vector,
        (unsigned long long)boot.bsp_lapic_entered,
        (unsigned long long)boot.bsp_lapic_returned,
        (unsigned int)boot.hpet_clocksource_verified,
        (unsigned int)boot.hpet_probe_samples,
        (unsigned long long)boot.hpet_counter_before,
        (unsigned long long)boot.hpet_counter_after,
        (unsigned long long)boot.hpet_counter_delta,
        (unsigned int)boot.keyboard_gsi);
    formatted = formatted && irq_boot_publish_record(
        record,
        "[IRQ][BOOT_CLOCK] clocksource=HPET hpet_clocksource_verified=%u"
        " hpet_timer0_quiescent=%u hpet_timer0_irq_enabled=%u"
        " hpet_timer0_route_enabled=%u clockevent=%s"
        " clockevent_bsp_slot=%u clockevent_period_us=%u"
        " clockevent_ticks=%llu clockevent_non_bsp=%llu"
        " clockevent_early=%llu clockevent_regressions=%llu"
        " hpet_stray_irqs=%llu\n",
        (unsigned int)boot.hpet_clocksource_verified,
        (unsigned int)boot.hpet_timer0_quiescent,
        (unsigned int)hpet.timer0_interrupt_enabled,
        (unsigned int)hpet.timer0_route_enabled,
        event.source == TIMER_CLOCKEVENT_BSP_LAPIC ? "BSP_LAPIC" : "NONE",
        (unsigned int)event.bsp_slot,
        (unsigned int)event.period_us,
        (unsigned long long)event.total_ticks,
        (unsigned long long)event.non_bsp_attempts,
        (unsigned long long)event.early_attempts,
        (unsigned long long)event.timestamp_regressions,
        (unsigned long long)hpet.stray_irqs);
    for (cpu_slot_t slot = 0; formatted && slot < snapshots; slot++)
    {
        irq_boot_sample_t *sample = &samples[slot];
        formatted = irq_boot_publish_record(
            record,
            "[IRQ][BOOT_CPU] slot=%u apic=%u first_vector=%u"
            " entered=%llu returned=%llu depth=%u unexpected=%llu"
            " handoff=%u preempt=%u ready=%u\n",
            (unsigned int)slot, (unsigned int)sample->cpu.apic_id,
            (unsigned int)sample->journal.first_vector,
            (unsigned long long)sample->journal.entered_total,
            (unsigned long long)sample->journal.returned_total,
            (unsigned int)sample->journal.depth,
            (unsigned long long)sample->journal.unexpected,
            (unsigned int)sample->cpu.handoff_complete,
            (unsigned int)sample->cpu.preemption_enabled,
            (unsigned int)sample->cpu.runtime_ready);
    }
    if (formatted && failed_slot != CPU_SLOT_INVALID)
    {
        irq_boot_sample_t *failed = &samples[failed_slot];
        formatted = irq_boot_publish_record(
            record,
            "[IRQ][BOOT_SNAPSHOT] FAIL slot=%u reason=%s polls=%u"
            " elapsed_ms=%llu entered=%llu returned=%llu depth=%u"
            " epilogue=%u underflow=%llu mismatch=%llu\n",
            (unsigned int)failed_slot, failure_reason,
            (unsigned int)snapshot_polls,
            (unsigned long long)acquisition_ms,
            (unsigned long long)failed->last.entered_total,
            (unsigned long long)failed->last.returned_total,
            (unsigned int)failed->last.depth,
            (unsigned int)failed->last.preempt_epilogue,
            (unsigned long long)failed->last.underflow,
            (unsigned long long)failed->last.mismatch);
    }
    if (!formatted)
    {
        kfree(samples);
        return irq_boot_format_error();
    }

    uint64_t publication_finished = timer_get_uptime_ms();
    uint64_t publication_ms = publication_finished - publication_started;
    uint64_t elapsed_ms = publication_finished - snapshot_started;
    if (failed_slot == CPU_SLOT_INVALID)
        formatted = irq_boot_publish_record(
            record,
            "[IRQ][BOOT_RESULT] PASS snapshots=%u polls=%u elapsed_ms=%llu"
            " acquisition_ms=%llu publication_ms=%llu budget_ms=%u\n",
            (unsigned int)snapshots, (unsigned int)snapshot_polls,
            (unsigned long long)elapsed_ms,
            (unsigned long long)acquisition_ms,
            (unsigned long long)publication_ms,
            (unsigned int)IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS);
    else
        formatted = irq_boot_publish_record(
            record,
            "[IRQ][BOOT_RESULT] FAIL snapshots=%u failed_slot=%u reason=%s"
            " polls=%u elapsed_ms=%llu acquisition_ms=%llu"
            " publication_ms=%llu budget_ms=%u\n",
            (unsigned int)snapshots, (unsigned int)failed_slot,
            failure_reason, (unsigned int)snapshot_polls,
            (unsigned long long)elapsed_ms,
            (unsigned long long)acquisition_ms,
            (unsigned long long)publication_ms,
            (unsigned int)IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS);
    kfree(samples);
    if (!formatted)
        return irq_boot_format_error();
    return snapshot_result == IRQ_BOOT_SNAPSHOT_OK ? 0 : 1;
}

#if defined(HOBBYOS_IRQ_SNAPSHOT_HOST_TEST)
int irq_boot_for_test(void)
{
    return irq_boot();
}

void irq_boot_set_record_capacity_for_test(size_t capacity)
{
    g_irq_boot_test_record_capacity = capacity;
}

size_t irq_boot_sample_storage_bytes_for_test(uint32_t cpu_count)
{
    if (!cpu_count || cpu_count > HOBBYOS_MAX_CPUS)
        return 0;
    return sizeof(irq_boot_sample_t) * cpu_count;
}
#endif

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

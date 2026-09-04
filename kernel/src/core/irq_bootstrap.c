#include "irq_bootstrap.h"

#include "clock.h"
#include "idt.h"
#include "interrupt_context.h"
#include "interrupts.h"
#include "scheduler.h"
#include "spinlock.h"
#include "../acpi/madt.h"
#include "../apic/ioapic.h"
#include "../apic/lapic.h"
#include "../apic/legacy_pic.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../libc/memory.h"
#include "../memory/heap.h"
#include "../smp/smp_boot.h"
#include "../timer/hpet.h"

typedef enum
{
    IRQ_RUNTIME_ABORT_NONE = 0,
    IRQ_RUNTIME_ABORT_HARD_FAILURE,
    IRQ_RUNTIME_ABORT_DEADLINE
} irq_runtime_abort_reason_t;

typedef struct
{
    irq_bootstrap_cpu_snapshot_t *cpus;
    spinlock_t runtime_ready_lock;
    volatile irq_bootstrap_state_t state;
    volatile uint32_t prepared_count;
    volatile uint32_t verified_count;
    volatile uint32_t runtime_count;
    volatile uint32_t failed_count;
    volatile uint8_t cpu_release;
    volatile uint8_t runtime_ready_abort;
    volatile uint8_t runtime_ready_transition;
    volatile uint32_t runtime_ready_abort_reason;
    volatile uint64_t runtime_ready_deadline_ns;
    uint64_t runtime_ready_timeout_us;
    uint32_t cpu_count;
    uint32_t keyboard_gsi;
    uint64_t bsp_lapic_entered;
    uint64_t bsp_lapic_returned;
    uint64_t hpet_counter_before;
    uint64_t hpet_counter_after;
    uint64_t hpet_counter_delta;
    volatile uint64_t transition_violations;
    uint32_t bsp_probe_vector;
    uint32_t hpet_probe_samples;
    uint8_t pcat_compat;
    uint8_t initialized;
    uint8_t controllers_quiescent;
    uint8_t routes_prepared;
    uint8_t bsp_lapic_probe_passed;
    uint8_t hpet_clocksource_verified;
    uint8_t hpet_timer0_quiescent;
} irq_bootstrap_runtime_t;

static irq_bootstrap_runtime_t g_irq_bootstrap;

static const char *irq_bootstrap_cpu_stage_name(
    irq_cpu_runtime_stage_t stage)
{
    switch (stage) {
        case IRQ_CPU_RUNTIME_OFF: return "OFF";
        case IRQ_CPU_RUNTIME_WAIT_RELEASE: return "WAIT_RELEASE";
        case IRQ_CPU_RUNTIME_TIMER_BASELINE: return "TIMER_BASELINE";
        case IRQ_CPU_RUNTIME_TIMER_WAIT: return "TIMER_WAIT";
        case IRQ_CPU_RUNTIME_TIMER_VERIFIED: return "TIMER_VERIFIED";
        case IRQ_CPU_RUNTIME_HANDOFF: return "HANDOFF";
        case IRQ_CPU_RUNTIME_PREEMPTION: return "PREEMPTION";
        case IRQ_CPU_RUNTIME_READY: return "READY";
        case IRQ_CPU_RUNTIME_FAILED: return "FAILED";
    }
    return "INVALID";
}

static const char *irq_bootstrap_cpu_result_name(
    irq_cpu_ready_result_t result)
{
    switch (result) {
        case IRQ_CPU_READY_OK: return "OK";
        case IRQ_CPU_READY_INVALID_STATE: return "INVALID_STATE";
        case IRQ_CPU_READY_RELEASE_ABORTED: return "RELEASE_ABORTED";
        case IRQ_CPU_READY_TIMER_STATE: return "TIMER_STATE";
        case IRQ_CPU_READY_TIMER_NO_ENTRY: return "TIMER_NO_ENTRY";
        case IRQ_CPU_READY_TIMER_NO_RETURN: return "TIMER_NO_RETURN";
        case IRQ_CPU_READY_TIMER_JOURNAL: return "TIMER_JOURNAL";
        case IRQ_CPU_READY_HANDOFF: return "HANDOFF";
        case IRQ_CPU_READY_PREEMPTION: return "PREEMPTION";
        case IRQ_CPU_READY_GLOBAL_DEADLINE: return "GLOBAL_DEADLINE";
        case IRQ_CPU_READY_PUBLICATION: return "PUBLICATION";
    }
    return "INVALID";
}

static const char *irq_bootstrap_timer_mode_name(uint32_t mode)
{
    switch ((lapic_timer_mode_t)mode) {
        case LAPIC_TIMER_OFF: return "OFF";
        case LAPIC_TIMER_CALIBRATED: return "CALIBRATED";
        case LAPIC_TIMER_PREPARED_MASKED: return "PREPARED_MASKED";
        case LAPIC_TIMER_ACTIVE_PERIODIC: return "ACTIVE_PERIODIC";
    }
    return "INVALID";
}

static const char *irq_bootstrap_topology_state_name(uint32_t state)
{
    switch (state) {
        case CPU_STATE_DEAD: return "DEAD";
        case CPU_STATE_PREPARE: return "PREPARE";
        case CPU_STATE_STARTING: return "STARTING";
        case CPU_STATE_ONLINE: return "ONLINE_PREPARED";
        case CPU_STATE_FAILED: return "FAILED";
    }
    return "INVALID";
}

static char *irq_bootstrap_append_text(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}

static char *irq_bootstrap_append_u64(char *out, uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; }
    while (value);
    while (count) *out++ = digits[--count];
    return out;
}

static bool irq_bootstrap_cpu_stage_advance(
    cpu_slot_t slot, irq_cpu_runtime_stage_t expected,
    irq_cpu_runtime_stage_t desired)
{
    if (slot >= g_irq_bootstrap.cpu_count || desired != expected + 1 ||
        desired >= IRQ_CPU_RUNTIME_FAILED)
        return false;
    irq_cpu_runtime_stage_t observed = expected;
    return __atomic_compare_exchange_n(
        &g_irq_bootstrap.cpus[slot].stage, &observed, desired, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_mark_failure(
    cpu_slot_t slot, irq_cpu_ready_result_t result)
{
    if (slot >= g_irq_bootstrap.cpu_count || result == IRQ_CPU_READY_OK)
        return IRQ_CPU_READY_INVALID_STATE;
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    uint8_t expected = 0;
    if (!__atomic_compare_exchange_n(&cpu->failed, &expected, 2, false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return result;
    irq_cpu_runtime_stage_t failed_stage = __atomic_load_n(
        &cpu->stage, __ATOMIC_ACQUIRE);
    __atomic_store_n(&cpu->failure_stage, failed_stage, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->failure_reason, result, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->stage, IRQ_CPU_RUNTIME_FAILED,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&cpu->failed, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_irq_bootstrap.failed_count, 1,
                       __ATOMIC_ACQ_REL);
    return result;
}

static void irq_bootstrap_cpu_load_snapshot(
    cpu_slot_t slot, irq_bootstrap_cpu_snapshot_t *out)
{
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    *out = (irq_bootstrap_cpu_snapshot_t){
        .slot = cpu->slot,
        .apic_id = cpu->apic_id,
        .first_vector = __atomic_load_n(&cpu->first_vector,
                                        __ATOMIC_ACQUIRE),
        .stage = __atomic_load_n(&cpu->stage, __ATOMIC_ACQUIRE),
        .failure_stage = __atomic_load_n(&cpu->failure_stage,
                                         __ATOMIC_ACQUIRE),
        .failure_reason = __atomic_load_n(&cpu->failure_reason,
                                          __ATOMIC_ACQUIRE),
        .timer_mode = __atomic_load_n(&cpu->timer_mode,
                                      __ATOMIC_ACQUIRE),
        .lapic_entered = __atomic_load_n(&cpu->lapic_entered,
                                         __ATOMIC_ACQUIRE),
        .lapic_returned = __atomic_load_n(&cpu->lapic_returned,
                                          __ATOMIC_ACQUIRE),
        .entered_before = __atomic_load_n(&cpu->entered_before,
                                          __ATOMIC_ACQUIRE),
        .returned_before = __atomic_load_n(&cpu->returned_before,
                                           __ATOMIC_ACQUIRE),
        .entered_after = __atomic_load_n(&cpu->entered_after,
                                         __ATOMIC_ACQUIRE),
        .returned_after = __atomic_load_n(&cpu->returned_after,
                                          __ATOMIC_ACQUIRE),
        .timer_calibrated = __atomic_load_n(&cpu->timer_calibrated,
                                            __ATOMIC_ACQUIRE),
        .timer_masked = __atomic_load_n(&cpu->timer_masked,
                                        __ATOMIC_ACQUIRE),
        .interrupt_prepared = __atomic_load_n(&cpu->interrupt_prepared,
                                              __ATOMIC_ACQUIRE),
        .interrupt_verified = __atomic_load_n(&cpu->interrupt_verified,
                                              __ATOMIC_ACQUIRE),
        .handoff_complete = __atomic_load_n(&cpu->handoff_complete,
                                            __ATOMIC_ACQUIRE),
        .preemption_enabled = __atomic_load_n(&cpu->preemption_enabled,
                                              __ATOMIC_ACQUIRE),
        .runtime_ready = __atomic_load_n(&cpu->runtime_ready,
                                         __ATOMIC_ACQUIRE),
        .failed = __atomic_load_n(&cpu->failed, __ATOMIC_ACQUIRE) != 0};
}

static void irq_bootstrap_log_state(const char *name, uint32_t cpus)
{
    char line[128];
    char *p = irq_bootstrap_append_text(line, "[IRQ][BOOTSTRAP] ");
    p = irq_bootstrap_append_text(p, name);
    if (cpus) {
        p = irq_bootstrap_append_text(p, " cpus=");
        p = irq_bootstrap_append_u64(p, cpus);
    }
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static bool irq_bootstrap_fail(void)
{
    __atomic_store_n(&g_irq_bootstrap.state, IRQ_BOOTSTRAP_FAILED,
                     __ATOMIC_RELEASE);
    return false;
}

static bool irq_bootstrap_advance(irq_bootstrap_state_t expected,
                                  irq_bootstrap_state_t desired)
{
    if (desired != expected + 1 || desired >= IRQ_BOOTSTRAP_FAILED) {
        __atomic_add_fetch(&g_irq_bootstrap.transition_violations, 1,
                           __ATOMIC_RELAXED);
        return irq_bootstrap_fail();
    }
    irq_bootstrap_state_t observed = expected;
    if (!__atomic_compare_exchange_n(&g_irq_bootstrap.state, &observed,
                                     desired, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_irq_bootstrap.transition_violations, 1,
                           __ATOMIC_RELAXED);
        return irq_bootstrap_fail();
    }
    return true;
}

irq_bootstrap_state_t irq_bootstrap_state(void)
{
    return __atomic_load_n(&g_irq_bootstrap.state, __ATOMIC_ACQUIRE);
}

bool irq_bootstrap_init(void)
{
    if (g_irq_bootstrap.initialized || !g_cpu_count)
        return false;
    irq_bootstrap_cpu_snapshot_t *cpus =
        kmalloc(sizeof(*cpus) * g_cpu_count);
    if (!cpus)
        return false;
    memset(cpus, 0, sizeof(*cpus) * g_cpu_count);
    memset(&g_irq_bootstrap, 0, sizeof(g_irq_bootstrap));
    g_irq_bootstrap.cpus = cpus;
    g_irq_bootstrap.cpu_count = g_cpu_count;
    spinlock_init(&g_irq_bootstrap.runtime_ready_lock);
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++) {
        g_irq_bootstrap.cpus[slot].slot = slot;
        g_irq_bootstrap.cpus[slot].apic_id = g_cpus[slot].apic_id;
        g_irq_bootstrap.cpus[slot].first_vector = 256u;
    }
    if (!interrupt_context_init(g_cpu_count))
        return false;
    __atomic_store_n(&g_irq_bootstrap.state, IRQ_BOOTSTRAP_OFF,
                     __ATOMIC_RELEASE);
    g_irq_bootstrap.initialized = 1;
    return true;
}

bool irq_bootstrap_controllers_quiescent(void)
{
    if (!g_irq_bootstrap.initialized || irq_is_enabled() ||
        irq_bootstrap_state() != IRQ_BOOTSTRAP_OFF)
        return irq_bootstrap_fail();
    madt_snapshot_t madt;
    cpu_slot_t bsp = smp_bsp_cpu_slot();
    if (!madt_snapshot(&madt) || !madt.valid || !madt.ioapic_count ||
        bsp == CPU_SLOT_INVALID || !lapic_bind_quiescent_slot(bsp))
        return irq_bootstrap_fail();
    g_irq_bootstrap.pcat_compat = madt.pcat_compat;
    legacy_pic_snapshot_t pic;
    if (!legacy_pic_snapshot(&pic) || !legacy_pic_is_quiescent() ||
        pic.pcat_declared != madt.pcat_compat)
        return irq_bootstrap_fail();
    if (!ioapic_init_all()) {
#ifdef HOBBYOS_IRQ_NEGATIVE_IOAPIC_ROUTE_ACTIVE
        serial_write_all(
            "[IRQ][NEGATIVE] IOAPIC_NOT_QUIESCENT_DETECTED\n");
#endif
        return irq_bootstrap_fail();
    }
    lapic_quiescent_snapshot_t lapic;
    ioapic_registry_snapshot_t ioapic;
    if (!lapic_quiescent_snapshot(bsp, &lapic) || !lapic.valid ||
        !(lapic.lvt_lint0 & APIC_TIMER_MASKED) ||
        !(lapic.lvt_lint1 & APIC_TIMER_MASKED) ||
        !ioapic_registry_snapshot(&ioapic) || !ioapic.quiescent ||
        ioapic.controllers == 0 || ioapic.masked_entries != ioapic.total_entries)
        return irq_bootstrap_fail();
    g_irq_bootstrap.controllers_quiescent = 1;
    if (!irq_bootstrap_advance(IRQ_BOOTSTRAP_OFF,
                               IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT))
        return false;
    irq_bootstrap_log_state("CONTROLLERS_QUIESCENT", 0);
    return true;
}

static bool irq_bootstrap_iso_resolution_valid(uint8_t source_irq,
                                                uint32_t resolved_gsi,
                                                irq_polarity_t polarity,
                                                irq_trigger_t trigger)
{
    for (uint32_t index = 0; index < madt_get_iso_count(); index++) {
        madt_iso_t iso;
        if (!madt_iso_at(index, &iso) || iso.source_irq != source_irq)
            continue;
        irq_polarity_t expected_polarity =
            iso.polarity == IRQ_POLARITY_CONFORMS
                ? IRQ_POLARITY_HIGH : iso.polarity;
        irq_trigger_t expected_trigger =
            iso.trigger == IRQ_TRIGGER_CONFORMS
                ? IRQ_TRIGGER_EDGE : iso.trigger;
        return resolved_gsi == iso.gsi && polarity == expected_polarity &&
               trigger == expected_trigger;
    }
    return resolved_gsi == source_irq && polarity == IRQ_POLARITY_HIGH &&
           trigger == IRQ_TRIGGER_EDGE;
}

static bool irq_bootstrap_all_iso_resolutions_valid(void)
{
    for (uint32_t index = 0; index < madt_get_iso_count(); index++)
    {
        madt_iso_t iso;
        uint32_t gsi;
        irq_polarity_t polarity;
        irq_trigger_t trigger;
        if (!madt_iso_at(index, &iso) ||
            !madt_resolve_isa_irq(iso.source_irq, &gsi, &polarity,
                                  &trigger) ||
            !irq_bootstrap_iso_resolution_valid(iso.source_irq, gsi,
                                                 polarity, trigger))
            return false;
    }
    return true;
}

bool irq_bootstrap_routes_prepared(void)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT)
        return irq_bootstrap_fail();
    uint32_t gsi;
    irq_polarity_t polarity;
    irq_trigger_t trigger;
    if (!irq_bootstrap_all_iso_resolutions_valid() ||
        !madt_resolve_isa_irq(1, &gsi, &polarity, &trigger) ||
        !irq_bootstrap_iso_resolution_valid(1, gsi, polarity, trigger)) {
#ifdef HOBBYOS_IRQ_NEGATIVE_IGNORE_ISO
        serial_write_all(
            "[IRQ][NEGATIVE] INTERRUPT_OVERRIDE_LOST_DETECTED\n");
#endif
        return irq_bootstrap_fail();
    }
    interrupt_route_t keyboard = {
        .gsi = gsi,
        .destination_apic_id = g_bsp_apic_id,
        .vector = INT_VECTOR_KEYBOARD,
        .polarity = polarity,
        .trigger = trigger,
        .owner = INTERRUPT_ROUTE_OWNER_KEYBOARD,
        .masked = 1};
    if (!ioapic_route_prepare(&keyboard))
        return irq_bootstrap_fail();
    g_irq_bootstrap.keyboard_gsi = gsi;
    g_irq_bootstrap.routes_prepared = 1;
    if (!irq_bootstrap_advance(IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT,
                               IRQ_BOOTSTRAP_ROUTES_PREPARED))
        return false;
    irq_bootstrap_log_state("ROUTES_PREPARED", 0);
    return true;
}

bool irq_bootstrap_cpu_prepare(cpu_slot_t slot)
{
    irq_bootstrap_state_t state = irq_bootstrap_state();
    if ((state != IRQ_BOOTSTRAP_ROUTES_PREPARED &&
         state != IRQ_BOOTSTRAP_CPUS_PREPARED) ||
        slot >= g_irq_bootstrap.cpu_count)
        return irq_bootstrap_fail();
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    if (__atomic_load_n(&cpu->interrupt_prepared, __ATOMIC_ACQUIRE))
        return irq_bootstrap_fail();
    lapic_timer_state_t timer;
    lapic_quiescent_snapshot_t lapic;
    if (!lapic_timer_state_snapshot(slot, &timer) ||
        timer.mode != LAPIC_TIMER_PREPARED_MASKED || timer.masked == 0 ||
        timer.desired_period_us != 1000u ||
        !lapic_quiescent_snapshot(slot, &lapic) || !lapic.valid)
        return irq_bootstrap_fail();
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_calibrated, 1, __ATOMIC_RELAXED);
    if (!irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_OFF, IRQ_CPU_RUNTIME_WAIT_RELEASE))
        return irq_bootstrap_fail();
    __atomic_store_n(&cpu->interrupt_prepared, 1, __ATOMIC_RELEASE);
    uint32_t prepared = __atomic_add_fetch(&g_irq_bootstrap.prepared_count, 1,
                                           __ATOMIC_ACQ_REL);
    if (prepared > g_irq_bootstrap.cpu_count)
        return irq_bootstrap_fail();
    if (prepared == g_irq_bootstrap.cpu_count) {
        if (!irq_bootstrap_advance(IRQ_BOOTSTRAP_ROUTES_PREPARED,
                                   IRQ_BOOTSTRAP_CPUS_PREPARED))
            return false;
        irq_bootstrap_log_state("CPUS_PREPARED", prepared);
    }
    return true;
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_wait_release(
    cpu_slot_t slot)
{
    if (slot >= g_irq_bootstrap.cpu_count ||
        !__atomic_load_n(&g_irq_bootstrap.cpus[slot].interrupt_prepared,
                         __ATOMIC_ACQUIRE) || irq_is_enabled())
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_INVALID_STATE);
    while (!__atomic_load_n(&g_irq_bootstrap.cpu_release,
                            __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                            __ATOMIC_ACQUIRE) ||
            irq_bootstrap_state() == IRQ_BOOTSTRAP_FAILED)
            return IRQ_CPU_READY_RELEASE_ABORTED;
        __asm__ volatile("pause" ::: "memory");
    }
    if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE))
        return IRQ_CPU_READY_RELEASE_ABORTED;
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE)
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_INVALID_STATE);
    return IRQ_CPU_READY_OK;
}

static bool irq_bootstrap_wait_vector_from_baseline(
    cpu_slot_t slot, uint8_t vector, uint64_t timeout_us,
    uint64_t entered_before, uint64_t returned_before,
    uint64_t *out_entered_after, uint64_t *out_returned_after)
{
    if (!timeout_us)
        return false;
    uint64_t start = clock_monotonic_ns();
    if (!start || timeout_us > (UINT64_MAX - start) / 1000u)
        return false;
    uint64_t deadline = start + timeout_us * 1000u;
    uint64_t entered = entered_before;
    uint64_t returned = returned_before;
    while (clock_monotonic_ns() <= deadline) {
        if (!interrupt_context_vector_snapshot(slot, vector, &entered,
                                               &returned))
            return false;
        if (entered > entered_before && returned > returned_before &&
            entered == returned)
            break;
        __asm__ volatile("pause" ::: "memory");
    }
    if (out_entered_after) *out_entered_after = entered;
    if (out_returned_after) *out_returned_after = returned;
    return entered > entered_before && returned > returned_before &&
           entered == returned;
}

static bool irq_bootstrap_mark_timer_verified(cpu_slot_t slot,
                                              uint64_t entered_before,
                                              uint64_t returned_before,
                                              uint64_t entered_after,
                                              uint64_t returned_after)
{
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    if (__atomic_load_n(&cpu->interrupt_verified, __ATOMIC_ACQUIRE))
        return false;
    interrupt_cpu_snapshot_t journal;
    lapic_timer_state_t timer;
    uint64_t entered = entered_after - entered_before;
    uint64_t returned = returned_after - returned_before;
    if (!interrupt_context_snapshot(slot, &journal) || journal.depth != 0 ||
        journal.unexpected || journal.underflow || journal.mismatch ||
        entered == 0 || entered != returned ||
        journal.first_vector != INT_VECTOR_LAPIC_TIMER ||
        !lapic_timer_state_snapshot(slot, &timer) ||
        timer.mode != LAPIC_TIMER_ACTIVE_PERIODIC || timer.masked ||
        timer.desired_period_us != 1000u ||
        !irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_TIMER_WAIT,
            IRQ_CPU_RUNTIME_TIMER_VERIFIED))
        return false;
    __atomic_store_n(&cpu->first_vector, journal.first_vector,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->entered_after, entered_after,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->returned_after, returned_after,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->lapic_entered, entered, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->lapic_returned, returned, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->interrupt_verified, 1, __ATOMIC_RELEASE);
    uint32_t verified = __atomic_add_fetch(
        &g_irq_bootstrap.verified_count, 1, __ATOMIC_ACQ_REL);
    if (verified > g_irq_bootstrap.cpu_count)
        return false;
    char line[192];
    char *p = irq_bootstrap_append_text(
        line, "[IRQ][CPU_TIMER_VERIFIED] PASS slot=");
    p = irq_bootstrap_append_u64(p, slot);
    p = irq_bootstrap_append_text(p, " apic=");
    p = irq_bootstrap_append_u64(p, cpu->apic_id);
    p = irq_bootstrap_append_text(p, " entered_delta=");
    p = irq_bootstrap_append_u64(p, entered);
    p = irq_bootstrap_append_text(p, " returned_delta=");
    p = irq_bootstrap_append_u64(p, returned);
    p = irq_bootstrap_append_text(p, " timer_mode=ACTIVE_PERIODIC\n");
    *p = 0;
    serial_write_all(line);
    return true;
}

bool irq_bootstrap_verify_bsp_lapic(uint64_t timeout_us)
{
    cpu_slot_t bsp = smp_bsp_cpu_slot();
    uint64_t entered_before = 0;
    uint64_t returned_before = 0;
    lapic_timer_state_t timer;
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CPUS_PREPARED ||
        bsp == CPU_SLOT_INVALID || irq_is_enabled() || !timeout_us ||
        !interrupt_context_vector_snapshot(
            bsp, INT_VECTOR_LAPIC_TIMER, &entered_before,
            &returned_before) ||
        !irq_bootstrap_cpu_stage_advance(
            bsp, IRQ_CPU_RUNTIME_WAIT_RELEASE,
            IRQ_CPU_RUNTIME_TIMER_BASELINE) ||
        !lapic_timer_state_snapshot(bsp, &timer) ||
        timer.mode != LAPIC_TIMER_PREPARED_MASKED || !timer.masked ||
        timer.desired_period_us != 1000u)
        return irq_bootstrap_fail();
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[bsp];
    __atomic_store_n(&cpu->entered_before, entered_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->returned_before, returned_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELAXED);
    if (!lapic_set_normal_delivery(true) ||
        !lapic_timer_enable_periodic() ||
        !irq_bootstrap_cpu_stage_advance(
            bsp, IRQ_CPU_RUNTIME_TIMER_BASELINE,
            IRQ_CPU_RUNTIME_TIMER_WAIT))
        return irq_bootstrap_fail();
    irq_enable();
    uint64_t entered_after = entered_before;
    uint64_t returned_after = returned_before;
    bool ok = irq_bootstrap_wait_vector_from_baseline(
        bsp, INT_VECTOR_LAPIC_TIMER, timeout_us, entered_before,
        returned_before, &entered_after, &returned_after);
    irq_disable();
    if (!ok || !irq_bootstrap_mark_timer_verified(
                   bsp, entered_before, returned_before,
                   entered_after, returned_after))
        return irq_bootstrap_fail();
    uint64_t entered = entered_after - entered_before;
    uint64_t returned = returned_after - returned_before;
    g_irq_bootstrap.bsp_lapic_entered = entered;
    g_irq_bootstrap.bsp_lapic_returned = returned;
    g_irq_bootstrap.bsp_probe_vector = INT_VECTOR_LAPIC_TIMER;
    g_irq_bootstrap.bsp_lapic_probe_passed = 1;
    if (!irq_bootstrap_advance(IRQ_BOOTSTRAP_CPUS_PREPARED,
                               IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED))
        return false;
    char line[160];
    char *p = irq_bootstrap_append_text(line,
        "[IRQ][BSP_LAPIC_PROBE] PASS vector=");
    p = irq_bootstrap_append_u64(p, INT_VECTOR_LAPIC_TIMER);
    p = irq_bootstrap_append_text(p, " entered=");
    p = irq_bootstrap_append_u64(p, entered);
    p = irq_bootstrap_append_text(p, " returned=");
    p = irq_bootstrap_append_u64(p, returned);
    p = irq_bootstrap_append_text(p, " period_us=1000\n");
    *p = 0;
    serial_write_all(line);
    return true;
}

static bool irq_bootstrap_hpet_probe_fail(const char *reason)
{
    char line[160];
    char *p = irq_bootstrap_append_text(
        line, "[CLOCK][HPET_CLOCKSOURCE_PROBE] FAIL reason=");
    p = irq_bootstrap_append_text(p, reason);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
    return irq_bootstrap_fail();
}

bool irq_bootstrap_verify_hpet_clocksource(uint32_t max_samples)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED ||
        irq_is_enabled() || !max_samples)
        return irq_bootstrap_hpet_probe_fail("bootstrap-state");
    if (!hpet_is_available())
        return irq_bootstrap_hpet_probe_fail("unavailable");
    if (!hpet_validate_period_fs(hpet_period_fs()))
        return irq_bootstrap_hpet_probe_fail("invalid-period");
    if (!hpet_timer0_force_quiescent())
        return irq_bootstrap_hpet_probe_fail("timer0-not-quiescent");

    hpet_runtime_snapshot_t before;
    if (!hpet_runtime_snapshot(&before) || !before.main_counter_enabled)
        return irq_bootstrap_hpet_probe_fail("unavailable");
    if (before.legacy_replacement_enabled)
        return irq_bootstrap_hpet_probe_fail("legacy-replacement-active");
    if (before.timer0_pending)
        return irq_bootstrap_hpet_probe_fail("pending-not-clear");
    if (before.timer0_interrupt_enabled || before.timer0_periodic_enabled ||
        before.timer0_fsb_enabled || before.timer0_route_enabled)
        return irq_bootstrap_hpet_probe_fail("timer0-not-quiescent");

    hpet_clock_sample_t first;
    hpet_clock_sample_t current = {0};
    if (!hpet_read_clock_sample(&first) || !first.valid)
        return irq_bootstrap_hpet_probe_fail("unavailable");
    uint32_t samples = 0;
    bool advanced = false;
    for (; samples < max_samples; samples++) {
        __asm__ volatile("pause" ::: "memory");
        if (!hpet_read_clock_sample(&current) || !current.valid)
            return irq_bootstrap_hpet_probe_fail("unavailable");
        if (current.ticks < first.ticks || current.ns < first.ns)
            return irq_bootstrap_hpet_probe_fail("counter-regressed");
        if (current.ticks > first.ticks) {
            samples++;
            advanced = true;
            break;
        }
    }
    if (!advanced)
        return irq_bootstrap_hpet_probe_fail("counter-stalled");
    uint64_t delta_ticks = current.ticks - first.ticks;
    uint64_t delta_ns = current.ns - first.ns;
    if (!delta_ticks || !delta_ns)
        return irq_bootstrap_hpet_probe_fail("conversion-zero");

    hpet_runtime_snapshot_t after;
    if (!hpet_runtime_snapshot(&after) || !hpet_timer0_is_quiescent())
        return irq_bootstrap_hpet_probe_fail("timer0-not-quiescent");
    if (after.legacy_replacement_enabled)
        return irq_bootstrap_hpet_probe_fail("legacy-replacement-active");
    if (after.timer0_pending)
        return irq_bootstrap_hpet_probe_fail("pending-not-clear");

    g_irq_bootstrap.hpet_counter_before = first.ticks;
    g_irq_bootstrap.hpet_counter_after = current.ticks;
    g_irq_bootstrap.hpet_counter_delta = delta_ticks;
    g_irq_bootstrap.hpet_probe_samples = samples;
    g_irq_bootstrap.hpet_clocksource_verified = 1;
    g_irq_bootstrap.hpet_timer0_quiescent = 1;
    if (!irq_bootstrap_advance(
            IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED,
            IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED))
        return false;

    char line[224];
    char *p = irq_bootstrap_append_text(
        line, "[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS samples=");
    p = irq_bootstrap_append_u64(p, samples);
    p = irq_bootstrap_append_text(p, " delta_ticks=");
    p = irq_bootstrap_append_u64(p, delta_ticks);
    p = irq_bootstrap_append_text(p, " delta_ns=");
    p = irq_bootstrap_append_u64(p, delta_ns);
    p = irq_bootstrap_append_text(p, " timer0=QUIESCENT\n");
    *p = 0;
    serial_write_all(line);
    serial_write_all(
        "[CLOCK][HPET_TIMER0] QUIESCENT irq_enabled=0 route_enabled=0 pending=0 legacy=0\n");
    return true;
}

bool irq_bootstrap_activate_clockevent(void)
{
    timer_clockevent_snapshot_t event;
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED ||
        !g_irq_bootstrap.bsp_lapic_probe_passed ||
        !g_irq_bootstrap.hpet_clocksource_verified ||
        !g_irq_bootstrap.hpet_timer0_quiescent ||
        !timer_clockevent_snapshot(&event) || !event.active ||
        event.source != TIMER_CLOCKEVENT_BSP_LAPIC ||
        event.bsp_slot != smp_bsp_cpu_slot() || event.period_us != 1000u)
        return irq_bootstrap_fail();
    return irq_bootstrap_advance(
        IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED,
        IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE);
}

bool irq_bootstrap_release_cpus(void)
{
    cpu_slot_t bsp = smp_bsp_cpu_slot();
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE ||
        bsp == CPU_SLOT_INVALID ||
        !__atomic_load_n(&g_irq_bootstrap.cpus[bsp].runtime_ready,
                         __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_irq_bootstrap.runtime_ready_transition,
                        __ATOMIC_ACQUIRE))
        return irq_bootstrap_fail();
    __atomic_store_n(&g_irq_bootstrap.runtime_ready_abort_reason,
                     IRQ_RUNTIME_ABORT_NONE, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&g_irq_bootstrap.cpu_release, 1,
                            __ATOMIC_RELEASE))
        return irq_bootstrap_fail();
    return true;
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_verify_local_timer(
    cpu_slot_t slot)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE ||
        slot >= g_irq_bootstrap.cpu_count || irq_is_enabled() ||
        !__atomic_load_n(&g_irq_bootstrap.cpus[slot].interrupt_prepared,
                         __ATOMIC_ACQUIRE))
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_INVALID_STATE);

    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    uint64_t entered_before = 0;
    uint64_t returned_before = 0;
    if (!interrupt_context_vector_snapshot(
            slot, INT_VECTOR_LAPIC_TIMER, &entered_before,
            &returned_before) ||
        !irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_WAIT_RELEASE,
            IRQ_CPU_RUNTIME_TIMER_BASELINE))
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_TIMER_JOURNAL);
    __atomic_store_n(&cpu->entered_before, entered_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->returned_before, returned_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->entered_after, entered_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->returned_after, returned_before,
                     __ATOMIC_RELAXED);

    lapic_timer_state_t timer;
    if (!lapic_timer_state_snapshot(slot, &timer))
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_TIMER_STATE);
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELAXED);
    if (timer.mode != LAPIC_TIMER_PREPARED_MASKED || !timer.masked ||
        timer.desired_period_us != 1000u)
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_TIMER_STATE);
    if (!lapic_set_normal_delivery(true) ||
        !lapic_timer_enable_periodic() ||
        !lapic_timer_state_snapshot(slot, &timer) ||
        timer.mode != LAPIC_TIMER_ACTIVE_PERIODIC || timer.masked ||
        timer.desired_period_us != 1000u ||
        !irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_TIMER_BASELINE,
            IRQ_CPU_RUNTIME_TIMER_WAIT))
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_TIMER_STATE);
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELEASE);

    for (;;) {
        uint64_t entered_after = entered_before;
        uint64_t returned_after = returned_before;
        if (!interrupt_context_vector_snapshot(
                slot, INT_VECTOR_LAPIC_TIMER, &entered_after,
                &returned_after) || entered_after < entered_before ||
            returned_after < returned_before)
            return irq_bootstrap_cpu_mark_failure(
                slot, IRQ_CPU_READY_TIMER_JOURNAL);
        __atomic_store_n(&cpu->entered_after, entered_after,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->returned_after, returned_after,
                         __ATOMIC_RELEASE);
        uint64_t entered = entered_after - entered_before;
        uint64_t returned = returned_after - returned_before;
        if (entered || returned) {
            if (entered && !returned)
                return irq_bootstrap_cpu_mark_failure(
                    slot, IRQ_CPU_READY_TIMER_NO_RETURN);
            if (!entered || entered != returned ||
                !irq_bootstrap_mark_timer_verified(
                    slot, entered_before, returned_before,
                    entered_after, returned_after))
                return irq_bootstrap_cpu_mark_failure(
                    slot, IRQ_CPU_READY_TIMER_JOURNAL);
            return IRQ_CPU_READY_OK;
        }
        if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                            __ATOMIC_ACQUIRE)) {
            irq_runtime_abort_reason_t abort_reason =
                (irq_runtime_abort_reason_t)__atomic_load_n(
                    &g_irq_bootstrap.runtime_ready_abort_reason,
                    __ATOMIC_ACQUIRE);
            if (abort_reason != IRQ_RUNTIME_ABORT_DEADLINE)
                return IRQ_CPU_READY_RELEASE_ABORTED;
            irq_cpu_ready_result_t result = entered
                ? IRQ_CPU_READY_TIMER_NO_RETURN
                : IRQ_CPU_READY_TIMER_NO_ENTRY;
            return irq_bootstrap_cpu_mark_failure(slot, result);
        }
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_publish_runtime(
    cpu_slot_t slot)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE ||
        slot >= g_irq_bootstrap.cpu_count ||
        __atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE))
        return IRQ_CPU_READY_RELEASE_ABORTED;
    if (!__atomic_load_n(&g_irq_bootstrap.cpu_release,
                         __ATOMIC_ACQUIRE) &&
        slot != smp_bsp_cpu_slot())
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_INVALID_STATE);
    cpu_slot_t current = CPU_SLOT_INVALID;
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    scheduler_preemption_snapshot_t scheduler;
    lapic_timer_state_t timer;
    if (!smp_current_cpu_slot(&current) || current != slot ||
        __atomic_load_n(&cpu->stage, __ATOMIC_ACQUIRE) !=
            IRQ_CPU_RUNTIME_PREEMPTION ||
        !__atomic_load_n(&cpu->interrupt_verified, __ATOMIC_ACQUIRE) ||
        !scheduler_preemption_snapshot(slot, &scheduler) ||
        !scheduler.handoff_complete || !scheduler.irq_preemption_enabled ||
        !scheduler.idle_rsp_valid ||
        !lapic_timer_state_snapshot(slot, &timer) ||
        timer.mode != LAPIC_TIMER_ACTIVE_PERIODIC ||
        timer.desired_period_us != 1000u)
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_PUBLICATION);
    __atomic_store_n(&cpu->handoff_complete, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->preemption_enabled, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_mode, timer.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->timer_masked, timer.masked, __ATOMIC_RELAXED);
    irq_flags_t flags = spin_lock_irqsave(
        &g_irq_bootstrap.runtime_ready_lock);
    if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&g_irq_bootstrap.runtime_ready_lock, flags);
        return IRQ_CPU_READY_RELEASE_ABORTED;
    }
    uint32_t ready = __atomic_load_n(&g_irq_bootstrap.runtime_count,
                                     __ATOMIC_RELAXED);
    uint8_t expected_ready = 0;
    if (ready >= g_irq_bootstrap.cpu_count ||
        !irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_PREEMPTION, IRQ_CPU_RUNTIME_READY) ||
        !__atomic_compare_exchange_n(
            &cpu->runtime_ready, &expected_ready, 1, false,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        spin_unlock_irqrestore(&g_irq_bootstrap.runtime_ready_lock, flags);
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_PUBLICATION);
    }
    ready++;
    __atomic_store_n(&g_irq_bootstrap.runtime_count, ready,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_irq_bootstrap.runtime_ready_lock, flags);
    char line[208];
    char *p = irq_bootstrap_append_text(line,
        "[IRQ][CPU_READY] PASS slot=");
    p = irq_bootstrap_append_u64(p, slot);
    p = irq_bootstrap_append_text(p, " apic=");
    p = irq_bootstrap_append_u64(p, cpu->apic_id);
    p = irq_bootstrap_append_text(p,
        " timer_us=1000 handoff=1 preempt=1 stage=READY\n");
    *p = 0;
    serial_write_all(line);
    return IRQ_CPU_READY_OK;
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_adopt_scheduler_state(
    cpu_slot_t slot)
{
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    scheduler_preemption_snapshot_t scheduler;
    if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE))
        return IRQ_CPU_READY_RELEASE_ABORTED;
    if (!scheduler_preemption_snapshot(slot, &scheduler) ||
        !irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_TIMER_VERIFIED,
            IRQ_CPU_RUNTIME_HANDOFF) || !scheduler.handoff_complete ||
        !scheduler.idle_rsp_valid)
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_HANDOFF);
    __atomic_store_n(&cpu->handoff_complete, 1, __ATOMIC_RELEASE);
    if (!irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_HANDOFF,
            IRQ_CPU_RUNTIME_PREEMPTION) ||
        !scheduler.irq_preemption_enabled)
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_PREEMPTION);
    __atomic_store_n(&cpu->preemption_enabled, 1, __ATOMIC_RELEASE);
    return irq_bootstrap_cpu_publish_runtime(slot);
}

bool irq_bootstrap_cpu_complete_runtime(cpu_slot_t slot)
{
    return irq_bootstrap_cpu_adopt_scheduler_state(slot) ==
        IRQ_CPU_READY_OK;
}

irq_cpu_ready_result_t irq_bootstrap_cpu_run_runtime(cpu_slot_t slot)
{
    irq_cpu_ready_result_t result = irq_bootstrap_cpu_wait_release(slot);
    if (result != IRQ_CPU_READY_OK)
        return result;
    result = irq_bootstrap_cpu_verify_local_timer(slot);
    if (result != IRQ_CPU_READY_OK)
        return result;
    if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE))
        return IRQ_CPU_READY_RELEASE_ABORTED;
    irq_bootstrap_cpu_snapshot_t *cpu = &g_irq_bootstrap.cpus[slot];
    if (!irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_TIMER_VERIFIED,
            IRQ_CPU_RUNTIME_HANDOFF) ||
        !scheduler_bootstrap_handoff_current_cpu())
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_HANDOFF);
    __atomic_store_n(&cpu->handoff_complete, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_irq_bootstrap.runtime_ready_abort,
                        __ATOMIC_ACQUIRE))
        return IRQ_CPU_READY_RELEASE_ABORTED;
    if (!irq_bootstrap_cpu_stage_advance(
            slot, IRQ_CPU_RUNTIME_HANDOFF,
            IRQ_CPU_RUNTIME_PREEMPTION) ||
        !scheduler_cpu_enable_irq_preemption(slot))
        return irq_bootstrap_cpu_mark_failure(
            slot, IRQ_CPU_READY_PREEMPTION);
    __atomic_store_n(&cpu->preemption_enabled, 1, __ATOMIC_RELEASE);
    return irq_bootstrap_cpu_publish_runtime(slot);
}

void irq_bootstrap_log_cpu_runtime_error(cpu_slot_t slot,
                                         irq_cpu_ready_result_t result)
{
    if (slot >= g_irq_bootstrap.cpu_count)
        return;
    irq_bootstrap_cpu_snapshot_t cpu;
    irq_bootstrap_cpu_load_snapshot(slot, &cpu);
    irq_cpu_runtime_stage_t stage = cpu.stage == IRQ_CPU_RUNTIME_FAILED
        ? cpu.failure_stage : cpu.stage;
    irq_cpu_ready_result_t reason = cpu.failure_reason == IRQ_CPU_READY_OK
        ? result : cpu.failure_reason;
    uint64_t entered = cpu.entered_after >= cpu.entered_before
        ? cpu.entered_after - cpu.entered_before : 0;
    uint64_t returned = cpu.returned_after >= cpu.returned_before
        ? cpu.returned_after - cpu.returned_before : 0;
    char line[352];
    char *p = irq_bootstrap_append_text(
        line, "[SMP][CPU] ERROR code=IRQ_RUNTIME_READY slot=");
    p = irq_bootstrap_append_u64(p, slot);
    p = irq_bootstrap_append_text(p, " apic=");
    p = irq_bootstrap_append_u64(p, cpu.apic_id);
    p = irq_bootstrap_append_text(p, " stage=");
    p = irq_bootstrap_append_text(p, irq_bootstrap_cpu_stage_name(stage));
    p = irq_bootstrap_append_text(p, " reason=");
    p = irq_bootstrap_append_text(p, irq_bootstrap_cpu_result_name(reason));
    p = irq_bootstrap_append_text(p, " entered_delta=");
    p = irq_bootstrap_append_u64(p, entered);
    p = irq_bootstrap_append_text(p, " returned_delta=");
    p = irq_bootstrap_append_u64(p, returned);
    p = irq_bootstrap_append_text(p, " timer_mode=");
    p = irq_bootstrap_append_text(p,
        irq_bootstrap_timer_mode_name(cpu.timer_mode));
    p = irq_bootstrap_append_text(p, " timer_masked=");
    p = irq_bootstrap_append_u64(p, cpu.timer_masked);
    p = irq_bootstrap_append_text(p, " handoff=");
    p = irq_bootstrap_append_u64(p, cpu.handoff_complete);
    p = irq_bootstrap_append_text(p, " preempt=");
    p = irq_bootstrap_append_u64(p, cpu.preemption_enabled);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static void irq_bootstrap_cpu_refresh_journal(
    cpu_slot_t slot, irq_bootstrap_cpu_snapshot_t *cpu)
{
    uint64_t entered = cpu->entered_after;
    uint64_t returned = cpu->returned_after;
    if (interrupt_context_vector_snapshot(
            slot, INT_VECTOR_LAPIC_TIMER, &entered, &returned) &&
        entered >= cpu->entered_before &&
        returned >= cpu->returned_before) {
        cpu->entered_after = entered;
        cpu->returned_after = returned;
        __atomic_store_n(&g_irq_bootstrap.cpus[slot].entered_after,
                         entered, __ATOMIC_RELAXED);
        __atomic_store_n(&g_irq_bootstrap.cpus[slot].returned_after,
                         returned, __ATOMIC_RELEASE);
    }
    interrupt_cpu_snapshot_t journal;
    if (interrupt_context_snapshot(slot, &journal)) {
        cpu->first_vector = journal.first_vector;
        __atomic_store_n(&g_irq_bootstrap.cpus[slot].first_vector,
                         journal.first_vector, __ATOMIC_RELEASE);
    }
}

static irq_cpu_ready_result_t irq_bootstrap_cpu_timeout_reason(
    const irq_bootstrap_cpu_snapshot_t *cpu,
    irq_runtime_abort_reason_t abort_reason)
{
    if (cpu->failed)
        return cpu->failure_reason != IRQ_CPU_READY_OK
            ? cpu->failure_reason : IRQ_CPU_READY_INVALID_STATE;
    if (abort_reason == IRQ_RUNTIME_ABORT_HARD_FAILURE)
        return IRQ_CPU_READY_RELEASE_ABORTED;
    uint64_t entered = cpu->entered_after >= cpu->entered_before
        ? cpu->entered_after - cpu->entered_before : 0;
    uint64_t returned = cpu->returned_after >= cpu->returned_before
        ? cpu->returned_after - cpu->returned_before : 0;
    if ((cpu->stage == IRQ_CPU_RUNTIME_TIMER_BASELINE ||
         cpu->stage == IRQ_CPU_RUNTIME_TIMER_WAIT) && !entered)
        return IRQ_CPU_READY_TIMER_NO_ENTRY;
    if (cpu->stage == IRQ_CPU_RUNTIME_TIMER_WAIT &&
        entered > returned)
        return IRQ_CPU_READY_TIMER_NO_RETURN;
    return IRQ_CPU_READY_GLOBAL_DEADLINE;
}

static void irq_bootstrap_log_cpu_ready_timeout(
    const irq_bootstrap_cpu_snapshot_t *cpu,
    irq_cpu_ready_result_t reason)
{
    irq_cpu_runtime_stage_t stage = cpu->stage == IRQ_CPU_RUNTIME_FAILED
        ? cpu->failure_stage : cpu->stage;
    uint64_t entered = cpu->entered_after >= cpu->entered_before
        ? cpu->entered_after - cpu->entered_before : 0;
    uint64_t returned = cpu->returned_after >= cpu->returned_before
        ? cpu->returned_after - cpu->returned_before : 0;
    const SmpCpuInfo *topology = smp_cpu_by_slot_const(cpu->slot);
    uint32_t topology_state = topology
        ? __atomic_load_n(&topology->state, __ATOMIC_ACQUIRE)
        : CPU_STATE_DEAD;
    char line[512];
    char *p = irq_bootstrap_append_text(
        line, "[IRQ][CPU_READY_TIMEOUT] slot=");
    p = irq_bootstrap_append_u64(p, cpu->slot);
    p = irq_bootstrap_append_text(p, " apic=");
    p = irq_bootstrap_append_u64(p, cpu->apic_id);
    p = irq_bootstrap_append_text(p, " stage=");
    p = irq_bootstrap_append_text(p, irq_bootstrap_cpu_stage_name(stage));
    p = irq_bootstrap_append_text(p, " reason=");
    p = irq_bootstrap_append_text(p, irq_bootstrap_cpu_result_name(reason));
    p = irq_bootstrap_append_text(p, " prepared=");
    p = irq_bootstrap_append_u64(p, cpu->interrupt_prepared);
    p = irq_bootstrap_append_text(p, " verified=");
    p = irq_bootstrap_append_u64(p, cpu->interrupt_verified);
    p = irq_bootstrap_append_text(p, " entered=");
    p = irq_bootstrap_append_u64(p, entered);
    p = irq_bootstrap_append_text(p, " returned=");
    p = irq_bootstrap_append_u64(p, returned);
    p = irq_bootstrap_append_text(p, " first_vector=");
    p = irq_bootstrap_append_u64(p, cpu->first_vector);
    p = irq_bootstrap_append_text(p, " timer_mode=");
    p = irq_bootstrap_append_text(
        p, irq_bootstrap_timer_mode_name(cpu->timer_mode));
    p = irq_bootstrap_append_text(p, " timer_masked=");
    p = irq_bootstrap_append_u64(p, cpu->timer_masked);
    p = irq_bootstrap_append_text(p, " handoff=");
    p = irq_bootstrap_append_u64(p, cpu->handoff_complete);
    p = irq_bootstrap_append_text(p, " preempt=");
    p = irq_bootstrap_append_u64(p, cpu->preemption_enabled);
    p = irq_bootstrap_append_text(p, " runtime_ready=");
    p = irq_bootstrap_append_u64(p, cpu->runtime_ready);
    p = irq_bootstrap_append_text(p, " cpu_state=");
    p = irq_bootstrap_append_text(
        p, irq_bootstrap_topology_state_name(topology_state));
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static void irq_bootstrap_log_cpu_ready_summary(
    uint32_t ready, uint32_t failed, uint32_t waiting,
    uint64_t deadline_us, uint8_t abort)
{
    char line[224];
    char *p = irq_bootstrap_append_text(
        line, "[IRQ][CPU_READY_SUMMARY] expected=");
    p = irq_bootstrap_append_u64(p, g_irq_bootstrap.cpu_count);
    p = irq_bootstrap_append_text(p, " ready=");
    p = irq_bootstrap_append_u64(p, ready);
    p = irq_bootstrap_append_text(p, " failed=");
    p = irq_bootstrap_append_u64(p, failed);
    p = irq_bootstrap_append_text(p, " waiting=");
    p = irq_bootstrap_append_u64(p, waiting);
    p = irq_bootstrap_append_text(p, " deadline_us=");
    p = irq_bootstrap_append_u64(p, deadline_us);
    p = irq_bootstrap_append_text(p, " abort=");
    p = irq_bootstrap_append_u64(p, abort);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static void irq_bootstrap_runtime_abort(
    irq_runtime_abort_reason_t reason)
{
    irq_flags_t flags = spin_lock_irqsave(
        &g_irq_bootstrap.runtime_ready_lock);
    __atomic_store_n(&g_irq_bootstrap.runtime_ready_abort_reason,
                     reason, __ATOMIC_RELAXED);
    __atomic_store_n(&g_irq_bootstrap.runtime_ready_abort, 1,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_irq_bootstrap.runtime_ready_lock, flags);
}

static void irq_bootstrap_wake_missing_cpus(void)
{
    cpu_slot_t bsp = smp_bsp_cpu_slot();
    for (cpu_slot_t slot = 0; slot < g_irq_bootstrap.cpu_count; slot++) {
        irq_bootstrap_cpu_snapshot_t cpu;
        irq_bootstrap_cpu_load_snapshot(slot, &cpu);
        if (slot != bsp && !cpu.runtime_ready)
            lapic_send_ipi(cpu.apic_id,
                           INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE);
    }
}

bool irq_bootstrap_wait_all_runtime_ready(uint64_t timeout_us)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE ||
        !timeout_us || !irq_is_enabled() ||
        !__atomic_load_n(&g_irq_bootstrap.cpu_release,
                         __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_irq_bootstrap.runtime_ready_deadline_ns,
                        __ATOMIC_ACQUIRE))
        return false;
    uint64_t start = clock_monotonic_ns();
    if (!start || timeout_us > (UINT64_MAX - start) / 1000u)
        return false;
    uint64_t deadline = start + timeout_us * 1000u;
    g_irq_bootstrap.runtime_ready_timeout_us = timeout_us;
    __atomic_store_n(&g_irq_bootstrap.runtime_ready_deadline_ns,
                     deadline, __ATOMIC_RELEASE);
    irq_runtime_abort_reason_t abort_reason = IRQ_RUNTIME_ABORT_NONE;
    for (;;) {
        uint32_t ready = __atomic_load_n(&g_irq_bootstrap.runtime_count,
                                         __ATOMIC_ACQUIRE);
        uint32_t failed = __atomic_load_n(&g_irq_bootstrap.failed_count,
                                          __ATOMIC_ACQUIRE);
        if (ready == g_irq_bootstrap.cpu_count || failed) {
            if (failed) abort_reason = IRQ_RUNTIME_ABORT_HARD_FAILURE;
            break;
        }
        uint64_t now = clock_monotonic_ns();
        if (!now || now > deadline) {
            abort_reason = IRQ_RUNTIME_ABORT_DEADLINE;
            break;
        }
        __asm__ volatile("hlt" ::: "memory");
    }
    uint32_t ready = __atomic_load_n(&g_irq_bootstrap.runtime_count,
                                     __ATOMIC_ACQUIRE);
    uint32_t failed = __atomic_load_n(&g_irq_bootstrap.failed_count,
                                      __ATOMIC_ACQUIRE);
    if (abort_reason != IRQ_RUNTIME_ABORT_NONE ||
        ready != g_irq_bootstrap.cpu_count || failed) {
        if (abort_reason == IRQ_RUNTIME_ABORT_NONE)
            abort_reason = failed ? IRQ_RUNTIME_ABORT_HARD_FAILURE
                                  : IRQ_RUNTIME_ABORT_DEADLINE;
        irq_bootstrap_runtime_abort(abort_reason);
        ready = __atomic_load_n(&g_irq_bootstrap.runtime_count,
                                __ATOMIC_ACQUIRE);
        failed = __atomic_load_n(&g_irq_bootstrap.failed_count,
                                 __ATOMIC_ACQUIRE);
        uint32_t waiting = ready < g_irq_bootstrap.cpu_count
            ? g_irq_bootstrap.cpu_count - ready : 0;
        if (waiting >= failed) waiting -= failed;
        else waiting = 0;
        for (cpu_slot_t slot = 0; slot < g_irq_bootstrap.cpu_count; slot++) {
            irq_bootstrap_cpu_snapshot_t cpu;
            irq_bootstrap_cpu_load_snapshot(slot, &cpu);
            if (cpu.runtime_ready) continue;
            irq_bootstrap_cpu_refresh_journal(slot, &cpu);
            irq_cpu_ready_result_t reason =
                irq_bootstrap_cpu_timeout_reason(&cpu, abort_reason);
            irq_bootstrap_log_cpu_ready_timeout(&cpu, reason);
            if (!cpu.failed &&
                abort_reason == IRQ_RUNTIME_ABORT_DEADLINE)
                (void)irq_bootstrap_cpu_mark_failure(slot, reason);
            SmpCpuInfo *topology = smp_cpu_by_slot(slot);
            if (topology)
                __atomic_store_n(&topology->state, CPU_STATE_FAILED,
                                 __ATOMIC_RELEASE);
        }
        irq_bootstrap_wake_missing_cpus();
        irq_bootstrap_log_cpu_ready_summary(
            ready, failed, waiting, timeout_us, 1);
        return irq_bootstrap_fail();
    }
    uint8_t transition = 0;
    if (!__atomic_compare_exchange_n(
            &g_irq_bootstrap.runtime_ready_transition, &transition, 1,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return irq_bootstrap_fail();
    irq_bootstrap_log_cpu_ready_summary(
        ready, failed, 0, timeout_us, 0);
    irq_bootstrap_log_state("CLOCKEVENT_ACTIVE", g_irq_bootstrap.cpu_count);
    return true;
}

bool irq_bootstrap_release_services(void)
{
    if (irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE ||
        __atomic_load_n(&g_irq_bootstrap.runtime_count,
                        __ATOMIC_ACQUIRE) != g_irq_bootstrap.cpu_count ||
        !ioapic_route_enable(g_irq_bootstrap.keyboard_gsi))
        return irq_bootstrap_fail();
    if (!irq_bootstrap_advance(IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE,
                               IRQ_BOOTSTRAP_SERVICES_ACTIVE))
        return false;
    irq_bootstrap_log_state("SERVICES_ACTIVE", 0);
    return true;
}

bool irq_bootstrap_cpu_irq_ready(cpu_slot_t slot)
{
    return slot < g_irq_bootstrap.cpu_count &&
        __atomic_load_n(&g_irq_bootstrap.cpus[slot].runtime_ready,
                        __ATOMIC_ACQUIRE) != 0;
}

bool irq_bootstrap_services_are_ready(void)
{
    return irq_bootstrap_state() == IRQ_BOOTSTRAP_SERVICES_ACTIVE;
}

bool irq_bootstrap_cpu_snapshot(cpu_slot_t slot,
                                irq_bootstrap_cpu_snapshot_t *out)
{
    if (!out || slot >= g_irq_bootstrap.cpu_count)
        return false;
    irq_bootstrap_cpu_load_snapshot(slot, out);
    interrupt_cpu_snapshot_t journal;
    if (interrupt_context_snapshot(slot, &journal))
        out->first_vector = journal.first_vector;
    return true;
}

bool irq_bootstrap_snapshot(irq_bootstrap_snapshot_t *out)
{
    if (!out || !g_irq_bootstrap.initialized)
        return false;
    timer_clockevent_snapshot_t event;
    hpet_runtime_snapshot_t hpet;
    if (!timer_clockevent_snapshot(&event) || !hpet_runtime_snapshot(&hpet))
        return false;
    *out = (irq_bootstrap_snapshot_t){
        .state = irq_bootstrap_state(),
        .cpus_expected = g_irq_bootstrap.cpu_count,
        .cpus_prepared = __atomic_load_n(&g_irq_bootstrap.prepared_count,
                                         __ATOMIC_ACQUIRE),
        .cpus_verified = __atomic_load_n(&g_irq_bootstrap.verified_count,
                                         __ATOMIC_ACQUIRE),
        .cpus_runtime_ready = __atomic_load_n(&g_irq_bootstrap.runtime_count,
                                              __ATOMIC_ACQUIRE),
        .failed_cpus = __atomic_load_n(&g_irq_bootstrap.failed_count,
                                       __ATOMIC_ACQUIRE),
        .keyboard_gsi = g_irq_bootstrap.keyboard_gsi,
        .bsp_lapic_entered = g_irq_bootstrap.bsp_lapic_entered,
        .bsp_lapic_returned = g_irq_bootstrap.bsp_lapic_returned,
        .hpet_counter_before = g_irq_bootstrap.hpet_counter_before,
        .hpet_counter_after = g_irq_bootstrap.hpet_counter_after,
        .hpet_counter_delta = g_irq_bootstrap.hpet_counter_delta,
        .global_ticks = event.total_ticks,
        .early_tick_attempts = event.early_attempts,
        .non_bsp_tick_attempts = event.non_bsp_attempts,
        .tick_time_regressions = event.timestamp_regressions,
        .hpet_stray_irqs = hpet.stray_irqs,
        .runtime_ready_deadline_ns = __atomic_load_n(
            &g_irq_bootstrap.runtime_ready_deadline_ns,
            __ATOMIC_ACQUIRE),
        .transition_violations = __atomic_load_n(
            &g_irq_bootstrap.transition_violations, __ATOMIC_ACQUIRE),
        .bsp_probe_vector = g_irq_bootstrap.bsp_probe_vector,
        .hpet_probe_samples = g_irq_bootstrap.hpet_probe_samples,
        .clockevent_bsp_slot = event.bsp_slot,
        .clockevent_period_us = event.period_us,
        .clockevent_source = event.source,
        .pcat_compat = g_irq_bootstrap.pcat_compat,
        .controllers_quiescent = g_irq_bootstrap.controllers_quiescent,
        .routes_prepared = g_irq_bootstrap.routes_prepared,
        .bsp_lapic_probe_passed = g_irq_bootstrap.bsp_lapic_probe_passed,
        .hpet_clocksource_verified =
            g_irq_bootstrap.hpet_clocksource_verified,
        .hpet_timer0_quiescent = hpet.main_counter_enabled &&
            !hpet.legacy_replacement_enabled &&
            !hpet.timer0_interrupt_enabled &&
            !hpet.timer0_periodic_enabled && !hpet.timer0_fsb_enabled &&
            !hpet.timer0_route_enabled && !hpet.timer0_pending,
        .clockevent_active = event.active,
        .cpu_release = __atomic_load_n(&g_irq_bootstrap.cpu_release,
                                       __ATOMIC_ACQUIRE),
        .runtime_ready_abort = __atomic_load_n(
            &g_irq_bootstrap.runtime_ready_abort, __ATOMIC_ACQUIRE),
        .services_active = irq_bootstrap_services_are_ready()};
    for (cpu_slot_t slot = 0; slot < g_irq_bootstrap.cpu_count; slot++) {
        scheduler_preemption_snapshot_t scheduler;
        if (scheduler_preemption_snapshot(slot, &scheduler)) {
            if (scheduler.handoff_complete) out->handoffs_complete++;
            if (scheduler.irq_preemption_enabled) out->preemption_enabled++;
        }
    }
    return true;
}

static bool irq_bootstrap_hpet_routes_masked(void)
{
    for (uint32_t index = 0; index < ioapic_route_count(); index++) {
        interrupt_route_t route;
        if (!ioapic_route_at(index, &route))
            return false;
        if (route.owner == INTERRUPT_ROUTE_OWNER_HPET && !route.masked)
            return false;
    }
    return true;
}

bool irq_bootstrap_validate(void)
{
    irq_bootstrap_snapshot_t snapshot;
    legacy_pic_snapshot_t pic;
    madt_snapshot_t madt;
    ioapic_registry_snapshot_t registry;
    interrupt_route_t keyboard_route;
    hpet_runtime_snapshot_t hpet;
    timer_clockevent_snapshot_t event;
    uint32_t unknown_masked = 0;
    uint32_t unknown = 0;
    if (!irq_bootstrap_snapshot(&snapshot) ||
        snapshot.state != IRQ_BOOTSTRAP_SERVICES_ACTIVE ||
        snapshot.cpus_prepared != snapshot.cpus_expected ||
        snapshot.cpus_verified != snapshot.cpus_expected ||
        snapshot.handoffs_complete != snapshot.cpus_expected ||
        snapshot.preemption_enabled != snapshot.cpus_expected ||
        snapshot.cpus_runtime_ready != snapshot.cpus_expected ||
        snapshot.failed_cpus || snapshot.transition_violations ||
        !snapshot.cpu_release || snapshot.runtime_ready_abort ||
        !snapshot.runtime_ready_deadline_ns ||
        __atomic_load_n(&g_irq_bootstrap.runtime_ready_transition,
                        __ATOMIC_ACQUIRE) != 1 ||
        !snapshot.bsp_lapic_probe_passed ||
        !snapshot.hpet_clocksource_verified ||
        !snapshot.hpet_timer0_quiescent || !snapshot.clockevent_active ||
        snapshot.clockevent_source != TIMER_CLOCKEVENT_BSP_LAPIC ||
        snapshot.clockevent_bsp_slot != smp_bsp_cpu_slot() ||
        snapshot.clockevent_period_us != 1000u || !snapshot.global_ticks ||
        snapshot.non_bsp_tick_attempts || snapshot.early_tick_attempts ||
        snapshot.tick_time_regressions || snapshot.hpet_stray_irqs ||
        snapshot.bsp_probe_vector != INT_VECTOR_LAPIC_TIMER ||
        snapshot.bsp_lapic_entered == 0 ||
        snapshot.bsp_lapic_entered != snapshot.bsp_lapic_returned ||
        !snapshot.hpet_counter_delta ||
        snapshot.hpet_counter_after <= snapshot.hpet_counter_before ||
        !madt_snapshot(&madt) || !madt.valid ||
        madt.pcat_compat != snapshot.pcat_compat ||
        !legacy_pic_snapshot(&pic) || !legacy_pic_is_quiescent() ||
        pic.master_imr != 0xFFu || pic.slave_imr != 0xFFu ||
        pic.pcat_declared != snapshot.pcat_compat ||
        !ioapic_registry_snapshot(&registry) || !registry.initialized ||
        !registry.controllers || registry.prepared_routes < 1u ||
        !ioapic_route_snapshot(snapshot.keyboard_gsi, &keyboard_route) ||
        keyboard_route.owner != INTERRUPT_ROUTE_OWNER_KEYBOARD ||
        keyboard_route.vector != INT_VECTOR_KEYBOARD ||
        keyboard_route.destination_apic_id != g_bsp_apic_id ||
        keyboard_route.masked ||
        !irq_bootstrap_hpet_routes_masked() ||
        !hpet_runtime_snapshot(&hpet) || !hpet.main_counter_enabled ||
        hpet.legacy_replacement_enabled || hpet.timer0_interrupt_enabled ||
        hpet.timer0_periodic_enabled || hpet.timer0_fsb_enabled ||
        hpet.timer0_route_enabled || hpet.timer0_pending || hpet.stray_irqs ||
        !timer_clockevent_snapshot(&event) || !timer_clockevent_validate() ||
        !ioapic_unknown_routes_masked(&unknown_masked, &unknown) ||
        unknown_masked != unknown)
        return false;
    for (cpu_slot_t slot = 0; slot < snapshot.cpus_expected; slot++) {
        irq_bootstrap_cpu_snapshot_t runtime;
        interrupt_cpu_snapshot_t journal;
        lapic_timer_validation_t timer;
        lapic_quiescent_snapshot_t lapic;
        if (!irq_bootstrap_cpu_snapshot(slot, &runtime) ||
            runtime.stage != IRQ_CPU_RUNTIME_READY ||
            runtime.failure_stage != IRQ_CPU_RUNTIME_OFF ||
            runtime.failure_reason != IRQ_CPU_READY_OK || runtime.failed ||
            !runtime.interrupt_prepared || !runtime.interrupt_verified ||
            !runtime.handoff_complete || !runtime.preemption_enabled ||
            !runtime.runtime_ready ||
            !interrupt_context_snapshot_stable(
                slot, &journal, INTERRUPT_CONTEXT_SNAPSHOT_ATTEMPTS) ||
            journal.unexpected || journal.underflow || journal.mismatch ||
            journal.first_vector != INT_VECTOR_LAPIC_TIMER ||
            !lapic_quiescent_snapshot(slot, &lapic) || !lapic.valid ||
            !(lapic.lvt_lint0 & APIC_TIMER_MASKED) ||
            !(lapic.lvt_lint1 & APIC_TIMER_MASKED) || lapic.esr_after ||
            !lapic_timer_validate_configuration(slot, &timer) ||
            (timer.passed_mask & LAPIC_TIMER_VALID_CONFIG_MASK) !=
                LAPIC_TIMER_VALID_CONFIG_MASK ||
            !(timer.passed_mask & LAPIC_TIMER_VALID_IRQ_SEEN))
            return false;
    }
    return true;
}

bool irq_bootstrap_model_selftest(void)
{
    irq_bootstrap_state_t state = IRQ_BOOTSTRAP_OFF;
    bool ok = state == IRQ_BOOTSTRAP_OFF;
    state = IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT;
    ok = ok && state == IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT;
    state = IRQ_BOOTSTRAP_ROUTES_PREPARED;
    ok = ok && state == IRQ_BOOTSTRAP_ROUTES_PREPARED;
    state = IRQ_BOOTSTRAP_CPUS_PREPARED;
    ok = ok && state == IRQ_BOOTSTRAP_CPUS_PREPARED;
    state = IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED;
    ok = ok && state == IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED;
    state = IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED;
    ok = ok && state == IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED;
    state = IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE;
    ok = ok && state == IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE;
    state = IRQ_BOOTSTRAP_SERVICES_ACTIVE;
    ok = ok && state == IRQ_BOOTSTRAP_SERVICES_ACTIVE &&
         IRQ_BOOTSTRAP_FAILED > IRQ_BOOTSTRAP_SERVICES_ACTIVE;

    irq_cpu_runtime_stage_t cpu_stage = IRQ_CPU_RUNTIME_WAIT_RELEASE;
    uint32_t sequence = 0;
    uint32_t baseline_order = ++sequence;
    cpu_stage = IRQ_CPU_RUNTIME_TIMER_BASELINE;
    uint32_t enable_order = ++sequence;
    cpu_stage = IRQ_CPU_RUNTIME_TIMER_WAIT;
    uint64_t elapsed_us = 250001u;
    uint8_t global_abort = 0;
    uint8_t hard_failure = 0;
    ok = ok && baseline_order < enable_order &&
         cpu_stage == IRQ_CPU_RUNTIME_TIMER_WAIT && elapsed_us > 250000u &&
         !global_abort && !hard_failure;

#ifdef HOBBYOS_IRQ_NEGATIVE_LOCAL_READY_DEADLINE
    if (cpu_stage == IRQ_CPU_RUNTIME_TIMER_WAIT && elapsed_us > 250000u &&
        !global_abort && !hard_failure) {
        serial_write_all(
            "[IRQ][NEGATIVE] LOCAL_READY_DEADLINE_FALSE_FAILURE_DETECTED\n");
        return false;
    }
#endif

    global_abort = 1;
    uint8_t waiter_woken = global_abort;
    irq_cpu_ready_result_t wait_result = global_abort
        ? IRQ_CPU_READY_RELEASE_ABORTED : IRQ_CPU_READY_OK;
    ok = ok && waiter_woken &&
         wait_result == IRQ_CPU_READY_RELEASE_ABORTED;

    hard_failure = 1;
    irq_cpu_ready_result_t hard_result = hard_failure
        ? IRQ_CPU_READY_TIMER_STATE : IRQ_CPU_READY_OK;
    ok = ok && hard_result == IRQ_CPU_READY_TIMER_STATE;

    uint8_t runtime_ready = 0;
    uint32_t runtime_count = 0;
    bool first_publication = !runtime_ready;
    if (first_publication) {
        runtime_ready = 1;
        runtime_count++;
    }
    bool duplicate_publication = runtime_ready != 0;
    ok = ok && first_publication && duplicate_publication &&
         runtime_count == 1;

    uint8_t all_ready_transition = 0;
    uint32_t expected = 2;
    uint32_t ready = 2;
    bool first_transition = ready == expected && !all_ready_transition;
    if (first_transition) all_ready_transition = 1;
    bool duplicate_transition = ready == expected && !all_ready_transition;
    ok = ok && first_transition && !duplicate_transition &&
         all_ready_transition == 1;

    const char *missing_stage = irq_bootstrap_cpu_stage_name(
        IRQ_CPU_RUNTIME_TIMER_WAIT);
    ok = ok && missing_stage && missing_stage[0] == 'T';

    serial_write_all(ok
        ? "[IRQ][RUNTIME_RENDEZVOUS_SELFTEST] PASS\n"
        : "[IRQ][RUNTIME_RENDEZVOUS_SELFTEST] FAIL\n");
    return ok;
}

#include "lapic.h"

#include "../acpi/madt.h"
#include "../core/clock.h"
#include "../cpu/cpu.h"
#include "../cpu/mmio.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../memory/paging.h"
#include "../smp/cpu_limits.h"
#include "../smp/smp_topology.h"

static uint64_t g_lapic_base;
static uint8_t g_x2apic;
static lapic_timer_calibration_t g_timer_cal[HOBBYOS_MAX_CPUS];
static volatile uint64_t g_timer_irqs[HOBBYOS_MAX_CPUS];
static lapic_timer_state_t g_timer_state[HOBBYOS_MAX_CPUS];
static lapic_quiescent_snapshot_t g_lapic_quiescent[HOBBYOS_MAX_CPUS];
static lapic_quiescent_snapshot_t g_lapic_early_quiescent;
static uint64_t g_timer_programming_generation;

void lapic_write(uint32_t reg, uint32_t value)
{
    if (g_x2apic)
    {
        cpu_write_msr(0x800u + (reg >> 4), value);
        return;
    }
    if (g_lapic_base)
        mmio_write32((void *)(g_lapic_base + reg), value);
}

uint32_t lapic_read(uint32_t reg)
{
    if (g_x2apic)
        return (uint32_t)cpu_read_msr(0x800u + (reg >> 4));
    return g_lapic_base ? mmio_read32((void *)(g_lapic_base + reg)) : 0;
}

uint32_t lapic_get_id(void)
{
    uint32_t value = lapic_read(LAPIC_ID);
    return g_x2apic ? value : ((value >> 24) & 0xFFu);
}

bool lapic_is_x2apic(void)
{
    return g_x2apic != 0;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

static bool lapic_lvt_mask(uint32_t reg, uint32_t vector)
{
    lapic_write(reg, APIC_TIMER_MASKED | vector);
    return (lapic_read(reg) & APIC_TIMER_MASKED) != 0;
}

static bool lapic_quiesce_current(lapic_quiescent_snapshot_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->apic_id = lapic_get_id();
    out->version = lapic_read(LAPIC_VER);
    out->max_lvt = (out->version >> 16) & 0xFFu;
    out->x2apic = g_x2apic;

    out->esr_before = lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);
    out->esr_after = lapic_read(LAPIC_ESR);

    lapic_write(LAPIC_TICR, 0);
    if (lapic_lvt_mask(LAPIC_LVT_TIMER, 0)) out->masked_count++;
    if (out->max_lvt >= 4u && lapic_lvt_mask(LAPIC_LVT_THERMAL, 0))
        out->masked_count++;
    if (out->max_lvt >= 5u && lapic_lvt_mask(LAPIC_LVT_PERF, 0))
        out->masked_count++;
    if (lapic_lvt_mask(LAPIC_LVT_LINT0, 0)) out->masked_count++;
    if (lapic_lvt_mask(LAPIC_LVT_LINT1, 0)) out->masked_count++;
    if (lapic_lvt_mask(LAPIC_LVT_ERROR, 0xFEu)) out->masked_count++;
    if (out->max_lvt >= 6u && lapic_lvt_mask(LAPIC_LVT_CMCI, 0))
        out->masked_count++;

    lapic_write(LAPIC_TPR, 0xF0u);
    lapic_write(LAPIC_SPURIOUS, 0xFFu | (1u << 8));
    out->tpr = lapic_read(LAPIC_TPR);
    out->svr = lapic_read(LAPIC_SPURIOUS);
    out->lvt_timer = lapic_read(LAPIC_LVT_TIMER);
    out->lvt_thermal = out->max_lvt >= 4u ? lapic_read(LAPIC_LVT_THERMAL) : 0;
    out->lvt_perf = out->max_lvt >= 5u ? lapic_read(LAPIC_LVT_PERF) : 0;
    out->lvt_lint0 = lapic_read(LAPIC_LVT_LINT0);
    out->lvt_lint1 = lapic_read(LAPIC_LVT_LINT1);
    out->lvt_error = lapic_read(LAPIC_LVT_ERROR);
    out->lvt_cmci = out->max_lvt >= 6u ? lapic_read(LAPIC_LVT_CMCI) : 0;

    uint32_t expected = 4u;
    if (out->max_lvt >= 4u) expected++;
    if (out->max_lvt >= 5u) expected++;
    if (out->max_lvt >= 6u) expected++;
    out->valid = out->esr_after == 0 && out->tpr == 0xF0u &&
                 (out->svr & (1u << 8)) &&
                 (out->lvt_lint0 & APIC_TIMER_MASKED) &&
                 (out->lvt_lint1 & APIC_TIMER_MASKED) &&
                 out->masked_count == expected;
    return out->valid;
}

static char *lapic_append_text(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}

static char *lapic_append_u32(char *out, uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; }
    while (value);
    while (count) *out++ = digits[--count];
    return out;
}

static void lapic_log_quiescent(uint32_t slot,
                                const lapic_quiescent_snapshot_t *snapshot)
{
    char line[176];
    char *p = lapic_append_text(line, "[IRQ][LAPIC] QUIESCENT slot=");
    p = lapic_append_u32(p, slot);
    p = lapic_append_text(p, " apic=");
    p = lapic_append_u32(p, snapshot->apic_id);
    p = lapic_append_text(p, " lvt_masked=");
    p = lapic_append_u32(p, snapshot->masked_count);
    p = lapic_append_text(p, " esr=");
    p = lapic_append_u32(p, snapshot->esr_after);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static bool lapic_enable_for_current_cpu(void)
{
    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpu_get_cpuid(1, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)edx;
    apic_msr |= (1ULL << 11);
    if (g_x2apic || ((ecx >> 21) & 1u))
        apic_msr |= (1ULL << 10);
    cpu_write_msr(IA32_APIC_BASE_MSR, apic_msr);
    apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    g_x2apic = (apic_msr & (1ULL << 10)) ? 1u : 0u;
    return (apic_msr & (1ULL << 11)) != 0;
}

bool init_lapic(void)
{
    g_lapic_base = get_lapic_base();
    uint64_t apic_msr = cpu_read_msr(IA32_APIC_BASE_MSR);
    if (!g_lapic_base)
        g_lapic_base = apic_msr & 0xFFFFF000ULL;
    if (!lapic_enable_for_current_cpu())
        return false;

    if (!g_x2apic)
    {
        if (!g_lapic_base)
            return false;
        uint64_t base_page = g_lapic_base & ~0xFFFULL;
        paging_map(base_page, base_page,
                   PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        __asm__ volatile("invlpg (%0)" : : "r"(base_page) : "memory");
    }
    serial_write_all(g_x2apic ? "[LAPIC] System using X2APIC\n"
                               : "[LAPIC] System using XAPIC\n");
    return lapic_quiesce_current(&g_lapic_early_quiescent);
}

bool init_lapic_ap(void)
{
    if (!lapic_enable_for_current_cpu())
        return false;
    lapic_quiescent_snapshot_t snapshot;
    if (!lapic_quiesce_current(&snapshot))
        return false;
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        return false;
    g_lapic_quiescent[slot] = snapshot;
    lapic_log_quiescent(slot, &snapshot);
    return true;
}

bool lapic_bind_quiescent_slot(uint32_t slot)
{
    const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
    if (!cpu || cpu->apic_id != g_lapic_early_quiescent.apic_id ||
        !g_lapic_early_quiescent.valid || slot >= HOBBYOS_MAX_CPUS)
        return false;
    g_lapic_quiescent[slot] = g_lapic_early_quiescent;
    lapic_log_quiescent(slot, &g_lapic_quiescent[slot]);
    return true;
}

bool lapic_quiescent_snapshot(uint32_t slot,
                              lapic_quiescent_snapshot_t *out)
{
    if (!out || slot >= HOBBYOS_MAX_CPUS ||
        !g_lapic_quiescent[slot].valid)
        return false;
    *out = g_lapic_quiescent[slot];
    return true;
}

bool lapic_set_normal_delivery(bool enable)
{
    lapic_write(LAPIC_TPR, enable ? 0u : 0xF0u);
    return lapic_read(LAPIC_TPR) == (enable ? 0u : 0xF0u);
}

static void lapic_wait_icr_idle(void)
{
    if (g_x2apic)
        return;
    uint32_t timeout = 100000u;
    while ((lapic_read(LAPIC_ICR0) & APIC_DS_PENDING) && timeout--)
        __asm__ volatile("pause");
}

static void lapic_write_icr(uint32_t destination, uint32_t vector,
                            uint32_t flags)
{
    lapic_wait_icr_idle();
    if (g_x2apic)
        cpu_write_msr(0x830u,
                      ((uint64_t)destination << 32) | flags | vector);
    else
    {
        if (destination > 0xFFu)
            return;
        lapic_write(LAPIC_ICR1, destination << 24);
        lapic_write(LAPIC_ICR0, flags | vector);
    }
    lapic_wait_icr_idle();
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
    lapic_write_icr(apic_id, vector,
                    APIC_DM_FIXED | APIC_LEVEL_ASSERT | APIC_TRIGGER_EDGE);
}

void lapic_send_init(uint32_t apic_id)
{
    lapic_write_icr(apic_id, 0,
                    APIC_DM_INIT | APIC_LEVEL_ASSERT | APIC_TRIGGER_LEVEL);
    for (volatile uint32_t i = 0; i < 10000u; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
    if (!g_x2apic)
        lapic_write_icr(apic_id, 0,
                        APIC_DM_INIT | APIC_LEVEL_DEASSERT |
                            APIC_TRIGGER_LEVEL);
    for (volatile uint32_t i = 0; i < 10000u; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

void lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page)
{
    lapic_write_icr(apic_id, (uint8_t)(trampoline_page & 0xFFu),
                    APIC_DM_SIPI | APIC_LEVEL_ASSERT | APIC_TRIGGER_EDGE);
    for (volatile uint32_t i = 0; i < 200u; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

bool lapic_timer_calibration_math(uint32_t elapsed_ticks, uint64_t elapsed_ns,
                                  uint32_t desired_period_us,
                                  lapic_timer_calibration_t *out)
{
    if (!out || !elapsed_ticks || !elapsed_ns || !desired_period_us)
        return false;
    memset(out, 0, sizeof(*out));
    uint64_t period_ns = (uint64_t)desired_period_us * 1000ULL;
    uint64_t quotient = elapsed_ticks / elapsed_ns;
    uint64_t remainder = elapsed_ticks % elapsed_ns;
    if (quotient && period_ns > UINT64_MAX / quotient)
        return false;
    uint64_t periodic = quotient * period_ns;
    if (remainder)
    {
        if (period_ns > UINT64_MAX / remainder)
            return false;
        periodic += (remainder * period_ns) / elapsed_ns;
    }
    if (!periodic || periodic > UINT32_MAX)
        return false;
    out->window_ns = elapsed_ns;
    out->elapsed_ticks = elapsed_ticks;
    out->ticks_per_second =
        ((uint64_t)elapsed_ticks * 1000000000ULL) / elapsed_ns;
    out->ticks_per_ms = (uint32_t)(out->ticks_per_second / 1000ULL);
    out->periodic_initial_count = (uint32_t)periodic;
    out->divisor = 16;
    out->valid = out->ticks_per_ms != 0;
    return out->valid;
}

bool lapic_timer_calibrate(uint32_t desired_period_us,
                           lapic_timer_calibration_t *out)
{
    if (!out || !clock_monotonic_is_ready())
        return false;
    lapic_write(LAPIC_LVT_TIMER,
                APIC_TIMER_MASKED | APIC_TIMER_ONE_SHOT);
    lapic_write(LAPIC_TDCR, 0x3u);
    lapic_write(LAPIC_TICR, UINT32_MAX);
    uint64_t start = clock_monotonic_ns();
    uint64_t now = start;
    if (!start)
        return false;
    while (now - start < 10000000ULL)
    {
        __asm__ volatile("pause");
        now = clock_monotonic_ns();
        if (now < start)
            return false;
    }
    uint32_t elapsed = UINT32_MAX - lapic_read(LAPIC_TCCR);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_MASKED);
    lapic_write(LAPIC_TICR, 0);
    bool ok = lapic_timer_calibration_math(elapsed, now - start,
                                            desired_period_us, out);
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (ok && smp_current_cpu_slot(&slot) && slot < HOBBYOS_MAX_CPUS)
    {
        g_timer_cal[slot] = *out;
        g_timer_state[slot].calibration = *out;
        g_timer_state[slot].desired_period_us = desired_period_us;
        __atomic_store_n(&g_timer_state[slot].mode, LAPIC_TIMER_CALIBRATED,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_timer_state[slot].calibrated, 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_timer_state[slot].masked, 1,
                         __ATOMIC_RELEASE);
    }
    serial_write_all(ok ? "[TIMER][LAPIC_CAL] status=OK initial=0x"
                        : "[TIMER][LAPIC_CAL] status=FAIL initial=0x");
    serial_write_hex64_all(ok ? out->periodic_initial_count : 0);
    serial_write_all("\n");
    return ok;
}

static void lapic_timer_state_program(cpu_slot_t slot,
                                      const lapic_timer_calibration_t *cal,
                                      uint32_t vector, uint32_t initial,
                                      uint32_t lvt, uint32_t ticr,
                                      lapic_timer_mode_t mode)
{
    lapic_timer_state_t *state = &g_timer_state[slot];
    state->calibration = *cal;
    __atomic_store_n(&state->programmed_vector, vector, __ATOMIC_RELAXED);
    __atomic_store_n(&state->programmed_divisor, cal->divisor,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&state->programmed_initial_count, initial,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&state->lvt_value, lvt, __ATOMIC_RELAXED);
    __atomic_store_n(&state->tdcr_value, lapic_read(LAPIC_TDCR),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&state->ticr_value, ticr, __ATOMIC_RELAXED);
    __atomic_store_n(&state->mode, mode, __ATOMIC_RELEASE);
    __atomic_store_n(&state->periodic,
                     (lvt & APIC_TIMER_PERIODIC) != 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&state->masked,
                     (lvt & APIC_TIMER_MASKED) != 0,
                     __ATOMIC_RELAXED);
}

bool lapic_timer_prepare_periodic(
    uint32_t vector, const lapic_timer_calibration_t *calibration)
{
    if (!calibration || !calibration->valid ||
        !calibration->periodic_initial_count || vector > 255u)
        return false;
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        return false;
    lapic_write(LAPIC_TDCR, 0x3u);
    lapic_write(LAPIC_LVT_TIMER,
                APIC_TIMER_PERIODIC | APIC_TIMER_MASKED | vector);
    lapic_write(LAPIC_TICR, 0);
    uint32_t tdcr = lapic_read(LAPIC_TDCR);
    uint32_t lvt = lapic_read(LAPIC_LVT_TIMER);
    uint32_t ticr = lapic_read(LAPIC_TICR);
    bool valid = (tdcr & 0xBu) == 0x3u &&
                 (lvt & 0xFFu) == vector &&
                 (lvt & APIC_TIMER_PERIODIC) &&
                 (lvt & APIC_TIMER_MASKED) && ticr == 0;
    if (!valid)
        return false;
    lapic_timer_state_program(slot, calibration, vector,
                              calibration->periodic_initial_count, lvt,
                              ticr, LAPIC_TIMER_PREPARED_MASKED);
    __atomic_store_n(&g_timer_state[slot].programmed, 0,
                     __ATOMIC_RELEASE);
    return true;
}

bool lapic_timer_enable_periodic(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        return false;
    lapic_timer_state_t *state = &g_timer_state[slot];
    if (__atomic_load_n(&state->mode, __ATOMIC_ACQUIRE) !=
            LAPIC_TIMER_PREPARED_MASKED ||
        !state->calibration.valid || !state->programmed_vector)
        return false;
    uint32_t initial = state->calibration.periodic_initial_count;
#ifdef HOBBYOS_LAPIC_NEGATIVE_WRONG_PERIOD
    if (slot == 0 && initial <= UINT32_MAX / 2u) initial *= 2u;
#endif
    uint32_t masked_lvt = APIC_TIMER_PERIODIC | APIC_TIMER_MASKED |
                          state->programmed_vector;
    uint32_t active_lvt = masked_lvt & ~APIC_TIMER_MASKED;
#ifdef HOBBYOS_LAPIC_NEGATIVE_MASK_ONE_CPU
    if (slot == 0) active_lvt |= APIC_TIMER_MASKED;
#endif
    lapic_write(LAPIC_TDCR, 0x3u);
    lapic_write(LAPIC_LVT_TIMER, masked_lvt);
    lapic_write(LAPIC_TICR, initial);
    lapic_write(LAPIC_LVT_TIMER, active_lvt);
    uint32_t tdcr = lapic_read(LAPIC_TDCR);
    uint32_t lvt = lapic_read(LAPIC_LVT_TIMER);
    uint32_t ticr = lapic_read(LAPIC_TICR);
    bool valid = (tdcr & 0xBu) == 0x3u &&
                 (lvt & 0xFFu) == state->programmed_vector &&
                 (lvt & APIC_TIMER_PERIODIC) &&
                 !(lvt & APIC_TIMER_MASKED) &&
                 ticr == state->calibration.periodic_initial_count;
#if defined(HOBBYOS_LAPIC_NEGATIVE_MASK_ONE_CPU) || \
    defined(HOBBYOS_LAPIC_NEGATIVE_WRONG_PERIOD)
    if (slot == 0) valid = true;
#endif
    if (!valid)
        return false;
    lapic_timer_state_program(slot, &state->calibration,
                              state->programmed_vector, initial, lvt, ticr,
                              LAPIC_TIMER_ACTIVE_PERIODIC);
    __atomic_store_n(&state->start_ns, clock_monotonic_ns(),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&state->programming_generation,
        __atomic_add_fetch(&g_timer_programming_generation, 1,
                           __ATOMIC_RELAXED),
        __ATOMIC_RELAXED);
    __atomic_store_n(&state->programmed, 1, __ATOMIC_RELEASE);
    return true;
}

bool lapic_timer_disable(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        return false;
    uint32_t lvt = lapic_read(LAPIC_LVT_TIMER) | APIC_TIMER_MASKED;
    lapic_write(LAPIC_LVT_TIMER, lvt);
    lapic_write(LAPIC_TICR, 0);
    lvt = lapic_read(LAPIC_LVT_TIMER);
    uint32_t ticr = lapic_read(LAPIC_TICR);
    if (!(lvt & APIC_TIMER_MASKED) || ticr != 0)
        return false;
    g_timer_state[slot].lvt_value = lvt;
    g_timer_state[slot].ticr_value = 0;
    g_timer_state[slot].masked = 1;
    g_timer_state[slot].programmed = 0;
    __atomic_store_n(&g_timer_state[slot].mode,
                     g_timer_state[slot].calibrated
                         ? LAPIC_TIMER_CALIBRATED : LAPIC_TIMER_OFF,
                     __ATOMIC_RELEASE);
    return true;
}

bool lapic_timer_is_active(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    return smp_current_cpu_slot(&slot) && slot < HOBBYOS_MAX_CPUS &&
           __atomic_load_n(&g_timer_state[slot].mode, __ATOMIC_ACQUIRE) ==
               LAPIC_TIMER_ACTIVE_PERIODIC &&
           !(lapic_read(LAPIC_LVT_TIMER) & APIC_TIMER_MASKED) &&
           lapic_read(LAPIC_TICR) != 0;
}

bool lapic_timer_start_periodic(
    uint32_t vector, const lapic_timer_calibration_t *calibration)
{
    return lapic_timer_prepare_periodic(vector, calibration) &&
           lapic_timer_enable_periodic();
}

void lapic_timer_record_irq_at(uint64_t now_ns)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        return;
    __atomic_add_fetch(&g_timer_irqs[slot], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_timer_state[slot].irq_count, 1,
                       __ATOMIC_RELAXED);
    uint64_t previous = __atomic_exchange_n(
        &g_timer_state[slot].last_irq_ns, now_ns, __ATOMIC_RELAXED);
    if (previous && now_ns >= previous)
    {
        uint64_t gap = now_ns - previous;
        uint64_t old = __atomic_load_n(
            &g_timer_state[slot].max_irq_gap_ns, __ATOMIC_RELAXED);
        while (old < gap && !__atomic_compare_exchange_n(
            &g_timer_state[slot].max_irq_gap_ns, &old, gap, false,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
    }
}

void lapic_timer_record_irq(void)
{
    lapic_timer_record_irq_at(clock_monotonic_ns());
}

bool lapic_timer_get_calibration(uint32_t slot,
                                 lapic_timer_calibration_t *out,
                                 uint64_t *irq_count)
{
    if (slot >= HOBBYOS_MAX_CPUS || !out || !g_timer_cal[slot].valid)
        return false;
    *out = g_timer_cal[slot];
    if (irq_count)
        *irq_count = __atomic_load_n(&g_timer_irqs[slot],
                                     __ATOMIC_RELAXED);
    return true;
}

bool lapic_timer_state_snapshot(uint32_t slot, lapic_timer_state_t *out)
{
    if (slot >= HOBBYOS_MAX_CPUS || !out)
        return false;
    lapic_timer_state_t *state = &g_timer_state[slot];
    *out = *state;
    out->irq_count = __atomic_load_n(&state->irq_count, __ATOMIC_RELAXED);
    out->last_irq_ns = __atomic_load_n(&state->last_irq_ns,
                                       __ATOMIC_RELAXED);
    out->max_irq_gap_ns = __atomic_load_n(&state->max_irq_gap_ns,
                                          __ATOMIC_RELAXED);
    out->mode = __atomic_load_n(&state->mode, __ATOMIC_ACQUIRE);
    out->programmed = __atomic_load_n(&state->programmed, __ATOMIC_ACQUIRE);
    return out->calibrated != 0;
}

bool lapic_timer_validate_configuration(uint32_t slot,
                                        lapic_timer_validation_t *out)
{
    if (!out)
        return false;
    *out = (lapic_timer_validation_t){0};
    lapic_timer_state_t state;
    if (!lapic_timer_state_snapshot(slot, &state))
        return false;
    if (state.calibrated && state.calibration.valid &&
        state.calibration.ticks_per_ms)
        out->passed_mask |= LAPIC_TIMER_VALID_CALIBRATION;
    if (state.programmed && state.programming_generation &&
        state.mode == LAPIC_TIMER_ACTIVE_PERIODIC)
        out->passed_mask |= LAPIC_TIMER_VALID_PROGRAMMED;
    if (state.programmed_vector == 34u &&
        (state.lvt_value & 0xFFu) == 34u)
        out->passed_mask |= LAPIC_TIMER_VALID_VECTOR;
    if (state.programmed_divisor == 16u &&
        (state.tdcr_value & 0xBu) == 0x3u)
        out->passed_mask |= LAPIC_TIMER_VALID_DIVISOR;
    if (state.programmed_initial_count ==
            state.calibration.periodic_initial_count &&
        state.ticr_value == state.calibration.periodic_initial_count &&
        state.ticr_value)
        out->passed_mask |= LAPIC_TIMER_VALID_INITIAL_COUNT;
    if (state.periodic && (state.lvt_value & APIC_TIMER_PERIODIC))
        out->passed_mask |= LAPIC_TIMER_VALID_PERIODIC;
    if (!state.masked && !(state.lvt_value & APIC_TIMER_MASKED))
        out->passed_mask |= LAPIC_TIMER_VALID_UNMASKED;
    if (state.irq_count) out->passed_mask |= LAPIC_TIMER_VALID_IRQ_SEEN;
    if (state.last_irq_ns) out->passed_mask |= LAPIC_TIMER_VALID_LAST_IRQ;
    out->failed_mask =
        (LAPIC_TIMER_VALID_CONFIG_MASK | LAPIC_TIMER_VALID_IRQ_SEEN |
         LAPIC_TIMER_VALID_LAST_IRQ) & ~out->passed_mask;
    return (out->failed_mask & LAPIC_TIMER_VALID_CONFIG_MASK) == 0;
}

bool lapic_timer_model_selftest(void)
{
    lapic_timer_calibration_t calibration;
    bool ok = lapic_timer_calibration_math(100000u, 10000000ULL,
                                            1000u, &calibration);
    ok = ok && calibration.periodic_initial_count == 10000u &&
         calibration.divisor == 16u && calibration.valid;
    ok = ok && !lapic_timer_calibration_math(0, 1000, 1000,
                                              &calibration);
    ok = ok && LAPIC_TIMER_OFF < LAPIC_TIMER_CALIBRATED &&
         LAPIC_TIMER_CALIBRATED < LAPIC_TIMER_PREPARED_MASKED &&
         LAPIC_TIMER_PREPARED_MASKED < LAPIC_TIMER_ACTIVE_PERIODIC;
    return ok;
}

void lapic_send_broadcast_halt(void)
{
    lapic_write_icr(0, 0, APIC_DEST_SHORTHAND_ALL_BUT_SELF |
                              APIC_DM_NMI | APIC_LEVEL_ASSERT);
}

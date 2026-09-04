#include "timer.h"
#include "../timer/hpet.h"
#include "../apic/lapic.h"
#include "../shell/shell.h"
#include "../graphics/console.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../core/scheduler.h"
#include "../core/timers.h"
#include "../core/task.h"
#include "../core/idt.h"
#include "../core/clock.h"
#include "../core/panic.h"
#include "../core/irq_bootstrap.h"
#include "../drivers/serial.h"

extern void xhci_poll_events(void);

static uint64_t g_last_processed_ms = 0;
static timer_clockevent_snapshot_t g_clockevent;

static int g_xhci_poll_counter = 0;
static const int XHCI_POLL_THRESHOLD = 1;

static int g_shell_tick_counter = 0;
static const int SHELL_TICK_DIVIDER = 10;

static char *timer_append_text(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *timer_append_u64(char *out, uint64_t value)
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

void timer_init(void)
{
    g_last_processed_ms = clock_monotonic_ms();
    g_xhci_poll_counter = 0;
    g_shell_tick_counter = 0;
    g_clockevent = (timer_clockevent_snapshot_t){
        .source = TIMER_CLOCKEVENT_NONE,
        .bsp_slot = CPU_SLOT_INVALID,
    };
}

uint64_t timer_get_uptime_ms(void)
{

    return clock_monotonic_ms();
}

void timer_sleep(uint64_t ms)
{
    task_wait_result_t r=timer_sleep_interruptible(ms);
    if(r==TASK_WAIT_RESULT_CANCELLED) task_cancel_point();
    if(r!=TASK_WAIT_RESULT_OK && r!=TASK_WAIT_RESULT_TIMEOUT) kpanic("TIMER: sleep wait failed");
    task_cancel_point();
}

void timer_run_deferred_at(uint64_t now_ms)
{
    if (!irq_bootstrap_services_are_ready())
        return;

    if (!now_ms || now_ms < g_last_processed_ms) return;
    uint64_t delta=now_ms-g_last_processed_ms;
    if (!delta) return;
    g_last_processed_ms=now_ms;

    g_shell_tick_counter += delta;
    if (g_shell_tick_counter >= SHELL_TICK_DIVIDER)
    {

        shell_on_tick();
        g_shell_tick_counter = 0;
    }

    g_xhci_poll_counter += delta;
    if (g_xhci_poll_counter >= XHCI_POLL_THRESHOLD)
    {

        xhci_poll_events();

        xhci_kbd_repeat_poll();

        g_xhci_poll_counter = 0;
    }
}

void timer_run_deferred(void)
{
    timer_run_deferred_at(clock_monotonic_ms());
}

bool timer_clockevent_configure_bsp_lapic(cpu_slot_t bsp_slot,
                                          uint32_t period_us)
{
    if (irq_is_enabled() || bsp_slot == CPU_SLOT_INVALID ||
        bsp_slot != smp_bsp_cpu_slot() || period_us != 1000u ||
        __atomic_load_n(&g_clockevent.configured, __ATOMIC_ACQUIRE))
        return false;
    g_clockevent.source = TIMER_CLOCKEVENT_BSP_LAPIC;
    g_clockevent.bsp_slot = bsp_slot;
    g_clockevent.period_us = period_us;
    __atomic_store_n(&g_clockevent.configured, 1, __ATOMIC_RELEASE);
    return true;
}

bool timer_clockevent_activate(void)
{
    irq_bootstrap_snapshot_t boot;
    if (irq_is_enabled() || !scheduler_is_started() ||
        irq_bootstrap_state() != IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED ||
        !irq_bootstrap_snapshot(&boot) || !boot.bsp_lapic_probe_passed ||
        !boot.hpet_clocksource_verified || !boot.hpet_timer0_quiescent ||
        !hpet_timer0_is_quiescent() ||
        !__atomic_load_n(&g_clockevent.configured, __ATOMIC_ACQUIRE) ||
        g_clockevent.source != TIMER_CLOCKEVENT_BSP_LAPIC ||
        g_clockevent.bsp_slot != smp_bsp_cpu_slot() ||
        g_clockevent.period_us != 1000u ||
        __atomic_exchange_n(&g_clockevent.active, 1, __ATOMIC_ACQ_REL))
        return false;

    char line[128];
    char *p = timer_append_text(
        line, "[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC bsp_slot=");
    p = timer_append_u64(p, g_clockevent.bsp_slot);
    p = timer_append_text(p, " period_us=1000\n");
    *p = 0;
    serial_write_all(line);
    return true;
}

bool timer_clockevent_is_active(void)
{
    return __atomic_load_n(&g_clockevent.active, __ATOMIC_ACQUIRE) != 0;
}

void timer_clockevent_on_lapic_tick(cpu_slot_t slot, uint64_t now_ns)
{
    if (!__atomic_load_n(&g_clockevent.configured, __ATOMIC_ACQUIRE))
        return;
    if (!__atomic_load_n(&g_clockevent.active, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_clockevent.early_attempts, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    if (slot != g_clockevent.bsp_slot) {
        __atomic_add_fetch(&g_clockevent.non_bsp_attempts, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    uint64_t previous = __atomic_load_n(&g_clockevent.last_tick_ns,
                                        __ATOMIC_RELAXED);
    if (!now_ns || (previous && now_ns < previous)) {
        __atomic_add_fetch(&g_clockevent.timestamp_regressions, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&g_clockevent.last_tick_ns, now_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_clockevent.total_ticks, 1, __ATOMIC_RELAXED);

    uint64_t now_ms = now_ns / 1000000u;
    timers_poll_at(now_ms);
    __atomic_add_fetch(&g_clockevent.software_timer_polls, 1,
                       __ATOMIC_RELAXED);
    if (irq_bootstrap_services_are_ready()) {
        timer_run_deferred_at(now_ms);
        __atomic_add_fetch(&g_clockevent.deferred_polls, 1,
                           __ATOMIC_RELAXED);
    }
}

bool timer_clockevent_snapshot(timer_clockevent_snapshot_t *out)
{
    if (!out)
        return false;
    *out = (timer_clockevent_snapshot_t){
        .source = g_clockevent.source,
        .bsp_slot = g_clockevent.bsp_slot,
        .period_us = g_clockevent.period_us,
        .configured = __atomic_load_n(&g_clockevent.configured,
                                      __ATOMIC_ACQUIRE),
        .active = __atomic_load_n(&g_clockevent.active, __ATOMIC_ACQUIRE),
        .total_ticks = __atomic_load_n(&g_clockevent.total_ticks,
                                       __ATOMIC_RELAXED),
        .early_attempts = __atomic_load_n(&g_clockevent.early_attempts,
                                          __ATOMIC_RELAXED),
        .non_bsp_attempts = __atomic_load_n(&g_clockevent.non_bsp_attempts,
                                            __ATOMIC_RELAXED),
        .timestamp_regressions = __atomic_load_n(
            &g_clockevent.timestamp_regressions, __ATOMIC_RELAXED),
        .last_tick_ns = __atomic_load_n(&g_clockevent.last_tick_ns,
                                        __ATOMIC_RELAXED),
        .software_timer_polls = __atomic_load_n(
            &g_clockevent.software_timer_polls, __ATOMIC_RELAXED),
        .deferred_polls = __atomic_load_n(&g_clockevent.deferred_polls,
                                          __ATOMIC_RELAXED),
    };
    return true;
}

bool timer_clockevent_validate(void)
{
    timer_clockevent_snapshot_t event;
    hpet_runtime_snapshot_t hpet;
    return timer_clockevent_snapshot(&event) && hpet_runtime_snapshot(&hpet) &&
           event.configured && event.active &&
           event.source == TIMER_CLOCKEVENT_BSP_LAPIC &&
           event.bsp_slot == smp_bsp_cpu_slot() && event.period_us == 1000u &&
           event.total_ticks && !event.early_attempts &&
           !event.non_bsp_attempts && !event.timestamp_regressions &&
           hpet.main_counter_enabled && !hpet.legacy_replacement_enabled &&
           !hpet.timer0_interrupt_enabled && !hpet.timer0_periodic_enabled &&
           !hpet.timer0_fsb_enabled && !hpet.timer0_route_enabled &&
           !hpet.timer0_pending && !hpet.stray_irqs;
}

static bool timer_clockevent_model_configure(timer_clockevent_snapshot_t *event,
                                             cpu_slot_t bsp_slot,
                                             uint32_t period_us)
{
    if (!event || event->configured || bsp_slot == CPU_SLOT_INVALID ||
        period_us != 1000u)
        return false;
    event->source = TIMER_CLOCKEVENT_BSP_LAPIC;
    event->bsp_slot = bsp_slot;
    event->period_us = period_us;
    event->configured = 1;
    return true;
}

static bool timer_clockevent_model_activate(timer_clockevent_snapshot_t *event,
                                            bool prerequisites)
{
    if (!event || !prerequisites || !event->configured || event->active ||
        event->source != TIMER_CLOCKEVENT_BSP_LAPIC)
        return false;
    event->active = 1;
    return true;
}

static void timer_clockevent_model_tick(timer_clockevent_snapshot_t *event,
                                        cpu_slot_t slot, uint64_t now_ns)
{
    if (!event || !event->configured)
        return;
    if (!event->active) {
        event->early_attempts++;
        return;
    }
    if (slot != event->bsp_slot) {
        event->non_bsp_attempts++;
        return;
    }
    if (!now_ns || (event->last_tick_ns && now_ns < event->last_tick_ns)) {
        event->timestamp_regressions++;
        return;
    }
    event->last_tick_ns = now_ns;
    event->total_ticks++;
}

bool timer_clockevent_model_selftest(void)
{
    timer_clockevent_snapshot_t event = {
        .source = TIMER_CLOCKEVENT_NONE,
        .bsp_slot = CPU_SLOT_INVALID,
    };
    bool wrong_period_rejected =
        !timer_clockevent_model_configure(&event, 0, 999u);
    bool configured = timer_clockevent_model_configure(&event, 0, 1000u);
    bool duplicate_rejected =
        !timer_clockevent_model_configure(&event, 0, 1000u);
    bool prerequisite_rejected =
        !timer_clockevent_model_activate(&event, false);
    timer_clockevent_snapshot_t early_probe = event;
    timer_clockevent_model_tick(&early_probe, 0, 1000u);
    bool early_rejected = early_probe.early_attempts == 1;
    bool setup_ok = wrong_period_rejected && configured &&
                    duplicate_rejected && prerequisite_rejected &&
                    early_rejected;

#ifdef HOBBYOS_TIMER_NEGATIVE_HPET_IRQ_BOOT
    bool hpet_irq_boot_dependency = true;
    bool detected = setup_ok && hpet_irq_boot_dependency;
    serial_write_all(detected
        ? "[TIMER][NEGATIVE] HPET_IRQ_BOOT_DEPENDENCY_DETECTED\n"
        : "[TIMER][NEGATIVE] HPET_IRQ_BOOT_DEPENDENCY_MISSED\n");
    return false;
#elif defined(HOBBYOS_TIMER_NEGATIVE_EARLY_CLOCKEVENT)
    serial_write_all(setup_ok
        ? "[TIMER][NEGATIVE] CLOCKEVENT_BEFORE_ACTIVATION_DETECTED\n"
        : "[TIMER][NEGATIVE] CLOCKEVENT_BEFORE_ACTIVATION_MISSED\n");
    return false;
#elif defined(HOBBYOS_TIMER_NEGATIVE_DUPLICATE_CLOCKEVENT)
    serial_write_all(setup_ok && duplicate_rejected
        ? "[TIMER][NEGATIVE] DUPLICATE_GLOBAL_CLOCKEVENT_DETECTED\n"
        : "[TIMER][NEGATIVE] DUPLICATE_GLOBAL_CLOCKEVENT_MISSED\n");
    return false;
#else
    bool activated = timer_clockevent_model_activate(&event, true);
#ifdef HOBBYOS_TIMER_NEGATIVE_GLOBAL_TICK_ON_AP
    timer_clockevent_model_tick(&event, 1, 1000u);
    serial_write_all(setup_ok && activated && event.non_bsp_attempts == 1
        ? "[TIMER][NEGATIVE] NON_BSP_GLOBAL_TICK_DETECTED\n"
        : "[TIMER][NEGATIVE] NON_BSP_GLOBAL_TICK_MISSED\n");
    return false;
#else
    timer_clockevent_model_tick(&event, 0, 1000u);
    timer_clockevent_model_tick(&event, 0, 2000u);
    timer_clockevent_model_tick(&event, 1, 3000u);
    timer_clockevent_model_tick(&event, 0, 1500u);
    timer_clockevent_model_tick(&event, 0, 0);
    bool ok = setup_ok && activated && event.total_ticks == 2 &&
              event.non_bsp_attempts == 1 &&
              event.timestamp_regressions == 2 && !event.early_attempts;
    serial_write_all(ok
        ? "[TIMER][SELFTEST] CLOCKEVENT_MODEL_OK\n"
        : "[TIMER][SELFTEST] CLOCKEVENT_MODEL_FAIL\n");
    return ok;
#endif
#endif
}

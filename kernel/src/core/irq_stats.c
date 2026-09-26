#include "irq_stats.h"
#include "../graphics/console.h"
#include "idt.h"
#include "interrupts.h"
#include "../smp/smp_topology.h"

typedef struct __attribute__((aligned(64)))
{
    volatile uint64_t vec_counts[256];
    volatile uint64_t unhandled;
    volatile uint64_t total;
} irq_cpu_stats_t;

/* The final row records interrupts observed before the CPU topology can map
   the current APIC ID.  Runtime IRQs use one cache-private row per CPU, so a
   1 ms LAPIC tick never queues every vCPU behind one global lock. */
static irq_cpu_stats_t g_cpu_stats[HOBBYOS_MAX_CPUS + 1u];
static volatile uint8_t g_last_vec;

static irq_cpu_stats_t *irq_stats_current(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
        slot = HOBBYOS_MAX_CPUS;
    return &g_cpu_stats[slot];
}

static uint64_t irq_stats_sum_vector(uint8_t vector)
{
    uint64_t total = 0;
    for (uint32_t slot = 0; slot <= HOBBYOS_MAX_CPUS; slot++)
        total += __atomic_load_n(&g_cpu_stats[slot].vec_counts[vector],
                                 __ATOMIC_RELAXED);
    return total;
}

static uint64_t irq_stats_sum_unhandled(void)
{
    uint64_t total = 0;
    for (uint32_t slot = 0; slot <= HOBBYOS_MAX_CPUS; slot++)
        total += __atomic_load_n(&g_cpu_stats[slot].unhandled,
                                 __ATOMIC_RELAXED);
    return total;
}

static uint64_t irq_stats_sum_total(void)
{
    uint64_t total = 0;
    for (uint32_t slot = 0; slot <= HOBBYOS_MAX_CPUS; slot++)
        total += __atomic_load_n(&g_cpu_stats[slot].total,
                                 __ATOMIC_RELAXED);
    return total;
}

void irq_stats_record(uint8_t vector)
{
    irq_cpu_stats_t *stats = irq_stats_current();
    __atomic_add_fetch(&stats->total, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stats->vec_counts[vector], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_vec, vector, __ATOMIC_RELAXED);
}

void irq_stats_record_unhandled(void)
{
    irq_cpu_stats_t *stats = irq_stats_current();
    __atomic_add_fetch(&stats->unhandled, 1, __ATOMIC_RELAXED);
}

void irq_stats_reset(void)
{
    irq_flags_t flags = irq_save();

    for (uint32_t slot = 0; slot <= HOBBYOS_MAX_CPUS; slot++)
    {
        for (uint32_t vector = 0; vector < 256u; vector++)
            __atomic_store_n(&g_cpu_stats[slot].vec_counts[vector], 0,
                             __ATOMIC_RELAXED);
        __atomic_store_n(&g_cpu_stats[slot].unhandled, 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_cpu_stats[slot].total, 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_last_vec, 0, __ATOMIC_RELAXED);

    irq_restore(flags);
}

uint64_t irq_stats_get(uint8_t vector) { return irq_stats_sum_vector(vector); }
uint64_t irq_stats_get_unhandled(void) { return irq_stats_sum_unhandled(); }
uint8_t irq_stats_last_vector(void)
{
    return __atomic_load_n(&g_last_vec, __ATOMIC_RELAXED);
}

void irq_stats_dump(void)
{
    uint64_t total;
    uint64_t unhandled;
    uint8_t last_vec;

    uint64_t timer_cnt;
    uint64_t kbd_cnt;
    uint64_t xhci_cnt;
    uint64_t spurious_cnt;

    total = irq_stats_sum_total();
    unhandled = irq_stats_sum_unhandled();
    last_vec = irq_stats_last_vector();

    timer_cnt = irq_stats_sum_vector(INT_VECTOR_HPET_TIMER) +
                irq_stats_sum_vector(INT_VECTOR_LAPIC_TIMER);
    kbd_cnt = irq_stats_sum_vector(INT_VECTOR_KEYBOARD);
    xhci_cnt = irq_stats_sum_vector(INT_VECTOR_XHCI);
    spurious_cnt = irq_stats_sum_vector(0xFF);

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("IRQ / INT stats\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    console_write("Total IRQs: ");
    console_print_dec((uint64_t)total);
    console_write("\n");

    console_write("Last vector: 0x");
    console_print_hex((uint64_t)last_vec);
    console_write("\n");

    console_write("Timer    (0x");
    console_print_hex((uint64_t)INT_VECTOR_HPET_TIMER);
    console_write("): ");
    console_print_dec((uint64_t)timer_cnt);
    console_write("\n");

    console_write("Keyboard (0x");
    console_print_hex((uint64_t)INT_VECTOR_KEYBOARD);
    console_write("): ");
    console_print_dec((uint64_t)kbd_cnt);
    console_write("\n");

    console_write("XHCI     (0x");
    console_print_hex((uint64_t)INT_VECTOR_XHCI);
    console_write("): ");
    console_print_dec((uint64_t)xhci_cnt);
    console_write("\n");

    console_write("Spurious (0xFF): ");
    console_print_dec((uint64_t)spurious_cnt);
    console_write("\n");

    console_write("Unhandled vectors (unknown): ");
    console_print_dec((uint64_t)unhandled);
    console_write("\n");
}

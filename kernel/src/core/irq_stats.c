#include "irq_stats.h"

#include "../graphics/console.h"
#include "idt.h"

static volatile uint64_t g_vec_counts[256];
static volatile uint64_t g_unhandled;
static volatile uint64_t g_total;
static volatile uint8_t g_last_vec;

void irq_stats_record(uint8_t vector)
{
    g_total++;
    g_last_vec = vector;
    g_vec_counts[vector]++;
}

void irq_stats_record_unhandled(void)
{
    g_total++;
    g_last_vec = 0xFE;
    g_unhandled++;
}

void irq_stats_reset(void)
{
    for (int i = 0; i < 256; i++)
        g_vec_counts[i] = 0;
    g_unhandled = 0;
    g_total = 0;
    g_last_vec = 0;
}

uint64_t irq_stats_get(uint8_t vector) { return g_vec_counts[vector]; }
uint64_t irq_stats_get_unhandled(void) { return g_unhandled; }
uint8_t irq_stats_last_vector(void) { return g_last_vec; }

void irq_stats_dump(void)
{
    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("IRQ / INT stats\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    console_write("Total IRQs: ");
    console_print_dec((uint64_t)g_total);
    console_write("\n");
    console_write("Last vector: 0x");
    console_print_hex((uint64_t)g_last_vec);
    console_write("\n");

    console_write("Timer    (0x");
    console_print_hex((uint64_t)INT_VECTOR_TIMER);
    console_write("): ");
    console_print_dec((uint64_t)g_vec_counts[INT_VECTOR_TIMER]);
    console_write("\n");

    console_write("Keyboard (0x");
    console_print_hex((uint64_t)INT_VECTOR_KEYBOARD);
    console_write("): ");
    console_print_dec((uint64_t)g_vec_counts[INT_VECTOR_KEYBOARD]);
    console_write("\n");

    console_write("XHCI     (0x");
    console_print_hex((uint64_t)INT_VECTOR_XHCI);
    console_write("): ");
    console_print_dec((uint64_t)g_vec_counts[INT_VECTOR_XHCI]);
    console_write("\n");

    console_write("Spurious (0xFF): ");
    console_print_dec((uint64_t)g_vec_counts[0xFF]);
    console_write("\n");

    console_write("Unhandled vectors (unknown): ");
    console_print_dec((uint64_t)g_unhandled);
    console_write("\n");
}

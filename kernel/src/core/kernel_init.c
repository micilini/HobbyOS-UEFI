#include "kernel_init.h"
#include "../memory/gdt.h"
#include "idt.h"
#include "../graphics/console.h"
#include "../memory/pmem.h"
#include "../memory/paging.h"
#include "../memory/heap.h"
#include "../acpi/acpi.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../apic/ioapic.h"
#include "../timer/hpet.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/pci.h"
#include "../drivers/watchdog/intel_tco.h"
#include "../drivers/watchdog/acpi_wdat.h"
#include "../drivers/watchdog/acpi_wddt.h"
#include "../drivers/watchdog/acpi_wdrt.h"
#include "panic.h"
#include "timers.h"

#include "list.h"
#include "queue.h"

static void busy_hlt_delay(uint64_t loops)
{
    for (uint64_t i = 0; i < loops; i++)
    {
        __asm__ volatile("hlt");
    }
}

static void delay_seconds_for_reading(uint32_t seconds)
{

    for (uint32_t s = 0; s < seconds; s++)
    {
        for (uint64_t i = 0; i < 250000ULL; i++)
        {
            __asm__ volatile("hlt");
        }
    }
}

typedef struct list_test_item
{
    uint32_t id;
    struct list_head node;
} list_test_item_t;

static void list_queue_selftest(void)
{
    console_write_debug("\n[LIST] self-test begin\n");

    typedef struct test_node
    {
        uint32_t value;
        struct list_head link;
    } test_node_t;

    // --- LIST TEST ---
    struct list_head head;
    list_init(&head);

    static test_node_t a = {1, {0}};
    static test_node_t b = {2, {0}};
    static test_node_t c = {3, {0}};

    list_add_tail(&a.link, &head);
    list_add_tail(&b.link, &head);
    list_add_tail(&c.link, &head);

    console_write_debug("[LIST] walk: ");
    struct list_head *pos;
    list_for_each(pos, &head)
    {
        test_node_t *n = list_entry(pos, test_node_t, link);
        console_print_dec_debug((uint64_t)n->value);
        console_write_debug(" ");
    }
    console_write_debug("\n");

    // remove middle (b)
    list_del(&b.link);

    console_write_debug("[LIST] after del(b): ");
    list_for_each(pos, &head)
    {
        test_node_t *n = list_entry(pos, test_node_t, link);
        console_print_dec_debug((uint64_t)n->value);
        console_write_debug(" ");
    }
    console_write_debug("\n");

    // --- QUEUE TEST ---
    queue_head_t q;
    queue_init(&q);

    queue_push(&q, &a.link);
    queue_push(&q, &b.link);
    queue_push(&q, &c.link);

    console_write_debug("[QUEUE] pop order: ");
    while (!queue_empty(&q))
    {
        struct list_head *ln = queue_pop(&q);
        test_node_t *n = list_entry(ln, test_node_t, link);
        console_print_dec_debug((uint64_t)n->value);
        console_write_debug(" ");
    }
    console_write_debug("\n");

    console_write_debug("[LIST] self-test end\n\n");
}

// Callback de teste para validar a FASE 1
static void timer_selftest_cb(void *ctx)
{
    uint64_t id = (uint64_t)ctx;
    console_write_debug("[TIMER_TEST] Callback executed! ID=");
    console_print_dec_debug(id);
    console_write_debug(" Tick=");
    console_print_dec_debug(timer_get_uptime_ms());
    console_write_debug("\n");
}

void init_system_core(BootInfo *boot_info)
{

    init_gdt();
    init_idt();

    init_pmm(boot_info->memory_map);

    init_paging((uint64_t)boot_info->framebuffer->BaseAddress,
                (uint64_t)boot_info->framebuffer->BufferSize);

    init_heap();

    console_init(boot_info);
    console_clear(CONSOLE_COLOR_BLACK);

    console_set_debug_enabled(1);

    void *test_heap = kmalloc(16);
    if (!test_heap)
        kpanic("CRITICAL: Heap Failed.");
    kfree(test_heap);

    // Self-test da FASE 3 (lista intrusiva / queue intrusiva)
    list_queue_selftest();

    init_acpi(boot_info->rsdp);

    acpi_list_tables_debug();
    acpi_enable_mode();

    intel_tco_disable();

    acpi_wdat_disable();
    acpi_wddt_disable();
    acpi_wdrt_disable();

    init_madt();

    init_hpet();
    init_lapic();
    init_ioapic();

    keyboard_init();
    timer_init();
    timers_init();

    // Agendar testes (100ms, 200ms, 300ms)
    console_write_debug("[INIT] Scheduling generic timers...\n");
    timers_add(100, timer_selftest_cb, (void*)100);
    timers_add(200, timer_selftest_cb, (void*)200);
    timers_add(300, timer_selftest_cb, (void*)300);

    ioapic_map_irq(1, 33, 0);

    irq_enable();

    console_begin_batch();

    pci_init();

    console_end_batch();
}

#include "cmd_mem.h"

#include "../../graphics/console.h"
#include "../../memory/pmem.h"
#include "../../memory/heap.h"

static void print_bytes_mib(uint64_t bytes)
{

    console_print_dec(bytes);
    console_write(" bytes (");

    const uint64_t mib_div = 1024ULL * 1024ULL;
    uint64_t mib = bytes / mib_div;
    uint64_t rem = bytes % mib_div;

    uint64_t dec2 = (rem * 100ULL) / mib_div;

    console_print_dec(mib);
    console_write(".");
    if (dec2 < 10)
        console_write("0");
    console_print_dec(dec2);
    console_write(" MiB)");
}

static void print_pages(uint64_t bytes)
{
    uint64_t pages = bytes / PAGE_SIZE;
    console_print_dec(pages);
    console_write(" pages");
}

int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Memory Statistics\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    uint64_t total = pmm_get_total_memory();
    uint64_t free = pmm_get_free_memory();
    uint64_t used = (total >= free) ? (total - free) : 0;

    console_write("\n[PMM]\n");
    console_write("  Total: ");
    print_bytes_mib(total);
    console_write(" | ");
    print_pages(total);
    console_write("\n");
    console_write("  Free : ");
    print_bytes_mib(free);
    console_write(" | ");
    print_pages(free);
    console_write("\n");
    console_write("  Used : ");
    print_bytes_mib(used);
    console_write(" | ");
    print_pages(used);
    console_write("\n");

    HeapStats hs;
    if (heap_get_stats(&hs))
    {
        console_write("\n[HEAP]\n");
        console_write("  Total: ");
        print_bytes_mib(hs.total_bytes);
        console_write("\n");
        console_write("  Used : ");
        print_bytes_mib(hs.used_bytes);
        console_write("\n");
        console_write("  Free : ");
        print_bytes_mib(hs.free_bytes);
        console_write("\n");

        console_write("  Blocks total: ");
        console_print_dec(hs.blocks_total);
        console_write(" | free: ");
        console_print_dec(hs.blocks_free);
        console_write("\n");

        console_write("  Largest free block: ");
        print_bytes_mib(hs.largest_free_bytes);
        console_write("\n");
    }
    else
    {
        console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("\n[HEAP] heap_get_stats() not available.\n");
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
    }

    console_write("\n");
    return 0;
}
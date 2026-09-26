#include "cmd_mem.h"

#include "../../drivers/serial.h"
#include "../../graphics/console.h"
#include "../../libc/string.h"
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

    HeapStats hs = {0};
    bool heap_available = heap_get_stats(&hs);
    if (heap_available)
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

    bool pmm_ok = total != 0 && free <= total && used == total - free &&
                  pmm_validate_integrity();
    bool heap_ok = heap_available && hs.total_bytes != 0 &&
                   hs.used_bytes <= hs.total_bytes &&
                   hs.free_bytes == hs.total_bytes - hs.used_bytes &&
                   hs.blocks_free <= hs.blocks_total &&
                   hs.largest_free_bytes <= hs.free_bytes &&
                   heap_validate_integrity();
    bool ok = pmm_ok && heap_ok;
    char record[512];
    int required = ksnprintf(
        record, sizeof(record),
        "[MEM][SUMMARY] %s pmm_total=%llu pmm_free=%llu pmm_used=%llu "
        "heap_total=%llu heap_used=%llu heap_free=%llu heap_blocks=%llu "
        "heap_free_blocks=%llu heap_largest_free=%llu pmm_integrity=%u "
        "heap_integrity=%u\n",
        ok ? "PASS" : "FAIL", (unsigned long long)total,
        (unsigned long long)free, (unsigned long long)used,
        (unsigned long long)hs.total_bytes,
        (unsigned long long)hs.used_bytes,
        (unsigned long long)hs.free_bytes,
        (unsigned long long)hs.blocks_total,
        (unsigned long long)hs.blocks_free,
        (unsigned long long)hs.largest_free_bytes,
        pmm_ok ? 1u : 0u, heap_ok ? 1u : 0u);
    if (required < 0 || (size_t)required >= sizeof(record))
    {
        serial_write_all("[MEM][SUMMARY] FAIL reason=format\n");
        return 1;
    }
    serial_write_all(record);
    return ok ? 0 : 1;
}

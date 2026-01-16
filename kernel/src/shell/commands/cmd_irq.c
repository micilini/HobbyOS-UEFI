#include "cmd_irq.h"

#include "../../graphics/console.h"
#include "../../libc/string.h"
#include "../../core/irq_stats.h"

static void irq_usage(void)
{
    console_write("Usage:\n");
    console_write("  irq            - shows simple IRQ statistics\n");
    console_write("  irq reset      - reset counters\n");
    console_write("Alias:\n");
    console_write("  int            - same as irq\n");
}

int cmd_irq(int argc, char **argv)
{
    if (argc >= 2)
    {
        if (strcmp(argv[1], "reset") == 0)
        {
            irq_stats_reset();
            console_write("[IRQ] Counters reset.\n");
            return 0;
        }
        if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "?") == 0)
        {
            irq_usage();
            return 0;
        }
    }

    irq_stats_dump();
    return 0;
}
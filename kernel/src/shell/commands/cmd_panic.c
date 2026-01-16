#include "cmd_panic.h"

#include "../../core/panic.h"
#include "../../graphics/console.h"

static void append_limited(char *dst, int dst_size, const char *src)
{
    if (!dst || dst_size <= 0 || !src)
        return;

    int i = 0;
    while (i < dst_size - 1 && dst[i])
        i++;

    int j = 0;
    while (i < dst_size - 1 && src[j])
    {
        dst[i++] = src[j++];
    }
    dst[i] = 0;
}

int cmd_panic(int argc, char **argv)
{

    char msg[160];
    msg[0] = 0;

    if (argc >= 2)
    {
        for (int i = 1; i < argc; i++)
        {
            if (i > 1)
                append_limited(msg, (int)sizeof(msg), " ");
            append_limited(msg, (int)sizeof(msg), argv[i]);
        }
    }
    else
    {
        append_limited(msg, (int)sizeof(msg), "panic command invoked (test)");
    }

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Triggering kernel panic...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    kpanic(msg);

    return 0;
}
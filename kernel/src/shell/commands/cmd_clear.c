#include "cmd_clear.h"

#include "../../graphics/console.h"

int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_clear(CONSOLE_COLOR_HOBBYOS_BLUE);
    return 0;
}
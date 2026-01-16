#include "cmd_version.h"

#include "../../graphics/console.h"

int cmd_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("HobbyOS 0.2\n");
    return 0;
}
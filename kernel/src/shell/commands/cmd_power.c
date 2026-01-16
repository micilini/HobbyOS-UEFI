#include "cmd_power.h"

#include "../../graphics/console.h"
#include "../../power/power.h"

int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Shutting down...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    power_shutdown();
    return 0;
}

int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Restarting...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    power_restart();
    return 0;
}
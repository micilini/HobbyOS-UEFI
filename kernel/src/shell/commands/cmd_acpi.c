#include "cmd_acpi.h"

#include "../../graphics/console.h"
#include "../../acpi/acpi.h"

int cmd_acpi(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("ACPI - Detected Tables\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    acpi_list_tables();
    return 0;
}
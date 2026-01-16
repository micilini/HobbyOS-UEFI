#include "cmd_pci.h"

#include "../../graphics/console.h"
#include "../../drivers/pci.h"

int cmd_pci(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("PCI - Devices\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    pci_list_devices();
    return 0;
}
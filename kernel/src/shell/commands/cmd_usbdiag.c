#include "cmd_usbdiag.h"

extern void xhci_diag_latency(void);

int cmd_usbdiag(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    xhci_diag_latency();
    return 0;
}
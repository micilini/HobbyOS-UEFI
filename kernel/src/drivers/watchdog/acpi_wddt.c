#include "acpi_wddt.h"

#include "../../acpi/acpi.h"
#include "../../graphics/console.h"

#pragma pack(push, 1)
typedef struct
{
    AcpiSdtHeader header;

    uint8_t data[64];
} AcpiWddtBlob;
#pragma pack(pop)

int acpi_wddt_disable(void)
{
    const void *tbl = acpi_find_table("WDDT");
    if (!tbl)
    {
        console_write_debug("[WATCHDOG] ACPI WDDT: not present.\n");
        return 0;
    }

    const AcpiWddtBlob *wddt = (const AcpiWddtBlob *)tbl;

    console_write_debug("[WATCHDOG] ACPI WDDT: present at 0x");
    console_print_hex((uint64_t)wddt);
    console_write_debug(" len=");
    console_print_dec((uint64_t)wddt->header.length);
    console_write_debug(" rev=");
    console_print_dec((uint64_t)wddt->header.revision);
    console_write_debug("\n");

    console_write_debug("[WATCHDOG] ACPI WDDT: detected (no HW write performed).\n");

    return 1;
}

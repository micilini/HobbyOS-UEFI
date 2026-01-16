#include "acpi_wdrt.h"

#include "../../acpi/acpi.h"
#include "../../core/io.h"
#include "../../graphics/console.h"

static uint32_t gas_read_u32(const AcpiGas *gas)
{
    if (!gas)
        return 0;

    if (gas->address_space_id != 1)
        return 0;

    uint16_t port = (uint16_t)(gas->address);

    uint8_t w = gas->register_bit_width ? gas->register_bit_width : 32;

    if (w <= 8)
        return (uint32_t)inb(port);
    if (w <= 16)
        return (uint32_t)inw(port);
    return inl(port);
}

static void gas_write_u32(const AcpiGas *gas, uint32_t value)
{
    if (!gas)
        return;

    if (gas->address_space_id != 1)
        return;

    uint16_t port = (uint16_t)(gas->address);
    uint8_t w = gas->register_bit_width ? gas->register_bit_width : 32;

    if (w <= 8)
    {
        outb(port, (uint8_t)value);
        return;
    }
    if (w <= 16)
    {
        outw(port, (uint16_t)value);
        return;
    }
    outl(port, value);
}

#pragma pack(push, 1)
typedef struct
{
    AcpiSdtHeader header;

    uint32_t timeout_value;
    uint16_t max_count;
    uint16_t min_count;
    uint16_t flags;
    uint16_t reserved;

    AcpiGas control_register;
    AcpiGas count_register;
} AcpiWdrtTable;
#pragma pack(pop)

int acpi_wdrt_disable(void)
{
    const void *tbl = acpi_find_table("WDRT");
    if (!tbl)
    {
        console_write_debug("[WATCHDOG] ACPI WDRT: not present.\n");
        return 0;
    }

    const AcpiWdrtTable *wdrt = (const AcpiWdrtTable *)tbl;

    console_write_debug("[WATCHDOG] ACPI WDRT: present at 0x");
    console_print_hex((uint64_t)wdrt);
    console_write_debug(" len=");
    console_print_dec((uint64_t)wdrt->header.length);
    console_write_debug(" rev=");
    console_print_dec((uint64_t)wdrt->header.revision);
    console_write_debug("\n");

    if (wdrt->control_register.address_space_id != 1)
    {
        console_write_debug("[WATCHDOG] ACPI WDRT: control_register not System I/O; skip.\n");
        return 1;
    }

    console_write_debug("[WATCHDOG] ACPI WDRT: control GAS port=0x");
    console_print_hex((uint64_t)wdrt->control_register.address);
    console_write_debug(" width=");
    console_print_dec((uint64_t)(wdrt->control_register.register_bit_width ? wdrt->control_register.register_bit_width : 32));
    console_write_debug("\n");

    uint32_t ctrl = gas_read_u32(&wdrt->control_register);
    uint32_t new_ctrl = (ctrl & ~1u);

    gas_write_u32(&wdrt->control_register, new_ctrl);

    console_write_debug("[WATCHDOG] ACPI WDRT: ctrl old=0x");
    console_print_hex((uint64_t)ctrl);
    console_write_debug(" new=0x");
    console_print_hex((uint64_t)new_ctrl);
    console_write_debug("\n");

    console_write_debug("[WATCHDOG] ACPI WDRT: disable attempt done.\n");
    return 1;
}
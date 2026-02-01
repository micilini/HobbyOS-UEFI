#include "power.h"
#include "../acpi/sleep.h"
#include "../acpi/acpi.h"
#include "../core/io.h"
#include "../core/idt.h"
#include "../graphics/console.h"

#include <stdint.h>
#include <stdbool.h>

#define ACPI_PM1_SLP_TYP_SHIFT 10
#define ACPI_PM1_SLP_TYP_MASK (7u << ACPI_PM1_SLP_TYP_SHIFT)
#define ACPI_PM1_SLP_EN (1u << 13)

#define ACPI_SLP_TYP_S5 7u

typedef struct
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) Idtr;

static uint16_t gas_io_port(const AcpiGas *gas)
{
    if (!gas)
        return 0;
    if (gas->address_space_id != 1)
        return 0;
    if (gas->address == 0)
        return 0;
    return (uint16_t)gas->address;
}

static void gas_write(const AcpiGas *gas, uint64_t value)
{
    if (!gas)
        return;

    if (gas->address_space_id != 1)
    {

        return;
    }

    uint16_t port = (uint16_t)gas->address;
    if (!port)
        return;

    uint8_t sz = gas->access_size;
    if (sz == 0)
    {
        if (gas->register_bit_width <= 8)
            sz = 1;
        else if (gas->register_bit_width <= 16)
            sz = 2;
        else if (gas->register_bit_width <= 32)
            sz = 3;
        else
            sz = 4;
    }

    switch (sz)
    {
    case 1:
        outb(port, (uint8_t)value);
        break;
    case 2:
        outw(port, (uint16_t)value);
        break;
    case 3:
        outl(port, (uint32_t)value);
        break;
    case 4:

        outl(port, (uint32_t)value);
        break;
    default:
        outb(port, (uint8_t)value);
        break;
    }
}

static void power_halt_forever(void)
{
    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[POWER] CPU halted.\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    disable_interrupts();

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

static bool power_try_acpi_shutdown(void)
{
    const AcpiFadt *f = acpi_get_fadt();
    if (!f)
        return false;

    acpi_enable_mode();

    uint16_t pm1a_cnt = 0;
    if (f->pm1a_cnt_blk)
        pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    if (!pm1a_cnt)
        pm1a_cnt = gas_io_port(&f->x_pm1a_cnt_blk);

    uint16_t pm1b_cnt = 0;
    if (f->pm1b_cnt_blk)
        pm1b_cnt = (uint16_t)f->pm1b_cnt_blk;
    if (!pm1b_cnt)
        pm1b_cnt = gas_io_port(&f->x_pm1b_cnt_blk);

    uint16_t pm1a_evt = 0;
    if (f->pm1a_evt_blk)
        pm1a_evt = (uint16_t)f->pm1a_evt_blk;
    if (!pm1a_evt)
        pm1a_evt = gas_io_port(&f->x_pm1a_evt_blk);

    uint16_t pm1b_evt = 0;
    if (f->pm1b_evt_blk)
        pm1b_evt = (uint16_t)f->pm1b_evt_blk;
    if (!pm1b_evt)
        pm1b_evt = gas_io_port(&f->x_pm1b_evt_blk);

    uint16_t gpe0 = 0;
    if (f->gpe0_blk)
        gpe0 = (uint16_t)f->gpe0_blk;
    if (!gpe0)
        gpe0 = gas_io_port(&f->x_gpe0_blk);

    uint16_t gpe1 = 0;
    if (f->gpe1_blk)
        gpe1 = (uint16_t)f->gpe1_blk;
    if (!gpe1)
        gpe1 = gas_io_port(&f->x_gpe1_blk);

    if (!pm1a_cnt && !pm1b_cnt)
        return false;

    uint8_t typa = 0, typb = 0;
    if (!acpi_get_s5_sleep_types(&typa, &typb))
    {
        console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("[POWER] DSDT _S5 not found; cannot determine SLP_TYP. Fallback halt.\n");
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
        return false;
    }

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[POWER] ACPI shutdown (S5) using DSDT _S5: TYPa=");
    console_print_hex(typa);
    console_write(" TYPb=");
    console_print_hex(typb);
    console_write("\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    if (pm1a_evt)
    {
        outw(pm1a_evt + 0, 0xFFFF);
        io_wait();
    }
    if (pm1b_evt)
    {
        outw(pm1b_evt + 0, 0xFFFF);
        io_wait();
    }

    if (gpe0 && f->gpe0_blk_len >= 2)
    {
        uint16_t status_len = (uint16_t)(f->gpe0_blk_len / 2);
        for (uint16_t off = 0; off < status_len; off += 2)
        {
            outw(gpe0 + off, 0xFFFF);
            io_wait();
        }
    }

    if (gpe1 && f->gpe1_blk_len >= 2)
    {
        uint16_t status_len = (uint16_t)(f->gpe1_blk_len / 2);
        for (uint16_t off = 0; off < status_len; off += 2)
        {
            outw(gpe1 + off, 0xFFFF);
            io_wait();
        }
    }

    uint16_t v;

    if (pm1a_cnt)
    {
        v = inw(pm1a_cnt);
        v &= ~ACPI_PM1_SLP_TYP_MASK;
        v |= (uint16_t)((uint16_t)typa << ACPI_PM1_SLP_TYP_SHIFT);
        v &= (uint16_t)~ACPI_PM1_SLP_EN;
        outw(pm1a_cnt, v);
        io_wait();
    }

    if (pm1b_cnt)
    {
        v = inw(pm1b_cnt);
        v &= ~ACPI_PM1_SLP_TYP_MASK;
        v |= (uint16_t)((uint16_t)typb << ACPI_PM1_SLP_TYP_SHIFT);
        v &= (uint16_t)~ACPI_PM1_SLP_EN;
        outw(pm1b_cnt, v);
        io_wait();
    }

    if (pm1a_cnt)
    {
        v = inw(pm1a_cnt);
        v |= (uint16_t)ACPI_PM1_SLP_EN;
        outw(pm1a_cnt, v);
        io_wait();
    }

    if (pm1b_cnt)
    {
        v = inw(pm1b_cnt);
        v |= (uint16_t)ACPI_PM1_SLP_EN;
        outw(pm1b_cnt, v);
        io_wait();
    }

    for (int i = 0; i < 400000; i++)
        io_wait();

    return true;
}

static bool power_try_acpi_reset(void)
{
    const AcpiFadt *f = acpi_get_fadt();
    if (!f)
        return false;

    if (f->reset_reg.address_space_id == 1 && f->reset_reg.address != 0)
    {
        console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("[POWER] ACPI reset_reg attempt...\n");
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

        gas_write(&f->reset_reg, f->reset_value);

        for (int i = 0; i < 200000; i++)
            io_wait();
        return true;
    }

    return false;
}

static void power_try_port_cf9_reset(void)
{

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[POWER] Port 0xCF9 reset attempt...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    outb(0xCF9, 0x06);
    io_wait();
    outb(0xCF9, 0x0E);
    io_wait();

    for (int i = 0; i < 200000; i++)
        io_wait();
}

static void power_try_8042_reset(void)
{

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[POWER] 8042 reset attempt...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    for (int i = 0; i < 100000; i++)
    {
        if ((inb(0x64) & 0x02) == 0)
            break;
        io_wait();
    }

    outb(0x64, 0xFE);
    io_wait();

    for (int i = 0; i < 200000; i++)
        io_wait();
}

static void power_triple_fault_reset(void)
{
    console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[POWER] Triple fault reset fallback...\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    Idtr idtr = {.limit = 0, .base = 0};

    disable_interrupts();

    __asm__ volatile(
        "lidt %0\n"
        "int $3\n"
        :
        : "m"(idtr));

    for (;;)
        __asm__ volatile("hlt");
}

void power_shutdown(void)
{

    (void)power_try_acpi_shutdown();

    power_halt_forever();
}

void power_restart(void)
{

    (void)power_try_acpi_reset();

    power_try_port_cf9_reset();
    power_try_8042_reset();
    power_triple_fault_reset();
}
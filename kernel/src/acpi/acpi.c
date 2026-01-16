#include "acpi.h"
#include "../core/io.h"
#include "../graphics/console.h"

#include <stddef.h>
#include <stdint.h>

static AcpiRsdp *g_rsdp = NULL;
static AcpiSdtHeader *g_xsdt = NULL;
static AcpiSdtHeader *g_rsdt = NULL;
static const AcpiFadt *g_fadt = 0;

void init_acpi(void *rsdp_address)
{
    g_rsdp = (AcpiRsdp *)rsdp_address;
    g_xsdt = NULL;
    g_rsdt = NULL;

    if (!g_rsdp)
        return;

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0)
    {
        g_xsdt = (AcpiSdtHeader *)((uint64_t)g_rsdp->xsdt_address);
    }
    else if (g_rsdp->rsdt_address != 0)
    {
        g_rsdt = (AcpiSdtHeader *)((uint64_t)g_rsdp->rsdt_address);
    }

    g_fadt = (const AcpiFadt *)acpi_find_table("FACP");
}

void *acpi_find_table(const char *signature)
{

    if (g_xsdt)
    {
        int entries = (g_xsdt->length - sizeof(AcpiSdtHeader)) / 8;
        uint64_t *pointers = (uint64_t *)((char *)g_xsdt + sizeof(AcpiSdtHeader));

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *header = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!header)
                continue;

            if (header->signature[0] == signature[0] &&
                header->signature[1] == signature[1] &&
                header->signature[2] == signature[2] &&
                header->signature[3] == signature[3])
            {
                return (void *)header;
            }
        }
        return NULL;
    }

    if (g_rsdt)
    {
        int entries = (g_rsdt->length - sizeof(AcpiSdtHeader)) / 4;
        uint32_t *pointers = (uint32_t *)((char *)g_rsdt + sizeof(AcpiSdtHeader));

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *header = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!header)
                continue;

            if (header->signature[0] == signature[0] &&
                header->signature[1] == signature[1] &&
                header->signature[2] == signature[2] &&
                header->signature[3] == signature[3])
            {
                return (void *)header;
            }
        }
    }

    return NULL;
}

void acpi_list_tables_debug(void)
{
    if (g_xsdt)
    {
        int entries = (g_xsdt->length - sizeof(AcpiSdtHeader)) / 8;
        uint64_t *pointers = (uint64_t *)((char *)g_xsdt + sizeof(AcpiSdtHeader));

        console_write_debug("[ACPI] Using XSDT. Tables: ");
        console_print_dec_debug((uint64_t)entries);
        console_write_debug("\n");

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *h = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!h)
                continue;

            console_write_debug("  [");
            console_print_dec_debug((uint64_t)i);
            console_write_debug("] ");
            console_put_char_debug(h->signature[0]);
            console_put_char_debug(h->signature[1]);
            console_put_char_debug(h->signature[2]);
            console_put_char_debug(h->signature[3]);
            console_write_debug(" addr=0x");
            console_print_hex_debug((uint64_t)(uintptr_t)h);
            console_write_debug(" len=");
            console_print_dec_debug((uint64_t)h->length);
            console_write_debug(" rev=");
            console_print_dec_debug((uint64_t)h->revision);
            console_write_debug("\n");
        }
        return;
    }

    if (g_rsdt)
    {
        int entries = (g_rsdt->length - sizeof(AcpiSdtHeader)) / 4;
        uint32_t *pointers = (uint32_t *)((char *)g_rsdt + sizeof(AcpiSdtHeader));

        console_write_debug("[ACPI] Using RSDT. Tables: ");
        console_print_dec_debug((uint64_t)entries);
        console_write_debug("\n");

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *h = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!h)
                continue;

            console_write_debug("  [");
            console_print_dec_debug((uint64_t)i);
            console_write_debug("] ");
            console_put_char_debug(h->signature[0]);
            console_put_char_debug(h->signature[1]);
            console_put_char_debug(h->signature[2]);
            console_put_char_debug(h->signature[3]);
            console_write_debug(" addr=0x");
            console_print_hex_debug((uint64_t)(uintptr_t)h);
            console_write_debug(" len=");
            console_print_dec_debug((uint64_t)h->length);
            console_write_debug(" rev=");
            console_print_dec_debug((uint64_t)h->revision);
            console_write_debug("\n");
        }
        return;
    }

    console_write_debug("[ACPI] No XSDT/RSDT present.\n");
}

void acpi_list_tables(void)
{
    if (g_xsdt)
    {
        int entries = (g_xsdt->length - sizeof(AcpiSdtHeader)) / 8;
        uint64_t *pointers = (uint64_t *)((char *)g_xsdt + sizeof(AcpiSdtHeader));

        console_write("[ACPI] Using XSDT. Tables: ");
        console_print_dec((uint64_t)entries);
        console_write("\n");

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *h = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!h)
                continue;

            console_write("  [");
            console_print_dec((uint64_t)i);
            console_write("] ");
            console_put_char(h->signature[0]);
            console_put_char(h->signature[1]);
            console_put_char(h->signature[2]);
            console_put_char(h->signature[3]);
            console_write(" addr=0x");
            console_print_hex((uint64_t)(uintptr_t)h);
            console_write(" len=");
            console_print_dec((uint64_t)h->length);
            console_write(" rev=");
            console_print_dec((uint64_t)h->revision);
            console_write("\n");
        }
        return;
    }

    if (g_rsdt)
    {
        int entries = (g_rsdt->length - sizeof(AcpiSdtHeader)) / 4;
        uint32_t *pointers = (uint32_t *)((char *)g_rsdt + sizeof(AcpiSdtHeader));

        console_write("[ACPI] Using RSDT. Tables: ");
        console_print_dec((uint64_t)entries);
        console_write("\n");

        for (int i = 0; i < entries; i++)
        {
            AcpiSdtHeader *h = (AcpiSdtHeader *)(uintptr_t)pointers[i];
            if (!h)
                continue;

            console_write("  [");
            console_print_dec((uint64_t)i);
            console_write("] ");
            console_put_char(h->signature[0]);
            console_put_char(h->signature[1]);
            console_put_char(h->signature[2]);
            console_put_char(h->signature[3]);
            console_write(" addr=0x");
            console_print_hex((uint64_t)(uintptr_t)h);
            console_write(" len=");
            console_print_dec((uint64_t)h->length);
            console_write(" rev=");
            console_print_dec((uint64_t)h->revision);
            console_write("\n");
        }
        return;
    }

    console_write("[ACPI] No XSDT/RSDT present.\n");
}

const AcpiFadt *acpi_get_fadt(void)
{
    if (!g_fadt)
    {
        g_fadt = (const AcpiFadt *)acpi_find_table("FACP");
    }
    return g_fadt;
}

const AcpiSdtHeader *acpi_get_dsdt(void)
{
    const AcpiFadt *f = acpi_get_fadt();
    if (!f)
        return NULL;

    uint64_t addr = 0;
    if (f->x_dsdt)
        addr = f->x_dsdt;
    else
        addr = (uint64_t)f->dsdt;

    if (!addr)
        return NULL;

    return (const AcpiSdtHeader *)(uintptr_t)addr;
}

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

uint16_t acpi_get_pmbase(void)
{
    const AcpiFadt *f = acpi_get_fadt();
    if (!f)
        return 0;

    uint16_t base = 0;

    if (f->pm1a_evt_blk)
    {
        base = (uint16_t)(f->pm1a_evt_blk & 0xFF80u);
        if (base)
            return base;
    }

    uint16_t x_evt = gas_io_port(&f->x_pm1a_evt_blk);
    if (x_evt)
    {
        base = (uint16_t)(x_evt & 0xFF80u);
        if (base)
            return base;
    }

    if (f->pm1a_cnt_blk)
    {
        base = (uint16_t)(f->pm1a_cnt_blk & 0xFF80u);
        if (base)
            return base;
    }

    uint16_t x_cnt = gas_io_port(&f->x_pm1a_cnt_blk);
    if (x_cnt)
    {
        base = (uint16_t)(x_cnt & 0xFF80u);
        if (base)
            return base;
    }

    return 0;
}

void acpi_enable_mode(void)
{
    const AcpiFadt *f = acpi_get_fadt();
    if (!f)
    {
        console_write_debug("[ACPI] FADT/FACP not found; cannot enable ACPI mode.\n");
        return;
    }

    if (f->smi_cmd == 0 || f->acpi_enable == 0)
    {
        console_write_debug("[ACPI] SMI_CMD or ACPI_ENABLE is 0; skipping ACPI enable.\n");
        return;
    }

    uint16_t pm1a_cnt = 0;

    if (f->pm1a_cnt_blk)
        pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    if (!pm1a_cnt)
        pm1a_cnt = gas_io_port(&f->x_pm1a_cnt_blk);

    console_write_debug("[ACPI] Enabling ACPI mode via SMI_CMD...\n");
    outb((uint16_t)f->smi_cmd, f->acpi_enable);

    if (pm1a_cnt)
    {
        for (int i = 0; i < 200000; i++)
        {
            uint16_t v = inw(pm1a_cnt);
            if (v & 1)
            {
                console_write_debug("[ACPI] SCI_EN=1 (ACPI mode enabled)\n");
                return;
            }
            io_wait();
        }
        console_write_debug("[ACPI] SCI_EN did not set (timeout)\n");
    }
    else
    {
        console_write_debug("[ACPI] PM1a_CNT not available; cannot verify SCI_EN\n");
    }
}

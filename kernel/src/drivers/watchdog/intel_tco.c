#include "intel_tco.h"
#include "../../graphics/console.h"
#include "../../core/io.h"
#include "../../acpi/acpi.h"
#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | ((uint32_t)(off & 0xFC));

    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static inline void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t value)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | ((uint32_t)(off & 0xFC));

    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

static inline uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFFu);
}

static inline uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_cfg_read32(bus, dev, func, off);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFFu);
}

static bool find_intel_lpc(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func)
{

    for (uint8_t dev = 0; dev < 32; dev++)
    {
        for (uint8_t func = 0; func < 8; func++)
        {
            uint16_t vendor = pci_cfg_read16(0, dev, func, 0x00);
            if (vendor == 0xFFFFu || vendor == 0x0000u)
                continue;
            if (vendor != 0x8086u)
                continue;

            uint8_t class_code = pci_cfg_read8(0, dev, func, 0x0B);
            uint8_t subclass = pci_cfg_read8(0, dev, func, 0x0A);

            if (class_code == 0x06 && subclass == 0x01)
            {
                *out_bus = 0;
                *out_dev = dev;
                *out_func = func;
                return true;
            }
        }
    }
    return false;
}

static void tco_clear_status(uint16_t tco_base)
{
    uint16_t sts1 = inw((uint16_t)(tco_base + 0x04));
    uint16_t sts2 = inw((uint16_t)(tco_base + 0x06));
    outw((uint16_t)(tco_base + 0x04), sts1);
    outw((uint16_t)(tco_base + 0x06), sts2);
}

#pragma pack(push, 1)
typedef struct
{
    AcpiSdtHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;

    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;

    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;

} AcpiFadtMin;
#pragma pack(pop)

static uint16_t derive_pmbase_from_fadt(void)
{
    AcpiFadtMin *fadt = (AcpiFadtMin *)acpi_find_table(ACPI_SIG_FADT);
    if (!fadt)
        return 0;

    uint16_t base = (uint16_t)(fadt->pm1a_evt_blk & 0xFF80u);
    if (base == 0)
    {

        base = (uint16_t)(fadt->pm1a_cnt_blk & 0xFF80u);
    }
    return base;
}

bool intel_tco_disable(void)
{
    uint8_t bus = 0, dev = 31, func = 0;

    uint16_t vendor_guess = pci_cfg_read16(bus, dev, func, 0x00);
    uint8_t class_guess = pci_cfg_read8(bus, dev, func, 0x0B);
    uint8_t sub_guess = pci_cfg_read8(bus, dev, func, 0x0A);

    if (!(vendor_guess == 0x8086u && class_guess == 0x06 && sub_guess == 0x01))
    {
        if (!find_intel_lpc(&bus, &dev, &func))
        {
            console_write_debug("[WATCHDOG] Intel TCO: ISA/LPC bridge not found on PCI bus 0.\n");
            return false;
        }
    }

    console_write_debug("[WATCHDOG] Intel TCO: bridge at 00:");
    console_print_hex_debug(dev);
    console_write_debug(".");
    console_print_hex_debug(func);
    console_write_debug("\n");

    uint32_t pmbase_raw = pci_cfg_read32(bus, dev, func, 0x40);
    uint32_t acpi_cntl = pci_cfg_read32(bus, dev, func, 0x44);

    console_write_debug("[WATCHDOG] Intel TCO: PMBASE_RAW=0x");
    console_print_hex_debug((uint64_t)pmbase_raw);
    console_write_debug(" ACPI_CNTL=0x");
    console_print_hex_debug((uint64_t)acpi_cntl);
    console_write_debug("\n");

    uint16_t pmbase = (uint16_t)(pmbase_raw & 0xFF80u);

    if (pmbase == 0x0000u || pmbase == 0xFF80u)
    {
        uint16_t acpi_base = acpi_get_pmbase();

        console_write_debug("[WATCHDOG] Intel TCO: PMBASE via ACPI(FADT)=0x");
        console_print_hex_debug((uint64_t)acpi_base);
        console_write_debug("\n");

        if (acpi_base)
        {
            pmbase = acpi_base;
        }
    }

    if (pmbase == 0x0000u || pmbase == 0xFF80u)
    {
        uint16_t derived = derive_pmbase_from_fadt();
        if (derived)
        {
            console_write_debug("[WATCHDOG] Intel TCO: PMBASE derived from FADT(legacy)=0x");
            console_print_hex_debug((uint64_t)derived);
            console_write_debug("\n");
            pmbase = derived;
        }
    }

    if (pmbase == 0x0000u || pmbase == 0xFF80u)
    {
        console_write_debug("[WATCHDOG] Intel TCO: PMBASE invalid; cannot disable.\n");
        return false;
    }

    if ((acpi_cntl & 1u) == 0)
    {
        pci_cfg_write32(bus, dev, func, 0x44, acpi_cntl | 1u);
    }
    if ((pmbase_raw & 1u) == 0)
    {
        pci_cfg_write32(bus, dev, func, 0x40, (uint32_t)pmbase | 1u);
    }

    uint16_t tco_base = (uint16_t)(pmbase + 0x60u);

    console_write_debug("[WATCHDOG] Intel TCO: PMBASE=0x");
    console_print_hex_debug((uint64_t)pmbase);
    console_write_debug(" TCOBASE=0x");
    console_print_hex_debug((uint64_t)tco_base);
    console_write_debug("\n");

    tco_clear_status(tco_base);
    outw((uint16_t)(tco_base + 0x00), 0x0000);

    uint16_t tco1_cnt = inw((uint16_t)(tco_base + 0x08));
    tco1_cnt |= (1u << 11);
    outw((uint16_t)(tco_base + 0x08), tco1_cnt);

    tco_clear_status(tco_base);

    console_write_debug("[WATCHDOG] Intel TCO: disabled (TCO_TMR_HLT set).\n");
    return true;
}
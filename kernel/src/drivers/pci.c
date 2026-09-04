#include "pci.h"
#include "pci_descriptors.h"
#include "../memory/paging.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../core/idt.h"
#include "../apic/lapic.h"
#include "../smp/smp_topology.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"

#define CONSOLE_COLOR_CYAN 0xFF00FFFF
#define CONSOLE_COLOR_DEBUG 0xFFAAAAAA
#define CONSOLE_COLOR_RED 0xFFFF0000
#define CONSOLE_COLOR_GREEN 0xFF00FF00

static pci_msi_snapshot_t g_xhci_msi;

static char *pci_progress_append_text(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *pci_progress_append_u64(char *out, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        *out++ = reverse[--count];
    return out;
}

static void pci_progress(const char *stage, const char *detail)
{
    timer_clockevent_snapshot_t event = {0};
    (void)timer_clockevent_snapshot(&event);
    char line[208];
    char *p = pci_progress_append_text(line, "[BOOT][PROGRESS] stage=");
    p = pci_progress_append_text(p, stage);
    p = pci_progress_append_text(p, " monotonic_ms=");
    p = pci_progress_append_u64(p, timer_get_uptime_ms());
    p = pci_progress_append_text(p, " clockevent_ticks=");
    p = pci_progress_append_u64(p, event.total_ticks);
    if (detail && *detail) {
        p = pci_progress_append_text(p, " ");
        p = pci_progress_append_text(p, detail);
    }
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_legacy_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_legacy_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    outl(0xCFC, value);
}

uint64_t pci_get_capability(uint64_t device_addr, uint8_t cap_id)
{
    PciDeviceHeader *header = (PciDeviceHeader *)device_addr;

    if (!(header->Status & (1 << 4)))
        return 0;

    uint8_t *ptr_to_cap = (uint8_t *)(device_addr + 0x34);
    uint8_t cap_offset = *ptr_to_cap;

    while (cap_offset != 0)
    {
        uint64_t cap_addr = device_addr + cap_offset;
        PciCapabilityHeader *cap_header = (PciCapabilityHeader *)cap_addr;
        if (cap_header->CapID == cap_id)
            return cap_addr;
        cap_offset = cap_header->NextCap;
    }
    return 0;
}

bool pci_msi_prepare(uint64_t device_addr, uint8_t vector,
                     uint32_t destination_apic_id)
{
    uint64_t msi_addr = pci_get_capability(device_addr, 0x05);

    if (!msi_addr || vector < 32u || destination_apic_id > UINT8_MAX)
    {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        console_write_debug("[PCI] MSI preparation rejected.\n");
        return false;
    }

    volatile uint8_t *cap = (volatile uint8_t *)msi_addr;
    volatile uint16_t *msg_ctl_p = (volatile uint16_t *)(cap + 0x02);
    uint16_t msg_ctl = *msg_ctl_p;

    const uint8_t is_64bit = (msg_ctl & PCI_MSI_CTL_64BIT) ? 1u : 0u;

    volatile uint32_t *msg_addr_lo_p = (volatile uint32_t *)(cap + 0x04);
    volatile uint32_t *msg_addr_hi_p = is_64bit ? (volatile uint32_t *)(cap + 0x08) : NULL;
    volatile uint16_t *msg_data_p = is_64bit ? (volatile uint16_t *)(cap + 0x0C)
                                             : (volatile uint16_t *)(cap + 0x08);

    msg_ctl &= (uint16_t)~(PCI_MSI_CTL_ENABLE | PCI_MSI_CTL_MME_MASK);
    *msg_ctl_p = msg_ctl;
    *msg_addr_lo_p = 0xFEE00000u | (destination_apic_id << 12);
    if (msg_addr_hi_p)
        *msg_addr_hi_p = 0;
    *msg_data_p = (uint16_t)(vector & 0xFFu);

    g_xhci_msi = (pci_msi_snapshot_t){
        .device_address = device_addr,
        .capability_address = msi_addr,
        .destination_apic_id = destination_apic_id,
        .vector = vector,
        .prepared = ((*msg_ctl_p & PCI_MSI_CTL_ENABLE) == 0)};

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    console_write_debug("[PCI] MSI Prepared: vector=");
    console_print_dec_debug(vector);
    console_write_debug(" apic_id=");
    console_print_dec_debug(destination_apic_id);
    console_write_debug("\n");
    return g_xhci_msi.prepared != 0;
}

bool pci_msi_enable(uint64_t device_addr)
{
    if (!g_xhci_msi.prepared || g_xhci_msi.device_address != device_addr ||
        !g_xhci_msi.capability_address || !xhci_interrupt_state_ready())
        return false;
    volatile uint16_t *control = (volatile uint16_t *)(
        g_xhci_msi.capability_address + 2u);
    uint16_t value = *control;
    value &= (uint16_t)~PCI_MSI_CTL_MME_MASK;
    value |= PCI_MSI_CTL_ENABLE;
    *control = value;
    g_xhci_msi.enabled = ((*control & PCI_MSI_CTL_ENABLE) != 0);
    return g_xhci_msi.enabled != 0;
}

bool pci_msi_disable(uint64_t device_addr)
{
    uint64_t capability = pci_get_capability(device_addr, 0x05);
    if (!capability)
        return false;
    volatile uint16_t *control = (volatile uint16_t *)(capability + 2u);
    *control = (uint16_t)(*control & ~PCI_MSI_CTL_ENABLE);
    bool disabled = (*control & PCI_MSI_CTL_ENABLE) == 0;
    if (g_xhci_msi.device_address == device_addr)
        g_xhci_msi.enabled = disabled ? 0u : 1u;
    return disabled;
}

bool pci_msi_enable_prepared(void)
{
    if (!g_xhci_msi.prepared)
        return true;
    if (!pci_msi_enable(g_xhci_msi.device_address))
        return false;
    console_write_debug("[PCI] MSI service release complete.\n");
    return true;
}

bool pci_msi_snapshot(pci_msi_snapshot_t *out)
{
    if (!out)
        return false;
    *out = g_xhci_msi;
    return true;
}

void pci_enumerate_function(uint64_t device_addr, uint64_t function, uint8_t bus, uint8_t slot)
{
    uint64_t offset = function << 12;
    uint64_t function_addr = device_addr + offset;

    paging_map(function_addr, function_addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD);

    PciDeviceHeader *pci_header = (PciDeviceHeader *)function_addr;

    if (pci_header->DeviceID == 0)
        return;
    if (pci_header->DeviceID == 0xFFFF)
        return;

    console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
    console_write_debug("[PCI] ");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
    console_write_debug(pci_get_vendor_name(pci_header->VendorID));
    console_write_debug(" / ");
    console_write_debug(pci_get_device_name(pci_header->VendorID, pci_header->DeviceID));
    console_write_debug(" / ");
    console_write_debug(pci_get_class_name(pci_header->Class));
    console_write_debug("\n");

    if (pci_header->Class == 0x0C && pci_header->Subclass == 0x03 && pci_header->ProgIF == 0x30)
    {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
        console_write_debug(" [XHCI FOUND!]");

        uint32_t *bars = (uint32_t *)((uint64_t)pci_header + 0x10);
        uint64_t bar0 = bars[0];
        uint64_t bar1 = bars[1];

        uint64_t base_addr = (bar1 << 32) | (bar0 & 0xFFFFFFF0);

        uint32_t cmd = pci_legacy_read(bus, slot, function, 0x04);

        console_write_debug("\n[PCI] Command Reg was: 0x");
        console_print_hex_debug(cmd);

        cmd |= (1 << 1) | (1 << 2);

        pci_legacy_write(bus, slot, function, 0x04, cmd);

        uint32_t cmd_verify = pci_legacy_read(bus, slot, function, 0x04);
        if (cmd_verify & (1 << 2))
        {
            console_write_debug(" -> Bus Master FORCED OK. New: 0x");
            console_print_hex_debug(cmd_verify);
            console_write_debug("\n");
        }
        else
        {
            console_write_debug(" -> FATAL: Failed to set Bus Master! RAM is Unreachable.\n");
        }

        console_write_debug("[PCI] Preparing MSI in masked service state...\n");
        if (!pci_msi_prepare(function_addr, INT_VECTOR_XHCI,
                             g_bsp_apic_id))
            console_write_debug("[PCI] MSI remains unavailable.\n");

        console_write_debug("Initializing XHCI Driver at 0x");
        console_print_hex_debug(base_addr);
        console_write_debug("...\n");

        pci_progress("PCI_SCAN_PROGRESS", "component=xhci action=begin");
        xhci_init(base_addr);
        pci_progress("PCI_SCAN_PROGRESS", "component=xhci action=complete");
    }
}

void pci_enumerate_device(uint64_t bus_addr, uint64_t device, uint8_t bus)
{
    uint64_t offset = device << 15;
    uint64_t device_addr = bus_addr + offset;

    paging_map(device_addr, device_addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD);
    PciDeviceHeader *pci_header = (PciDeviceHeader *)device_addr;

    if (pci_header->DeviceID == 0)
        return;
    if (pci_header->DeviceID == 0xFFFF)
        return;

    for (uint64_t function = 0; function < 8; function++)
    {
        pci_enumerate_function(device_addr, function, bus, device);
    }
}

void pci_enumerate_bus(uint64_t base_addr, uint64_t bus)
{
    uint64_t offset = bus << 20;
    uint64_t bus_addr = base_addr + offset;

    for (uint64_t device = 0; device < 32; device++)
    {
        pci_enumerate_device(bus_addr, device, bus);
    }
}

void pci_init()
{
    pci_progress("PCI_BEGIN", "source=MCFG");
    console_set_color(CONSOLE_COLOR_DEBUG, CONSOLE_COLOR_BLACK);
    console_write_debug("PCI: Searching for ACPI MCFG table...\n");

    McfgHeader *mcfg = (McfgHeader *)acpi_find_table(ACPI_SIG_MCFG);

    if (!mcfg)
    {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_BLACK);
        console_write_debug("[PCI] Error: ACPI MCFG Table not found!\n");
        pci_progress("PCI_SCAN_COMPLETE", "status=MCFG_MISSING");
        return;
    }

    paging_map((uint64_t)mcfg, (uint64_t)mcfg, PAGE_PRESENT | PAGE_RW);

    console_set_color(CONSOLE_COLOR_DEBUG, CONSOLE_COLOR_BLACK);
    console_write_debug("PCI: MCFG Table found. Parsing entries...\n");

    int entries = ((mcfg->Header.length) - sizeof(McfgHeader)) / sizeof(McfgDeviceConfig);

    if (entries == 0)
    {
        console_write_debug("PCI: Warning - MCFG table has 0 entries.\n");
    }

    for (int t = 0; t < entries; t++)
    {
        McfgDeviceConfig *config = (McfgDeviceConfig *)((uint64_t)mcfg + sizeof(McfgHeader) + (sizeof(McfgDeviceConfig) * t));

        console_write_debug("PCI: Scanning Segment Group...\n");
        pci_progress("PCI_SCAN_PROGRESS", "component=segment action=begin");

        for (uint64_t bus = config->StartBus; bus < config->EndBus; bus++)
        {
            pci_enumerate_bus(config->BaseAddress, bus);
        }
    }

    console_write_debug("PCI: Scan complete.\n");
    pci_progress("PCI_SCAN_COMPLETE", "status=COMPLETE");
}

static void pci_list_function(uint64_t device_addr, uint64_t function, uint8_t bus, uint8_t slot)
{
    uint64_t offset = ((uint64_t)function) << 12;
    uint64_t function_addr = device_addr + offset;

    paging_map(function_addr, function_addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD);

    PciDeviceHeader *pci_header = (PciDeviceHeader *)function_addr;

    if (pci_header->DeviceID == 0)
        return;
    if (pci_header->DeviceID == 0xFFFF)
        return;

    console_set_color(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("[PCI] ");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_print_dec(bus);
    console_write(":");
    console_print_dec(slot);
    console_write(".");
    console_print_dec(function);

    console_write("  ");
    console_write(pci_get_vendor_name(pci_header->VendorID));
    console_write(" / ");
    console_write(pci_get_device_name(pci_header->VendorID, pci_header->DeviceID));
    console_write(" / ");
    console_write(pci_get_class_name(pci_header->Class));
    console_write("\n");
}

static void pci_list_device(uint64_t bus_addr, uint64_t device, uint8_t bus)
{
    uint64_t offset = ((uint64_t)device) << 15;
    uint64_t device_addr = bus_addr + offset;

    paging_map(device_addr, device_addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD);

    PciDeviceHeader *pci_header = (PciDeviceHeader *)device_addr;

    if (pci_header->DeviceID == 0)
        return;
    if (pci_header->DeviceID == 0xFFFF)
        return;

    for (uint64_t function = 0; function < 8; function++)
    {
        pci_list_function(device_addr, function, bus, (uint8_t)device);
    }
}

static void pci_list_bus(uint64_t base_addr, uint64_t bus, uint64_t start_bus)
{

    uint64_t offset = ((uint64_t)(bus - start_bus)) << 20;
    uint64_t bus_addr = base_addr + offset;

    for (uint64_t device = 0; device < 32; device++)
    {
        pci_list_device(bus_addr, device, (uint8_t)bus);
    }
}

void pci_list_devices(void)
{
    McfgHeader *mcfg = (McfgHeader *)acpi_find_table("MCFG");
    if (!mcfg)
    {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("[PCI] MCFG not found.\n");
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
        return;
    }

    paging_map((uint64_t)mcfg, (uint64_t)mcfg, PAGE_PRESENT | PAGE_RW | PAGE_PCD);

    uint64_t entries_len = (uint64_t)mcfg->Header.length - sizeof(McfgHeader);
    uint64_t entries_count = entries_len / sizeof(McfgDeviceConfig);

    McfgDeviceConfig *config = (McfgDeviceConfig *)((uint64_t)mcfg + sizeof(McfgHeader));

    for (uint64_t i = 0; i < entries_count; i++)
    {

        uint64_t base = config[i].BaseAddress;
        uint64_t start_bus = config[i].StartBus;
        uint64_t end_bus = config[i].EndBus;

        paging_map(base, base, PAGE_PRESENT | PAGE_RW | PAGE_PCD);

        for (uint64_t bus = start_bus; bus <= end_bus; bus++)
        {
            pci_list_bus(base, bus, start_bus);
        }
    }
}

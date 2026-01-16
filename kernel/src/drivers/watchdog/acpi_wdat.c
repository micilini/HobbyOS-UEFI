#include "acpi_wdat.h"

#include "../../acpi/acpi.h"
#include "../../graphics/console.h"
#include "../../memory/paging.h"

#define ACPI_SIG_WDAT "WDAT"

typedef struct
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) AcpiGenericAddress;

typedef struct
{
    AcpiSdtHeader header;
    uint32_t header_length;
    uint16_t pci_segment;
    uint8_t pci_bus;
    uint8_t pci_device;
    uint8_t pci_function;
    uint8_t reserved[3];
    uint32_t timer_period;
    uint32_t max_count;
    uint32_t min_count;
    uint8_t flags;
    uint8_t reserved2[3];
    uint32_t entries;
} __attribute__((packed)) AcpiTableWdat;

typedef struct
{
    uint8_t action;
    uint8_t instruction;
    uint16_t reserved;
    AcpiGenericAddress register_region;
    uint32_t value;
    uint32_t mask;
} __attribute__((packed)) AcpiWdatEntry;

#define ACPI_WDAT_ENABLED (1)
#define ACPI_WDAT_STOPPED (0x80)

enum
{
    ACPI_WDAT_RESET = 1,
    ACPI_WDAT_GET_CURRENT_COUNTDOWN = 4,
    ACPI_WDAT_GET_COUNTDOWN = 5,
    ACPI_WDAT_SET_COUNTDOWN = 6,
    ACPI_WDAT_GET_RUNNING_STATE = 8,
    ACPI_WDAT_SET_RUNNING_STATE = 9,
    ACPI_WDAT_GET_STOPPED_STATE = 10,
    ACPI_WDAT_SET_STOPPED_STATE = 11,
    ACPI_WDAT_GET_REBOOT = 16,
    ACPI_WDAT_SET_REBOOT = 17,
    ACPI_WDAT_GET_SHUTDOWN = 18,
    ACPI_WDAT_SET_SHUTDOWN = 19,
    ACPI_WDAT_GET_STATUS = 32,
    ACPI_WDAT_SET_STATUS = 33,
};

enum
{
    ACPI_WDAT_READ_VALUE = 0,
    ACPI_WDAT_READ_COUNTDOWN = 1,
    ACPI_WDAT_WRITE_VALUE = 2,
    ACPI_WDAT_WRITE_COUNTDOWN = 3,
    ACPI_WDAT_PRESERVE_REGISTER = 0x80
};

#define ACPI_ADR_SPACE_SYSTEM_MEMORY 0
#define ACPI_ADR_SPACE_SYSTEM_IO 1

static inline uint8_t io_in8(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint16_t io_in16(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint32_t io_in32(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_out8(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}
static inline void io_out16(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}
static inline void io_out32(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

static uint8_t wdat_access_size(const AcpiGenericAddress *gas)
{

    if (gas->access_size)
        return gas->access_size;

    if (gas->register_bit_width <= 8)
        return 1;
    if (gas->register_bit_width <= 16)
        return 2;
    if (gas->register_bit_width <= 32)
        return 3;
    return 4;
}

static void wdat_ensure_mapped(uint64_t phys)
{

    uint64_t page = phys & ~0xFFFULL;
    paging_map(page, page, PAGE_PRESENT | PAGE_RW | PAGE_PCD);
}

static int wdat_read_reg(const AcpiGenericAddress *gas, uint64_t *out_val)
{
    if (!gas || !out_val)
        return -1;

    uint8_t as = wdat_access_size(gas);

    if (gas->address_space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY)
    {
        wdat_ensure_mapped(gas->address);

        volatile void *p = (volatile void *)(uintptr_t)gas->address;

        switch (as)
        {
        case 1:
            *out_val = *(volatile uint8_t *)p;
            return 0;
        case 2:
            *out_val = *(volatile uint16_t *)p;
            return 0;
        case 3:
            *out_val = *(volatile uint32_t *)p;
            return 0;
        case 4:
            *out_val = *(volatile uint64_t *)p;
            return 0;
        default:
            return -2;
        }
    }
    else if (gas->address_space_id == ACPI_ADR_SPACE_SYSTEM_IO)
    {
        uint16_t port = (uint16_t)(gas->address & 0xFFFF);

        switch (as)
        {
        case 1:
            *out_val = io_in8(port);
            return 0;
        case 2:
            *out_val = io_in16(port);
            return 0;
        case 3:
            *out_val = io_in32(port);
            return 0;
        default:
            return -2;
        }
    }

    return -3;
}

static int wdat_write_reg(const AcpiGenericAddress *gas, uint64_t val)
{
    if (!gas)
        return -1;

    uint8_t as = wdat_access_size(gas);

    if (gas->address_space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY)
    {
        wdat_ensure_mapped(gas->address);

        volatile void *p = (volatile void *)(uintptr_t)gas->address;

        switch (as)
        {
        case 1:
            *(volatile uint8_t *)p = (uint8_t)val;
            return 0;
        case 2:
            *(volatile uint16_t *)p = (uint16_t)val;
            return 0;
        case 3:
            *(volatile uint32_t *)p = (uint32_t)val;
            return 0;
        case 4:
            *(volatile uint64_t *)p = (uint64_t)val;
            return 0;
        default:
            return -2;
        }
    }
    else if (gas->address_space_id == ACPI_ADR_SPACE_SYSTEM_IO)
    {
        uint16_t port = (uint16_t)(gas->address & 0xFFFF);

        switch (as)
        {
        case 1:
            io_out8(port, (uint8_t)val);
            return 0;
        case 2:
            io_out16(port, (uint16_t)val);
            return 0;
        case 3:
            io_out32(port, (uint32_t)val);
            return 0;
        default:
            return -2;
        }
    }

    return -3;
}

static int wdat_exec_entry(const AcpiWdatEntry *e, uint32_t param, uint64_t *opt_readback)
{
    const AcpiGenericAddress *gas = &e->register_region;

    uint32_t mask = e->mask;
    if (mask == 0)
        mask = 0xFFFFFFFFu;

    uint8_t preserve = (e->instruction & ACPI_WDAT_PRESERVE_REGISTER) ? 1 : 0;
    uint8_t instr = (uint8_t)(e->instruction & ~ACPI_WDAT_PRESERVE_REGISTER);

    uint64_t y = 0;
    uint64_t x = 0;

    switch (instr)
    {
    case ACPI_WDAT_READ_VALUE:
    case ACPI_WDAT_READ_COUNTDOWN:
    {
        uint64_t raw = 0;
        if (wdat_read_reg(gas, &raw) != 0)
            return -10;

        uint8_t shift = gas->register_bit_offset;
        x = (raw >> shift) & mask;

        if (opt_readback)
            *opt_readback = x;
        return 0;
    }

    case ACPI_WDAT_WRITE_VALUE:
    case ACPI_WDAT_WRITE_COUNTDOWN:
    {
        uint32_t v = (instr == ACPI_WDAT_WRITE_COUNTDOWN) ? param : e->value;

        x = (uint64_t)(v & mask);
        x <<= gas->register_bit_offset;

        if (preserve)
        {
            if (wdat_read_reg(gas, &y) != 0)
                return -11;
            y = y & ~((uint64_t)mask << gas->register_bit_offset);
            x |= y;
        }

        if (wdat_write_reg(gas, x) != 0)
            return -12;
        return 0;
    }

    default:
        return -20;
    }
}

static int wdat_run_action(const AcpiTableWdat *wdat, uint8_t action, uint32_t param)
{
    if (!wdat)
        return -1;

    const uint8_t *base = (const uint8_t *)wdat;
    const AcpiWdatEntry *entries = (const AcpiWdatEntry *)(base + wdat->header_length);

    uint32_t count = wdat->entries;
    int ran_any = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        const AcpiWdatEntry *e = &entries[i];
        if (e->action != action)
            continue;

        ran_any = 1;

        int r = wdat_exec_entry(e, param, NULL);
        if (r != 0)
            return r;
    }

    return ran_any ? 0 : 1;
}

int acpi_wdat_disable(void)
{
    AcpiTableWdat *wdat = (AcpiTableWdat *)acpi_find_table(ACPI_SIG_WDAT);
    if (!wdat)
    {
        return 1;
    }

    console_write_debug("[WATCHDOG][WDAT] Table found. Trying SET_STOPPED_STATE...\n");

    int r = wdat_run_action(wdat, ACPI_WDAT_SET_STOPPED_STATE, 0);
    if (r == 0)
    {
        console_write_debug("[WATCHDOG][WDAT] SET_STOPPED_STATE executed.\n");
        return 0;
    }

    console_write_debug("[WATCHDOG][WDAT] SET_STOPPED_STATE not available/failed. Trying fallbacks...\n");

    (void)wdat_run_action(wdat, ACPI_WDAT_SET_REBOOT, 0);
    (void)wdat_run_action(wdat, ACPI_WDAT_SET_SHUTDOWN, 0);
    (void)wdat_run_action(wdat, ACPI_WDAT_SET_STATUS, 0);

    console_write_debug("[WATCHDOG][WDAT] Fallbacks attempted.\n");
    return -1;
}
#include "serial.h"
#include "utils.h"
#include <efipciio.h>
#include <stdint.h>
#include <stddef.h>





#ifndef HOBBYOS_SERIAL_SCAN_KNUP_OFFSETS
#define HOBBYOS_SERIAL_SCAN_KNUP_OFFSETS 0
#endif


#ifndef HOBBYOS_SERIAL_INCLUDE_KNUP_FALLBACK
#define HOBBYOS_SERIAL_INCLUDE_KNUP_FALLBACK 0
#endif

static inline void outb_u8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_u8(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) { outb_u8(0x80, 0); }

enum {
    UART_RBR_THR_DLL = 0,
    UART_IER_DLM     = 1,
    UART_IIR_FCR     = 2,
    UART_LCR         = 3,
    UART_MCR         = 4,
    UART_LSR         = 5,
};

#define UART_LSR_THRE (1u << 5)


static int uart_probe(uint16_t base);
static int bootinfo_add_serial_port(BootInfo* bi, uint16_t io_base);





#pragma pack(push, 1)
typedef struct {
    char     Signature[8];      
    uint8_t  Checksum;
    char     OemId[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;
    
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t  ExtendedChecksum;
    uint8_t  Reserved[3];
} AcpiRsdp;

typedef struct {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OemId[6];
    char     OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} AcpiSdtHeader;

typedef struct {
    uint8_t  AddressSpace;   
    uint8_t  BitWidth;
    uint8_t  BitOffset;
    uint8_t  AccessSize;
    uint64_t Address;
} AcpiGas;

typedef struct {
    AcpiSdtHeader Header;
    uint8_t  InterfaceType;
    uint8_t  Reserved[3];
    AcpiGas  BaseAddress;
    
} AcpiSpcr;
#pragma pack(pop)

static uint8_t acpi_checksum8(const void* p, uint32_t len)
{
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum;
}

static const AcpiSdtHeader* acpi_find_sdt(const void* rsdp_void, const char sig4[4])
{
    if (!rsdp_void) return NULL;

    const AcpiRsdp* rsdp = (const AcpiRsdp*)rsdp_void;
    if (memcmp(rsdp->Signature, "RSD PTR ", 8) != 0) return NULL;

    
    if (acpi_checksum8(rsdp, 20) != 0) return NULL;

    
    if (rsdp->Revision >= 2 && rsdp->Length >= sizeof(AcpiRsdp)) {
        if (acpi_checksum8(rsdp, rsdp->Length) != 0) return NULL;
    }

    const AcpiSdtHeader* xsdt = NULL;
    const AcpiSdtHeader* rsdt = NULL;

    if (rsdp->Revision >= 2 && rsdp->XsdtAddress) {
        xsdt = (const AcpiSdtHeader*)(uintptr_t)rsdp->XsdtAddress;
        if (memcmp(xsdt->Signature, "XSDT", 4) != 0) xsdt = NULL;
        else if (acpi_checksum8(xsdt, xsdt->Length) != 0) xsdt = NULL;
    }

    if (!xsdt && rsdp->RsdtAddress) {
        rsdt = (const AcpiSdtHeader*)(uintptr_t)rsdp->RsdtAddress;
        if (memcmp(rsdt->Signature, "RSDT", 4) != 0) rsdt = NULL;
        else if (acpi_checksum8(rsdt, rsdt->Length) != 0) rsdt = NULL;
    }

    const AcpiSdtHeader* root = xsdt ? xsdt : rsdt;
    if (!root) return NULL;

    uint32_t hdr_len = (uint32_t)sizeof(AcpiSdtHeader);
    if (root->Length < hdr_len) return NULL;

    if (memcmp(root->Signature, "XSDT", 4) == 0)
    {
        uint32_t entries = (root->Length - hdr_len) / 8;
        const uint64_t* table_ptrs = (const uint64_t*)((const uint8_t*)root + hdr_len);
        for (uint32_t i = 0; i < entries; i++)
        {
            const AcpiSdtHeader* h = (const AcpiSdtHeader*)(uintptr_t)table_ptrs[i];
            if (!h) continue;
            if (memcmp(h->Signature, sig4, 4) == 0)
            {
                if (h->Length >= sizeof(AcpiSdtHeader) && acpi_checksum8(h, h->Length) == 0)
                    return h;
            }
        }
    }
    else
    {
        uint32_t entries = (root->Length - hdr_len) / 4;
        const uint32_t* table_ptrs = (const uint32_t*)((const uint8_t*)root + hdr_len);
        for (uint32_t i = 0; i < entries; i++)
        {
            const AcpiSdtHeader* h = (const AcpiSdtHeader*)(uintptr_t)table_ptrs[i];
            if (!h) continue;
            if (memcmp(h->Signature, sig4, 4) == 0)
            {
                if (h->Length >= sizeof(AcpiSdtHeader) && acpi_checksum8(h, h->Length) == 0)
                    return h;
            }
        }
    }

    return NULL;
}

static void serial_discover_from_spcr(BootInfo* boot_info)
{
    if (!boot_info) return;
    if (!boot_info->rsdp) return;

    const AcpiSdtHeader* spcr_hdr = acpi_find_sdt(boot_info->rsdp, "SPCR");
    if (!spcr_hdr) return;
    if (spcr_hdr->Length < sizeof(AcpiSpcr)) return;

    const AcpiSpcr* spcr = (const AcpiSpcr*)spcr_hdr;

    
    
    if (!(spcr->InterfaceType == 0 || spcr->InterfaceType == 1 || spcr->InterfaceType == 0x12))
        return;

    
    if (spcr->BaseAddress.AddressSpace != 1) return;
    if (spcr->BaseAddress.Address == 0) return;

    uint16_t io_base = (uint16_t)(spcr->BaseAddress.Address & 0xFFFF);
    if (uart_probe(io_base))
        bootinfo_add_serial_port(boot_info, io_base);
}





static void uart_init_115200_8n1(uint16_t base)
{
    outb_u8(base + UART_IER_DLM, 0x00);
    outb_u8(base + UART_LCR, 0x80);
    outb_u8(base + UART_RBR_THR_DLL, 0x01);
    outb_u8(base + UART_IER_DLM, 0x00);
    outb_u8(base + UART_LCR, 0x03);
    outb_u8(base + UART_IIR_FCR, 0xC7);
    outb_u8(base + UART_MCR, 0x03);
    io_wait();
}

static int uart_wait_thre(uint16_t base)
{
    
    for (uint32_t spin = 0; spin < 2000000; spin++)
    {
        uint8_t lsr = inb_u8(base + UART_LSR);
        if (lsr & UART_LSR_THRE) return 1;
    }
    return 0;
}

static void uart_putc(uint16_t base, char c)
{
    if (c == '\n') uart_putc(base, '\r');

    if (!uart_wait_thre(base)) {
        
        return;
    }

    outb_u8(base + UART_RBR_THR_DLL, (uint8_t)c);
}

static int uart_probe(uint16_t base)
{
    
    uint8_t lsr = inb_u8(base + UART_LSR);
    uint8_t iir = inb_u8(base + UART_IIR_FCR);

    
    if (lsr == 0xFF && iir == 0xFF) return 0;

    
    uint8_t old = inb_u8(base + UART_LCR);
    outb_u8(base + UART_LCR, (uint8_t)(old ^ 0x03));
    uint8_t now = inb_u8(base + UART_LCR);
    outb_u8(base + UART_LCR, old);

    if (now == 0xFF) return 0;
    if (now == old) return 0;

    return 1;
}

static void uart_write(uint16_t base, const char* s)
{
    while (*s) uart_putc(base, *s++);
}

static void uart_write_hex64(uint16_t base, uint64_t v)
{
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (v >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        uart_putc(base, c);
    }
}





static void pci_enable_io_space(EFI_PCI_IO_PROTOCOL *PciIo)
{
    UINT16 cmd = 0;
    EFI_STATUS st;

    st = uefi_call_wrapper(PciIo->Pci.Read, 5,
                           PciIo,
                           EfiPciIoWidthUint16,
                           0x04,
                           1,
                           &cmd);
    if (EFI_ERROR(st)) return;

    if ((cmd & 0x0001) == 0) {
        UINT16 new_cmd = (UINT16)(cmd | 0x0001);
        uefi_call_wrapper(PciIo->Pci.Write, 5,
                          PciIo,
                          EfiPciIoWidthUint16,
                          0x04,
                          1,
                          &new_cmd);
    }
}





static int bootinfo_add_serial_port(BootInfo* bi, uint16_t io_base)
{
    for (uint32_t i = 0; i < bi->serial.count; i++) {
        if (bi->serial.ports[i].io_base == io_base) return 0;
    }

    if (bi->serial.count >= HOBBYOS_MAX_SERIAL_PORTS) return -1;

    bi->serial.ports[bi->serial.count].io_base = io_base;
    bi->serial.ports[bi->serial.count].kind = HOBBYOS_SERIAL_KIND_16550_IO;
    bi->serial.count++;
    return 1;
}





void serial_try_init_and_write(uint16_t io_base, const char* msg)
{
    uart_init_115200_8n1(io_base);
    uart_write(io_base, msg);
}

void serial_write_all(const BootInfo* boot_info, const char* msg)
{
    if (!boot_info) return;

    for (uint32_t i = 0; i < boot_info->serial.count; i++)
    {
        uint16_t base = boot_info->serial.ports[i].io_base;
        uart_init_115200_8n1(base);
        uart_write(base, msg);
    }
}

void serial_write_hex64_all(const BootInfo* boot_info, uint64_t value)
{
    if (!boot_info) return;

    for (uint32_t i = 0; i < boot_info->serial.count; i++)
    {
        uint16_t base = boot_info->serial.ports[i].io_base;
        uart_init_115200_8n1(base);
        uart_write_hex64(base, value);
    }
}

void serial_discover_ports(BootInfo* boot_info)
{
    boot_info->serial.count = 0;

    
    serial_discover_from_spcr(boot_info);

    EFI_STATUS st;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;

    st = uefi_call_wrapper(BS->LocateHandleBuffer, 5,
                           ByProtocol,
                           &gEfiPciIoProtocolGuid,
                           NULL,
                           &count,
                           &handles);
    if (EFI_ERROR(st) || count == 0) goto fallback;

    for (UINTN i = 0; i < count; i++)
    {
        EFI_PCI_IO_PROTOCOL *PciIo = NULL;
        st = uefi_call_wrapper(BS->HandleProtocol, 3,
                               handles[i],
                               &gEfiPciIoProtocolGuid,
                               (void **)&PciIo);
        if (EFI_ERROR(st) || !PciIo) continue;

        UINT8 cfg[256];
        st = uefi_call_wrapper(PciIo->Pci.Read, 5,
                               PciIo,
                               EfiPciIoWidthUint8,
                               0,
                               sizeof(cfg),
                               cfg);
        if (EFI_ERROR(st)) continue;

        UINT8 class_base = cfg[0x0B];
        UINT8 class_sub  = cfg[0x0A];

        if (class_base != 0x07) continue;
        
        if (!(class_sub == 0x00 || class_sub == 0x02)) continue;

        int has_io_bar = 0;
        for (int bar = 0; bar < 6; bar++) {
            UINT32 barv = *(UINT32*)&cfg[0x10 + bar*4];
            if (!barv) continue;
            if (barv & 0x1) { has_io_bar = 1; break; }
        }
        if (!has_io_bar) continue;

        pci_enable_io_space(PciIo);

        for (int bar = 0; bar < 6; bar++)
        {
            UINT32 barv = *(UINT32*)&cfg[0x10 + bar*4];
            if (!barv) continue;

            if (barv & 0x1) {
                uint16_t io_base = (uint16_t)(barv & ~0x3);

                if (uart_probe(io_base))
                    bootinfo_add_serial_port(boot_info, io_base);

                #if HOBBYOS_SERIAL_SCAN_KNUP_OFFSETS
                uint16_t b1 = (uint16_t)(io_base + 0xC0);
                if (uart_probe(b1))
                    bootinfo_add_serial_port(boot_info, b1);

                uint16_t b2 = (uint16_t)(io_base + 0xC8);
                if (uart_probe(b2))
                    bootinfo_add_serial_port(boot_info, b2);
                #endif
            }
        }
    }

    if (handles) uefi_call_wrapper(BS->FreePool, 1, handles);

fallback:

    
    if (boot_info->serial.count == 0)
    {
        bootinfo_add_serial_port(boot_info, 0x3F8); 
        bootinfo_add_serial_port(boot_info, 0x2F8); 
        bootinfo_add_serial_port(boot_info, 0x3E8); 
        bootinfo_add_serial_port(boot_info, 0x2E8); 
    }

    
    
    #if HOBBYOS_SERIAL_INCLUDE_KNUP_FALLBACK
    bootinfo_add_serial_port(boot_info, 0x40C0);
    bootinfo_add_serial_port(boot_info, 0x40C8);
    #endif

}

void serial_broadcast_boot_banner(const BootInfo* boot_info)
{
    const char* msg = "\nHobbyOS Started (UEFI Bootloader)\n";

    for (uint32_t i = 0; i < boot_info->serial.count; i++)
    {
        uint16_t base = boot_info->serial.ports[i].io_base;
        serial_try_init_and_write(base, msg);
    }
}

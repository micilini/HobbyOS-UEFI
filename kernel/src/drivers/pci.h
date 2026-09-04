#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>
#include "../acpi/acpi.h"

typedef struct
{
    uint16_t VendorID;
    uint16_t DeviceID;
    uint16_t Command;
    uint16_t Status;
    uint8_t RevisionID;
    uint8_t ProgIF;
    uint8_t Subclass;
    uint8_t Class;
    uint8_t CacheLineSize;
    uint8_t LatencyTimer;
    uint8_t HeaderType;
    uint8_t BIST;
} __attribute__((packed)) PciDeviceHeader;

typedef struct
{
    uint8_t CapID;
    uint8_t NextCap;
} __attribute__((packed)) PciCapabilityHeader;

#define PCI_MSI_CTL_ENABLE (1u << 0)
#define PCI_MSI_CTL_MMC_MASK (0x000Eu)
#define PCI_MSI_CTL_MME_MASK (0x0070u)
#define PCI_MSI_CTL_64BIT (1u << 7)
#define PCI_MSI_CTL_PERVEC_MASK (1u << 8)

typedef struct
{
    uint64_t BaseAddress;
    uint16_t PciSegmentGroupNumber;
    uint8_t StartBus;
    uint8_t EndBus;
    uint32_t Reserved;
} __attribute__((packed)) McfgDeviceConfig;

typedef struct
{
    AcpiSdtHeader Header;
    uint64_t Reserved;
} __attribute__((packed)) McfgHeader;

typedef struct
{
    uint64_t device_address;
    uint64_t capability_address;
    uint32_t destination_apic_id;
    uint8_t vector;
    uint8_t prepared;
    uint8_t enabled;
} pci_msi_snapshot_t;

void pci_init();
bool pci_msi_prepare(uint64_t device_addr, uint8_t vector,
                     uint32_t destination_apic_id);
bool pci_msi_enable(uint64_t device_addr);
bool pci_msi_disable(uint64_t device_addr);
bool pci_msi_enable_prepared(void);
bool pci_msi_snapshot(pci_msi_snapshot_t *out);

void pci_list_devices(void);

#endif

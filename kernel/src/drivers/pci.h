#ifndef PCI_H
#define PCI_H

#include <stdint.h>
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

typedef struct
{
    PciCapabilityHeader Header;
    uint16_t MessageControl;
    uint32_t MessageAddress;
    uint32_t MessageAddressHigh;
    uint16_t MessageData;
} __attribute__((packed)) PciMsiCapability;

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

void pci_init();
uint32_t pci_enable_msi(uint64_t device_addr, uint8_t vector);

void pci_list_devices(void);

#endif
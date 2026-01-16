#include "pci_descriptors.h"
#include "../libc/string.h"

const char *hex_str(uint16_t val)
{
    return "Unknown ID";
}

const char *pci_get_vendor_name(uint16_t vendor_id)
{
    switch (vendor_id)
    {
    case 0x8086:
        return "Intel Corp";
    case 0x1022:
        return "AMD";
    case 0x10DE:
        return "NVIDIA Corporation";
    case 0x1AE0:
        return "Google";
    case 0x1AF4:
        return "VirtIO (QEMU)";
    }
    return hex_str(vendor_id);
}

const char *pci_get_device_name(uint16_t vendor_id, uint16_t device_id)
{
    switch (vendor_id)
    {
    case 0x8086:
        switch (device_id)
        {
        case 0x29C0:
            return "Express DRAM Controller";
        case 0x2918:
            return "LPC Interface Controller";
        case 0x2922:
            return "6 port SATA Controller [AHCI]";
        case 0x2930:
            return "SMBus Controller";
        case 0x1237:
            return "440FX - 82441FX PMC [Natoma]";
        case 0x7000:
            return "82371SB PIIX3 ISA [Natoma/Triton II]";
        case 0x7010:
            return "82371SB PIIX3 IDE [Natoma/Triton II]";
        case 0x7020:
            return "82371SB PIIX3 USB [Natoma/Triton II]";
        }
        break;
    }
    return hex_str(device_id);
}

const char *pci_get_class_name(uint8_t class_code)
{
    switch (class_code)
    {
    case 0x00:
        return "Unclassified";
    case 0x01:
        return "Mass Storage Controller";
    case 0x02:
        return "Network Controller";
    case 0x03:
        return "Display Controller";
    case 0x04:
        return "Multimedia Controller";
    case 0x05:
        return "Memory Controller";
    case 0x06:
        return "Bridge Device";
    case 0x07:
        return "Simple Communication Controller";
    case 0x08:
        return "Base System Peripheral";
    case 0x09:
        return "Input Device Controller";
    case 0x0A:
        return "Docking Station";
    case 0x0B:
        return "Processor";
    case 0x0C:
        return "Serial Bus Controller";
    case 0x0D:
        return "Wireless Controller";
    case 0x0E:
        return "Intelligent Controller";
    case 0x0F:
        return "Satellite Communication Controller";
    case 0x10:
        return "Encryption Controller";
    case 0x11:
        return "Signal Processing Controller";
    }
    return "Unknown Class";
}

const char *pci_get_subclass_name(uint8_t class_code, uint8_t subclass_code)
{
    switch (class_code)
    {
    case 0x01:
        switch (subclass_code)
        {
        case 0x00:
            return "SCSI Bus Controller";
        case 0x01:
            return "IDE Controller";
        case 0x02:
            return "Floppy Disk Controller";
        case 0x03:
            return "IPI Bus Controller";
        case 0x04:
            return "RAID Controller";
        case 0x05:
            return "ATA Controller";
        case 0x06:
            return "Serial ATA";
        case 0x80:
            return "Other";
        }
        break;
    case 0x03:
        switch (subclass_code)
        {
        case 0x00:
            return "VGA Compatible Controller";
        }
        break;
    case 0x06:
        switch (subclass_code)
        {
        case 0x00:
            return "Host Bridge";
        case 0x01:
            return "ISA Bridge";
        case 0x04:
            return "PCI-to-PCI Bridge";
        }
        break;
    case 0x0C:
        switch (subclass_code)
        {
        case 0x00:
            return "FireWire (IEEE 1394)";
        case 0x03:
            return "USB Controller";
        case 0x05:
            return "SMBus";
        }
        break;
    }
    return "Unknown Subclass";
}

const char *pci_get_progif_name(uint8_t class_code, uint8_t subclass_code, uint8_t prog_if)
{
    if (class_code == 0x01 && subclass_code == 0x06)
    {
        if (prog_if == 0x01)
            return "AHCI 1.0";
    }
    if (class_code == 0x0C && subclass_code == 0x03)
    {
        switch (prog_if)
        {
        case 0x00:
            return "UHCI (USB1)";
        case 0x10:
            return "OHCI (USB1)";
        case 0x20:
            return "EHCI (USB2)";
        case 0x30:
            return "XHCI (USB3)";
        case 0xFE:
            return "USB Device";
        }
    }
    return "Generic";
}
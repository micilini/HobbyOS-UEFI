#ifndef PCI_DESCRIPTORS_H
#define PCI_DESCRIPTORS_H

#include <stdint.h>

const char *pci_get_vendor_name(uint16_t vendor_id);
const char *pci_get_device_name(uint16_t vendor_id, uint16_t device_id);
const char *pci_get_class_name(uint8_t class_code);
const char *pci_get_subclass_name(uint8_t class_code, uint8_t subclass_code);
const char *pci_get_progif_name(uint8_t class_code, uint8_t subclass_code, uint8_t prog_if);

#endif
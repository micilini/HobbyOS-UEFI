#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>


#define ACPI_SIG_RSDP "RSD PTR "
#define ACPI_SIG_MADT "APIC" 
#define ACPI_SIG_HPET "HPET"


typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) AcpiSdtHeader;


typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) AcpiRsdp;

void init_acpi(void* rsdp_address);
void* acpi_find_table(const char* signature);

#endif
#ifndef HPET_H
#define HPET_H

#include "../acpi/acpi.h"
#include <stdint.h>


#define HPET_REG_CAPABILITIES       0x000
#define HPET_REG_CONFIG             0x010
#define HPET_REG_INT_STATUS         0x020
#define HPET_REG_MAIN_COUNTER       0x0F0


#define HPET_TN_CONFIG_CAP(n)       (0x100 + (0x20 * n))
#define HPET_TN_COMPARATOR(n)       (0x108 + (0x20 * n))
#define HPET_TN_FSB_ROUTE(n)        (0x110 + (0x20 * n))


typedef struct {
    AcpiSdtHeader header;
    uint8_t hardware_rev_id;
    uint8_t comparator_count : 5;
    uint8_t counter_size : 1;
    uint8_t reserved : 1;
    uint8_t legacy_replacement : 1;
    uint16_t pci_vendor_id;
    
    
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved2;
    uint64_t address; 
    
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed)) HpetTable;


void init_hpet();


void hpet_usleep(uint64_t microseconds);



void hpet_set_timer(uint64_t milliseconds);

#endif
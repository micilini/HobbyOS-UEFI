#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>


#define IOREGSEL 0x00 
#define IOWIN    0x10 


#define IOAPICID  0x00
#define IOAPICVER 0x01
#define IOREDTBL  0x10 


typedef union {
    struct {
        uint64_t vector       : 8; 
        uint64_t delv_mode    : 3;
        uint64_t dest_mode    : 1;
        uint64_t delv_status  : 1;
        uint64_t pin_polarity : 1;
        uint64_t remote_irr   : 1;
        uint64_t trigger_mode : 1;
        uint64_t mask         : 1; 
        uint64_t reserved     : 39;
        uint64_t destination  : 8; 
    } __attribute__((packed));
    struct {
        uint32_t lower;
        uint32_t upper;
    };
} IoApicRedirEntry;

void init_ioapic();
void ioapic_set_entry(uint8_t index, uint64_t data);

void ioapic_map_irq(uint8_t irq, uint8_t vector, uint8_t apic_id);

#endif
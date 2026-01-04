#ifndef IDT_H
#define IDT_H

#include <stdint.h>


#define IDT_ENTRIES 256


#define IDT_TA_INTERRUPT_GATE 0x8E 
#define IDT_TA_TRAP_GATE      0x8F 


typedef struct {
    uint16_t offset_low;       
    uint16_t selector;         
    uint8_t  ist;              
    uint8_t  type_attributes;  
    uint16_t offset_mid;       
    uint32_t offset_high;      
    uint32_t reserved;         
} __attribute__((packed)) IdtEntry;


typedef struct {
    uint16_t limit;            
    uint64_t base;             
} __attribute__((packed)) IdtPtr;


void init_idt();


void enable_interrupts();


void disable_interrupts();

#endif
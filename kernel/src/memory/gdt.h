#ifndef GDT_H
#define GDT_H

#include <stdint.h>


#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20



typedef struct {
    uint16_t limit_low;     
    uint16_t base_low;      
    uint8_t  base_middle;   
    uint8_t  access;        
    uint8_t  granularity;   
    uint8_t  base_high;     
} __attribute__((packed)) GdtEntry;


typedef struct {
    uint16_t size;          
    uint64_t offset;        
} __attribute__((packed)) GdtPtr;



typedef struct {
    GdtEntry null;
    GdtEntry kernel_code;
    GdtEntry kernel_data;
    GdtEntry user_code;
    GdtEntry user_data;
} __attribute__((packed, aligned(0x1000))) GdtTable;


void init_gdt();

#endif
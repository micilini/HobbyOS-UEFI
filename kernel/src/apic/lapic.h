#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>


#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800


#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ICR0      0x300
#define LAPIC_ICR1      0x310
#define LAPIC_TIMER     0x320

void init_lapic();
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_eoi(); 

#endif
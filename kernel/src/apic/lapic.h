#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MSR_ENABLE 0x800

#define LAPIC_ID 0x020
#define LAPIC_VER 0x030
#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SPURIOUS 0x0F0
#define LAPIC_ICR0 0x300
#define LAPIC_ICR1 0x310
#define LAPIC_TIMER 0x320

#define APIC_DM_FIXED 0x00000000
#define APIC_DM_LOWEST 0x00000100
#define APIC_DM_SMI 0x00000200
#define APIC_DM_NMI 0x00000400
#define APIC_DM_INIT 0x00000500
#define APIC_DM_SIPI 0x00000600

#define APIC_DEST_PHYSICAL 0x00000000
#define APIC_DEST_LOGICAL 0x00000800

#define APIC_DS_IDLE 0x00000000
#define APIC_DS_PENDING 0x00001000

#define APIC_LEVEL_DEASSERT 0x00000000
#define APIC_LEVEL_ASSERT 0x00004000

#define APIC_TRIGGER_EDGE 0x00000000
#define APIC_TRIGGER_LEVEL 0x00008000

#define APIC_DEST_SHORTHAND_NONE 0x00000000
#define APIC_DEST_SHORTHAND_SELF 0x00040000
#define APIC_DEST_SHORTHAND_ALL 0x00080000
#define APIC_DEST_SHORTHAND_ALL_BUT_SELF 0x000C0000

#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TICR 0x380
#define LAPIC_TCCR 0x390
#define LAPIC_TDCR 0x3E0

#define APIC_TIMER_PERIODIC 0x00020000
#define APIC_TIMER_ONE_SHOT 0x00000000

void init_lapic();
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_eoi();
uint32_t lapic_get_id(void);

void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page);

void init_lapic_ap(void);

void lapic_timer_set_periodic(uint32_t vector, uint32_t ticks);

void lapic_send_broadcast_halt();

#endif
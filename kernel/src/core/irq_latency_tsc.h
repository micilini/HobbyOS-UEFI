#ifndef HOBBYOS_IRQ_LATENCY_TSC_H
#define HOBBYOS_IRQ_LATENCY_TSC_H

#ifndef HOBBYOS_IRQ_LOCAL_TSC_ORDER
#define HOBBYOS_IRQ_LOCAL_TSC_ORDER 0
#endif
#if HOBBYOS_IRQ_LOCAL_TSC_ORDER == 1
#define IRQ_LATENCY_TSC_SEQUENCE "lfence; rdtsc; lfence"
#define IRQ_LATENCY_PRIMITIVE "intel-local-tsc-irq-envelope-v1"
#elif HOBBYOS_IRQ_LOCAL_TSC_ORDER == 0
#define IRQ_LATENCY_TSC_SEQUENCE "mfence; lfence; rdtsc; lfence"
#define IRQ_LATENCY_PRIMITIVE "ordered-tsc-irq-envelope"
#else
#error HOBBYOS_IRQ_LOCAL_TSC_ORDER must be 0 or 1
#endif

#ifdef __ASSEMBLER__
#if HOBBYOS_IRQ_LOCAL_TSC_ORDER
#define IRQ_LATENCY_ORDERED_TSC lfence; rdtsc; lfence
#else
#define IRQ_LATENCY_ORDERED_TSC mfence; lfence; rdtsc; lfence
#endif
#else
#include <stdbool.h>
#include <stdint.h>
#include "../cpu/cpu.h"
static inline bool irq_latency_tsc_capability(uint32_t edx, uint32_t vendor_b,
    uint32_t vendor_d, uint32_t vendor_c)
{
#if HOBBYOS_IRQ_LOCAL_TSC_ORDER
    return (edx & ((1u<<4)|(1u<<26))) == ((1u<<4)|(1u<<26)) &&
        vendor_b==0x756e6547u && vendor_d==0x49656e69u && vendor_c==0x6c65746eu;
#else
    (void)vendor_b;(void)vendor_d;(void)vendor_c;
    return (edx & (1u<<4)) != 0;
#endif
}
static inline bool irq_latency_tsc_available(void)
{
    uint32_t a,b,c,d,vb=0,vd=0,vc=0;
#if HOBBYOS_IRQ_LOCAL_TSC_ORDER
    if (!cpu_get_cpuid_count(0,0,&a,&vb,&vc,&vd) || a<1) return false;
#endif
    return cpu_get_cpuid_count(1,0,&a,&b,&c,&d) && irq_latency_tsc_capability(d,vb,vd,vc);
}
static inline uint64_t irq_latency_read_tsc(void)
{
#ifdef IRQ_LATENCY_HOSTED_CLOCK
#if !__STDC_HOSTED__
#error IRQ_LATENCY_HOSTED_CLOCK is restricted to hosted implementation tests
#endif
    extern uint64_t irq_latency_host_read_tsc(void);
    return irq_latency_host_read_tsc();
#else
    uint32_t low, high;
#if HOBBYOS_IRQ_LOCAL_TSC_ORDER
    __asm__ volatile("lfence; rdtsc; lfence"
                     : "=a"(low), "=d"(high) :: "memory");
#else
    __asm__ volatile("mfence; lfence; rdtsc; lfence"
                     : "=a"(low), "=d"(high) :: "memory");
#endif
    return ((uint64_t)high << 32) | low;
#endif
}
#endif

#endif

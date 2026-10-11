#include "tss.h"
#include "../libc/memory.h"
#include "../memory/physical_layout.h"
#include "../memory/kernel_stack.h"
#include <stddef.h>

#define IST_STACK_SIZE (32 * 1024)

__attribute__((aligned(4096))) static uint8_t g_ist1[IST_STACK_SIZE];
__attribute__((aligned(4096))) static uint8_t g_ist2[IST_STACK_SIZE];
__attribute__((aligned(4096))) static uint8_t g_ist3[IST_STACK_SIZE];

static Tss64 g_tss __attribute__((aligned(64)));
static bool g_guarded_ist;

static inline uint64_t top_of(uint8_t *buf)
{
    return (uint64_t)(buf + IST_STACK_SIZE);
}

void tss_init(void)
{
    memset(&g_tss, 0, sizeof(g_tss));

    g_tss.ist1 = top_of(g_ist1);
    g_tss.ist2 = top_of(g_ist2);
    g_tss.ist3 = top_of(g_ist3);

    g_tss.iomap_base = (uint16_t)sizeof(Tss64);
}

Tss64 *tss_get(void)
{
    return &g_tss;
}

bool tss_bsp_backing(uint64_t physical[3])
{
    if (!physical || g_guarded_ist || g_tss.ist1!=top_of(g_ist1) ||
        g_tss.ist2!=top_of(g_ist2) || g_tss.ist3!=top_of(g_ist3)) return false;
    physical[0]=(uintptr_t)g_ist1-KERNEL_IMAGE_OFFSET;
    physical[1]=(uintptr_t)g_ist2-KERNEL_IMAGE_OFFSET;
    physical[2]=(uintptr_t)g_ist3-KERNEL_IMAGE_OFFSET;
    return true;
}

bool tss_install_guarded_ist(const uint64_t tops[3])
{
    if (!tops || g_guarded_ist) return false;
    for (unsigned i=0;i<3;++i) {
        stack_owner_view_t v;
        if (!tops[i] || !kernel_stack_guard_owner(tops[i]-IST_STACK_SIZE-1,&v) ||
            v.end!=tops[i] || v.kind!=(stack_kind_t)(STACK_KIND_IST1+i) || v.owned ||
            v.capacity!=IST_STACK_SIZE || v.state!=STACK_ALLOCATED) return false;
    }
    /* Packed TSS fields retain their architectural offsets. Locked XCHG
     * publishes each complete pointer even to asynchronous NMI/MC entry;
     * both old and new payloads remain mapped throughout the handoff. */
    for (unsigned i=0;i<3;++i) {
        uint64_t value=tops[i];
        uint64_t *field=(uint64_t *)((uintptr_t)&g_tss+offsetof(Tss64,ist1)+i*8);
        __asm__ volatile("xchgq %0,%1" : "+r"(value), "+m"(*field) :: "memory");
    }
    if (g_tss.ist1!=tops[0] || g_tss.ist2!=tops[1] || g_tss.ist3!=tops[2]) return false;
    g_guarded_ist=true;return true;
}

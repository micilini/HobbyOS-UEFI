#include "tss.h"
#include "../libc/memory.h"

#define IST_STACK_SIZE (32 * 1024)

__attribute__((aligned(16))) static uint8_t g_ist1[IST_STACK_SIZE];
__attribute__((aligned(16))) static uint8_t g_ist2[IST_STACK_SIZE];
__attribute__((aligned(16))) static uint8_t g_ist3[IST_STACK_SIZE];

static Tss64 g_tss;

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
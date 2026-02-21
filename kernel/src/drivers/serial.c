#include "serial.h"
#include <stddef.h>
#include "../core/spinlock.h"

#ifndef HOBBYOS_MAX_SERIAL_PORTS
#define HOBBYOS_MAX_SERIAL_PORTS 32
#endif

#ifndef HOBBYOS_KERNEL_SERIAL_DEV_PORTS
#define HOBBYOS_KERNEL_SERIAL_DEV_PORTS 0
#endif

static uint16_t g_serial_ports[HOBBYOS_MAX_SERIAL_PORTS];
static uint32_t g_serial_count = 0;
static int g_serial_inited = 0;

extern volatile int g_panic_in_progress;

static spinlock_t g_serial_lock;
static int g_serial_lock_inited = 0;

static uint8_t g_serial_dead[HOBBYOS_MAX_SERIAL_PORTS];
static uint8_t g_serial_fail[HOBBYOS_MAX_SERIAL_PORTS];

static inline void outb_u8(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_u8(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb_u8(0x80, 0);
}

enum
{
    UART_RBR_THR_DLL = 0,
    UART_IER_DLM = 1,
    UART_IIR_FCR = 2,
    UART_LCR = 3,
    UART_MCR = 4,
    UART_LSR = 5,
    UART_SCR = 7,
};

#define UART_LSR_THRE (1u << 5)

static int uart_wait_thre(uint16_t base)
{

    uint8_t first = inb_u8(base + UART_LSR);
    if (first == 0xFF)
        return 0;
    if (first & UART_LSR_THRE)
        return 1;

    for (uint32_t spin = 0; spin < 20000; spin++)
    {
        uint8_t lsr = inb_u8(base + UART_LSR);
        if (lsr == 0xFF)
            return 0;
        if (lsr & UART_LSR_THRE)
            return 1;
    }
    return 0;
}

static int uart_probe(uint16_t base)
{

    uint8_t lsr = inb_u8(base + UART_LSR);
    uint8_t iir = inb_u8(base + UART_IIR_FCR);

    if (lsr == 0xFF && iir == 0xFF)
        return 0;

    uint8_t old = inb_u8(base + UART_LCR);
    outb_u8(base + UART_LCR, (uint8_t)(old ^ 0x03));
    uint8_t now = inb_u8(base + UART_LCR);
    outb_u8(base + UART_LCR, old);

    if (now == 0xFF)
        return 0;
    if (now == old)
        return 0;

    return 1;
}

static void uart_init_115200_8n1(uint16_t base)
{
    outb_u8(base + UART_IER_DLM, 0x00);
    outb_u8(base + UART_LCR, 0x80);
    outb_u8(base + UART_RBR_THR_DLL, 0x01);
    outb_u8(base + UART_IER_DLM, 0x00);
    outb_u8(base + UART_LCR, 0x03);
    outb_u8(base + UART_IIR_FCR, 0xC7);
    outb_u8(base + UART_MCR, 0x03);
    io_wait();
}

static void uart_putc_idx(uint32_t idx, uint16_t base, char c)
{
    if (g_serial_dead[idx])
        return;

    if (c == '\n')
        uart_putc_idx(idx, base, '\r');

    if (!uart_wait_thre(base))
    {
        if (g_serial_fail[idx] < 0xFF)
            g_serial_fail[idx]++;
        if (g_serial_fail[idx] >= 4)
            g_serial_dead[idx] = 1;
        return;
    }

    g_serial_fail[idx] = 0;
    outb_u8(base + UART_RBR_THR_DLL, (uint8_t)c);
}

static void uart_write_idx(uint32_t idx, uint16_t base, const char *s)
{
    while (*s)
        uart_putc_idx(idx, base, *s++);
}

static void uart_write_hex64_idx(uint32_t idx, uint16_t base, uint64_t v)
{
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (v >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        uart_putc_idx(idx, base, c);
    }
}

static void add_port(uint16_t base)
{

    if ((base & 0x7) != 0)
        return;

    for (uint32_t i = 0; i < g_serial_count; i++)
        if (g_serial_ports[i] == base)
            return;

    if (g_serial_count >= HOBBYOS_MAX_SERIAL_PORTS)
        return;

    if (!uart_probe(base))
        return;

    g_serial_ports[g_serial_count++] = base;
}

void serial_init_from_bootinfo(const BootInfo *boot_info)
{
    if (!g_serial_lock_inited)
    {
        spinlock_init(&g_serial_lock);
        g_serial_lock_inited = 1;
    }

    g_serial_count = 0;
    g_serial_inited = 0;

    for (uint32_t i = 0; i < HOBBYOS_MAX_SERIAL_PORTS; i++)
    {
        g_serial_dead[i] = 0;
        g_serial_fail[i] = 0;
    }

    if ((uintptr_t)boot_info < 0x1000)
    {
        add_port(0x3F8);
        add_port(0x2F8);
        add_port(0x3E8);
        add_port(0x2E8);

#if HOBBYOS_KERNEL_SERIAL_DEV_PORTS
        add_port(0x40C0);
        add_port(0x40C8);
#endif

        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_init_115200_8n1(g_serial_ports[i]);

        g_serial_inited = 1;
        return;
    }

    if (boot_info && boot_info->serial.count > 0)
    {
        for (uint32_t i = 0; i < boot_info->serial.count; i++)
        {
            if (boot_info->serial.ports[i].kind != HOBBYOS_SERIAL_KIND_16550_IO)
                continue;

            add_port((uint16_t)boot_info->serial.ports[i].io_base);
        }
    }

#if HOBBYOS_KERNEL_SERIAL_DEV_PORTS
    add_port(0x40C0);
    add_port(0x40C8);
#endif

    if (g_serial_count == 0)
    {
        add_port(0x3F8);
        add_port(0x2F8);
        add_port(0x3E8);
        add_port(0x2E8);
    }

    for (uint32_t i = 0; i < g_serial_count; i++)
        uart_init_115200_8n1(g_serial_ports[i]);

    g_serial_inited = 1;
}

void serial_putc_all(char c)
{
    if (!g_serial_inited)
        return;

    if (!g_panic_in_progress)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);
        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_putc_idx(i, g_serial_ports[i], c);
        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    for (uint32_t i = 0; i < g_serial_count; i++)
        uart_putc_idx(i, g_serial_ports[i], c);
}

void serial_write_all(const char *s)
{
    if (!g_serial_inited || !s)
        return;

    if (!g_panic_in_progress)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);
        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_write_idx(i, g_serial_ports[i], s);
        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    for (uint32_t i = 0; i < g_serial_count; i++)
        uart_write_idx(i, g_serial_ports[i], s);
}

void serial_write_hex64_all(uint64_t v)
{
    if (!g_serial_inited)
        return;

    if (!g_panic_in_progress)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);

        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_write_hex64_idx(i, g_serial_ports[i], v);

        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    for (uint32_t i = 0; i < g_serial_count; i++)
        uart_write_hex64_idx(i, g_serial_ports[i], v);
}

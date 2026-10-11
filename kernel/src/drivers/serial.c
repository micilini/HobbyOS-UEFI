#include "serial.h"
#include <stddef.h>
#include <stdbool.h>
#include "../core/spinlock.h"

#ifndef HOBBYOS_MAX_SERIAL_PORTS
#define HOBBYOS_MAX_SERIAL_PORTS 32
#endif

#ifndef HOBBYOS_KERNEL_SERIAL_DEV_PORTS
#define HOBBYOS_KERNEL_SERIAL_DEV_PORTS 0
#endif

#define UART_NORMAL_POLL_LIMIT 20000u
#define PANIC_UART_TOTAL_POLL_BUDGET 1000000ULL
#define PANIC_SERIAL_MAX_TEXT_BYTES 512u

static uint16_t g_serial_ports[HOBBYOS_MAX_SERIAL_PORTS];
static uint32_t g_serial_count = 0;
static int g_serial_inited = 0;

extern volatile int g_panic_in_progress;

static spinlock_t g_serial_lock;
static int g_serial_lock_inited = 0;

static uint8_t g_serial_dead[HOBBYOS_MAX_SERIAL_PORTS];
static uint8_t g_serial_fail[HOBBYOS_MAX_SERIAL_PORTS];
static volatile uint64_t g_panic_serial_poll_budget;

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#if !defined(HOBBYOS_SELFTEST) || !HOBBYOS_IRQ_LATENCY
#error Serial proof transport requires the measured SELFTEST build
#endif
#include "../libc/string.h"
#define SERIAL_PROOF_PENDING_BYTES 65536u
#define SERIAL_PROOF_DRAIN_BYTES 1048576u
static unsigned char g_serial_proof_pending[SERIAL_PROOF_PENDING_BYTES];
static size_t g_serial_proof_pending_length, g_serial_proof_pending_head;
static uint64_t g_serial_proof_session;
static uint32_t g_serial_proof_ports;
static bool g_serial_proof_failed;
static uint64_t g_serial_aux_sequence, g_serial_aux_bytes;
static uint32_t g_serial_proof_failed_ports;

/* Called only under g_serial_lock. Never retains the caller's pointer. */
static void serial_proof_queue_byte(unsigned char c)
{
    size_t needed = c == '\n' ? 2u : 1u;
    if (needed > SERIAL_PROOF_PENDING_BYTES - g_serial_proof_pending_length) {
        __atomic_store_n(&g_serial_proof_failed, true, __ATOMIC_RELEASE);
        return;
    }
    size_t tail=(g_serial_proof_pending_head+g_serial_proof_pending_length)%SERIAL_PROOF_PENDING_BYTES;
    if (c == '\n') {
        g_serial_proof_pending[tail]='\r';
        tail=(tail+1u)%SERIAL_PROOF_PENDING_BYTES;
        ++g_serial_proof_pending_length;
    }
    g_serial_proof_pending[tail]=c;
    ++g_serial_proof_pending_length;
}

static void serial_proof_queue_text(const char *s)
{
    while (*s && !__atomic_load_n(&g_serial_proof_failed, __ATOMIC_ACQUIRE))
        serial_proof_queue_byte((unsigned char)*s++);
}
#endif

#ifdef HOBBYOS_PANIC_TEST
volatile uint32_t g_panic_test_uart_unresponsive;
volatile uint64_t g_panic_test_uart_polls;
#endif

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

static bool panic_uart_take_poll(void)
{
    uint64_t old = __atomic_load_n(&g_panic_serial_poll_budget,
                                   __ATOMIC_RELAXED);
    while (old)
    {
        if (__atomic_compare_exchange_n(&g_panic_serial_poll_budget, &old,
                                        old - 1u, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

static uint8_t uart_read_lsr(uint16_t base, int emergency)
{
#ifdef HOBBYOS_PANIC_TEST
    if (emergency && __atomic_load_n(&g_panic_test_uart_unresponsive,
                                     __ATOMIC_ACQUIRE))
    {
        __atomic_add_fetch(&g_panic_test_uart_polls, 1, __ATOMIC_RELAXED);
        return 0;
    }
#else
    (void)emergency;
#endif
    return inb_u8(base + UART_LSR);
}

static int uart_wait_thre(uint16_t base, int emergency)
{
    if (emergency && !panic_uart_take_poll())
        return 0;

    uint8_t first = uart_read_lsr(base, emergency);
    if (first == 0xFF)
        return 0;
    if (first & UART_LSR_THRE)
        return 1;

    for (uint32_t spin = 1; spin < UART_NORMAL_POLL_LIMIT; spin++)
    {
        if (emergency && !panic_uart_take_poll())
            return 0;
        uint8_t lsr = uart_read_lsr(base, emergency);
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

static void uart_putc_idx(uint32_t idx, uint16_t base, char c, int emergency)
{
    if (g_serial_dead[idx])
        return;

    if (c == '\n')
        uart_putc_idx(idx, base, '\r', emergency);

    if (!uart_wait_thre(base, emergency))
    {
        if (g_serial_fail[idx] < 0xFF)
            g_serial_fail[idx]++;
        if (g_serial_fail[idx] >= 4)
            g_serial_dead[idx] = 1;
        if (emergency &&
            __atomic_load_n(&g_panic_serial_poll_budget,
                            __ATOMIC_RELAXED) == 0)
            g_serial_dead[idx] = 1;
        return;
    }

    g_serial_fail[idx] = 0;
    outb_u8(base + UART_RBR_THR_DLL, (uint8_t)c);
}

static void uart_write_idx(uint32_t idx, uint16_t base, const char *s,
                           int emergency)
{
    uint32_t count = 0;
    while (*s && (!emergency || count < PANIC_SERIAL_MAX_TEXT_BYTES))
    {
        uart_putc_idx(idx, base, *s++, emergency);
        count++;
    }
}

static void uart_write_hex64_idx(uint32_t idx, uint16_t base, uint64_t v,
                                 int emergency)
{
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (v >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        uart_putc_idx(idx, base, c, emergency);
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
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    __atomic_store_n(&g_serial_proof_failed_ports,0,__ATOMIC_RELEASE);
#endif
    __atomic_store_n(&g_panic_serial_poll_budget,
                     PANIC_UART_TOTAL_POLL_BUDGET, __ATOMIC_RELAXED);
#ifdef HOBBYOS_PANIC_TEST
    __atomic_store_n(&g_panic_test_uart_unresponsive, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_panic_test_uart_polls, 0, __ATOMIC_RELAXED);
#endif

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

    int emergency = __atomic_load_n(&g_panic_in_progress,
                                    __ATOMIC_ACQUIRE) != 0;
    if (!emergency)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        if (g_serial_proof_session) {
            serial_proof_queue_byte((unsigned char)c);
            spin_unlock_irqrestore(&g_serial_lock, flags);
            return;
        }
#endif
        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_putc_idx(i, g_serial_ports[i], c, 0);
        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    uint32_t count = g_serial_count;
    if (count > HOBBYOS_MAX_SERIAL_PORTS)
        count = HOBBYOS_MAX_SERIAL_PORTS;
    for (uint32_t i = 0; i < count; i++)
        uart_putc_idx(i, g_serial_ports[i], c, 1);
}

void serial_write_all(const char *s)
{
    if (!g_serial_inited || !s)
        return;

    int emergency = __atomic_load_n(&g_panic_in_progress,
                                    __ATOMIC_ACQUIRE) != 0;
    if (!emergency)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        if (g_serial_proof_session) {
            serial_proof_queue_text(s);
            spin_unlock_irqrestore(&g_serial_lock, flags);
            return;
        }
#endif
        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_write_idx(i, g_serial_ports[i], s, 0);
        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    uint32_t count = g_serial_count;
    if (count > HOBBYOS_MAX_SERIAL_PORTS)
        count = HOBBYOS_MAX_SERIAL_PORTS;
    for (uint32_t i = 0; i < count; i++)
        uart_write_idx(i, g_serial_ports[i], s, 1);
}

void serial_write_hex64_all(uint64_t v)
{
    if (!g_serial_inited)
        return;

    int emergency = __atomic_load_n(&g_panic_in_progress,
                                    __ATOMIC_ACQUIRE) != 0;
    if (!emergency)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_serial_lock);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        if (g_serial_proof_session) {
            for (int i=60; i>=0; i-=4) {
                unsigned digit=(unsigned)((v >> i) & 15u);
                serial_proof_queue_byte((unsigned char)(digit < 10u ? '0'+digit : 'A'+digit-10u));
            }
            spin_unlock_irqrestore(&g_serial_lock, flags);
            return;
        }
#endif

        for (uint32_t i = 0; i < g_serial_count; i++)
            uart_write_hex64_idx(i, g_serial_ports[i], v, 0);

        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }

    uint32_t count = g_serial_count;
    if (count > HOBBYOS_MAX_SERIAL_PORTS)
        count = HOBBYOS_MAX_SERIAL_PORTS;
    for (uint32_t i = 0; i < count; i++)
        uart_write_hex64_idx(i, g_serial_ports[i], v, 1);
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
bool serial_proof_begin(uint64_t session)
{
    if (!session || !irq_is_enabled() || !g_serial_inited ||
        __atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_serial_lock);
    bool ok=!g_serial_proof_session && g_serial_count && g_serial_count<=32u;
    uint32_t ports=0;
    if (ok) for (uint32_t i=0; i<g_serial_count; ++i)
        if (!g_serial_dead[i]) ports |= UINT32_C(1) << i;
    ok=ok && ports;
    if (ok) {
        g_serial_proof_pending_length=0;
        g_serial_proof_pending_head=0;
        g_serial_proof_ports=ports;
        __atomic_store_n(&g_serial_proof_failed, false, __ATOMIC_RELEASE);
        __atomic_store_n(&g_serial_proof_session, session, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_serial_lock, flags);
    return ok;
}

/* UART polling and every output instruction run with IF=1, outside locks.
 * A checked-write failure quarantines the port until UART initialization.
 * Rejoining at the next record could put a complete footer after lost bytes. */
static bool serial_proof_raw_byte(unsigned char c)
{
    if (!irq_is_enabled() || __atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE))
        return false;
    for (uint32_t i=0; i<g_serial_count; ++i) if (g_serial_proof_ports & (UINT32_C(1) << i)) {
        if (!uart_wait_thre(g_serial_ports[i], 0)) {
            g_serial_proof_ports &= ~(UINT32_C(1) << i);
            irq_flags_t flags=spin_lock_irqsave(&g_serial_lock);
            g_serial_dead[i]=1;
            __atomic_fetch_or(&g_serial_proof_failed_ports,UINT32_C(1)<<i,__ATOMIC_RELEASE);
            spin_unlock_irqrestore(&g_serial_lock,flags);
            continue;
        }
        if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) return false;
        outb_u8(g_serial_ports[i] + UART_RBR_THR_DLL, c);
    }
    return g_serial_proof_ports != 0;
}

bool serial_proof_write(uint64_t session, const void *bytes, size_t length)
{
    if (!session || !irq_is_enabled() || (length && !bytes) ||
        __atomic_load_n(&g_serial_proof_session, __ATOMIC_ACQUIRE)!=session ||
        __atomic_load_n(&g_serial_proof_failed, __ATOMIC_ACQUIRE)) return false;
    const unsigned char *data=bytes;
    for (size_t i=0; i<length; ++i)
        if (__atomic_load_n(&g_serial_proof_failed, __ATOMIC_ACQUIRE) ||
            !serial_proof_raw_byte(data[i])) {
            __atomic_store_n(&g_serial_proof_failed, true, __ATOMIC_RELEASE);
            return false;
        }
    return true;
}

static bool serial_proof_drain(uint64_t session, bool close)
{
    if (!session || !irq_is_enabled() ||
        __atomic_load_n(&g_serial_proof_session, __ATOMIC_ACQUIRE)!=session) return false;
    size_t drained=0;
    for (;;) {
        unsigned char chunk[128];
        irq_flags_t flags=spin_lock_irqsave(&g_serial_lock);
        bool ok=!g_serial_proof_failed && !g_panic_in_progress;
        if (g_serial_proof_pending_length && drained==SERIAL_PROOF_DRAIN_BYTES) {
            __atomic_store_n(&g_serial_proof_failed,true,__ATOMIC_RELEASE);
            ok=false;
        }
        size_t count=ok ? g_serial_proof_pending_length : 0;
        if (count>sizeof(chunk)) count=sizeof(chunk);
        if (count>SERIAL_PROOF_DRAIN_BYTES-drained) count=SERIAL_PROOF_DRAIN_BYTES-drained;
        for (size_t i=0;i<count;++i)
            chunk[i]=g_serial_proof_pending[(g_serial_proof_pending_head+i)%SERIAL_PROOF_PENDING_BYTES];
        g_serial_proof_pending_head=(g_serial_proof_pending_head+count)%SERIAL_PROOF_PENDING_BYTES;
        g_serial_proof_pending_length-=count;
        if (!count && close) __atomic_store_n(&g_serial_proof_session,0,__ATOMIC_RELEASE);
        spin_unlock_irqrestore(&g_serial_lock, flags);
        if (!count) return ok;
        /* Copy before reclaiming ring slots. Encode ordinary output so even
         * partial lines remain exact, separate from immutable proof records. */
        char encoded[257],line[448];
        static const char hex[]="0123456789abcdef";
        uint64_t digest=UINT64_C(14695981039346656037);
        for (size_t i=0;i<count;++i) {
            encoded[2*i]=hex[chunk[i]>>4];encoded[2*i+1]=hex[chunk[i]&15u];
            digest=(digest^chunk[i])*UINT64_C(1099511628211);
        }
        encoded[2*count]=0;
        if (g_serial_aux_sequence==UINT64_MAX || g_serial_aux_bytes>UINT64_MAX-count) {
            __atomic_store_n(&g_serial_proof_failed,true,__ATOMIC_RELEASE);
            continue;
        }
        int n=ksnprintf(line,sizeof(line),"[SERIAL_AUX] build=0x%llx seq=%llu bytes=%llu data=%s fnv1a=0x%llx\n",
            (unsigned long long)HOBBYOS_SERIAL_PROOF_ID,
            (unsigned long long)(g_serial_aux_sequence+1u),(unsigned long long)count,encoded,
            (unsigned long long)digest);
        if (n<1 || (size_t)n>=sizeof(line) || !serial_proof_write(session,line,(size_t)n)) {
            __atomic_store_n(&g_serial_proof_failed,true,__ATOMIC_RELEASE);
            continue;
        }
        flags=spin_lock_irqsave(&g_serial_lock);
        ++g_serial_aux_sequence;g_serial_aux_bytes+=count;
        spin_unlock_irqrestore(&g_serial_lock,flags);
        drained+=count;
    }
}

bool serial_proof_flush(uint64_t session) { return serial_proof_drain(session,false); }
bool serial_proof_end(uint64_t session) { return serial_proof_drain(session,true); }

void serial_proof_aux_totals(uint64_t *records,uint64_t *bytes)
{
    irq_flags_t flags=spin_lock_irqsave(&g_serial_lock);
    *records=g_serial_aux_sequence;*bytes=g_serial_aux_bytes;
    spin_unlock_irqrestore(&g_serial_lock,flags);
}

uint32_t serial_proof_failure_mask(void)
{
    return __atomic_load_n(&g_serial_proof_failed_ports,__ATOMIC_ACQUIRE);
}

bool serial_proof_record(const void *bytes, size_t length)
{
    /* One bounded diagnostic record, with ordinary writers queued until its
     * checked transmission ends. No UART polling holds the serial lock. */
    if (!bytes || !length || length > 768u || !serial_proof_begin(UINT64_MAX)) return false;
    bool sent=serial_proof_write(UINT64_MAX,bytes,length);
    bool closed=serial_proof_end(UINT64_MAX);
    return sent && closed;
}
#endif

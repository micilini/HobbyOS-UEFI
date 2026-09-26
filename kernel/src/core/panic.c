#include "panic.h"

#include "idt.h"
#include "interrupts.h"
#include "io.h"
#include "scheduler.h"
#include "../acpi/acpi.h"
#include "../acpi/sleep.h"
#include "../apic/lapic.h"
#include "../drivers/serial.h"
#include "../smp/smp_boot.h"
#include "../smp/smp_topology.h"
#include "../cpu/mmio.h"
#include "../libc/string.h"

#ifdef HOBBYOS_ASSERT_TEST
#include "assert_selftest.h"
#endif
#ifdef HOBBYOS_PANIC_TEST
#include "spinlock.h"
#endif

#include <stddef.h>
#include <stdint.h>

#define PANIC_TIMEOUT_MIN_SECONDS 1u
#define PANIC_TIMEOUT_MAX_SECONDS 30u
#define PANIC_REASON_MAX_BYTES 240u
#define ASSERTION_FILE_MAX_BYTES 96u
#define ASSERTION_EXPRESSION_MAX_BYTES 160u
#define ASSERTION_WARNING_MAX_BYTES 1024u
#define PANIC_COUNTDOWN_POLLS_PER_SECOND 50000000ULL
#define PANIC_COUNTDOWN_POLL_LIMIT 500000000ULL
#define PANIC_COUNTDOWN_STALL_LIMIT 2000000ULL
#define PANIC_FALLBACK_POLLS_PER_SECOND 1000000ULL
#define PANIC_ACPI_TABLE_LIMIT (16u * 1024u * 1024u)
#define PANIC_ACPI_SLP_TYP_SHIFT 10u
#define PANIC_ACPI_SLP_TYP_MASK (7u << PANIC_ACPI_SLP_TYP_SHIFT)
#define PANIC_ACPI_SLP_EN (1u << 13)

volatile int g_panic_in_progress = 0;

/* High 32 bits: architectural CPU ID plus one. Low 32 bits: depth. */
static volatile uint64_t g_panic_owner;
static volatile uint32_t g_panic_action = PANIC_ACTION_RESTART;
static volatile uint32_t g_panic_timeout_seconds = 8u;

typedef enum
{
    PANIC_ENTRY_OWNER = 1,
    PANIC_ENTRY_REENTRY = 2,
    PANIC_ENTRY_FINAL_REENTRY = 3,
    PANIC_ENTRY_PEER = 4
} panic_entry_kind_t;

typedef struct
{
    uint32_t cpu_id;
    cpu_slot_t slot;
    uint8_t slot_valid;
    uint8_t source;
} panic_identity_t;

#if HOBBYOS_DEBUG_ASSERT
typedef struct
{
    uint64_t id;
    uint64_t generation;
    uint8_t valid;
} assertion_task_identity_t;

typedef struct
{
    uint32_t bytes;
    uint8_t valid;
    uint8_t truncated;
} assertion_text_info_t;
#endif

typedef struct
{
    uint16_t pm1a_port;
    uint16_t pm1b_port;
    uint8_t typa;
    uint8_t typb;
    volatile uint8_t valid;
} panic_shutdown_state_t;

static panic_shutdown_state_t g_panic_shutdown;

#ifdef HOBBYOS_PANIC_TEST
panic_test_state_t g_panic_test_state;
extern volatile uint32_t g_panic_test_uart_unresponsive;

/* Keep the debugger transport's compact, versioned memory layout explicit. */
_Static_assert(offsetof(panic_test_state_t, console_lock_addr) == 64,
               "panic test state console lock offset");
_Static_assert(offsetof(panic_test_state_t, countdown_samples) == 104,
               "panic test state sample offset");
_Static_assert(sizeof(panic_test_state_t) == 112,
               "panic test state size");
#endif

static void panic_cpuid(uint32_t leaf, uint32_t subleaf,
                        uint32_t *eax, uint32_t *ebx,
                        uint32_t *ecx, uint32_t *edx)
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
    if (eax)
        *eax = a;
    if (ebx)
        *ebx = b;
    if (ecx)
        *ecx = c;
    if (edx)
        *edx = d;
}

static uint32_t panic_complete_topology_count(void)
{
    uint32_t count = __atomic_load_n(&g_cpu_count, __ATOMIC_ACQUIRE);
    if (count == 0 || count > HOBBYOS_MAX_CPUS)
        return 0;

    uint32_t bsp_count = 0;
    for (cpu_slot_t slot = 0; slot < count; slot++)
    {
        uint32_t state = __atomic_load_n(&g_cpus[slot].state,
                                         __ATOMIC_ACQUIRE);
        if (state == CPU_STATE_DEAD || g_cpus[slot].slot != slot)
            return 0;
        if (g_cpus[slot].is_bsp)
            bsp_count++;
        for (cpu_slot_t prior = 0; prior < slot; prior++)
            if (g_cpus[prior].apic_id == g_cpus[slot].apic_id)
                return 0;
    }
    return bsp_count == 1u ? count : 0;
}

static panic_identity_t panic_local_identity(void)
{
    panic_identity_t identity = {
        .cpu_id = 0,
        .slot = CPU_SLOT_INVALID,
        .slot_valid = 0,
        .source = 0};
    uint32_t max_leaf = 0;
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    panic_cpuid(0, 0, &max_leaf, NULL, NULL, NULL);
    if (max_leaf >= 0x1Fu)
    {
        panic_cpuid(0x1Fu, 0, &eax, &ebx, &ecx, &edx);
        if (ebx != 0 && edx != UINT32_MAX)
        {
            identity.cpu_id = edx;
            identity.source = 1;
        }
    }
    if (!identity.source && max_leaf >= 0xBu)
    {
        panic_cpuid(0xBu, 0, &eax, &ebx, &ecx, &edx);
        if (ebx != 0 && edx != UINT32_MAX)
        {
            identity.cpu_id = edx;
            identity.source = 2;
        }
    }
    if (!identity.source && max_leaf >= 1u)
    {
        panic_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        identity.cpu_id = ebx >> 24;
        identity.source = 3;
    }

    return identity;
}

static void panic_resolve_topology(panic_identity_t *identity)
{
    if (!identity)
        return;
    uint32_t count = panic_complete_topology_count();
    if (count)
    {
        for (cpu_slot_t slot = 0; slot < count; slot++)
        {
            if (g_cpus[slot].apic_id == identity->cpu_id)
            {
                identity->slot = slot;
                identity->slot_valid = 1;
                break;
            }
        }
    }
}

#if HOBBYOS_DEBUG_ASSERT
static assertion_task_identity_t assertion_capture_task_identity(void)
{
    assertion_task_identity_t identity = {0};
    irq_flags_t flags = irq_save();
    task_t *task = get_current_task();
    if (task && task->id != TASK_ID_INVALID &&
        task->lifecycle_generation != 0)
    {
        identity.id = task->id;
        identity.generation = task->lifecycle_generation;
        identity.valid = 1;
    }
    irq_restore(flags);
    return identity;
}

static assertion_text_info_t assertion_measure_text(const char *text,
                                                     uint32_t limit)
{
    assertion_text_info_t info = {0};
    if (!text)
        return info;
    info.valid = 1;
    while (info.bytes < limit && text[info.bytes])
        info.bytes++;
    if (info.bytes == limit && text[info.bytes])
        info.truncated = 1;
    return info;
}

static void assertion_hex_encode(char *dst, const char *text,
                                 assertion_text_info_t info)
{
    static const char digits[] = "0123456789abcdef";
    for (uint32_t index = 0; index < info.bytes; index++)
    {
        uint8_t value = (uint8_t)text[index];
        dst[index * 2u] = digits[value >> 4];
        dst[index * 2u + 1u] = digits[value & 0x0Fu];
    }
    dst[info.bytes * 2u] = '\0';
}

void assertion_warn_report(const assertion_context_t *context)
{
    irq_flags_t flags = irq_save();
    panic_identity_t cpu = panic_local_identity();
    panic_resolve_topology(&cpu);
    assertion_task_identity_t task = assertion_capture_task_identity();
#ifdef HOBBYOS_ASSERT_TEST
    assertion_test_context_observe(
        cpu.cpu_id, (uint32_t)cpu.slot,
        (cpu.source != 0 ? 1u : 0u) |
        (cpu.slot_valid ? 2u : 0u) | (task.valid ? 4u : 0u),
        task.id, task.generation);
#endif
    assertion_text_info_t file = assertion_measure_text(
        context ? context->file : NULL, ASSERTION_FILE_MAX_BYTES);
    assertion_text_info_t expression = assertion_measure_text(
        context ? context->expression : NULL,
        ASSERTION_EXPRESSION_MAX_BYTES);
    char file_hex[ASSERTION_FILE_MAX_BYTES * 2u + 1u];
    char expression_hex[ASSERTION_EXPRESSION_MAX_BYTES * 2u + 1u];
    char line[ASSERTION_WARNING_MAX_BYTES];
    assertion_hex_encode(file_hex, context ? context->file : NULL, file);
    assertion_hex_encode(expression_hex,
                         context ? context->expression : NULL, expression);
    uint32_t source_line = context ? context->line : 0;
    uint32_t if_enabled = (flags & (1ULL << 9)) != 0;
    int result = ksnprintf(
        line, sizeof(line),
        "[ASSERT][WARN] file_hex=%s file_bytes=%u file_valid=%u "
        "file_truncated=%u line=%u expression_hex=%s expression_bytes=%u "
        "expression_valid=%u expression_truncated=%u cpu_id=%u "
        "cpu_valid=%u cpu_slot=%u slot_valid=%u task_id=%llu "
        "task_generation=%llu task_valid=%u if_enabled=%u\n",
        file_hex, file.bytes, file.valid, file.truncated, source_line,
        expression_hex, expression.bytes, expression.valid,
        expression.truncated, cpu.cpu_id, cpu.source != 0,
        (uint32_t)cpu.slot, cpu.slot_valid,
        (unsigned long long)task.id,
        (unsigned long long)task.generation, task.valid, if_enabled);
    if (result < 0 || (size_t)result >= sizeof(line))
        serial_write_all("[ASSERT][WARN] format_error=1\n");
    else
        serial_write_all(line);
    irq_restore(flags);
}
#endif

static const char *panic_identity_source_name(uint8_t source)
{
    if (source == 1)
        return "cpuid-1f";
    if (source == 2)
        return "cpuid-0b";
    if (source == 3)
        return "cpuid-01";
    return "cpuid-fallback";
}

static uint64_t panic_owner_token(uint32_t cpu_id)
{
    return (uint64_t)cpu_id + 1ULL;
}

static panic_entry_kind_t panic_claim(const panic_identity_t *identity,
                                      uint32_t *out_depth)
{
    uint64_t token = panic_owner_token(identity->cpu_id);
    for (;;)
    {
        uint64_t old = __atomic_load_n(&g_panic_owner, __ATOMIC_ACQUIRE);
        if (old == 0)
        {
            uint64_t desired = (token << 32) | 1u;
            if (__atomic_compare_exchange_n(&g_panic_owner, &old, desired,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
            {
                __atomic_store_n(&g_panic_in_progress, 1, __ATOMIC_RELEASE);
                *out_depth = 1;
                return PANIC_ENTRY_OWNER;
            }
            continue;
        }

        if ((old >> 32) != token)
        {
            *out_depth = 0;
            return PANIC_ENTRY_PEER;
        }

        uint32_t depth = (uint32_t)old;
        if (depth >= 3u)
        {
            *out_depth = depth;
            return PANIC_ENTRY_FINAL_REENTRY;
        }
        uint64_t desired = (token << 32) | (uint64_t)(depth + 1u);
        if (__atomic_compare_exchange_n(&g_panic_owner, &old, desired,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
        {
            __atomic_store_n(&g_panic_in_progress, 1, __ATOMIC_RELEASE);
            *out_depth = depth + 1u;
            return depth + 1u >= 3u ? PANIC_ENTRY_FINAL_REENTRY
                                    : PANIC_ENTRY_REENTRY;
        }
    }
}

bool panic_current_cpu_is_owner(void)
{
    panic_identity_t identity = panic_local_identity();
    uint64_t state = __atomic_load_n(&g_panic_owner, __ATOMIC_ACQUIRE);
    return state != 0 && (state >> 32) == panic_owner_token(identity.cpu_id);
}

void panic_halt_secondary(void)
{
    __asm__ volatile("cli" ::: "memory");
    for (;;)
        __asm__ volatile("hlt" ::: "memory");
}

static void panic_emit_u64(uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count)
        serial_putc_all(digits[--count]);
}

static void panic_emit_bool(uint8_t value)
{
    serial_write_all(value ? "1" : "0");
}

static void panic_emit_action_name(PanicAction action)
{
    if (action == PANIC_ACTION_HALT)
        serial_write_all("halt");
    else if (action == PANIC_ACTION_RESTART)
        serial_write_all("restart");
    else if (action == PANIC_ACTION_SHUTDOWN)
        serial_write_all("shutdown");
    else
        serial_write_all("invalid");
}

static void panic_emit_limited_text(const char *text)
{
    if (!text)
    {
        serial_write_all("(null)");
        return;
    }
    for (uint32_t index = 0; index < PANIC_REASON_MAX_BYTES; index++)
    {
        char value = text[index];
        if (!value)
            return;
        if (value == '\n' || value == '\r' || value == '\t')
            value = ' ';
        serial_putc_all(value);
    }
    serial_write_all("...(bounded)");
}

#if HOBBYOS_DEBUG_ASSERT
static void panic_emit_hex_text(const char *text, uint32_t bytes)
{
    static const char digits[] = "0123456789abcdef";
    for (uint32_t index = 0; index < bytes; index++)
    {
        uint8_t value = (uint8_t)text[index];
        serial_putc_all(digits[value >> 4]);
        serial_putc_all(digits[value & 0x0Fu]);
    }
}

static void panic_emit_assertion(const assertion_context_t *context,
                                 const panic_identity_t *cpu,
                                 const assertion_task_identity_t *task)
{
    assertion_text_info_t file = assertion_measure_text(
        context ? context->file : NULL, ASSERTION_FILE_MAX_BYTES);
    assertion_text_info_t expression = assertion_measure_text(
        context ? context->expression : NULL,
        ASSERTION_EXPRESSION_MAX_BYTES);
    serial_write_all("[ASSERT][BUG] file_hex=");
    if (file.valid)
        panic_emit_hex_text(context->file, file.bytes);
    serial_write_all(" file_bytes=");
    panic_emit_u64(file.bytes);
    serial_write_all(" file_valid=");
    panic_emit_bool(file.valid);
    serial_write_all(" file_truncated=");
    panic_emit_bool(file.truncated);
    serial_write_all(" line=");
    panic_emit_u64(context ? context->line : 0);
    serial_write_all(" expression_hex=");
    if (expression.valid)
        panic_emit_hex_text(context->expression, expression.bytes);
    serial_write_all(" expression_bytes=");
    panic_emit_u64(expression.bytes);
    serial_write_all(" expression_valid=");
    panic_emit_bool(expression.valid);
    serial_write_all(" expression_truncated=");
    panic_emit_bool(expression.truncated);
    serial_write_all(" cpu_id=");
    panic_emit_u64(cpu->cpu_id);
    serial_write_all(" cpu_valid=");
    panic_emit_bool(cpu->source != 0);
    serial_write_all(" cpu_slot=");
    panic_emit_u64(cpu->slot);
    serial_write_all(" slot_valid=");
    panic_emit_bool(cpu->slot_valid);
    serial_write_all(" task_id=");
    panic_emit_u64(task->id);
    serial_write_all(" task_generation=");
    panic_emit_u64(task->generation);
    serial_write_all(" task_valid=");
    panic_emit_bool(task->valid);
    serial_write_all("\n");
}
#endif

static mmio_trace_snapshot_t panic_capture_mmio(void)
{
    mmio_trace_snapshot_t snapshot = {0};
#if HOBBYOS_DEBUG_ASSERT
    (void)mmio_trace_snapshot_current(&snapshot, 8u);
#endif
    return snapshot;
}

static bool panic_signature_is(const char signature[4], const char *wanted)
{
    return signature[0] == wanted[0] && signature[1] == wanted[1] &&
           signature[2] == wanted[2] && signature[3] == wanted[3];
}

static bool panic_fadt_contains(const AcpiFadt *fadt, size_t offset,
                                size_t size)
{
    if (!fadt || fadt->header.length < sizeof(AcpiSdtHeader))
        return false;
    if (offset > fadt->header.length)
        return false;
    return size <= (size_t)fadt->header.length - offset;
}

static uint16_t panic_gas_io16(const AcpiGas *gas)
{
    if (!gas || gas->address_space_id != 1u || gas->address == 0 ||
        gas->address > UINT16_MAX || gas->register_bit_offset != 0u)
        return 0;
    if (gas->register_bit_width && gas->register_bit_width < 16u)
        return 0;
    if (gas->access_size != 0u && gas->access_size != 2u)
        return 0;
    return (uint16_t)gas->address;
}

static void panic_prepare_shutdown(void)
{
    __atomic_store_n(&g_panic_shutdown.valid, 0, __ATOMIC_RELEASE);
    const AcpiFadt *fadt = acpi_get_fadt();
    if (!fadt || !panic_signature_is(fadt->header.signature, "FACP") ||
        fadt->header.length > PANIC_ACPI_TABLE_LIMIT ||
        !panic_fadt_contains(fadt, offsetof(AcpiFadt, pm1_cnt_len),
                             sizeof(fadt->pm1_cnt_len)))
        return;

    const AcpiSdtHeader *dsdt = acpi_get_dsdt();
    if (!dsdt || !panic_signature_is(dsdt->signature, "DSDT") ||
        dsdt->length < sizeof(AcpiSdtHeader) ||
        dsdt->length > PANIC_ACPI_TABLE_LIMIT)
        return;

    uint8_t typa = 0;
    uint8_t typb = 0;
    if (!acpi_get_s5_sleep_types(&typa, &typb) || typa > 7u || typb > 7u)
        return;

    uint16_t pm1a = 0;
    uint16_t pm1b = 0;
    if (fadt->pm1_cnt_len >= 2u && fadt->pm1a_cnt_blk != 0 &&
        fadt->pm1a_cnt_blk <= UINT16_MAX)
        pm1a = (uint16_t)fadt->pm1a_cnt_blk;
    if (fadt->pm1_cnt_len >= 2u && fadt->pm1b_cnt_blk != 0 &&
        fadt->pm1b_cnt_blk <= UINT16_MAX)
        pm1b = (uint16_t)fadt->pm1b_cnt_blk;

    if (!pm1a && panic_fadt_contains(
                     fadt, offsetof(AcpiFadt, x_pm1a_cnt_blk),
                     sizeof(fadt->x_pm1a_cnt_blk)))
        pm1a = panic_gas_io16(&fadt->x_pm1a_cnt_blk);
    if (!pm1b && panic_fadt_contains(
                     fadt, offsetof(AcpiFadt, x_pm1b_cnt_blk),
                     sizeof(fadt->x_pm1b_cnt_blk)))
        pm1b = panic_gas_io16(&fadt->x_pm1b_cnt_blk);
    if (!pm1a && !pm1b)
        return;

    g_panic_shutdown.pm1a_port = pm1a;
    g_panic_shutdown.pm1b_port = pm1b;
    g_panic_shutdown.typa = typa;
    g_panic_shutdown.typb = typb;
    __atomic_store_n(&g_panic_shutdown.valid, 1, __ATOMIC_RELEASE);
}

void panic_config(PanicAction action, uint32_t timeout_seconds)
{
    if (action != PANIC_ACTION_HALT && action != PANIC_ACTION_RESTART &&
        action != PANIC_ACTION_SHUTDOWN)
        action = PANIC_ACTION_HALT;
    if (timeout_seconds < PANIC_TIMEOUT_MIN_SECONDS)
        timeout_seconds = PANIC_TIMEOUT_MIN_SECONDS;
    if (timeout_seconds > PANIC_TIMEOUT_MAX_SECONDS)
        timeout_seconds = PANIC_TIMEOUT_MAX_SECONDS;

    if (action == PANIC_ACTION_SHUTDOWN &&
        !__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE))
        panic_prepare_shutdown();
    __atomic_store_n(&g_panic_timeout_seconds, timeout_seconds,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_panic_action, (uint32_t)action, __ATOMIC_RELEASE);
}

static uint64_t panic_read_tsc(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

static bool panic_tsc_frequency(uint64_t *out_hz)
{
    uint32_t max_leaf = 0;
    uint32_t denominator = 0;
    uint32_t numerator = 0;
    uint32_t crystal_hz = 0;
    uint32_t unused = 0;
    panic_cpuid(0, 0, &max_leaf, NULL, NULL, NULL);
    uint64_t hz = 0;

    if (max_leaf >= 0x15u)
    {
        panic_cpuid(0x15u, 0, &denominator, &numerator, &crystal_hz,
                    &unused);
        if (denominator && numerator && crystal_hz)
        {
            uint64_t quotient = crystal_hz / denominator;
            uint64_t remainder = crystal_hz % denominator;
            if (quotient <= UINT64_MAX / numerator &&
                remainder <= UINT64_MAX / numerator)
            {
                uint64_t whole = quotient * numerator;
                uint64_t fraction =
                    (remainder * numerator) / denominator;
                if (fraction <= UINT64_MAX - whole)
                    hz = whole + fraction;
            }
        }
    }
    if (hz < 1000000ULL || hz > 20000000000ULL)
        return false;
    *out_hz = hz;
    return true;
}

static bool panic_counter_sample(uint64_t *out)
{
#ifdef HOBBYOS_PANIC_TEST
    uint32_t mode = __atomic_load_n(&g_panic_test_state.clock_mode,
                                    __ATOMIC_ACQUIRE);
    uint64_t sample = __atomic_add_fetch(
        &g_panic_test_state.countdown_samples, 1, __ATOMIC_RELAXED);
    uint64_t value = __atomic_load_n(&g_panic_test_state.counter_value,
                                     __ATOMIC_RELAXED);
    if (mode == PANIC_TEST_CLOCK_INVALID)
        return false;
    if (mode == PANIC_TEST_CLOCK_CONSTANT)
    {
        *out = value;
        return true;
    }
    if (mode == PANIC_TEST_CLOCK_REGRESSING)
    {
        *out = value - sample;
        return true;
    }
    if (mode == PANIC_TEST_CLOCK_INTERMITTENT)
    {
        *out = value + sample / 1024ULL;
        return true;
    }
#endif
    *out = panic_read_tsc();
    return true;
}

static uint64_t panic_scaled_poll_budget(uint64_t per_second,
                                         uint32_t seconds,
                                         uint64_t cap)
{
    if (seconds && per_second > UINT64_MAX / seconds)
        return cap;
    uint64_t value = per_second * seconds;
    return value > cap ? cap : value;
}

static void panic_countdown(uint32_t seconds)
{
    uint64_t hz = 0;
    uint64_t start = 0;
    bool frequency_valid = panic_tsc_frequency(&hz);
#ifdef HOBBYOS_PANIC_TEST
    uint32_t test_clock_mode = __atomic_load_n(
        &g_panic_test_state.clock_mode, __ATOMIC_ACQUIRE);
    if (test_clock_mode != PANIC_TEST_CLOCK_NORMAL)
    {
        hz = 1000000ULL;
        frequency_valid = true;
    }
#endif
    bool sample_valid = panic_counter_sample(&start);
    uint64_t poll_limit = panic_scaled_poll_budget(
        PANIC_COUNTDOWN_POLLS_PER_SECOND, seconds,
        PANIC_COUNTDOWN_POLL_LIMIT);
    if (poll_limit < PANIC_COUNTDOWN_STALL_LIMIT)
        poll_limit = PANIC_COUNTDOWN_STALL_LIMIT;

    serial_write_all("[PANIC][COUNTDOWN] state=start seconds=");
    panic_emit_u64(seconds);
    serial_write_all(" source=");
#ifdef HOBBYOS_PANIC_TEST
    if (test_clock_mode != PANIC_TEST_CLOCK_NORMAL)
        serial_write_all("test-counter");
    else
#endif
    serial_write_all(frequency_valid && sample_valid ? "tsc" : "iterations");
    serial_write_all(" hz=");
    panic_emit_u64(frequency_valid ? hz : 0);
    serial_write_all(" poll_limit=");
    panic_emit_u64(poll_limit);
    serial_write_all(" stall_limit=");
    panic_emit_u64(PANIC_COUNTDOWN_STALL_LIMIT);
    serial_write_all("\n");

    if (!frequency_valid || !sample_valid || hz > UINT64_MAX / seconds)
    {
        uint64_t fallback = panic_scaled_poll_budget(
            PANIC_FALLBACK_POLLS_PER_SECOND, seconds,
            PANIC_COUNTDOWN_POLL_LIMIT);
        for (uint64_t poll = 0; poll < fallback; poll++)
            __asm__ volatile("pause" ::: "memory");
        serial_write_all("[PANIC][COUNTDOWN] state=fallback-expired polls=");
        panic_emit_u64(fallback);
        serial_write_all("\n");
        return;
    }

    uint64_t target_delta = hz * seconds;
    uint64_t previous = start;
    uint64_t stalled = 0;
    uint64_t polls = 0;
    const char *state = "iteration-limit";
    for (; polls < poll_limit; polls++)
    {
        uint64_t now = 0;
        bool valid = panic_counter_sample(&now);
        if (!valid || now <= previous)
            stalled++;
        else
            stalled = 0;

        if (valid && now >= start && now - start >= target_delta)
        {
            state = "elapsed";
            break;
        }
        if (stalled >= PANIC_COUNTDOWN_STALL_LIMIT)
        {
            state = valid ? "clock-stalled" : "clock-invalid";
            break;
        }
        if (valid && now > previous)
            previous = now;
        __asm__ volatile("pause" ::: "memory");
    }

    serial_write_all("[PANIC][COUNTDOWN] state=");
    serial_write_all(state);
    serial_write_all(" polls=");
    panic_emit_u64(polls);
    serial_write_all("\n");
}

static void panic_io_delay(uint32_t iterations)
{
    for (uint32_t index = 0; index < iterations; index++)
        io_wait();
}

static void panic_force_triple_fault(void) __attribute__((noreturn));
static void panic_force_triple_fault(void)
{
    struct
    {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {0, 0};
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
    __asm__ volatile("int $3");
    panic_halt_secondary();
}

static void panic_restart(void) __attribute__((noreturn));
static void panic_restart(void)
{
    serial_write_all("[PANIC][ACTION] requested=restart attempt=cf9 budget=200000\n");
    outb(0xCF9, 0x06);
    io_wait();
    outb(0xCF9, 0x0E);
    panic_io_delay(200000u);

    serial_write_all("[PANIC][ACTION] requested=restart attempt=8042 budget=300000\n");
    uint32_t input_budget = 100000u;
    while (input_budget && (inb(0x64) & 0x02u))
    {
        input_budget--;
        io_wait();
    }
    outb(0x64, 0xFE);
    panic_io_delay(200000u);

    serial_write_all("[PANIC][ACTION] requested=restart attempt=triple-fault terminal=1\n");
    panic_force_triple_fault();
}

static void panic_shutdown(void) __attribute__((noreturn));
static void panic_shutdown(void)
{
    if (__atomic_load_n(&g_panic_shutdown.valid, __ATOMIC_ACQUIRE))
    {
        uint16_t pm1a = g_panic_shutdown.pm1a_port;
        uint16_t pm1b = g_panic_shutdown.pm1b_port;
        serial_write_all("[PANIC][ACTION] requested=shutdown attempt=acpi-s5 pm1a=0x");
        serial_write_hex64_all(pm1a);
        serial_write_all(" pm1b=0x");
        serial_write_hex64_all(pm1b);
        serial_write_all("\n");
        if (pm1a)
        {
            uint16_t value = inw(pm1a);
            value &= (uint16_t)~PANIC_ACPI_SLP_TYP_MASK;
            value |= (uint16_t)((uint16_t)g_panic_shutdown.typa
                                << PANIC_ACPI_SLP_TYP_SHIFT);
            value |= PANIC_ACPI_SLP_EN;
            outw(pm1a, value);
            io_wait();
        }
        if (pm1b)
        {
            uint16_t value = inw(pm1b);
            value &= (uint16_t)~PANIC_ACPI_SLP_TYP_MASK;
            value |= (uint16_t)((uint16_t)g_panic_shutdown.typb
                                << PANIC_ACPI_SLP_TYP_SHIFT);
            value |= PANIC_ACPI_SLP_EN;
            outw(pm1b, value);
            io_wait();
        }
        panic_io_delay(400000u);
        serial_write_all("[PANIC][ACTION] requested=shutdown fallback=halt reason=no-effect\n");
    }
    else
    {
        serial_write_all("[PANIC][ACTION] requested=shutdown fallback=halt reason=platform-data-unavailable\n");
    }
    panic_halt_secondary();
}

static void panic_do_action(PanicAction action) __attribute__((noreturn));
static void panic_do_action(PanicAction action)
{
    if (action == PANIC_ACTION_RESTART)
        panic_restart();
    if (action == PANIC_ACTION_SHUTDOWN)
        panic_shutdown();
    serial_write_all("[PANIC][ACTION] requested=halt terminal=halt if=0\n");
    panic_halt_secondary();
}

static void panic_emit_owner(const panic_identity_t *identity,
                             const char *origin, PanicAction action,
                             uint32_t timeout)
{
    serial_write_all("\n[PANIC][OWNER] cpu_id=");
    panic_emit_u64(identity->cpu_id);
    serial_write_all(" cpu_slot=");
    if (identity->slot_valid)
        panic_emit_u64(identity->slot);
    else
        serial_write_all("unknown");
    serial_write_all(" slot_valid=");
    panic_emit_bool(identity->slot_valid);
    serial_write_all(" identity=");
    serial_write_all(panic_identity_source_name(identity->source));
    serial_write_all(" depth=1 origin=");
    serial_write_all(origin);
    serial_write_all(" stage=owner-established action=");
    panic_emit_action_name(action);
    serial_write_all(" timeout_s=");
    panic_emit_u64(timeout);
    serial_write_all("\n");
}

static void panic_emit_frame(InterruptFrame *frame)
{
    if (!frame)
    {
        serial_write_all("[PANIC][FRAME] available=0\n");
        return;
    }
    serial_write_all("[PANIC][FRAME] available=1 rip=0x");
    serial_write_hex64_all(frame->rip);
    serial_write_all(" cs=0x");
    serial_write_hex64_all(frame->cs);
    serial_write_all(" rflags=0x");
    serial_write_hex64_all(frame->rflags);
    serial_write_all(" rsp=0x");
    serial_write_hex64_all(frame->rsp);
    serial_write_all(" ss=0x");
    serial_write_hex64_all(frame->ss);
    serial_write_all("\n");
}

static void panic_emit_page_fault(uint64_t error_code)
{
    serial_write_all("[PANIC][PAGE_FAULT] present=");
    panic_emit_u64((error_code >> 0) & 1u);
    serial_write_all(" write=");
    panic_emit_u64((error_code >> 1) & 1u);
    serial_write_all(" user=");
    panic_emit_u64((error_code >> 2) & 1u);
    serial_write_all(" reserved=");
    panic_emit_u64((error_code >> 3) & 1u);
    serial_write_all(" instruction=");
    panic_emit_u64((error_code >> 4) & 1u);
    serial_write_all("\n");
}

static void panic_emit_mmio(const mmio_trace_snapshot_t *snapshot)
{
#if HOBBYOS_DEBUG_ASSERT
    serial_write_all("[PANIC][MMIO] available=");
    panic_emit_bool(snapshot->coherent);
    serial_write_all(" trace=per-cpu coherence=sequence cpu_id=");
    panic_emit_u64(snapshot->cpu_id);
    serial_write_all(" cpu_valid=");
    panic_emit_bool(snapshot->cpu_valid);
    serial_write_all(" cpu_slot=");
    panic_emit_u64(snapshot->slot);
    serial_write_all(" slot_valid=");
    panic_emit_bool(snapshot->slot_valid);
    if (snapshot->coherent)
    {
        serial_write_all(" addr=0x");
        serial_write_hex64_all(snapshot->address);
        serial_write_all(" operation=");
        serial_write_all(snapshot->operation == MMIO_TRACE_OPERATION_WRITE
                             ? "write" : "read");
        serial_write_all(" width=");
        panic_emit_u64(snapshot->width);
        serial_write_all(" phase=");
        serial_write_all(snapshot->phase == MMIO_TRACE_PHASE_COMPLETE
                             ? "complete" : "attempt");
        serial_write_all(" value_valid=");
        panic_emit_bool(snapshot->value_valid);
        if (snapshot->value_valid)
        {
            serial_write_all(" value=0x");
            serial_write_hex64_all(snapshot->value);
        }
    }
    serial_write_all("\n");
#else
    (void)snapshot;
    serial_write_all("[PANIC][MMIO] available=0 trace=disabled\n");
#endif
}

#ifdef HOBBYOS_PANIC_TEST
static void panic_test_fault_full(void)
{
    uint32_t scenario = __atomic_load_n(&g_panic_test_state.scenario,
                                        __ATOMIC_ACQUIRE);
    if (scenario != PANIC_TEST_SCENARIO_REENTRY &&
        scenario != PANIC_TEST_SCENARIO_SECOND_REENTRY)
        return;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_panic_test_state.reentry_faults,
                                    &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __asm__ volatile(".globl panic_test_full_ud2_site\n"
                         "panic_test_full_ud2_site:\n\tud2" ::: "memory");
}

static void panic_test_fault_minimal(void)
{
    if (__atomic_load_n(&g_panic_test_state.scenario, __ATOMIC_ACQUIRE) !=
        PANIC_TEST_SCENARIO_SECOND_REENTRY)
        return;
    uint32_t expected = 1;
    if (__atomic_compare_exchange_n(&g_panic_test_state.reentry_faults,
                                    &expected, 2, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __asm__ volatile(".globl panic_test_minimal_ud2_site\n"
                         "panic_test_minimal_ud2_site:\n\tud2" ::: "memory");
}
#endif

static void panic_emit_reentry(const panic_identity_t *identity,
                               uint32_t depth, uint8_t vector,
                               int vector_known, PanicAction action)
{
    serial_write_all("[PANIC][REENTRY] cpu_id=");
    panic_emit_u64(identity->cpu_id);
    serial_write_all(" cpu_slot=");
    if (identity->slot_valid)
        panic_emit_u64(identity->slot);
    else
        serial_write_all("unknown");
    serial_write_all(" depth=");
    panic_emit_u64(depth);
    serial_write_all(" vector_known=");
    panic_emit_bool((uint8_t)vector_known);
    if (vector_known)
    {
        serial_write_all(" vector=");
        panic_emit_u64(vector);
    }
    serial_write_all(" path=minimal action=");
    panic_emit_action_name(action);
    serial_write_all("\n");
#ifdef HOBBYOS_PANIC_TEST
    panic_test_fault_minimal();
#endif
    panic_do_action(action);
}

static void panic_emit_exception(const char *title, uint8_t vector,
                                 InterruptFrame *frame,
                                 uint64_t error_code, int has_error_code,
                                 uint64_t cr2, int has_cr2,
                                 const char *origin
#if HOBBYOS_DEBUG_ASSERT
                                 , const assertion_context_t *assertion
#endif
                                 )
    __attribute__((noreturn));
static void panic_emit_exception(const char *title, uint8_t vector,
                                 InterruptFrame *frame,
                                 uint64_t error_code, int has_error_code,
                                 uint64_t cr2, int has_cr2,
                                 const char *origin
#if HOBBYOS_DEBUG_ASSERT
                                 , const assertion_context_t *assertion
#endif
                                 )
{
    /* Keep the owner on this path once emergency coordination starts. */
    __asm__ volatile("cli" ::: "memory");
    panic_identity_t identity = panic_local_identity();
    uint32_t depth = 0;
    panic_entry_kind_t entry = panic_claim(&identity, &depth);
    if (entry == PANIC_ENTRY_PEER || entry == PANIC_ENTRY_FINAL_REENTRY)
        panic_halt_secondary();

    PanicAction action = (PanicAction)__atomic_load_n(
        &g_panic_action, __ATOMIC_ACQUIRE);
    if (entry == PANIC_ENTRY_REENTRY)
        panic_emit_reentry(&identity, depth, vector, vector != 0xFFu,
                           action);

    mmio_trace_snapshot_t mmio = panic_capture_mmio();
    panic_resolve_topology(&identity);
#if HOBBYOS_DEBUG_ASSERT
    assertion_task_identity_t assertion_task = {0};
    if (assertion)
        assertion_task = assertion_capture_task_identity();
#endif
    uint32_t timeout = __atomic_load_n(&g_panic_timeout_seconds,
                                       __ATOMIC_ACQUIRE);
    panic_emit_owner(&identity, origin, action, timeout);

#if defined(HOBBYOS_PANIC_TEST) && defined(HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT)
    if (g_panic_test_state.console_lock_addr)
    {
        serial_write_all("[PANIC_TEST][NEGATIVE] waiting_for_lock=console\n");
        spin_lock((spinlock_t *)(uintptr_t)
                  g_panic_test_state.console_lock_addr);
    }
#endif

    serial_write_all("[PANIC][DUMP_BEGIN] channel=serial kind=");
    serial_write_all(vector == 0xFFu ? "panic" : "exception");
    serial_write_all("\n");
    lapic_send_broadcast_halt();

#if HOBBYOS_DEBUG_ASSERT
    if (assertion)
    {
#ifdef HOBBYOS_ASSERT_TEST
        assertion_test_context_observe(
            identity.cpu_id, (uint32_t)identity.slot,
            (identity.source != 0 ? 1u : 0u) |
            (identity.slot_valid ? 2u : 0u) |
            (assertion_task.valid ? 4u : 0u),
            assertion_task.id, assertion_task.generation);
#endif
        panic_emit_assertion(assertion, &identity, &assertion_task);
    }
#endif

#ifdef HOBBYOS_PANIC_TEST
    panic_test_fault_full();
#endif

    serial_write_all("[PANIC][REASON] text=");
    panic_emit_limited_text(title);
    serial_write_all("\n");
    serial_write_all("[PANIC][VECTOR] known=");
    panic_emit_bool(vector != 0xFFu);
    if (vector != 0xFFu)
    {
        serial_write_all(" value=");
        panic_emit_u64(vector);
    }
    serial_write_all("\n");
    panic_emit_frame(frame);

    serial_write_all("[PANIC][ERROR_CODE] valid=");
    panic_emit_bool((uint8_t)has_error_code);
    if (has_error_code)
    {
        serial_write_all(" value=0x");
        serial_write_hex64_all(error_code);
    }
    serial_write_all("\n");

    serial_write_all("[PANIC][CR2] valid=");
    panic_emit_bool((uint8_t)has_cr2);
    if (has_cr2)
    {
        serial_write_all(" value=0x");
        serial_write_hex64_all(cr2);
    }
    serial_write_all("\n");
    if (vector == 14u && has_error_code && has_cr2)
        panic_emit_page_fault(error_code);

    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    serial_write_all("[PANIC][CR3] value=0x");
    serial_write_hex64_all(cr3);
    serial_write_all("\n");
    panic_emit_mmio(&mmio);
    serial_write_all("[PANIC][DUMP_END] status=complete channel=serial\n");
    panic_countdown(timeout);
    panic_do_action(action);
}

void kpanic(const char *message)
{
    panic_emit_exception(message, 0xFFu, NULL, 0, 0, 0, 0, "kpanic"
#if HOBBYOS_DEBUG_ASSERT
                         , NULL
#endif
                         );
}

#if HOBBYOS_DEBUG_ASSERT
void assertion_bug_report(const assertion_context_t *context)
{
    panic_emit_exception("runtime invariant violated", 0xFFu, NULL,
                         0, 0, 0, 0, "assertion", context);
}
#endif

void kpanic_exception(const char *title, void *frame,
                      uint64_t error_code, int has_error_code,
                      uint64_t cr2, int has_cr2)
{
    kpanic_exception_ex(title, 0xFFu, frame, error_code, has_error_code,
                        cr2, has_cr2);
}

void kpanic_exception_ex(const char *title, uint8_t vector, void *frame,
                         uint64_t error_code, int has_error_code,
                         uint64_t cr2, int has_cr2)
{
    panic_emit_exception(title, vector, (InterruptFrame *)frame,
                         error_code, has_error_code, cr2, has_cr2,
                         "exception"
#if HOBBYOS_DEBUG_ASSERT
                         , NULL
#endif
                         );
}

#ifdef HOBBYOS_PANIC_TEST
static bool panic_test_lock_address_valid(uint64_t address)
{
    return address >= 0x1000ULL &&
           (address & (_Alignof(spinlock_t) - 1u)) == 0;
}

static void panic_test_hold_locks(uint32_t slot, uint32_t mask)
{
    spinlock_t *console_lock = (spinlock_t *)(uintptr_t)
        g_panic_test_state.console_lock_addr;
    spinlock_t *clock_lock = (spinlock_t *)(uintptr_t)
        g_panic_test_state.clock_lock_addr;
    irq_flags_t flags = 0;
    if ((mask & PANIC_TEST_LOCK_CONSOLE) != 0)
        flags = spin_lock_irqsave(console_lock);
    if ((mask & PANIC_TEST_LOCK_CLOCK) != 0)
    {
        if ((mask & PANIC_TEST_LOCK_CONSOLE) != 0)
            spin_lock(clock_lock);
        else
            flags = spin_lock_irqsave(clock_lock);
    }
    (void)flags;

    panic_identity_t identity = panic_local_identity();
    __atomic_store_n(&g_panic_test_state.peer_observed_slot, slot,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_panic_test_state.peer_cpu_id, identity.cpu_id,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_panic_test_state.console_lock_value,
                     (mask & PANIC_TEST_LOCK_CONSOLE)
                         ? __atomic_load_n(&console_lock->locked,
                                           __ATOMIC_RELAXED)
                         : 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_panic_test_state.clock_lock_value,
                     (mask & PANIC_TEST_LOCK_CLOCK)
                         ? __atomic_load_n(&clock_lock->locked,
                                           __ATOMIC_RELAXED)
                         : 0,
                     __ATOMIC_RELAXED);
    serial_write_all("[PANIC_TEST][LOCK_HELD] peer_slot=");
    panic_emit_u64(slot);
    serial_write_all(" peer_cpu_id=");
    panic_emit_u64(identity.cpu_id);
    serial_write_all(" mask=");
    panic_emit_u64(mask);
    serial_write_all(" console_locked=");
    panic_emit_u64(g_panic_test_state.console_lock_value);
    serial_write_all(" clock_locked=");
    panic_emit_u64(g_panic_test_state.clock_lock_value);
    serial_write_all("\n");
    __atomic_store_n(&g_panic_test_state.lock_ack_mask, mask,
                     __ATOMIC_RELEASE);
    panic_halt_secondary();
}

static void panic_test_raise_exception(void) __attribute__((noreturn));
static void panic_test_raise_exception(void)
{
    __asm__ volatile(".globl panic_test_exception_ud2_site\n"
                     "panic_test_exception_ud2_site:\n\tud2" ::: "memory");
    panic_halt_secondary();
}

void panic_test_timer_hook(uint32_t slot)
{
#ifdef HOBBYOS_ASSERT_TEST
    if (assertion_test_timer_hook(slot))
        return;
#endif
    if (!__atomic_load_n(&g_panic_test_state.armed, __ATOMIC_ACQUIRE))
        return;

    uint32_t owner_slot = __atomic_load_n(&g_panic_test_state.owner_slot,
                                          __ATOMIC_RELAXED);
    uint32_t peer_slot = __atomic_load_n(&g_panic_test_state.peer_slot,
                                         __ATOMIC_RELAXED);
    uint32_t mask = __atomic_load_n(&g_panic_test_state.lock_mask,
                                    __ATOMIC_RELAXED);
    if (owner_slot >= HOBBYOS_MAX_CPUS ||
        (mask && (peer_slot >= HOBBYOS_MAX_CPUS || peer_slot == owner_slot)) ||
        ((mask & PANIC_TEST_LOCK_CONSOLE) &&
         !panic_test_lock_address_valid(
             g_panic_test_state.console_lock_addr)) ||
        ((mask & PANIC_TEST_LOCK_CLOCK) &&
         !panic_test_lock_address_valid(g_panic_test_state.clock_lock_addr)))
    {
        __atomic_store_n(&g_panic_test_state.arm_error, 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_panic_test_state.armed, 0, __ATOMIC_RELEASE);
        serial_write_all("[PANIC_TEST][ARM] status=invalid\n");
        return;
    }

    if (slot == peer_slot && mask)
    {
        while (__atomic_load_n(&g_panic_test_state.fired,
                               __ATOMIC_ACQUIRE) != 1)
            __asm__ volatile("pause" ::: "memory");
        if (__atomic_load_n(&g_panic_test_state.lock_ack_mask,
                            __ATOMIC_ACQUIRE) == 0)
            panic_test_hold_locks(slot, mask);
        return;
    }

    if (slot != owner_slot)
        return;

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_panic_test_state.fired, &expected, 3,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;

    serial_write_all("[PANIC_TEST][OWNER_READY] owner_slot=");
    panic_emit_u64(slot);
    serial_write_all(" lock_mask=");
    panic_emit_u64(mask);
    serial_write_all("\n");
    __atomic_store_n(&g_panic_test_state.fired, 1, __ATOMIC_RELEASE);
    while (mask && __atomic_load_n(&g_panic_test_state.lock_ack_mask,
                                   __ATOMIC_ACQUIRE) != mask)
        __asm__ volatile("pause" ::: "memory");
    if (mask)
        __atomic_store_n(&g_panic_test_state.fired, 2, __ATOMIC_RELEASE);

    panic_identity_t identity = panic_local_identity();
    __atomic_store_n(&g_panic_test_state.owner_observed_slot, slot,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_panic_test_state.owner_cpu_id, identity.cpu_id,
                     __ATOMIC_RELAXED);
    PanicAction action = (PanicAction)__atomic_load_n(
        &g_panic_test_state.action, __ATOMIC_RELAXED);
    uint32_t timeout = __atomic_load_n(&g_panic_test_state.timeout_seconds,
                                       __ATOMIC_RELAXED);
    panic_config(action, timeout);
    serial_write_all("[PANIC_TEST][TRIGGER] owner_slot=");
    panic_emit_u64(slot);
    serial_write_all(" owner_cpu_id=");
    panic_emit_u64(identity.cpu_id);
    serial_write_all(" scenario=");
    panic_emit_u64(g_panic_test_state.scenario);
    serial_write_all("\n");

    uint32_t scenario = __atomic_load_n(&g_panic_test_state.scenario,
                                        __ATOMIC_ACQUIRE);
    if (scenario == PANIC_TEST_SCENARIO_UART_UNRESPONSIVE)
        __atomic_store_n(&g_panic_test_uart_unresponsive, 1,
                         __ATOMIC_RELEASE);
    if (scenario == PANIC_TEST_SCENARIO_EXCEPTION)
        panic_test_raise_exception();
    kpanic("panic test trigger");
}
#endif

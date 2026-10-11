#ifndef CPU_REQUEST_TIMING_H
#define CPU_REQUEST_TIMING_H
#include <stdbool.h>
#include <stdint.h>

#define CPU_REQUEST_TIMING_SCHEMA 1u
#if (defined(HOBBYOS_DEBUG_ASSERT) && HOBBYOS_DEBUG_ASSERT) || defined(HOBBYOS_SELFTEST)
#define CPU_REQUEST_TIMING_ENABLED 1
#else
#define CPU_REQUEST_TIMING_ENABLED 0
#endif

typedef enum {
    CPU_REQUEST_TIMING_OMITTED,
    CPU_REQUEST_TIMING_CYCLES,
    CPU_REQUEST_TIMING_NO_TSC,
    CPU_REQUEST_TIMING_CONTEXT,
    CPU_REQUEST_TIMING_ORDER,
    CPU_REQUEST_TIMING_INCOMPLETE,
    CPU_REQUEST_TIMING_IDENTITY
} cpu_request_timing_status_t;

typedef struct {
    uint64_t handler_begin, operation_begin, operation_end, handler_end;
    uint64_t generation, root, epoch, end_epoch, begin_flags, end_flags;
    uint32_t cpu, apic, status;
} cpu_request_local_cycles_t;
typedef struct {
    uint64_t begin, end, epoch, end_epoch, begin_flags, end_flags;
    uint32_t cpu, apic, status;
} cpu_request_send_cycles_t;
typedef struct {
    cpu_request_local_cycles_t local;
    cpu_request_send_cycles_t send;
} cpu_request_timing_t;

static inline uint32_t cpu_request_local_cycles_check(const cpu_request_local_cycles_t *t,
    uint64_t generation, uint64_t root, uint64_t epoch, uint32_t cpu, uint32_t apic)
{
    if (t->generation != generation || t->root != root || t->epoch != epoch || t->end_epoch != epoch ||
        t->cpu != cpu || t->apic != apic || !generation || !root || (root & 4095u))
        return CPU_REQUEST_TIMING_IDENTITY;
    if ((t->begin_flags | t->end_flags) & 512u) return CPU_REQUEST_TIMING_CONTEXT;
    if (!t->handler_begin || t->handler_begin >= t->operation_begin ||
        t->operation_begin >= t->operation_end || t->operation_end >= t->handler_end)
        return CPU_REQUEST_TIMING_ORDER;
    return CPU_REQUEST_TIMING_CYCLES;
}
static inline uint32_t cpu_request_send_cycles_check(const cpu_request_send_cycles_t *t,
    uint64_t epoch, uint32_t cpu, uint32_t apic)
{
    if (t->epoch != epoch || t->end_epoch != epoch || t->cpu != cpu || t->apic != apic) return CPU_REQUEST_TIMING_IDENTITY;
    if ((t->begin_flags | t->end_flags) & 512u) return CPU_REQUEST_TIMING_CONTEXT;
    if (!t->begin || t->end <= t->begin) return CPU_REQUEST_TIMING_ORDER;
    return CPU_REQUEST_TIMING_CYCLES;
}
#endif

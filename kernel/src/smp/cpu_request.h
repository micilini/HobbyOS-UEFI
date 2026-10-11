#ifndef CPU_REQUEST_H
#define CPU_REQUEST_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cpu_limits.h"
#include "cpu_request_timing.h"
#include "../memory/tlb_interval.h"

typedef enum {
    CPU_REQUEST_OK, CPU_REQUEST_INVALID, CPU_REQUEST_CONTEXT,
    CPU_REQUEST_BUSY, CPU_REQUEST_PANIC, CPU_REQUEST_UNAVAILABLE,
    CPU_REQUEST_SEND_FAILED, CPU_REQUEST_TIMEOUT, CPU_REQUEST_LOCAL_FAILED,
    CPU_REQUEST_QUARANTINED, CPU_REQUEST_EXHAUSTED
} cpu_request_status_t;
typedef enum {
    CPU_REQUEST_INVALIDATE = 1,
    CPU_REQUEST_CACHE_WRITEBACK = 3,
#ifdef HOBBYOS_SELFTEST
    CPU_REQUEST_SAMPLE = 2,
    CPU_REQUEST_PROTECTION = 4,
#endif
} cpu_request_operation_t;
typedef struct {
    uint64_t generation, root;
    tlb_interval_t interval;
    uint32_t targets, apic[HOBBYOS_MAX_CPUS];
    cpu_request_operation_t operation;
} cpu_request_spec_t;
typedef struct {
    uint64_t value, cr3;
    uint32_t slot, apic, loads;
    bool success;
#if CPU_REQUEST_TIMING_ENABLED
    cpu_request_timing_t timing;
#endif
} cpu_request_sample_t;
typedef struct {
    cpu_request_spec_t spec;
    uint64_t publication[HOBBYOS_MAX_CPUS], active;
    cpu_request_sample_t samples[HOBBYOS_MAX_CPUS];
    uint64_t next_generation;
    uint32_t owner;
    bool quarantined;
} cpu_request_storage_t;
typedef struct {
    cpu_request_status_t status;
    uint32_t requested, acknowledged, missing, failed, sent, send_failed, admission_spins, notifications;
    uint64_t generation, elapsed_ns, timing_schema;
} cpu_request_result_t;
typedef bool (*cpu_request_execute_fn)(void *, const cpu_request_spec_t *,
                                      cpu_request_sample_t *);

/* Internal owned-storage transitions, also compiled by adversarial host tests.
 * A successful claim pins this generation until the final release store. The
 * handler must not touch request storage after that store. A delayed interrupt
 * has no generation; it acquires whichever pending generation is published. */
bool cpu_request_admit(cpu_request_storage_t *storage);
cpu_request_status_t cpu_request_publish(cpu_request_storage_t *storage,
                                         const cpu_request_spec_t *spec);
bool cpu_request_handle(cpu_request_storage_t *storage, uint32_t slot,
    uint32_t apic, cpu_request_execute_fn execute, void *context);
void cpu_request_collect(const cpu_request_storage_t *storage,
                          cpu_request_result_t *out);
bool cpu_request_retire(cpu_request_storage_t *storage, bool success);

#define CPU_REQUEST_DEADLINE_NS UINT64_C(1000000000)
/* Runtime waits require eligible thread context, IF=1, no hard IRQ/NMI/panic,
 * paging lock, CPU pin, or target-needed resource. Admission ownership is atomic;
 * handlers never acquire it. No caller stack pointer crosses an interrupt.
 * Timeout quarantines storage for the boot; no frame or VA may be reused. */
bool cpu_request_runtime_init(uint64_t root);
cpu_request_result_t cpu_request_run(cpu_request_operation_t operation,
                                     const tlb_interval_t *interval);
void cpu_request_irq(void);
bool cpu_request_context_allowed(void);
/* Device adoption begins after every CPU's IRQ handoff, before external
 * device services are released. Only mapping maintenance uses this admission. */
bool cpu_request_mapping_enable(void);
bool cpu_request_mapping_context_allowed(void);
cpu_request_result_t cpu_request_mapping_run(cpu_request_operation_t operation,
                                             const tlb_interval_t *interval);
bool cpu_request_context_valid(bool interrupts_enabled, uint32_t irq_depth,
    bool thread_context, bool runtime_ready, bool panic, bool critical);
#ifdef HOBBYOS_SELFTEST
#define CPU_REQUEST_AUDIT_CAPACITY 8192u
typedef struct { cpu_request_spec_t spec; cpu_request_result_t result; bool mapping; } cpu_request_audit_t;
bool cpu_request_audit_snapshot(uint64_t *admitted, uint64_t *completed);
bool cpu_request_audit_copy(uint64_t generation, cpu_request_audit_t *out);
bool cpu_request_timing_copy(uint64_t generation, uint32_t target, cpu_request_timing_t *out);
bool cpu_request_timing_format(char *out, size_t capacity, uint64_t generation,
    uint64_t root, uint32_t target, const cpu_request_timing_t *timing);
bool cpu_request_test_delay(uint32_t slot);
uint32_t cpu_request_test_delayed_entries(void);
void cpu_request_test_release_delay(void);
cpu_request_result_t cpu_request_run_observed(cpu_request_operation_t operation,
    const tlb_interval_t *interval, cpu_request_sample_t *out, uint32_t capacity);
#endif
#endif

#ifndef STACK_DOMAIN_H
#define STACK_DOMAIN_H
#include <stdbool.h>
#include <stdint.h>
#include "physical_layout.h"
#include "../smp/cpu_limits.h"

#define STACK_REGION_BASE FUTURE_STACK_BASE
#define STACK_SLOT_BYTES UINT64_C(65536)
#define STACK_GUARD_BYTES UINT64_C(4096)
#define STACK_TASK_BYTES UINT64_C(16384)
#define STACK_IST_BYTES UINT64_C(32768)
#define STACK_CPU_SLOTS (4u * HOBBYOS_MAX_CPUS)
/* Includes exited workers still within the unchanged reaper grace interval. */
#define STACK_TASK_SLOTS 1536u
#define STACK_REGION_SLOTS (STACK_CPU_SLOTS + STACK_TASK_SLOTS)
#define STACK_REGION_BYTES (STACK_SLOT_BYTES * STACK_REGION_SLOTS)
#define STACK_REGION_END (STACK_REGION_BASE + STACK_REGION_BYTES)
#define STACK_BSP_OWNER STACK_REGION_SLOTS
#define STACK_OWNER_COUNT (STACK_REGION_SLOTS + 1u)
#define STACK_PREPARED_TASKS 8u
#define STACK_NAME_BYTES 32u

typedef enum {
    STACK_KIND_NONE, STACK_KIND_TASK, STACK_KIND_BSP_BOOT, STACK_KIND_AP_BOOT,
    STACK_KIND_IST1, STACK_KIND_IST2, STACK_KIND_IST3, STACK_KIND_PREPARED
} stack_kind_t;
typedef enum {
    STACK_FREE, STACK_RESERVED, STACK_PREPARED, STACK_ALLOCATED, STACK_BOUND,
    STACK_LIVE, STACK_RETIRING, STACK_QUARANTINED
} stack_state_t;
typedef struct { uint64_t generation; uint32_t slot; } stack_handle_t;
#define STACK_HANDLE_INVALID ((stack_handle_t){0, UINT32_MAX})

typedef struct {
    uint64_t guard, base, end, physical, generation, task_id, task_lifecycle;
    uint64_t retirement_generation;
    uint32_t slot, capacity, cpu, apic;
    stack_kind_t kind;
    stack_state_t state;
    bool owned, prepared_origin, ever_published;
    char name[STACK_NAME_BYTES];
} stack_owner_view_t;

/* Every shared scalar is accessed atomically. A fault reader makes exactly
 * one consistency attempt and never waits for an interrupted publisher. */
enum {
    STACK_META_GENERATION, STACK_META_GUARD, STACK_META_BASE, STACK_META_END,
    STACK_META_PHYSICAL, STACK_META_TASK, STACK_META_LIFECYCLE,
    STACK_META_RETIREMENT, STACK_META_SLOT, STACK_META_CAPACITY,
    STACK_META_CPU, STACK_META_APIC, STACK_META_KIND, STACK_META_STATE,
    STACK_META_OWNED, STACK_META_PREPARED, STACK_META_PUBLISHED,
    STACK_META_NAME, STACK_META_CELLS = STACK_META_NAME + STACK_NAME_BYTES
};
typedef struct { uint64_t sequence, cells[STACK_META_CELLS]; } stack_owner_record_t;

bool stack_slot_geometry(uint32_t slot, uint32_t capacity, uint64_t *guard,
                         uint64_t *base, uint64_t *end);
bool stack_owner_read(const stack_owner_record_t *record, stack_owner_view_t *out);
bool stack_owner_reserve(stack_owner_record_t *record, uint32_t slot,
                         stack_kind_t kind, uint32_t capacity, uint64_t physical,
                         bool owned, uint64_t bsp_guard, stack_handle_t *out);
bool stack_owner_mapped(stack_owner_record_t *record, stack_handle_t handle, bool prepared);
bool stack_owner_take_prepared(stack_owner_record_t *record, stack_handle_t *out);
bool stack_owner_bind_task(stack_owner_record_t *record, stack_handle_t handle,
                          uint64_t id, uint64_t lifecycle, const char *name);
bool stack_owner_bind_cpu(stack_owner_record_t *record, stack_handle_t handle,
                         uint32_t cpu, uint32_t apic);
bool stack_owner_publish(stack_owner_record_t *record, stack_handle_t handle);
bool stack_owner_return_prepared(stack_owner_record_t *record, stack_handle_t handle);
bool stack_owner_begin_retire(stack_owner_record_t *record, stack_handle_t handle,
                             uint64_t id, uint64_t lifecycle, bool unpublished);
bool stack_owner_finish_retire(stack_owner_record_t *record, stack_handle_t handle,
                              uint64_t request_generation, bool acknowledged);
bool stack_owner_cancel_reservation(stack_owner_record_t *record, stack_handle_t handle);
bool stack_owner_guard_lookup(const stack_owner_record_t *records, uint64_t address,
                              stack_owner_view_t *out);
bool stack_owner_payload(const stack_owner_view_t *owner, uint64_t address,
                         uint64_t bytes, uint64_t *physical);
#endif

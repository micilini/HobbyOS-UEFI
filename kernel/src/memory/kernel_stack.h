#ifndef KERNEL_STACK_H
#define KERNEL_STACK_H
#include "stack_domain.h"

/* These handles own stacks, never arbitrary virtual ranges or caller sizes. */
bool kernel_stack_acquire(stack_handle_t *handle, void **base);
bool kernel_stack_bind_task(stack_handle_t handle, uint64_t id, uint64_t lifecycle, const char *name);
bool kernel_stack_publish(stack_handle_t handle);
void kernel_stack_discard(stack_handle_t handle);
void kernel_stack_retire(stack_handle_t handle, uint64_t id, uint64_t lifecycle);
bool kernel_stack_cpu(stack_kind_t kind, uint32_t cpu, uint32_t apic, uint64_t *top);
bool kernel_stack_bind_bsp(uint32_t cpu, uint32_t apic);
bool kernel_stack_close_bootstrap(void);
bool kernel_stack_runtime_enable(void);
bool kernel_stack_read(uint32_t slot, stack_owner_view_t *owner);
bool kernel_stack_guard_owner(uint64_t address, stack_owner_view_t *owner);
bool kernel_stack_physical(uint64_t address, uint64_t bytes, uint64_t *physical);
bool kernel_stack_bsp_guard_overlap(uint64_t address, uint64_t bytes);
bool kernel_stack_cpu_owner(uint32_t cpu, stack_kind_t kind, stack_owner_view_t *owner);
typedef struct {
    uint64_t allocated, retired, quarantined, owned_bytes, borrowed_bytes;
    uint32_t live, prepared, reserved, retiring, free_slots, high_water;
} kernel_stack_stats_t;
bool kernel_stack_stats(kernel_stack_stats_t *out);
#endif

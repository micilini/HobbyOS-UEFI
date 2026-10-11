#ifndef STACK_INTERNAL_H
#define STACK_INTERNAL_H
#include "kernel_stack.h"
#include "stack_map.h"
/* Construction and leaf transactions are private to memory. */
const stack_layout_t *kernel_stack_prepare(const image_permissions_t *image, const ram_map_plan_t *ram);
const stack_layout_t *kernel_stack_layout(void);
bool kernel_stack_activate(void);
/* Paging lock -> backing lock; neither is held across CPU completion waits. */
bool kernel_stack_reserved_backing(stack_handle_t handle, stack_owner_view_t *owner);
bool paging_stack_install(stack_handle_t handle, bool prepared);
bool paging_stack_remove(stack_handle_t handle, uint64_t id, uint64_t lifecycle, bool unpublished);
bool paging_stack_validate(stack_handle_t handle);
#endif

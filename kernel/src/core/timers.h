#ifndef CORE_TIMERS_H
#define CORE_TIMERS_H
#include <stdint.h>
#include <stdbool.h>
#include "list.h"
#include "task.h"
typedef void (*timer_callback_t)(void *ctx);
typedef uint64_t timer_id_t;
#define TIMER_ID_INVALID ((timer_id_t)0)
typedef struct { timer_id_t id; uint64_t generation; } timer_handle_t;
typedef enum { TIMER_NODE_PENDING = 0, TIMER_NODE_CLAIMED, TIMER_NODE_CANCELLED } timer_node_state_t;
typedef enum { TIMER_KIND_CALLBACK = 0, TIMER_KIND_TASK_WAKE } timer_kind_t;
typedef enum { TIMER_CANCELLED = 0, TIMER_CANCEL_ALREADY_CLAIMED, TIMER_CANCEL_NOT_FOUND, TIMER_CANCEL_INVALID } timer_cancel_result_t;
typedef struct {
    uint64_t armed, cancelled, claimed, dispatched, alloc_failures;
    uint64_t stale_task_wakes, duplicate_cancel;
    uint64_t task_refs_acquired, task_refs_released, task_ref_release_failures;
    uint64_t task_ref_duplicate_release;
    uint32_t pending;
    uint32_t claimed_now;
} timer_stats_t;
typedef struct { uint8_t active; task_id_t task_id; uint64_t generation; } timers_test_claim_hold_snapshot_t;
typedef struct {
    uint8_t active;
    uint8_t old_selected;
    uint8_t new_selected;
    uint8_t order_violation;
    uint32_t remaining;
    uint32_t residual;
    uint64_t generation;
    timer_handle_t old_handle;
    timer_handle_t new_handle;
} timers_test_order_snapshot_t;
typedef struct {
 task_handle_t target; uint32_t pending_nodes,claimed_nodes,refs_held;
 timer_handle_t first_pending,first_claimed; uint64_t wait_generation;
 uint8_t found_wrong_lifecycle;
} timers_test_task_wake_snapshot_t;
void timers_init(void);
int timers_add(uint64_t delay_ms, timer_callback_t cb, void *ctx);
bool timers_add_callback(uint64_t delay_ms, timer_callback_t cb, void *ctx, timer_handle_t *out);
bool timers_arm_task_wake(uint64_t delay_ms, task_id_t id, uint64_t lifecycle,
                          uint64_t wait_generation, task_wake_reason_t reason, timer_handle_t *out);
timer_cancel_result_t timers_cancel(timer_handle_t handle);
void timers_poll(void);
void timers_poll_at(uint64_t now_ms);
void timers_test_fail_next_allocation(void);
void timers_test_hold_claimed(bool hold);
void timers_test_hold_claimed_task(task_id_t id,bool hold);
bool timers_test_set_claim_hold(task_id_t id,bool hold);
void timers_test_claim_hold_snapshot(timers_test_claim_hold_snapshot_t *out);
bool timers_test_arm_order_pair(timer_callback_t callback, void *old_ctx,
                                void *new_ctx, uint64_t *out_generation);
bool timers_test_order_finish(uint64_t generation,
                              timers_test_order_snapshot_t *out);
bool timers_test_handle_present(timer_handle_t handle,
                                timer_node_state_t *out_state);
bool timers_test_find_claimed_task_wake(task_id_t id,timer_handle_t *out,uint64_t *lifecycle,uint64_t *wait_generation);
bool timers_test_task_wake_snapshot(task_handle_t target,timers_test_task_wake_snapshot_t *out);
int timers_test_dispatch_task_wake(task_id_t id, uint64_t lifecycle, uint64_t wait_generation);
task_wait_result_t timers_test_fallback_delay(uint64_t ms, bool interrupts_enabled);
void timers_get_stats(timer_stats_t *out);
bool timers_validate(void);
task_wait_result_t timer_sleep_interruptible(uint64_t ms);
#endif

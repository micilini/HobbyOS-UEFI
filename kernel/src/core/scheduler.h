#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"
#include "idt.h"
#include "../smp/smp_topology.h"

typedef enum
{
    SCHED_BOOT_OK = 0,
    SCHED_BOOT_ERR_ALREADY_INITIALIZED = -1,
    SCHED_BOOT_ERR_GLOBAL_NOT_READY = -2,
    SCHED_BOOT_ERR_INVALID_SLOT = -3,
    SCHED_BOOT_ERR_SLOT_TOPOLOGY_MISMATCH = -4,
    SCHED_BOOT_ERR_CPU_ALREADY_INITIALIZED = -5,
    SCHED_BOOT_ERR_CURRENT_APIC_MISMATCH = -6,
    SCHED_BOOT_ERR_ALLOCATION = -7,
    SCHED_BOOT_ERR_CPU_NOT_READY = -8,
    SCHED_BOOT_ERR_ALREADY_STARTED = -9,
    SCHED_BOOT_ERR_UNKNOWN_CPU = -10
} scheduler_boot_result_t;

int scheduler_global_init(void);
int scheduler_cpu_init_bsp(cpu_slot_t slot);
int scheduler_cpu_init_ap(cpu_slot_t slot);
int scheduler_cpu_mark_timer_ready(cpu_slot_t slot);
int scheduler_start(void);
bool scheduler_is_started(void);
bool scheduler_cpu_is_ready(cpu_slot_t slot);
bool scheduler_bootstrap_selftest(void);

typedef struct
{
    uint64_t idle_rsp;
    uint64_t early_preemption_attempts;
    uint8_t bootstrap_attached;
    uint8_t handoff_complete;
    uint8_t irq_preemption_enabled;
    uint8_t idle_rsp_valid;
} scheduler_preemption_snapshot_t;

bool scheduler_bootstrap_handoff_current_cpu(void);
bool scheduler_cpu_enable_irq_preemption(cpu_slot_t slot);
bool scheduler_cpu_irq_preemption_enabled(cpu_slot_t slot);
bool scheduler_cpu_bootstrap_handoff_complete(cpu_slot_t slot);
bool scheduler_preemption_snapshot(cpu_slot_t slot,
                                   scheduler_preemption_snapshot_t *out);
bool scheduler_preemption_gate_selftest(void);

typedef struct
{
    const char *name;
    int task_class;
    uint32_t flags;
    task_cleanup_fn cleanup_fn;
    void *cleanup_ctx;
    /* Test-only: bind a reap hold to this exact task while it is published. */
    uint8_t test_reap_hold;
} task_create_options_t;

bool thread_create_ex_handle(void (*entry_point)(void *),void *arg,const task_create_options_t *options,task_handle_t *out);
bool thread_create_named_with_class_flags_handle(void (*entry_point)(void *),void *arg,int task_class,const char *name,uint32_t flags,task_handle_t *out);
bool thread_create_named_with_class_handle(void (*entry_point)(void *),void *arg,int task_class,const char *name,task_handle_t *out);
bool thread_create_with_class_handle(void (*entry_point)(void *),void *arg,int task_class,task_handle_t *out);
bool thread_create_named_handle(void (*entry_point)(void *),void *arg,const char *name,task_handle_t *out);
bool thread_create_handle(void (*entry_point)(void *),void *arg,task_handle_t *out);
bool thread_create_ex(void (*entry_point)(void *),void *arg,const task_create_options_t *options);
bool thread_create_named_with_class_flags(void (*entry_point)(void *),void *arg,int task_class,const char *name,uint32_t flags);
bool thread_create_named_with_class(void (*entry_point)(void *),void *arg,int task_class,const char *name);
bool thread_create_with_class(void (*entry_point)(void *),void *arg,int task_class);
bool thread_create_named(void (*entry_point)(void *),void *arg,const char *name);
bool thread_create(void (*entry_point)(void *),void *arg);
bool scheduler_current_task_handle(task_handle_t *out);

void thread_set_name(task_t *t, const char *name);
bool scheduler_set_task_name_by_id(task_id_t id,const char *name);

void thread_set_current_name(const char *name);

void schedule(void);
void schedule_voluntary(void);

void scheduler_account_time(uint64_t now_ns);

void schedule_impl(int voluntary);
void scheduler_preempt_from_irq(void);
void scheduler_finish_switch(void *cpu_state, uint64_t expected_sequence);

typedef struct
{
    irq_flags_t irq_flags;
    cpu_slot_t slot;
    uint32_t apic_id;
    task_t *task;
    uint8_t active;
} scheduler_cpu_pin_t;

bool scheduler_cpu_pin(scheduler_cpu_pin_t *pin);
bool scheduler_cpu_pin_validate(const scheduler_cpu_pin_t *pin);
void scheduler_cpu_unpin(scheduler_cpu_pin_t *pin);

task_t *get_current_task(void);

void thread_block(wait_queue_t *wq, task_state_t state);

int thread_wake_one(wait_queue_t *wq);

void thread_wake(task_t *t);

void thread_exit_with_reason(task_exit_reason_t reason) __attribute__((noreturn));
void thread_exit_normal(void) __attribute__((noreturn));
void thread_exit(void) __attribute__((noreturn));
bool task_cancel_requested(void);
void task_cancel_point(void);

typedef struct
{
    task_t *task;
    uint64_t lifecycle_generation;
    uint64_t wait_generation;
    task_wait_kind_t kind;
    uint8_t prepared;
} task_block_token_t;

typedef enum
{
    SCHED_BLOCK_PREPARED = 0,
    SCHED_BLOCK_CANCELLED,
    SCHED_BLOCK_ERROR
} scheduler_block_prepare_result_t;

typedef struct
{
    uint64_t prepared;
    uint64_t committed;
    uint64_t aborted;
    uint64_t preblock_cancelled;
    uint64_t prepare_errors;
} scheduler_wait_stats_t;

bool scheduler_prepare_block(wait_queue_t *wq, task_state_t state,
                             task_wait_kind_t kind, uintptr_t object_key,
                             task_block_token_t *out_token);
scheduler_block_prepare_result_t scheduler_prepare_block_interruptible(
    wait_queue_t *wq, task_state_t state, task_wait_kind_t kind,
    uintptr_t object_key, task_block_token_t *out_token);
void scheduler_wait_stats_snapshot(scheduler_wait_stats_t *out);
typedef struct {
    uint8_t armed, consumed;
    task_handle_t target;
    task_wait_kind_t kind;
    uintptr_t object_key;
    uint64_t generation;
} scheduler_test_preblock_negative_t;
bool scheduler_test_arm_ignore_preblock_cancel(task_handle_t target,
    task_wait_kind_t kind, uintptr_t object_key);
void scheduler_test_preblock_negative_snapshot(scheduler_test_preblock_negative_t *out);
void scheduler_test_clear_preblock_negative(void);
task_wait_result_t scheduler_commit_block(task_block_token_t *token);
void scheduler_abort_block(task_block_token_t *token, task_wake_reason_t reason);
typedef enum { SCHED_WAKE_NO_MATCH = 0, SCHED_WAKE_WON, SCHED_WAKE_FATAL } scheduler_wake_status_t;
scheduler_wake_status_t scheduler_wake_task_by_identity(task_id_t id, uint64_t lifecycle_generation,
                                                        uint64_t wait_generation, task_wake_reason_t reason);
scheduler_wake_status_t scheduler_wake_one_waiter(wait_queue_t *wq, task_wait_kind_t expected_kind,
                                                  uintptr_t object_key, task_wake_reason_t reason);
bool scheduler_current_context_can_block(void);

typedef struct
{
    task_id_t id;
    char name[32];
    task_state_t state;

    int task_class;
    int quantum;
    int quantum_default;

    uint64_t rsp;
    void *stack_base;

    uint32_t kernel_stack_bytes;
    uint32_t kernel_ctx_bytes_est;
    uint64_t kernel_mem_est_bytes;

    uint32_t current_cpu;
    uint8_t is_current;
    uint8_t is_idle;
    uint8_t on_cpu;
    uint32_t current_cpu_slot;

    uint32_t last_cpu_slot;
    uint64_t runtime_ns_total;
    uint64_t schedule_count;

    uint32_t flags;
    uint8_t kill_pending, exit_started, cleanup_started, cleanup_done;
    uint8_t lifecycle_notify_started, lifecycle_notify_completed, lifecycle_notify_failed, reap_claimed;
    task_exit_reason_t exit_reason;
    uint64_t kill_requested_ns;
    uint64_t kill_request_count;
    uint64_t cancellation_points;
    uint64_t lifecycle_generation;
    uint64_t wait_generation;
    task_wait_kind_t wait_kind;
    task_wake_reason_t wake_reason;
    uint8_t wait_active;
    task_queue_membership_t queue_membership;
    uintptr_t wait_object_key;
    uint8_t deferred_ready;
    uint32_t task_wake_timer_refs;
    uint64_t timer_ref_acquires,timer_ref_releases,reap_defer_mask_last;
    uint64_t zombie_generation, exit_started_ns, cleanup_completed_ns, zombie_entered_ns;
} task_snapshot_t;

typedef struct
{
    uint32_t written;
    uint32_t total;
    uint32_t offset;
    uint8_t truncated;
    uint64_t registry_generation;
    uint64_t sample_time_ns;
} task_snapshot_result_t;

task_snapshot_result_t scheduler_snapshot_tasks(task_snapshot_t *out_entries,
                                                uint32_t capacity,
                                                uint32_t offset);
bool scheduler_snapshot_task_by_id(task_id_t id, task_snapshot_t *out);
bool scheduler_snapshot_task_by_handle(task_handle_t handle,
                                       task_snapshot_t *out);
bool scheduler_validate_task_identity(void);
bool scheduler_task_stack_guard_ok(const task_t *task);
bool scheduler_task_stack_rsp_in_bounds(const task_t *task);
void scheduler_report_stack_usage(void);
typedef struct
{
    uint32_t cpus;
    uint32_t tasks;
    uint64_t started;
    uint64_t finished;
    uint32_t pending;
    uint64_t violations;
    uint64_t stale_cpu_slot_entries;
    uint64_t pinned_current_mismatches;
    uint64_t finish_wrong_cpu;
    uint64_t finish_sequence_mismatch;
    uint64_t finish_without_pending;
    uint64_t pending_overwrite;
    uint64_t pending_cancel_wait_violations;
    uint32_t failure_code;
} scheduler_runtime_stats_t;
bool scheduler_validate_runtime_invariants(scheduler_runtime_stats_t *out_stats);
typedef enum
{
    TASK_ID_PARSE_OK = 0,
    TASK_ID_PARSE_ZERO,
    TASK_ID_PARSE_EMPTY,
    TASK_ID_PARSE_SIGN,
    TASK_ID_PARSE_NON_DECIMAL,
    TASK_ID_PARSE_OVERFLOW,
    TASK_ID_PARSE_INVALID_ARGUMENT
} task_id_parse_result_t;

task_id_parse_result_t task_id_parse_decimal_ex(const char *text,
                                                 task_id_t *out_id);
const char *task_id_parse_result_to_string(task_id_parse_result_t result);
bool task_id_parse_decimal(const char *text, task_id_t *out_id);
typedef struct {
 uint64_t accounting_events,runtime_ns_accounted,runtime_overflows,clock_regressions;
} scheduler_accounting_stats_t;
void scheduler_accounting_stats_snapshot(scheduler_accounting_stats_t *out);

const char *scheduler_task_state_to_string(task_state_t state);

typedef enum {
 TASK_KILL_ACCEPTED=0, TASK_KILL_ALREADY_PENDING=1, TASK_KILL_ALREADY_ZOMBIE=2, TASK_KILL_ALREADY_EXITING=3,
 TASK_KILL_ERR_NOT_FOUND=-1, TASK_KILL_ERR_PROTECTED=-2,
 TASK_KILL_ERR_NOT_KILLABLE=-3, TASK_KILL_ERR_INVALID=-4
} task_kill_result_t;
typedef struct { uint64_t requests,accepted,already_pending,already_zombie,already_exiting,not_found,protected_rejections,not_killable_rejections,cancellation_points,exits_normal,exits_killed,exits_init_failure,exits_internal_error,exit_phases_started,exit_phases_completed,cleanup_started,cleanup_completed,cleanup_callbacks_invoked,cleanup_duplicate_attempts,exit_duplicate_attempts; } task_kill_stats_t;

task_kill_result_t scheduler_request_kill(task_id_t task_id);
const char *scheduler_task_kill_result_to_string(task_kill_result_t result);
typedef enum { TASK_TEST_MATCH_READY_OFF_CPU, TASK_TEST_MATCH_RUNNING_ON_CPU, TASK_TEST_MATCH_SLEEPING_WAIT, TASK_TEST_MATCH_BLOCKED_WAIT, TASK_TEST_MATCH_ZOMBIE, TASK_TEST_MATCH_EXITING } task_test_match_t;
typedef enum { TASK_TEST_REQUEST_NOT_READY=0, TASK_TEST_REQUEST_APPLIED, TASK_TEST_REQUEST_GONE } task_test_request_status_t;
task_test_request_status_t scheduler_test_request_kill_when(task_id_t id, task_test_match_t match, task_kill_result_t *out_result, task_snapshot_t *out_observed);
void scheduler_test_hold_next_ready_creation(void);
void scheduler_test_hold_reap_next_creation(void);
void scheduler_test_cancel_hold_reap_next_creation(void);
void scheduler_test_set_lifecycle_log_quiet(bool quiet);
bool scheduler_test_hold_reap(task_id_t id, bool hold);
uint32_t scheduler_test_reap_holds(void);
void scheduler_test_force_finish_validation_failure(uint32_t bit);
bool scheduler_test_corrupt_stack_guard(task_id_t id, uint32_t offset);
void scheduler_kill_stats_snapshot(task_kill_stats_t *out);
const char *scheduler_task_exit_reason_to_string(task_exit_reason_t reason);
const char *scheduler_task_kill_state_to_string(const task_snapshot_t *task);
bool scheduler_cancellation_selftest(void);
int scheduler_current_task_should_exit(void);

uint32_t scheduler_reap_zombies(uint64_t grace_ms);

bool scheduler_task_timer_ref_acquire_by_identity(task_id_t id, uint64_t lifecycle_generation,
                                                   uint64_t wait_generation);
bool scheduler_task_timer_ref_acquire_locked(task_t *task,uint64_t lifecycle_generation,
                                              uint64_t wait_generation);
bool scheduler_task_timer_ref_release_by_identity(task_id_t id, uint64_t lifecycle_generation,
                                                   uint64_t wait_generation);
typedef enum {
 TASK_REAP_DEFER_NOT_ZOMBIE=1ull<<0,TASK_REAP_DEFER_TEST_HOLD=1ull<<1,TASK_REAP_DEFER_IDLE=1ull<<2,
 TASK_REAP_DEFER_EXIT_NOT_STARTED=1ull<<3,TASK_REAP_DEFER_CLEANUP_NOT_DONE=1ull<<4,
 TASK_REAP_DEFER_INVALID_REASON=1ull<<5,TASK_REAP_DEFER_NOTIFY_NOT_DONE=1ull<<6,
 TASK_REAP_DEFER_ON_CPU=1ull<<7,TASK_REAP_DEFER_CURRENT=1ull<<8,TASK_REAP_DEFER_CPU_SLOT=1ull<<9,
 TASK_REAP_DEFER_QUEUE=1ull<<10,TASK_REAP_DEFER_WAIT_ACTIVE=1ull<<11,TASK_REAP_DEFER_WAIT_KIND=1ull<<12,
 TASK_REAP_DEFER_WAIT_OBJECT=1ull<<13,TASK_REAP_DEFER_DEFERRED_READY=1ull<<14,
 TASK_REAP_DEFER_TIMER_REFS=1ull<<15,TASK_REAP_DEFER_STACK_GUARD=1ull<<16,
 TASK_REAP_DEFER_RSP_BOUNDS=1ull<<17,TASK_REAP_DEFER_ALREADY_CLAIMED=1ull<<18,
 TASK_REAP_DEFER_NO_TIMESTAMP=1ull<<19,TASK_REAP_DEFER_CLOCK=1ull<<20,TASK_REAP_DEFER_GRACE=1ull<<21,
 TASK_REAP_DEFER_NOTIFY_NOT_STARTED=1ull<<22,TASK_REAP_DEFER_ZOMBIE_GENERATION=1ull<<23,
 TASK_REAP_DEFER_TIMESTAMP_ORDER=1ull<<24,TASK_REAP_DEFER_TIMER_ACCOUNTING=1ull<<25
} task_reap_defer_mask_t;
typedef struct {uint64_t scans,batches,candidates,claimed,reaped,current_zombies,max_zombie_backlog;
 uint64_t deferred_safety,deferred_expected,deferred_structural,deferred_grace,deferred_test_hold,deferred_current,deferred_on_cpu,deferred_queue,deferred_wait,deferred_timer_ref;
 uint64_t deferred_notification,deferred_cleanup,deferred_stack,claim_failures,invariant_failures;
 uint64_t timer_ref_underflows,notification_failures,refs_acquired,refs_released,refs_current,max_refs_per_task;
 uint64_t free_inflight,structural_faults;} task_reaper_stats_t;
void scheduler_reaper_stats_snapshot(task_reaper_stats_t *out);
bool scheduler_test_set_reap_grace_hold(task_id_t id,bool hold);
void scheduler_test_hold_next_zombie_on_cpu(void);
void scheduler_test_release_zombie_on_cpu(void);
bool scheduler_test_reset_zombie_age(task_id_t id);
bool scheduler_test_set_zombie_since_ms(task_id_t id,uint64_t since_ms);
bool scheduler_test_reap_mask(task_id_t id,uint64_t now_ms,uint64_t grace_ms,task_reap_defer_mask_t *out);
typedef struct {
 task_snapshot_t snapshot; task_reap_defer_mask_t mask; uint32_t total_holds;
 uint64_t observed_now_ms,zombie_since_ms;
 uint8_t found,held;
} scheduler_test_reap_observation_t;
bool scheduler_test_reap_observe_at(task_id_t id,uint64_t now_ms,uint64_t grace_ms,
                                    scheduler_test_reap_observation_t *out);
bool scheduler_test_reap_observe_now(task_id_t id,uint64_t grace_ms,
                                     scheduler_test_reap_observation_t *out);

void reaper_thread_entry(void *arg);

#endif

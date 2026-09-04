#ifndef MODAL_UI_H
#define MODAL_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "modal_session.h"

typedef int (*modal_ui_entry_fn)(modal_session_token_t token, void *ctx);
typedef void (*modal_ui_cleanup_fn)(void *ctx, int entry_result,
                                    bool entry_completed);
typedef enum {
    MODAL_UI_RUN_OK = 0, MODAL_UI_RUN_ENTRY_ERROR, MODAL_UI_RUN_BUSY,
    MODAL_UI_RUN_CONTEXT_ALLOC_FAILED, MODAL_UI_RUN_WORKER_CREATE_FAILED,
    MODAL_UI_RUN_BEGIN_FAILED, MODAL_UI_RUN_OWNER_KILLED,
    MODAL_UI_RUN_RECOVERED_OWNER_DEATH, MODAL_UI_RUN_TIMEOUT,
    MODAL_UI_RUN_INTERNAL_ERROR
} modal_ui_run_status_t;
typedef enum {
    MODAL_UI_CONTEXT_NEW = 0,
    MODAL_UI_CONTEXT_RESERVED,
    MODAL_UI_CONTEXT_WORKER_CREATED,
    MODAL_UI_CONTEXT_WAITING_CALLER,
    MODAL_UI_CONTEXT_RUNNING,
    MODAL_UI_CONTEXT_COMPLETING,
    MODAL_UI_CONTEXT_DONE,
    MODAL_UI_CONTEXT_QUARANTINED
} modal_ui_context_state_t;
typedef struct {
    modal_ui_run_status_t status;
    int entry_result;
    task_handle_t caller, worker;
    uint8_t worker_distinct, cleanup_completed, session_recovered;
    uint8_t caller_blocked_observed;
} modal_ui_run_result_t;
typedef struct {
    uint64_t runs, workers_created, worker_create_failures;
    uint64_t context_alloc_failures, begins, begin_failures;
    uint64_t normal_completions, killed_completions;
    uint64_t cleanup_closes, lifecycle_recoveries;
    uint64_t second_session_rejections, completion_signals;
    uint64_t completion_duplicates, contexts_live, contexts_quarantined;
    uint8_t active;
} modal_ui_stats_t;
typedef struct {
    uint8_t active;
    modal_ui_context_state_t state;
    task_handle_t caller, worker;
    uint8_t completion_claimed, completion_signaled;
    uint8_t cleanup_completed, lifecycle_recovered;
    uint8_t caller_blocked_observed;
    uint64_t contexts_live, contexts_quarantined;
} modal_ui_runtime_snapshot_t;
typedef struct {
    uint64_t generation;
    task_handle_t caller;
    task_handle_t worker;
    task_wait_result_t caller_wait_result;
    bool caller_cancel_latched;
    bool caller_before_wait_observed;
    bool caller_blocked_observed;
    bool worker_before_wait_allowed;
    bool completion_ready_before_wait;
    task_kill_result_t worker_kill_result;
    task_exit_reason_t worker_exit_reason;
    modal_ui_run_status_t status;
    bool cleanup_completed;
    bool session_recovered;
} modal_ui_test_run_snapshot_t;
typedef struct {
    bool fail_alloc;
    bool fail_create;
    bool skip_close;
    bool kill_next;
    bool kill_before_begin;
    bool hold_after_completion;
    bool release_after_completion;
    bool hold_caller_before_wait;
    bool release_caller_before_wait;
    bool caller_before_wait_observed;
    bool allow_worker_before_wait;
    bool hold_worker_for_reap;
    bool armed;
} modal_ui_test_controls_snapshot_t;

bool modal_ui_init(void);
modal_ui_run_status_t modal_ui_run_sync(const char *name,
    modal_ui_entry_fn entry, modal_ui_cleanup_fn cleanup, void *user_ctx,
    modal_ui_run_result_t *out);
void modal_ui_stats_snapshot(modal_ui_stats_t *out);
bool modal_ui_validate(uint64_t *violations);
bool modal_ui_runtime_snapshot(modal_ui_runtime_snapshot_t *out);
void modal_ui_test_fail_next_context_allocation(void);
void modal_ui_test_fail_next_worker_creation(void);
void modal_ui_test_skip_next_cleanup_close(void);
void modal_ui_test_kill_next_worker(void);
void modal_ui_test_kill_next_worker_before_begin(void);
void modal_ui_test_hold_next_worker_after_completion(void);
void modal_ui_test_release_worker_after_completion(void);
void modal_ui_test_hold_next_caller_before_wait(void);
void modal_ui_test_release_caller_before_wait(void);
bool modal_ui_test_caller_before_wait_observed(void);
void modal_ui_test_allow_next_worker_before_caller_wait(void);
void modal_ui_test_hold_next_worker_for_reap(void);
bool modal_ui_test_last_run_snapshot(modal_ui_test_run_snapshot_t *out);
void modal_ui_test_reset_controls(void);
bool modal_ui_test_controls_snapshot(modal_ui_test_controls_snapshot_t *out);

#endif

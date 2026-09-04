#ifndef MODAL_SESSION_H
#define MODAL_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include "input_event.h"
#include "input_router.h"
#include "scheduler.h"

typedef enum {
    MODAL_SESSION_INACTIVE = 0, MODAL_SESSION_OPENING,
    MODAL_SESSION_ACTIVE, MODAL_SESSION_CLOSING
} modal_session_state_t;

typedef struct {
    uint64_t session_generation, route_generation;
    task_id_t owner_task_id;
    uint64_t owner_lifecycle_generation;
} modal_session_token_t;
#define MODAL_SESSION_TOKEN_INVALID ((modal_session_token_t){0,0,TASK_ID_INVALID,0})

typedef enum {
    MODAL_BEGIN_OK = 0, MODAL_BEGIN_BUSY, MODAL_BEGIN_INVALID_OWNER,
    MODAL_BEGIN_ROUTER_REFUSED, MODAL_BEGIN_GENERATION_EXHAUSTED,
    MODAL_BEGIN_INTERNAL_ERROR
} modal_begin_result_t;
typedef enum {
    MODAL_END_OK = 0, MODAL_END_ALREADY_CLOSED, MODAL_END_INVALID_TOKEN,
    MODAL_END_WRONG_OWNER, MODAL_END_NOT_ACTIVE, MODAL_END_ROUTER_REFUSED,
    MODAL_END_INTERNAL_ERROR
} modal_end_result_t;
typedef enum {
    MODAL_INPUT_OK = 0, MODAL_INPUT_EMPTY, MODAL_INPUT_CANCELLED,
    MODAL_INPUT_INVALID_TOKEN, MODAL_INPUT_WRONG_OWNER,
    MODAL_INPUT_SESSION_CLOSED, MODAL_INPUT_STALE_EVENT, MODAL_INPUT_ERROR
} modal_input_result_t;
typedef enum {
    MODAL_RECOVERY_CLEANUP = 1, MODAL_RECOVERY_LIFECYCLE = 2,
    MODAL_RECOVERY_TEST = 3
} modal_recovery_reason_t;

typedef struct {
    modal_session_state_t state;
    uint64_t active_generation, route_generation;
    task_handle_t owner;
    char owner_name[32];
    modal_session_token_t last_closed_token;
    uint8_t shell_paused;
    input_route_state_t router_state;
    uint64_t opens, closes, rejected, invalid_tokens;
    uint64_t owner_death_recoveries, cleanup_recoveries;
} modal_session_snapshot_t;

void modal_session_init(void);
bool modal_session_token_is_valid(modal_session_token_t token);
bool modal_session_token_equal(modal_session_token_t a, modal_session_token_t b);
modal_begin_result_t modal_session_begin(const char *owner_name,
                                         modal_session_token_t *out_token);
modal_end_result_t modal_session_end(modal_session_token_t token);
bool modal_session_recover_owner(task_handle_t owner,
                                 modal_recovery_reason_t reason);
modal_input_result_t modal_session_try_pop(modal_session_token_t token,
                                           input_event_t *out);
modal_input_result_t modal_session_wait(modal_session_token_t token,
                                        input_event_t *out);
bool modal_session_is_active(void);
modal_session_state_t modal_session_state(void);
bool modal_session_snapshot(modal_session_snapshot_t *out);
bool modal_session_validate(uint64_t *violations);
void modal_session_test_set_quiet(bool quiet);

/* Test-only ownership regression helpers intentionally bypass caller ownership. */
bool modal_session_test_begin(const char *name);
bool modal_session_test_end(void);
bool modal_session_test_try_pop(input_event_t *out);

#endif

#ifndef INPUT_ROUTER_H
#define INPUT_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "input_event.h"
#include "input_queue.h"

typedef enum {
    INPUT_ROUTE_DEFAULT = 0,
    INPUT_ROUTE_OPENING,
    INPUT_ROUTE_MODAL,
    INPUT_ROUTE_CLOSING
} input_route_state_t;

typedef enum {
    INPUT_TRACE_NONE = 0,
    INPUT_TRACE_KEYBOARD = 1u << 0,
    INPUT_TRACE_ROUTER = 1u << 1,
    INPUT_TRACE_CONSUMER = 1u << 3,
    INPUT_TRACE_ALL = 0x0Bu
} input_trace_flags_t;

typedef enum {
    INPUT_TEST_HOOK_NONE = 0,
    INPUT_TEST_HOOK_BEGIN_OPENING,
    INPUT_TEST_HOOK_BEGIN_BEFORE_ACTIVE,
    INPUT_TEST_HOOK_END_CLOSING,
    INPUT_TEST_HOOK_END_BEFORE_DEFAULT
} input_test_hook_point_t;

typedef struct {
    uint64_t generation;
    uint32_t default_drained;
    uint32_t modal_drained;
} input_router_transition_result_t;

typedef struct {
    input_route_state_t state;
    uint64_t route_generation, dispatch_sequence;
    uint64_t dispatched, chars, specials, invalid;
    uint64_t default_routed, modal_routed;
    input_queue_stats_t default_queue, modal_queue;
    uint64_t begin_transitions, end_transitions, transition_failures;
    uint64_t typed_ahead_default_dropped, modal_tail_dropped;
    uint64_t default_drops, modal_drops;
} input_router_diagnostics_t;

void input_router_init(void);
void input_debug_set_trace_flags(uint32_t flags);
uint32_t input_debug_get_trace_flags(void);
void input_router_dispatch_char(char c);
void input_router_dispatch_special(uint8_t key);
void input_router_dispatch_event(input_event_t event);
input_event_t input_router_default_wait(void);
bool input_router_default_try_pop(input_event_t *out);
bool input_router_begin_modal(input_router_transition_result_t *out);
bool input_router_end_modal(input_router_transition_result_t *out);
bool input_router_set_modal_active(void);
void input_router_clear_modal(void);
bool input_router_has_modal(void);
input_route_state_t input_router_route_state(void);
input_event_t input_router_modal_wait(void);
bool input_router_modal_try_pop(input_event_t *out);
input_queue_pop_result_t input_router_modal_try_pop_entry(input_queue_entry_t *out);
input_queue_pop_result_t input_router_modal_wait_entry(input_queue_entry_t *out);
bool input_router_diagnostics_snapshot(input_router_diagnostics_t *out);
bool input_router_validate(uint64_t *violations);
void input_router_test_hook_arm(input_test_hook_point_t point);
bool input_router_test_hook_entered(void);
void input_router_test_hook_release(void);
bool input_router_test_nonatomic_route_negative(void);
void input_router_test_reject_next_end(bool reject);
void input_router_test_reject_next_begin(bool reject);

#endif

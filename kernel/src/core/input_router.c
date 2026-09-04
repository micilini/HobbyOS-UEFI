#include "input_router.h"
#include "scheduler.h"
#include "spinlock.h"
#include "panic.h"
#include "../drivers/serial.h"

#define INPUT_ROUTER_QUEUE_CAPACITY 128u

typedef struct {
    spinlock_t lock;
    input_route_state_t state;
    uint64_t generation, dispatch_sequence;
    uint64_t dispatched, chars, specials, invalid_events;
    uint64_t default_routed, modal_routed;
    uint64_t begin_transitions, end_transitions, transition_failures;
    uint64_t default_drained, modal_drained;
    uint64_t default_drops, modal_drops;
} input_router_state_t;

static input_queue_entry_t g_default_storage[INPUT_ROUTER_QUEUE_CAPACITY];
static input_queue_entry_t g_modal_storage[INPUT_ROUTER_QUEUE_CAPACITY];
static input_queue_t g_default_queue, g_modal_queue;
static input_router_state_t g_router;
static volatile uint32_t g_trace_flags;
static volatile uint32_t g_test_hook_point;
static volatile uint8_t g_test_hook_entered;
static volatile uint8_t g_test_hook_release;
static volatile uint8_t g_test_reject_next_end;
static volatile uint8_t g_test_reject_next_begin;

static void router_test_hook(input_test_hook_point_t point)
{
    if (__atomic_load_n(&g_test_hook_point, __ATOMIC_ACQUIRE) != (uint32_t)point)
        return;
    __atomic_store_n(&g_test_hook_entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_test_hook_release, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    __atomic_store_n(&g_test_hook_entered, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hook_point, INPUT_TEST_HOOK_NONE, __ATOMIC_RELEASE);
}

void input_router_test_hook_arm(input_test_hook_point_t point)
{
    __atomic_store_n(&g_test_hook_entered, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hook_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hook_point, (uint32_t)point, __ATOMIC_RELEASE);
}

bool input_router_test_hook_entered(void)
{
    return __atomic_load_n(&g_test_hook_entered, __ATOMIC_ACQUIRE) != 0;
}

void input_router_test_hook_release(void)
{
    __atomic_store_n(&g_test_hook_release, 1, __ATOMIC_RELEASE);
}

void input_router_test_reject_next_end(bool reject)
{
    __atomic_store_n(&g_test_reject_next_end, reject ? 1u : 0u,
                     __ATOMIC_RELEASE);
}

void input_router_test_reject_next_begin(bool reject)
{
    __atomic_store_n(&g_test_reject_next_begin, reject ? 1u : 0u,
                     __ATOMIC_RELEASE);
}

static void router_trace(input_event_t event, uint8_t destination,
                         uint64_t generation)
{
    if (!(input_debug_get_trace_flags() & INPUT_TRACE_ROUTER))
        return;
    serial_write_all("[INPUT-TRACE][ROUTER] destination=");
    serial_write_hex64_all(destination);
    serial_write_all(" generation=");
    serial_write_hex64_all(generation);
    serial_write_all(" type=");
    serial_write_hex64_all(event.type);
    serial_write_all(" value=");
    serial_write_hex64_all(event.value);
    serial_write_all("\n");
}

void input_debug_set_trace_flags(uint32_t flags)
{
    __atomic_store_n(&g_trace_flags, flags & INPUT_TRACE_ALL, __ATOMIC_RELEASE);
}

uint32_t input_debug_get_trace_flags(void)
{
    return __atomic_load_n(&g_trace_flags, __ATOMIC_ACQUIRE);
}

void input_router_init(void)
{
    spinlock_init(&g_router.lock);
    g_router.state = INPUT_ROUTE_DEFAULT;
    g_router.generation = 1;
    g_router.dispatch_sequence = 0;
    g_router.dispatched = g_router.chars = g_router.specials = 0;
    g_router.invalid_events = 0;
    g_router.default_routed = g_router.modal_routed = 0;
    g_router.begin_transitions = g_router.end_transitions = 0;
    g_router.transition_failures = 0;
    g_router.default_drained = g_router.modal_drained = 0;
    g_router.default_drops = g_router.modal_drops = 0;
    input_queue_init(&g_default_queue, g_default_storage,
                     INPUT_ROUTER_QUEUE_CAPACITY, "input-default");
    input_queue_init(&g_modal_queue, g_modal_storage,
                     INPUT_ROUTER_QUEUE_CAPACITY, "input-modal");
    input_debug_set_trace_flags(INPUT_TRACE_NONE);
    input_router_test_hook_arm(INPUT_TEST_HOOK_NONE);
    input_router_test_hook_release();
}

void input_router_dispatch_event(input_event_t event)
{
    uint8_t destination = 0;
    uint64_t generation = 0;
    bool fatal = false;
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    g_router.dispatch_sequence++;
    if (!input_event_is_valid(event)) {
        g_router.invalid_events++;
        spin_unlock_irqrestore(&g_router.lock, flags);
        return;
    }
    input_queue_t *queue = NULL;
    if (g_router.state == INPUT_ROUTE_DEFAULT) {
        queue = &g_default_queue;
        destination = INPUT_DEST_DEFAULT;
    } else if (g_router.state == INPUT_ROUTE_MODAL) {
        queue = &g_modal_queue;
        destination = INPUT_DEST_MODAL;
    } else {
        g_router.invalid_events++;
        spin_unlock_irqrestore(&g_router.lock, flags);
        return;
    }
    generation = g_router.generation;
    input_queue_push_result_t pushed = input_queue_push(
        queue, event, generation, destination, NULL);
    if (pushed == INPUT_QUEUE_PUSH_OK) {
        g_router.dispatched++;
        if (event.type == INPUT_EVENT_CHAR) g_router.chars++;
        else g_router.specials++;
        if (destination == INPUT_DEST_DEFAULT) g_router.default_routed++;
        else g_router.modal_routed++;
    } else if (pushed == INPUT_QUEUE_PUSH_FULL) {
        if (destination == INPUT_DEST_DEFAULT) g_router.default_drops++;
        else g_router.modal_drops++;
    } else {
        fatal = true;
    }
    spin_unlock_irqrestore(&g_router.lock, flags);
    router_trace(event, destination, generation);
    if (fatal)
        kpanic("INPUT_ROUTER: destination enqueue failed");
}

void input_router_dispatch_char(char c)
{
    input_router_dispatch_event(input_event_char(c));
}

void input_router_dispatch_special(uint8_t key)
{
    input_router_dispatch_event(input_event_special(key));
}

static input_event_t wait_event(input_queue_t *queue)
{
    input_event_t event = {0};
    input_queue_pop_result_t result = input_queue_wait_pop(queue, &event);
    if (result == INPUT_QUEUE_POP_CANCELLED)
        task_cancel_point();
    if (result != INPUT_QUEUE_POP_OK)
        kpanic("INPUT_ROUTER: wait failed");
    return event;
}

input_event_t input_router_default_wait(void)
{
    return wait_event(&g_default_queue);
}

bool input_router_default_try_pop(input_event_t *out)
{
    return out && input_queue_try_pop(&g_default_queue, out) == INPUT_QUEUE_POP_OK;
}

bool input_router_begin_modal(input_router_transition_result_t *out)
{
    input_router_transition_result_t result = {0};
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    if (__atomic_exchange_n(&g_test_reject_next_begin, 0, __ATOMIC_ACQ_REL) ||
        g_router.state != INPUT_ROUTE_DEFAULT) {
        g_router.transition_failures++;
        spin_unlock_irqrestore(&g_router.lock, flags);
        return false;
    }
    g_router.state = INPUT_ROUTE_OPENING;
    router_test_hook(INPUT_TEST_HOOK_BEGIN_OPENING);
    result.default_drained = input_queue_drain(&g_default_queue);
    result.modal_drained = input_queue_drain(&g_modal_queue);
    if (g_router.generation == UINT64_MAX) {
        spin_unlock_irqrestore(&g_router.lock, flags);
        kpanic("INPUT_ROUTER: generation exhausted");
    }
    result.generation = ++g_router.generation;
    g_router.default_drained += result.default_drained;
    g_router.modal_drained += result.modal_drained;
    g_router.begin_transitions++;
    router_test_hook(INPUT_TEST_HOOK_BEGIN_BEFORE_ACTIVE);
    g_router.state = INPUT_ROUTE_MODAL;
    spin_unlock_irqrestore(&g_router.lock, flags);
    if (out) *out = result;
    return true;
}

bool input_router_end_modal(input_router_transition_result_t *out)
{
    input_router_transition_result_t result = {0};
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    if (__atomic_exchange_n(&g_test_reject_next_end, 0,
                            __ATOMIC_ACQ_REL)) {
        g_router.transition_failures++;
        spin_unlock_irqrestore(&g_router.lock, flags);
        return false;
    }
    if (g_router.state != INPUT_ROUTE_MODAL) {
        g_router.transition_failures++;
        spin_unlock_irqrestore(&g_router.lock, flags);
        return false;
    }
    g_router.state = INPUT_ROUTE_CLOSING;
    router_test_hook(INPUT_TEST_HOOK_END_CLOSING);
    result.modal_drained = input_queue_drain(&g_modal_queue);
    if (g_router.generation == UINT64_MAX) {
        spin_unlock_irqrestore(&g_router.lock, flags);
        kpanic("INPUT_ROUTER: generation exhausted");
    }
    result.generation = ++g_router.generation;
    g_router.modal_drained += result.modal_drained;
    g_router.end_transitions++;
    router_test_hook(INPUT_TEST_HOOK_END_BEFORE_DEFAULT);
    g_router.state = INPUT_ROUTE_DEFAULT;
    spin_unlock_irqrestore(&g_router.lock, flags);
    if (out) *out = result;
    return true;
}

bool input_router_set_modal_active(void)
{
    return input_router_begin_modal(NULL);
}

void input_router_clear_modal(void)
{
    (void)input_router_end_modal(NULL);
}

bool input_router_has_modal(void)
{
    input_route_state_t state = input_router_route_state();
    return state != INPUT_ROUTE_DEFAULT;
}

input_route_state_t input_router_route_state(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    input_route_state_t state = g_router.state;
    spin_unlock_irqrestore(&g_router.lock, flags);
    return state;
}

input_event_t input_router_modal_wait(void)
{
    return wait_event(&g_modal_queue);
}

bool input_router_modal_try_pop(input_event_t *out)
{
    return out && input_queue_try_pop(&g_modal_queue, out) == INPUT_QUEUE_POP_OK;
}

input_queue_pop_result_t input_router_modal_try_pop_entry(input_queue_entry_t *out)
{
    return input_queue_try_pop_entry(&g_modal_queue, out);
}

input_queue_pop_result_t input_router_modal_wait_entry(input_queue_entry_t *out)
{
    return input_queue_wait_pop_entry(&g_modal_queue, out);
}

bool input_router_diagnostics_snapshot(input_router_diagnostics_t *out)
{
    if (!out) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    *out = (input_router_diagnostics_t){
        .state = g_router.state, .route_generation = g_router.generation,
        .dispatch_sequence = g_router.dispatch_sequence,
        .dispatched = g_router.dispatched, .chars = g_router.chars,
        .specials = g_router.specials, .invalid = g_router.invalid_events,
        .default_routed = g_router.default_routed,
        .modal_routed = g_router.modal_routed,
        .begin_transitions = g_router.begin_transitions,
        .end_transitions = g_router.end_transitions,
        .transition_failures = g_router.transition_failures,
        .typed_ahead_default_dropped = g_router.default_drained,
        .modal_tail_dropped = g_router.modal_drained,
        .default_drops = g_router.default_drops,
        .modal_drops = g_router.modal_drops
    };
    input_queue_snapshot(&g_default_queue, &out->default_queue);
    input_queue_snapshot(&g_modal_queue, &out->modal_queue);
    spin_unlock_irqrestore(&g_router.lock, flags);
    return true;
}

bool input_router_validate(uint64_t *violations)
{
    input_router_diagnostics_t diagnostics;
    input_queue_validation_t default_validation, modal_validation;
    uint64_t count = 0;
    if (!input_router_diagnostics_snapshot(&diagnostics)) count++;
    if (!input_queue_validate(&g_default_queue, &default_validation))
        count += default_validation.violations;
    if (!input_queue_validate(&g_modal_queue, &modal_validation))
        count += modal_validation.violations;
    if (diagnostics.state != INPUT_ROUTE_DEFAULT &&
        diagnostics.state != INPUT_ROUTE_MODAL)
        count++;
    if (!diagnostics.route_generation)
        count++;
    if (diagnostics.dispatched != diagnostics.default_routed +
                                  diagnostics.modal_routed)
        count++;
    if (diagnostics.dispatch_sequence != diagnostics.dispatched +
                                         diagnostics.invalid +
                                         diagnostics.default_drops +
                                         diagnostics.modal_drops)
        count++;
    if (violations) *violations = count;
    return count == 0;
}

bool input_router_test_nonatomic_route_negative(void)
{
#ifdef HOBBYOS_INPUT_NEGATIVE_ROUTE_AFTER_UNLOCK
    input_event_t event = input_event_char('N');
    input_queue_t *stale_destination;
    uint64_t stale_generation;
    irq_flags_t flags = spin_lock_irqsave(&g_router.lock);
    if (g_router.state != INPUT_ROUTE_MODAL) {
        spin_unlock_irqrestore(&g_router.lock, flags);
        return false;
    }
    stale_destination = &g_modal_queue;
    stale_generation = g_router.generation;
    spin_unlock_irqrestore(&g_router.lock, flags);
    if (!input_router_end_modal(NULL)) return false;
    if (input_queue_push(stale_destination, event, stale_generation,
                         INPUT_DEST_MODAL, NULL) != INPUT_QUEUE_PUSH_OK)
        return false;
    input_event_t leaked;
    bool detected = input_queue_try_pop(stale_destination, &leaked) == INPUT_QUEUE_POP_OK &&
                    leaked.value == event.value && g_router.state == INPUT_ROUTE_DEFAULT;
    return detected;
#else
    return false;
#endif
}

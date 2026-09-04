#include "input_queue.h"
#include "scheduler.h"
#include "panic.h"
#include "../libc/memory.h"

bool input_event_is_valid(input_event_t event)
{
    return event.type == INPUT_EVENT_CHAR || event.type == INPUT_EVENT_SPECIAL;
}

bool input_queue_init(input_queue_t *queue, input_queue_entry_t *storage,
                      uint32_t capacity, const char *debug_name)
{
    if (!queue || !storage || !capacity)
        return false;
    memset(queue, 0, sizeof(*queue));
    memset(storage, 0, capacity * sizeof(*storage));
    queue->storage = storage;
    queue->capacity = capacity;
    queue->next_sequence = 1;
    queue->generation = 1;
    queue->debug_name = debug_name;
    wait_queue_init(&queue->waiters);
    spinlock_init(&queue->lock);
    return true;
}

input_queue_push_result_t input_queue_push(input_queue_t *queue,
    input_event_t event, uint64_t route_generation, uint8_t destination,
    input_queue_entry_t *out_enqueued)
{
    if (!queue || !queue->storage || !queue->capacity ||
        !input_event_is_valid(event) || !destination)
        return INPUT_QUEUE_PUSH_INVALID;
    bool fatal = false;
    input_queue_entry_t entry;
    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    if (queue->count == queue->capacity) {
        queue->drops++;
        spin_unlock_irqrestore(&queue->lock, flags);
        return INPUT_QUEUE_PUSH_FULL;
    }
    if (!queue->next_sequence) {
        spin_unlock_irqrestore(&queue->lock, flags);
        return INPUT_QUEUE_PUSH_FATAL;
    }
    entry = (input_queue_entry_t){
        .event = event,
        .queue_sequence = queue->next_sequence++,
        .route_generation = route_generation,
        .destination = destination
    };
    queue->storage[queue->head] = entry;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;
    queue->pushes++;
#ifdef HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT
    if (destination == INPUT_DEST_TEST) {
        queue->negative_test_queue = 1;
        queue->negative_legacy_permits++;
    }
#endif
    if (queue->count > queue->max_depth)
        queue->max_depth = queue->count;
    scheduler_wake_status_t wake = scheduler_wake_one_waiter(
        &queue->waiters, TASK_WAIT_INPUT_QUEUE, (uintptr_t)queue,
        TASK_WAKE_SIGNAL);
    if (wake == SCHED_WAKE_WON)
        queue->wake_wins++;
    else if (wake == SCHED_WAKE_NO_MATCH)
        queue->wake_no_match++;
    else
        fatal = true;
    spin_unlock_irqrestore(&queue->lock, flags);
    if (out_enqueued)
        *out_enqueued = entry;
    if (fatal)
        kpanic("INPUT_QUEUE: waiter wake failed");
    return fatal ? INPUT_QUEUE_PUSH_FATAL : INPUT_QUEUE_PUSH_OK;
}

static input_queue_pop_result_t pop_entry_locked(input_queue_t *queue,
                                                  input_queue_entry_t *out)
{
    if (!queue->count)
        return INPUT_QUEUE_POP_EMPTY;
    input_queue_entry_t entry = queue->storage[queue->tail];
    if (!entry.queue_sequence || !input_event_is_valid(entry.event))
        return INPUT_QUEUE_POP_ERROR;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;
    queue->pops++;
    if (out)
        *out = entry;
    return INPUT_QUEUE_POP_OK;
}

input_queue_pop_result_t input_queue_try_pop_entry(input_queue_t *queue,
                                                   input_queue_entry_t *out)
{
    if (!queue || !out || !queue->storage || !queue->capacity)
        return INPUT_QUEUE_POP_ERROR;
    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    input_queue_pop_result_t result = pop_entry_locked(queue, out);
    spin_unlock_irqrestore(&queue->lock, flags);
    return result;
}

input_queue_pop_result_t input_queue_try_pop(input_queue_t *queue,
                                             input_event_t *out)
{
    if (!out)
        return INPUT_QUEUE_POP_ERROR;
    input_queue_entry_t entry;
    input_queue_pop_result_t result = input_queue_try_pop_entry(queue, &entry);
    if (result == INPUT_QUEUE_POP_OK)
        *out = entry.event;
    return result;
}

input_queue_pop_result_t input_queue_wait_pop_entry(input_queue_t *queue,
                                                    input_queue_entry_t *out)
{
    if (!queue || !out || !queue->storage || !queue->capacity)
        return INPUT_QUEUE_POP_ERROR;
    for (;;) {
        irq_flags_t flags = spin_lock_irqsave(&queue->lock);
        input_queue_entry_t entry;
        input_queue_pop_result_t popped = pop_entry_locked(queue, &entry);
        if (popped == INPUT_QUEUE_POP_OK) {
            spin_unlock_irqrestore(&queue->lock, flags);
            *out = entry;
            return INPUT_QUEUE_POP_OK;
        }
        if (popped == INPUT_QUEUE_POP_ERROR) {
            spin_unlock_irqrestore(&queue->lock, flags);
            return INPUT_QUEUE_POP_ERROR;
        }
#ifdef HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT
        if (queue->negative_test_queue && queue->negative_legacy_permits) {
            queue->negative_legacy_permits--;
            queue->phantom_empty_wakes++;
            spin_unlock_irqrestore(&queue->lock, flags);
            return INPUT_QUEUE_POP_ERROR;
        }
#endif
        task_block_token_t token;
        scheduler_block_prepare_result_t prepared = scheduler_prepare_block_interruptible(
                                     &queue->waiters, TASK_BLOCKED,
                                     TASK_WAIT_INPUT_QUEUE, (uintptr_t)queue, &token);
        if (prepared != SCHED_BLOCK_PREPARED) {
            spin_unlock_irqrestore(&queue->lock, flags);
            if (prepared == SCHED_BLOCK_CANCELLED) {
                irq_flags_t stats_flags = spin_lock_irqsave(&queue->lock);
                queue->cancelled_waits++;
                spin_unlock_irqrestore(&queue->lock, stats_flags);
                return INPUT_QUEUE_POP_CANCELLED;
            }
            return INPUT_QUEUE_POP_ERROR;
        }
        spin_unlock(&queue->lock);
        task_wait_result_t wait = scheduler_commit_block(&token);
        irq_restore(flags);
        if (wait == TASK_WAIT_RESULT_OK)
            continue;
        if (wait == TASK_WAIT_RESULT_SPURIOUS) {
            irq_flags_t stats_flags = spin_lock_irqsave(&queue->lock);
            queue->spurious_wakes++;
            spin_unlock_irqrestore(&queue->lock, stats_flags);
            continue;
        }
        if (wait == TASK_WAIT_RESULT_CANCELLED) {
            irq_flags_t stats_flags = spin_lock_irqsave(&queue->lock);
            queue->cancelled_waits++;
            spin_unlock_irqrestore(&queue->lock, stats_flags);
            return INPUT_QUEUE_POP_CANCELLED;
        }
        return INPUT_QUEUE_POP_ERROR;
    }
}

input_queue_pop_result_t input_queue_wait_pop(input_queue_t *queue,
                                              input_event_t *out)
{
    if (!out)
        return INPUT_QUEUE_POP_ERROR;
    input_queue_entry_t entry;
    input_queue_pop_result_t result = input_queue_wait_pop_entry(queue, &entry);
    if (result == INPUT_QUEUE_POP_OK)
        *out = entry.event;
    return result;
}

uint32_t input_queue_drain(input_queue_t *queue)
{
    if (!queue || !queue->storage || !queue->capacity)
        return 0;
    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    uint32_t drained = queue->count;
    queue->tail = queue->head;
    queue->count = 0;
    queue->generation++;
    if (!queue->generation)
        queue->generation = 1;
    queue->drains++;
    queue->drained_events += drained;
    spin_unlock_irqrestore(&queue->lock, flags);
    return drained;
}

bool input_queue_snapshot(input_queue_t *queue, input_queue_stats_t *out)
{
    if (!queue || !out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    *out = (input_queue_stats_t){
        .capacity = queue->capacity, .head = queue->head,
        .tail = queue->tail, .count = queue->count,
        .max_depth = queue->max_depth, .next_sequence = queue->next_sequence,
        .generation = queue->generation, .pushes = queue->pushes,
        .pops = queue->pops, .drains = queue->drains,
        .drained_events = queue->drained_events, .drops = queue->drops,
        .wake_wins = queue->wake_wins, .wake_no_match = queue->wake_no_match,
        .spurious_wakes = queue->spurious_wakes,
        .cancelled_waits = queue->cancelled_waits,
        .phantom_empty_wakes = queue->phantom_empty_wakes
    };
    spin_unlock_irqrestore(&queue->lock, flags);
    return true;
}

bool input_queue_validate(input_queue_t *queue,
                          input_queue_validation_t *out)
{
    if (!queue || !out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&queue->lock);
    uint64_t violations = 0;
    if (!queue->storage || !queue->capacity)
        violations++;
    if (queue->capacity && (queue->head >= queue->capacity ||
                            queue->tail >= queue->capacity))
        violations++;
    if (queue->count > queue->capacity)
        violations++;
    if ((queue->count == 0 || queue->count == queue->capacity) &&
        queue->head != queue->tail)
        violations++;
    if (queue->pushes != queue->pops + queue->drained_events + queue->count)
        violations++;
    if (queue->max_depth > queue->capacity || !queue->next_sequence ||
        !queue->generation)
        violations++;
    *out = (input_queue_validation_t){.valid = violations == 0,
                                      .violations = violations};
    spin_unlock_irqrestore(&queue->lock, flags);
    return violations == 0;
}

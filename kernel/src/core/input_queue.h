#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "input_event.h"
#include "spinlock.h"
#include "task.h"

typedef enum {
    INPUT_DEST_DEFAULT = 1,
    INPUT_DEST_MODAL = 2,
    INPUT_DEST_KEYBOARD_FIFO = 3,
    INPUT_DEST_TEST = 4
} input_destination_t;

typedef struct {
    input_event_t event;
    uint64_t queue_sequence;
    uint64_t route_generation;
    uint8_t destination;
} input_queue_entry_t;

typedef struct {
    uint32_t capacity, head, tail, count, max_depth;
    uint64_t next_sequence, generation, pushes, pops, drains, drained_events;
    uint64_t drops, wake_wins, wake_no_match, spurious_wakes, cancelled_waits;
    uint64_t phantom_empty_wakes;
} input_queue_stats_t;

typedef struct {
    uint8_t valid;
    uint64_t violations;
} input_queue_validation_t;

typedef struct {
    input_queue_entry_t *storage;
    uint32_t capacity, head, tail, count;
    uint64_t next_sequence, generation;
    uint64_t pushes, pops, drains, drained_events, drops;
    uint64_t wake_wins, wake_no_match, spurious_wakes, cancelled_waits;
    uint64_t phantom_empty_wakes;
    uint32_t negative_legacy_permits;
    uint8_t negative_test_queue;
    uint32_t max_depth;
    wait_queue_t waiters;
    spinlock_t lock;
    const char *debug_name;
} input_queue_t;

typedef enum {
    INPUT_QUEUE_PUSH_OK = 0,
    INPUT_QUEUE_PUSH_FULL,
    INPUT_QUEUE_PUSH_INVALID,
    INPUT_QUEUE_PUSH_FATAL
} input_queue_push_result_t;

typedef enum {
    INPUT_QUEUE_POP_OK = 0,
    INPUT_QUEUE_POP_EMPTY,
    INPUT_QUEUE_POP_CANCELLED,
    INPUT_QUEUE_POP_ERROR
} input_queue_pop_result_t;

bool input_event_is_valid(input_event_t event);
bool input_queue_init(input_queue_t *queue, input_queue_entry_t *storage,
                      uint32_t capacity, const char *debug_name);
input_queue_push_result_t input_queue_push(input_queue_t *queue,
    input_event_t event, uint64_t route_generation, uint8_t destination,
    input_queue_entry_t *out_enqueued);
input_queue_pop_result_t input_queue_try_pop(input_queue_t *queue,
                                             input_event_t *out);
input_queue_pop_result_t input_queue_try_pop_entry(input_queue_t *queue,
                                                   input_queue_entry_t *out);
input_queue_pop_result_t input_queue_wait_pop(input_queue_t *queue,
                                              input_event_t *out);
input_queue_pop_result_t input_queue_wait_pop_entry(input_queue_t *queue,
                                                    input_queue_entry_t *out);
uint32_t input_queue_drain(input_queue_t *queue);
bool input_queue_snapshot(input_queue_t *queue, input_queue_stats_t *out);
bool input_queue_validate(input_queue_t *queue,
                          input_queue_validation_t *out);

#endif

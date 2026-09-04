#ifndef SEMAPHORE_H
#define SEMAPHORE_H
#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"
#include "task.h"
typedef struct { volatile int count; wait_queue_t wait_queue; spinlock_t lock; } semaphore_t;

typedef enum
{
    SEM_SIGNAL_WOKE_WAITER = 0,
    SEM_SIGNAL_ADDED_PERMIT,
    SEM_SIGNAL_OVERFLOW,
    SEM_SIGNAL_FATAL,
    SEM_SIGNAL_INVALID
} sem_signal_result_t;

typedef struct
{
    int count;
    uint32_t waiters;
} semaphore_debug_snapshot_t;

typedef struct
{
    semaphore_t *semaphore;
    task_handle_t task;
    uint64_t wait_generation;
    task_wait_kind_t wait_kind;
    uint8_t prepared;
} sem_test_after_prepare_event_t;

typedef void (*sem_test_after_prepare_hook_fn)(
    const sem_test_after_prepare_event_t *event,
    void *ctx);

typedef struct
{
    uint8_t active;
    semaphore_t *target_semaphore;
    task_handle_t target_task;
    uint64_t generation;
    uint64_t target_hits;
    uint64_t foreign_hits;
    uint64_t callback_calls;
    uint64_t callback_inflight;
} sem_test_after_prepare_snapshot_t;

void sem_init(semaphore_t *sem, int initial_count);
bool sem_try_wait(semaphore_t *sem);
task_wait_result_t sem_wait_interruptible(semaphore_t *sem);
void sem_wait(semaphore_t *sem);
sem_signal_result_t sem_signal_detailed(semaphore_t *sem);
bool sem_signal(semaphore_t *sem);
bool semaphore_debug_snapshot(semaphore_t *sem,
                              semaphore_debug_snapshot_t *out);
bool sem_test_arm_after_prepare_observer(
    semaphore_t *target_semaphore,
    task_handle_t target_task,
    sem_test_after_prepare_hook_fn hook,
    void *ctx);
bool sem_test_clear_after_prepare_observer(uint64_t timeout_ms);
void sem_test_after_prepare_snapshot(
    sem_test_after_prepare_snapshot_t *out);
bool sem_test_after_prepare_observer_selftest(void);
#endif

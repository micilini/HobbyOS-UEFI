#include "semaphore.h"
#include "scheduler.h"
#include <limits.h>
#include "panic.h"
#include "../drivers/timer.h"

typedef struct
{
    uint8_t active;
    semaphore_t *target_semaphore;
    task_handle_t target_task;
    sem_test_after_prepare_hook_fn hook;
    void *ctx;
    uint64_t generation;
    uint64_t target_hits;
    uint64_t foreign_hits;
    uint64_t callback_calls;
    uint64_t callback_inflight;
} sem_test_observer_t;

static spinlock_t g_sem_test_observer_lock;
static sem_test_observer_t g_sem_test_observer;

static bool task_handle_valid(task_handle_t handle)
{
    return handle.id != TASK_ID_INVALID && handle.lifecycle_generation != 0;
}

bool sem_test_arm_after_prepare_observer(
    semaphore_t *target_semaphore,
    task_handle_t target_task,
    sem_test_after_prepare_hook_fn hook,
    void *ctx)
{
    if (!target_semaphore || !task_handle_valid(target_task) || !hook || !ctx)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_sem_test_observer_lock);
    if (g_sem_test_observer.active || g_sem_test_observer.callback_inflight ||
        g_sem_test_observer.hook || g_sem_test_observer.ctx)
    {
        spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
        return false;
    }
    g_sem_test_observer.target_semaphore = target_semaphore;
    g_sem_test_observer.target_task = target_task;
    g_sem_test_observer.hook = hook;
    g_sem_test_observer.ctx = ctx;
    g_sem_test_observer.target_hits = 0;
    g_sem_test_observer.foreign_hits = 0;
    g_sem_test_observer.callback_calls = 0;
    g_sem_test_observer.callback_inflight = 0;
    g_sem_test_observer.generation++;
    if (!g_sem_test_observer.generation)
        g_sem_test_observer.generation++;
    g_sem_test_observer.active = 1;
    spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
    return true;
}

bool sem_test_clear_after_prepare_observer(uint64_t timeout_ms)
{
    irq_flags_t flags = spin_lock_irqsave(&g_sem_test_observer_lock);
    g_sem_test_observer.active = 0;
    g_sem_test_observer.generation++;
    if (!g_sem_test_observer.generation)
        g_sem_test_observer.generation++;
    spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);

    uint64_t started = timer_get_uptime_ms();
    for (;;)
    {
        flags = spin_lock_irqsave(&g_sem_test_observer_lock);
        uint64_t inflight = g_sem_test_observer.callback_inflight;
        if (!inflight)
        {
            g_sem_test_observer.target_semaphore = NULL;
            g_sem_test_observer.target_task = TASK_HANDLE_INVALID;
            g_sem_test_observer.hook = NULL;
            g_sem_test_observer.ctx = NULL;
            spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
            return true;
        }
        spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
        if (timer_get_uptime_ms() - started >= timeout_ms)
            return false;
        spin_cpu_relax();
    }
}

void sem_test_after_prepare_snapshot(sem_test_after_prepare_snapshot_t *out)
{
    if (!out)
        return;
    irq_flags_t flags = spin_lock_irqsave(&g_sem_test_observer_lock);
    out->active = g_sem_test_observer.active;
    out->target_semaphore = g_sem_test_observer.target_semaphore;
    out->target_task = g_sem_test_observer.target_task;
    out->generation = g_sem_test_observer.generation;
    out->target_hits = g_sem_test_observer.target_hits;
    out->foreign_hits = g_sem_test_observer.foreign_hits;
    out->callback_calls = g_sem_test_observer.callback_calls;
    out->callback_inflight = g_sem_test_observer.callback_inflight;
    spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
}

static void sem_test_notify_after_prepare(
    const sem_test_after_prepare_event_t *event)
{
    sem_test_after_prepare_hook_fn hook = NULL;
    void *ctx = NULL;
    irq_flags_t flags = spin_lock_irqsave(&g_sem_test_observer_lock);
    if (g_sem_test_observer.active)
    {
        bool target = event &&
            event->semaphore == g_sem_test_observer.target_semaphore &&
            event->task.id == g_sem_test_observer.target_task.id &&
            event->task.lifecycle_generation ==
                g_sem_test_observer.target_task.lifecycle_generation;
        if (target)
        {
            g_sem_test_observer.target_hits++;
            g_sem_test_observer.callback_calls++;
            g_sem_test_observer.callback_inflight++;
            hook = g_sem_test_observer.hook;
            ctx = g_sem_test_observer.ctx;
        }
        else
        {
            g_sem_test_observer.foreign_hits++;
        }
    }
    spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);

    if (!hook)
        return;
    hook(event, ctx);
    flags = spin_lock_irqsave(&g_sem_test_observer_lock);
    if (g_sem_test_observer.callback_inflight)
        g_sem_test_observer.callback_inflight--;
    spin_unlock_irqrestore(&g_sem_test_observer_lock, flags);
}

void sem_init(semaphore_t *sem, int initial_count)
{
    if (!sem) return;
    sem->count = initial_count < 0 ? 0 : initial_count;
    wait_queue_init(&sem->wait_queue);
    spinlock_init(&sem->lock);
}

bool sem_try_wait(semaphore_t *sem)
{
    if (!sem) return false;
    irq_flags_t flags = spin_lock_irqsave(&sem->lock);
    bool acquired = sem->count > 0;
    if (acquired) sem->count--;
    spin_unlock_irqrestore(&sem->lock, flags);
    return acquired;
}

task_wait_result_t sem_wait_interruptible(semaphore_t *sem)
{
    if (!sem) return TASK_WAIT_RESULT_ERROR;
    for (;;) {
        irq_flags_t flags = spin_lock_irqsave(&sem->lock);
        if (sem->count > 0) {
            sem->count--;
            spin_unlock_irqrestore(&sem->lock, flags);
            return TASK_WAIT_RESULT_OK;
        }
        task_block_token_t token;
#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
        spin_unlock(&sem->lock);
        sem_test_after_prepare_event_t event = {
            .semaphore = sem,
            .task = TASK_HANDLE_INVALID,
            .wait_generation = 0,
            .wait_kind = TASK_WAIT_SEMAPHORE,
            .prepared = 0
        };
        (void)scheduler_current_task_handle(&event.task);
        sem_test_notify_after_prepare(&event);
        if (scheduler_prepare_block_interruptible(&sem->wait_queue, TASK_BLOCKED,
                                     TASK_WAIT_SEMAPHORE, (uintptr_t)sem, &token) != SCHED_BLOCK_PREPARED) {
            irq_restore(flags); return TASK_WAIT_RESULT_ERROR;
        }
#else
        scheduler_block_prepare_result_t prepared = scheduler_prepare_block_interruptible(
                                     &sem->wait_queue, TASK_BLOCKED,
                                     TASK_WAIT_SEMAPHORE, (uintptr_t)sem, &token);
        if (prepared != SCHED_BLOCK_PREPARED) {
            spin_unlock_irqrestore(&sem->lock, flags);
            return prepared == SCHED_BLOCK_CANCELLED ? TASK_WAIT_RESULT_CANCELLED : TASK_WAIT_RESULT_ERROR;
        }
        spin_unlock(&sem->lock);
        sem_test_after_prepare_event_t event = {
            .semaphore = sem,
            .task = {
                .id = token.task->id,
                .lifecycle_generation = token.lifecycle_generation
            },
            .wait_generation = token.wait_generation,
            .wait_kind = token.kind,
            .prepared = 1
        };
        sem_test_notify_after_prepare(&event);
#endif
        task_wait_result_t result = scheduler_commit_block(&token);
        irq_restore(flags);
        if (result == TASK_WAIT_RESULT_SPURIOUS) continue;
        return result;
    }
}

void sem_wait(semaphore_t *sem)
{
    task_wait_result_t r=sem_wait_interruptible(sem);
    if(r==TASK_WAIT_RESULT_CANCELLED) task_cancel_point();
    if(r!=TASK_WAIT_RESULT_OK) kpanic("SEM: non-success wait result");
}

sem_signal_result_t sem_signal_detailed(semaphore_t *sem)
{
    if (!sem) return SEM_SIGNAL_INVALID;
    irq_flags_t flags = spin_lock_irqsave(&sem->lock);
    scheduler_wake_status_t wake = scheduler_wake_one_waiter(&sem->wait_queue, TASK_WAIT_SEMAPHORE,
                                                                  (uintptr_t)sem, TASK_WAKE_SIGNAL);
    if (wake == SCHED_WAKE_WON) {
        spin_unlock_irqrestore(&sem->lock, flags);
        return SEM_SIGNAL_WOKE_WAITER;
    }
    if (wake == SCHED_WAKE_FATAL) {
        spin_unlock_irqrestore(&sem->lock, flags);
        kpanic("SEM: waiter wake enqueue failed");
        return SEM_SIGNAL_FATAL;
    }
    if (sem->count == INT_MAX) {
        spin_unlock_irqrestore(&sem->lock, flags);
        return SEM_SIGNAL_OVERFLOW;
    }
    sem->count++;
    spin_unlock_irqrestore(&sem->lock, flags);
    return SEM_SIGNAL_ADDED_PERMIT;
}

bool sem_signal(semaphore_t *sem)
{
    sem_signal_result_t result = sem_signal_detailed(sem);
    return result == SEM_SIGNAL_WOKE_WAITER ||
           result == SEM_SIGNAL_ADDED_PERMIT;
}

bool semaphore_debug_snapshot(semaphore_t *sem,
                              semaphore_debug_snapshot_t *out)
{
    if (!sem || !out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&sem->lock);
    uint32_t waiters = 0;
    struct list_head *position;
    list_for_each(position, &sem->wait_queue.head)
        waiters++;
    out->count = sem->count;
    out->waiters = waiters;
    spin_unlock_irqrestore(&sem->lock, flags);
    return true;
}

static void sem_test_selftest_hook(
    const sem_test_after_prepare_event_t *event,
    void *ctx)
{
    (void)event;
    (void)ctx;
}

bool sem_test_after_prepare_observer_selftest(void)
{
    static semaphore_t test_semaphore;
    static uint8_t test_ctx;
    sem_test_after_prepare_snapshot_t before = {0}, armed = {0}, after = {0};
    sem_test_after_prepare_snapshot(&before);
    if (before.active || before.callback_inflight)
        return false;
    sem_init(&test_semaphore, 0);
    task_handle_t target = {.id = UINT64_MAX, .lifecycle_generation = 1};
    if (sem_test_arm_after_prepare_observer(
            &test_semaphore, target, sem_test_selftest_hook, NULL))
        return false;
    if (!sem_test_arm_after_prepare_observer(
            &test_semaphore, target, sem_test_selftest_hook, &test_ctx))
        return false;
    sem_test_after_prepare_snapshot(&armed);
    bool coherent = armed.active &&
        armed.target_semaphore == &test_semaphore &&
        armed.target_task.id == target.id &&
        armed.target_task.lifecycle_generation == target.lifecycle_generation &&
        armed.generation > before.generation &&
        armed.callback_inflight == 0;
    bool exclusive = !sem_test_arm_after_prepare_observer(
        &test_semaphore, target, sem_test_selftest_hook, &test_ctx);
    bool cleared = sem_test_clear_after_prepare_observer(10);
    sem_test_after_prepare_snapshot(&after);
    return coherent && exclusive && cleared && !after.active &&
           after.callback_inflight == 0 && after.generation > armed.generation;
}

#ifndef TASK_LIFECYCLE_H
#define TASK_LIFECYCLE_H
#include <stdbool.h>
#include <stdint.h>
#include "task.h"

typedef struct {
    task_id_t task_id;
    uint64_t lifecycle_generation, zombie_generation;
    task_exit_reason_t reason;
    uint64_t kill_requested_ns, runtime_ns_total, exit_started_ns;
    uint64_t cleanup_completed_ns, notification_ns;
    char name[32];
    uint32_t flags, last_cpu_slot;
} task_lifecycle_exit_event_t;

typedef void (*task_lifecycle_exit_listener_fn)(const task_lifecycle_exit_event_t *, void *);
typedef struct {
    uint64_t registered, unregistered, published, callbacks_invoked;
    uint64_t duplicate_publish, publish_failures;
    uint64_t registration_failures, unregister_busy, invalid_events;
    uint64_t callbacks_in_flight;
    uint32_t listeners;
} task_lifecycle_stats_t;

void task_lifecycle_init(void);
bool task_lifecycle_register_exit_listener(task_lifecycle_exit_listener_fn fn, void *ctx);
bool task_lifecycle_unregister_exit_listener(task_lifecycle_exit_listener_fn fn, void *ctx);
/* false after a match means RETIRING/BUSY: ctx must remain alive and the
 * caller must retry until true. No new publish copies a retiring slot. */
bool task_lifecycle_publish_exit(const task_lifecycle_exit_event_t *event);
void task_lifecycle_get_stats(task_lifecycle_stats_t *out);
#endif

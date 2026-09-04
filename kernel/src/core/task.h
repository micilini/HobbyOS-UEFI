#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "list.h"

typedef uint64_t task_id_t;
#define TASK_ID_INVALID ((task_id_t)0)
typedef struct { task_id_t id; uint64_t lifecycle_generation; } task_handle_t;
#define TASK_HANDLE_INVALID ((task_handle_t){.id=TASK_ID_INVALID,.lifecycle_generation=0})
#define TASK_CPU_SLOT_NONE UINT32_MAX
#define HOBBYOS_KERNEL_STACK_SIZE (16u * 1024u)
#define HOBBYOS_KERNEL_STACK_GUARD_BYTES 512u
#define HOBBYOS_KERNEL_STACK_CANARY 0xA5A5A5A5A5A5A5A5ULL
#define HOBBYOS_MAX_STATIC_STACK_FRAME 2048u

typedef enum
{
    TASK_EXIT_NONE = 0, TASK_EXIT_NORMAL, TASK_EXIT_KILLED,
    TASK_EXIT_INIT_FAILURE, TASK_EXIT_INTERNAL_ERROR
} task_exit_reason_t;
typedef void (*task_cleanup_fn)(void *ctx);

typedef enum
{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state_t;

#define TASK_CLASS_INTERACTIVE 0
#define TASK_CLASS_NORMAL 1

#ifdef HOBBYOS_SCHED_TEST_QUANTUM
#define DEFAULT_QUANTUM HOBBYOS_SCHED_TEST_QUANTUM
#define INTERACTIVE_QUANTUM HOBBYOS_SCHED_TEST_QUANTUM
#else
#define DEFAULT_QUANTUM 20
#define INTERACTIVE_QUANTUM 5
#endif

#define TASK_FLAG_IDLE           (1u << 0)
#define TASK_FLAG_SYSTEM         (1u << 1)
#define TASK_FLAG_USER           (1u << 2)
#define TASK_FLAG_KILL_PROTECTED (1u << 3)
#define TASK_FLAG_KILLABLE       (1u << 4)
#define TASK_FLAG_MODAL_UI       (1u << 5)
#define TASK_CREATION_FLAGS_MASK (TASK_FLAG_IDLE | TASK_FLAG_SYSTEM | TASK_FLAG_USER | \
                                  TASK_FLAG_KILL_PROTECTED | TASK_FLAG_KILLABLE | \
                                  TASK_FLAG_MODAL_UI)

typedef enum
{
    TASK_WAIT_NONE = 0, TASK_WAIT_SEMAPHORE, TASK_WAIT_TIMER_SLEEP,
    TASK_WAIT_GENERIC, TASK_WAIT_INPUT_QUEUE
} task_wait_kind_t;

typedef enum
{
    TASK_WAKE_NONE = 0, TASK_WAKE_SIGNAL, TASK_WAKE_TIMEOUT, TASK_WAKE_CANCELLED,
    TASK_WAKE_SPURIOUS, TASK_WAKE_ERROR
} task_wake_reason_t;

typedef enum
{
    TASK_WAIT_RESULT_OK = 0, TASK_WAIT_RESULT_TIMEOUT, TASK_WAIT_RESULT_CANCELLED,
    TASK_WAIT_RESULT_SPURIOUS, TASK_WAIT_RESULT_ERROR
} task_wait_result_t;

typedef enum
{
    TASK_QUEUE_NONE = 0,
    TASK_QUEUE_RUNQ_INTERACTIVE,
    TASK_QUEUE_RUNQ_NORMAL,
    TASK_QUEUE_WAIT
} task_queue_membership_t;

typedef struct
{
    uint64_t rsp;
    task_id_t id;
    char name[32];
    task_state_t state;
    uint64_t cr3;
    void *stack_base;


    uint32_t kernel_stack_bytes;
    uint32_t kernel_stack_guard_bytes;
    uint64_t kernel_stack_canary;
    uint64_t kernel_stack_low_watermark;
    uint8_t kernel_stack_guard_valid;
    uint64_t kernel_stack_guard_failures;
    uint32_t kernel_ctx_bytes_est;

    int quantum;
    int quantum_default;
    int task_class;

    uint64_t runtime_ns_total;
    uint64_t schedule_count;
    uint32_t last_cpu_slot;

    uint32_t flags;
    uint8_t registry_registered;
    task_queue_membership_t queue_membership;
    uint8_t on_cpu;
    uint8_t deferred_ready;
    uint32_t current_cpu_slot;
    uint64_t lifecycle_generation;
    uint64_t wait_generation;
    task_wait_kind_t wait_kind;
    task_wake_reason_t wake_reason;
    uint8_t wait_active;
    uintptr_t wait_object_key;
    uint64_t wake_count;
    uint64_t duplicate_wake_count;

    volatile uint32_t kill_pending;
    task_cleanup_fn cleanup_fn;
    void *cleanup_ctx;
    uint8_t cleanup_started, cleanup_done, exit_started;
    uint8_t lifecycle_notify_started, lifecycle_notify_completed;
    uint8_t lifecycle_notify_failed, reap_claimed;
    uint8_t test_ready_hold;
    task_exit_reason_t exit_reason;
    uint64_t kill_requested_ns, kill_request_count;
    uint64_t cancellation_points, cleanup_invocations;

    uint32_t task_wake_timer_refs;
    uint64_t timer_ref_acquires, timer_ref_releases;
    uint64_t exit_started_ns, cleanup_completed_ns, zombie_entered_ns;
    uint64_t reap_claimed_ns, zombie_generation;
    uint64_t reap_defer_mask_last, lifecycle_notify_count;

    uint64_t zombie_since_ms;

    struct list_head list;
    struct list_head registry_list;
} task_t;

typedef struct
{
    struct list_head head;
} wait_queue_t;

static inline void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
}

extern void switch_context(task_t *prev, task_t *next, void *cpu_state,
                           uint64_t sequence);

#endif

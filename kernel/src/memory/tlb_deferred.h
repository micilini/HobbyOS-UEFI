#ifndef TLB_DEFERRED_H
#define TLB_DEFERRED_H
#include "../smp/cpu_request.h"
#define TLB_DEFERRED_CAPACITY 8u
typedef struct { uint64_t generation; uint32_t slot; } tlb_completion_t;
typedef enum {
    TLB_DEFERRED_QUEUED, TLB_DEFERRED_PENDING, TLB_DEFERRED_COMPLETE,
    TLB_DEFERRED_FAILED, TLB_DEFERRED_BUSY, TLB_DEFERRED_INVALID,
    TLB_DEFERRED_EXHAUSTED
} tlb_deferred_status_t;
typedef struct {
    uint64_t state;
    tlb_interval_t interval;
    cpu_request_result_t result;
} tlb_deferred_slot_t;
typedef struct {
    uint64_t next_generation;
    tlb_deferred_slot_t slots[TLB_DEFERRED_CAPACITY];
} tlb_deferred_queue_t;

/* A token owns a queue slot until successful completion is consumed/released.
 * Queued is never a retirement guarantee. Failed slots remain owned for the
 * boot, retaining the handle and result instead of silently dropping work. */
tlb_deferred_status_t tlb_deferred_enqueue(tlb_deferred_queue_t *queue,
    const tlb_interval_t *interval, tlb_completion_t *out);
bool tlb_deferred_claim(tlb_deferred_queue_t *queue, unsigned slot,
    tlb_completion_t *out, tlb_interval_t *interval);
bool tlb_deferred_finish(tlb_deferred_queue_t *queue, tlb_completion_t handle,
    const cpu_request_result_t *result);
tlb_deferred_status_t tlb_deferred_poll(tlb_deferred_queue_t *queue,
    tlb_completion_t handle, cpu_request_result_t *out);
bool tlb_deferred_release(tlb_deferred_queue_t *queue, tlb_completion_t handle);
#endif

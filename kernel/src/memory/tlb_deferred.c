#include "tlb_deferred.h"

tlb_deferred_status_t tlb_deferred_enqueue(tlb_deferred_queue_t *q,
    const tlb_interval_t *interval, tlb_completion_t *out)
{
    if (!q || !out || !tlb_interval_valid(interval)) return TLB_DEFERRED_INVALID;
    uint64_t generation = __atomic_load_n(&q->next_generation, __ATOMIC_RELAXED);
    bool allocated = false;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        if (generation >= (UINT64_MAX >> 3)) return TLB_DEFERRED_EXHAUSTED;
        if (__atomic_compare_exchange_n(&q->next_generation, &generation,
            generation + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            ++generation; allocated = true; break;
        }
    }
    if (!allocated) return TLB_DEFERRED_BUSY;
    for (unsigned slot = 0; slot < TLB_DEFERRED_CAPACITY; ++slot) {
        uint64_t free = 0;
        tlb_deferred_slot_t *entry = &q->slots[slot];
        if (!__atomic_compare_exchange_n(&entry->state, &free,
            (generation << 3) | 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        entry->interval = *interval;
        entry->result = (cpu_request_result_t){.status = CPU_REQUEST_BUSY};
        *out = (tlb_completion_t){generation, slot};
        __atomic_store_n(&entry->state, (generation << 3) | 2, __ATOMIC_RELEASE);
        return TLB_DEFERRED_QUEUED;
    }
    return TLB_DEFERRED_BUSY;
}

bool tlb_deferred_claim(tlb_deferred_queue_t *q, unsigned slot,
    tlb_completion_t *out, tlb_interval_t *interval)
{
    if (!q || slot >= TLB_DEFERRED_CAPACITY || !out || !interval) return false;
    tlb_deferred_slot_t *entry = &q->slots[slot];
    uint64_t queued = __atomic_load_n(&entry->state, __ATOMIC_ACQUIRE);
    if ((queued & 7) != 2 || !__atomic_compare_exchange_n(&entry->state,
        &queued, (queued & ~UINT64_C(7)) | 3, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    *interval = entry->interval;
    *out = (tlb_completion_t){queued >> 3, slot};
    return true;
}

static bool valid(tlb_deferred_queue_t *q, tlb_completion_t h)
{ return q && h.slot < TLB_DEFERRED_CAPACITY && h.generation && h.generation <= (UINT64_MAX >> 3); }

bool tlb_deferred_finish(tlb_deferred_queue_t *q, tlb_completion_t h,
    const cpu_request_result_t *result)
{
    if (!valid(q, h) || !result) return false;
    tlb_deferred_slot_t *entry = &q->slots[h.slot];
    uint64_t running = (h.generation << 3) | 3;
    /* Claim completion publication too, so duplicate finishes cannot overwrite. */
    if (!__atomic_compare_exchange_n(&entry->state, &running,
        (h.generation << 3) | 7, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    if (result->status == CPU_REQUEST_OK && ((!result->requested && entry->interval.pages) ||
        result->requested != result->acknowledged || result->missing ||
        result->failed || result->send_failed)) {
        __atomic_store_n(&entry->state, running, __ATOMIC_RELEASE);
        return false;
    }
    entry->result = *result;
    __atomic_store_n(&entry->state, (h.generation << 3) |
        (result->status == CPU_REQUEST_OK ? 4 : 5), __ATOMIC_RELEASE);
    return true;
}

tlb_deferred_status_t tlb_deferred_poll(tlb_deferred_queue_t *q,
    tlb_completion_t h, cpu_request_result_t *out)
{
    if (!valid(q,h) || !out) return TLB_DEFERRED_INVALID;
    tlb_deferred_slot_t *entry = &q->slots[h.slot];
    uint64_t state = __atomic_load_n(&entry->state, __ATOMIC_ACQUIRE);
    if ((state >> 3) != h.generation) return TLB_DEFERRED_INVALID;
    unsigned phase = state & 7;
    if (phase != 4 && phase != 5) return TLB_DEFERRED_PENDING;
    uint64_t original = state;
    if (!__atomic_compare_exchange_n(&entry->state, &state,
        (h.generation << 3) | 6, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return TLB_DEFERRED_PENDING;
    *out = entry->result;
    __atomic_store_n(&entry->state, original, __ATOMIC_RELEASE);
    return phase == 4 ? TLB_DEFERRED_COMPLETE : TLB_DEFERRED_FAILED;
}

bool tlb_deferred_release(tlb_deferred_queue_t *q, tlb_completion_t h)
{
    if (!valid(q,h)) return false;
    uint64_t complete = (h.generation << 3) | 4;
    return __atomic_compare_exchange_n(&q->slots[h.slot].state, &complete, 0,
        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

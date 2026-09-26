#include "clock.h"
#include "spinlock.h"
#include "panic.h"
#include "../timer/hpet.h"
#include "../drivers/serial.h"
#include "../smp/smp_topology.h"
#include "../apic/lapic.h"
#include "../libc/memory.h"
#include <stddef.h>

#define CLOCK_STATE_CANARY 0x434C4F434B535441ULL
#define CLOCK_ANOMALY_LOG_MAX 32u
#define CLOCK_CLASSIFIER_CASES 10u

typedef struct {
    uint64_t last_raw_ticks;
    uint64_t last_raw_ns;
    uint64_t last_returned_ns;
    uint64_t local_regressions;
    uint64_t cross_cpu_lags;
    uint8_t initialized;
} clock_cpu_state_t;

typedef struct {
    uint64_t canary_begin;
    uint8_t ready;
    uint64_t last_ns;
    clock_stats_t stats;
    clock_cpu_state_t cpu[HOBBYOS_MAX_CPUS + 1];
    uint64_t event_sequence;
    clock_anomaly_event_t events[CLOCK_ANOMALY_LOG_MAX];
    uint64_t canary_end;
} clock_state_t;

_Static_assert(offsetof(clock_state_t, canary_begin) == 0,
               "clock canary must begin the state");
_Static_assert(offsetof(clock_state_t, canary_end) >
                   offsetof(clock_state_t, events),
               "clock end canary must follow events");

static spinlock_t g_clock_event_lock;
static clock_state_t g_clock_state;

static inline void clock_stats_add_u64(uint64_t *value, uint64_t amount)
{
    __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static inline void clock_stats_max_u64(uint64_t *value, uint64_t candidate)
{
    uint64_t observed = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (observed < candidate &&
           !__atomic_compare_exchange_n(value, &observed, candidate, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        ;
}

/* Publish a clock value without a lock owner.  A failed compare-exchange
   returns the value published by the competing CPU, so callers never wait for
   a descheduled vCPU and still return a value from one linearizable global
   high-water mark. */
static uint64_t clock_publish_max(uint64_t *value, uint64_t candidate)
{
    uint64_t observed = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    while (observed < candidate) {
        if (__atomic_compare_exchange_n(value, &observed, candidate, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return candidate;
    }
    return observed;
}

static bool clock_publication_selftest(void)
{
    uint64_t value = 100;
    return clock_publish_max(&value, 90) == 100 && value == 100 &&
           clock_publish_max(&value, 100) == 100 && value == 100 &&
           clock_publish_max(&value, 120) == 120 && value == 120 &&
           clock_publish_max(&value, UINT64_MAX) == UINT64_MAX &&
           value == UINT64_MAX &&
           clock_publish_max(&value, 0) == UINT64_MAX;
}

bool clock_classify_sample(uint64_t cpu_last_raw_ns,
                           uint8_t cpu_initialized,
                           uint64_t global_last_ns,
                           uint64_t sample_ns,
                           uint32_t retries,
                           uint8_t sample_valid,
                           clock_sample_decision_t *out)
{
    if (!out)
        return false;
    *out = (clock_sample_decision_t){
        .classification = CLOCK_SAMPLE_INVALID,
        .returned_ns = global_last_ns,
        .hard_failure = 1
    };
    if (!sample_valid)
        return true;

    out->returned_ns = sample_ns >= global_last_ns ? sample_ns : global_last_ns;
    out->update_global = 1;
    out->update_local_raw = 1;
    out->hard_failure = 0;

#ifdef HOBBYOS_CLOCK_NEGATIVE_GLOBAL_ONLY_REGRESSION
    if (sample_ns < global_last_ns) {
        out->classification = CLOCK_SAMPLE_LOCAL_REGRESSION;
        out->local_backward_ns = global_last_ns - sample_ns;
        out->hard_failure = 1;
        out->update_local_raw = 0;
        return true;
    }
#endif

    if (cpu_initialized && sample_ns < cpu_last_raw_ns) {
#ifdef HOBBYOS_CLOCK_NEGATIVE_LOCAL_BACKWARD
        out->classification = CLOCK_SAMPLE_CROSS_CPU_LAG;
        out->cross_cpu_lag_ns = global_last_ns > sample_ns
                                    ? global_last_ns - sample_ns : 0;
        out->hard_failure = 0;
        return true;
#else
        out->classification = retries >= CLOCK_SAMPLE_RETRY_LIMIT
                                  ? CLOCK_SAMPLE_RETRY_EXHAUSTED
                                  : CLOCK_SAMPLE_LOCAL_REGRESSION;
        out->local_backward_ns = cpu_last_raw_ns - sample_ns;
        out->hard_failure = 1;
        out->update_local_raw = 0;
        return true;
#endif
    }
    if (sample_ns < global_last_ns) {
        out->classification = CLOCK_SAMPLE_CROSS_CPU_LAG;
        out->cross_cpu_lag_ns = global_last_ns - sample_ns;
        return true;
    }
    out->classification = CLOCK_SAMPLE_OK;
    return true;
}

static bool decision_is(const clock_sample_decision_t *d,
                        clock_sample_class_t classification,
                        uint64_t returned_ns, uint64_t local_ns,
                        uint64_t cross_ns, uint8_t update_local,
                        uint8_t update_global, uint8_t hard)
{
    return d->classification == classification &&
           d->returned_ns == returned_ns &&
           d->local_backward_ns == local_ns &&
           d->cross_cpu_lag_ns == cross_ns &&
           d->update_local_raw == update_local &&
           d->update_global == update_global &&
           d->hard_failure == hard;
}

bool clock_sample_classifier_selftest(void)
{
    clock_sample_decision_t d;
#ifdef HOBBYOS_CLOCK_NEGATIVE_GLOBAL_ONLY_REGRESSION
    return clock_classify_sample(80, 1, 100, 90, 0, 1, &d) &&
           d.classification == CLOCK_SAMPLE_LOCAL_REGRESSION &&
           d.hard_failure;
#elif defined(HOBBYOS_CLOCK_NEGATIVE_LOCAL_BACKWARD)
    return clock_classify_sample(100, 1, 150, 90, 0, 1, &d) &&
           d.classification == CLOCK_SAMPLE_CROSS_CPU_LAG &&
           !d.hard_failure;
#else
    bool ok = clock_classify_sample(0, 0, 0, 100, 0, 1, &d) &&
              decision_is(&d, CLOCK_SAMPLE_OK, 100, 0, 0, 1, 1, 0);
    ok = ok && clock_classify_sample(100, 1, 100, 120, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_OK, 120, 0, 0, 1, 1, 0);
    ok = ok && clock_classify_sample(80, 1, 100, 90, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_CROSS_CPU_LAG, 100, 0, 10, 1, 1, 0);
    ok = ok && clock_classify_sample(100, 1, 100, 90, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_LOCAL_REGRESSION, 100, 10, 0, 0, 1, 1);
    ok = ok && clock_classify_sample(100, 1, 150, 90, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_LOCAL_REGRESSION, 150, 10, 0, 0, 1, 1);
    ok = ok && clock_classify_sample(100, 1, 100, 90,
                                      CLOCK_SAMPLE_RETRY_LIMIT, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_RETRY_EXHAUSTED, 100, 10, 0, 0, 1, 1);
    ok = ok && clock_classify_sample(0, 0, 100, 0, 0, 0, &d) &&
         decision_is(&d, CLOCK_SAMPLE_INVALID, 100, 0, 0, 0, 0, 1);
    ok = ok && clock_classify_sample(100, 1, 100, 100, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_OK, 100, 0, 0, 1, 1, 0);
    ok = ok && clock_classify_sample(90, 1, 100, 90, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_CROSS_CPU_LAG, 100, 0, 10, 1, 1, 0);
    ok = ok && clock_classify_sample(UINT64_MAX, 1, UINT64_MAX,
                                      UINT64_MAX, 0, 1, &d) &&
         decision_is(&d, CLOCK_SAMPLE_OK, UINT64_MAX, 0, 0, 1, 1, 0);
    return ok;
#endif
}

uint32_t clock_sample_classifier_case_count(void)
{
    return CLOCK_CLASSIFIER_CASES;
}

void clock_state_layout(uint64_t *begin_offset, uint64_t *end_offset,
                        uint64_t *protected_bytes)
{
    if (begin_offset)
        *begin_offset = offsetof(clock_state_t, canary_begin);
    if (end_offset)
        *end_offset = offsetof(clock_state_t, canary_end);
    if (protected_bytes)
        *protected_bytes = offsetof(clock_state_t, canary_end) -
                           sizeof(g_clock_state.canary_begin);
}

static void record_event(uint8_t kind, uint32_t slot, uint32_t apic,
                         const hpet_clock_sample_t *sample, uint64_t local,
                         uint64_t global,
                         const clock_sample_decision_t *decision)
{
    /* Diagnostics must never turn a clock read in hard IRQ context into a
       blocking lock acquisition.  The aggregate anomaly counters remain the
       authoritative record if another CPU is already writing the ring. */
    if (!spin_trylock(&g_clock_event_lock))
        return;
    uint64_t sequence = ++g_clock_state.event_sequence;
    uint64_t delta = decision->local_backward_ns
                         ? decision->local_backward_ns
                         : decision->cross_cpu_lag_ns;
    clock_anomaly_event_t *event =
        &g_clock_state.events[(sequence - 1) % CLOCK_ANOMALY_LOG_MAX];
    *event = (clock_anomaly_event_t){
        .sequence = sequence,
        .cpu_slot = slot,
        .apic_id = apic,
        .raw_ticks = sample->ticks,
        .raw_ns = sample->ns,
        .cpu_last_raw_ns = local,
        .global_last_ns = global,
        .returned_ns = decision->returned_ns,
        .delta_ns = delta,
        .retries = sample->retries,
        .raw_high = (uint32_t)(sample->ticks >> 32),
        .raw_low = (uint32_t)sample->ticks,
        .read_mode = (uint8_t)sample->mode,
        .kind = kind
    };
    spin_unlock(&g_clock_event_lock);
}

bool clock_monotonic_init(void)
{
    spinlock_init(&g_clock_event_lock);
    memset(&g_clock_state, 0, sizeof(g_clock_state));
    g_clock_state.canary_begin = CLOCK_STATE_CANARY;
    g_clock_state.canary_end = CLOCK_STATE_CANARY;
    uint8_t ready = hpet_is_available() && hpet_frequency_hz() != 0;
    __atomic_store_n(&g_clock_state.ready, ready, __ATOMIC_RELEASE);
    return ready != 0;
}

bool clock_monotonic_is_ready(void)
{
    return __atomic_load_n(&g_clock_state.ready, __ATOMIC_ACQUIRE) != 0;
}

uint64_t clock_monotonic_ns(void)
{
    /* Disabling IRQs pins the caller to its CPU-local raw-source state.  The
       shared return value is published with an ownerless atomic maximum. */
    irq_flags_t flags = irq_save();
    clock_state_t *state = &g_clock_state;
    clock_stats_add_u64(&state->stats.reads, 1);
    uint8_t ready = __atomic_load_n(&state->ready, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&state->canary_begin, __ATOMIC_RELAXED) !=
            CLOCK_STATE_CANARY ||
        __atomic_load_n(&state->canary_end, __ATOMIC_RELAXED) !=
            CLOCK_STATE_CANARY) {
        clock_stats_add_u64(&state->stats.state_corruptions, 1);
        irq_restore(flags);
        kpanic("CLOCK: state corruption");
    }
    if (!ready) {
        clock_stats_add_u64(&state->stats.not_ready, 1);
        irq_restore(flags);
        return 0;
    }

    cpu_slot_t slot;
    bool known = smp_current_cpu_slot(&slot) && slot < HOBBYOS_MAX_CPUS;
    uint32_t index = known ? slot : HOBBYOS_MAX_CPUS;
    if (!known)
        clock_stats_add_u64(&state->stats.unknown_cpu_reads, 1);
    clock_cpu_state_t *cpu = known ? &state->cpu[index] : NULL;
    uint64_t cpu_last_raw_ns = cpu ? cpu->last_raw_ns : 0;
    uint64_t cpu_last_returned_ns = cpu ? cpu->last_returned_ns : 0;
    uint8_t cpu_initialized = cpu ? cpu->initialized : 0;

    /* This snapshot precedes the MMIO read deliberately.  A CPU that read an
       older HPET sample before a concurrent publisher must not be diagnosed as
       a source regression merely because it completed later. */
    uint64_t global_before = __atomic_load_n(&state->last_ns,
                                             __ATOMIC_ACQUIRE);
    hpet_clock_sample_t sample = {0};
    bool sampled = hpet_read_clock_sample(&sample);
    clock_stats_max_u64(&state->stats.max_retries, sample.retries);

    clock_sample_decision_t decision;
    if (!clock_classify_sample(cpu_last_raw_ns, cpu_initialized,
                               global_before, sample.ns, sample.retries,
                               sampled && sample.valid, &decision)) {
        decision = (clock_sample_decision_t){
            .classification = CLOCK_SAMPLE_INVALID,
            .returned_ns = global_before,
            .hard_failure = 1
        };
    }

    if (decision.classification == CLOCK_SAMPLE_INVALID) {
        clock_stats_add_u64(&state->stats.saturations, 1);
    } else if (decision.classification == CLOCK_SAMPLE_CROSS_CPU_LAG) {
        if (cpu)
            cpu->cross_cpu_lags++;
        clock_stats_add_u64(&state->stats.cross_cpu_lag_clamps, 1);
        clock_stats_max_u64(&state->stats.max_cross_cpu_lag_ns,
                            decision.cross_cpu_lag_ns);
    } else if (decision.classification == CLOCK_SAMPLE_LOCAL_REGRESSION ||
               decision.classification == CLOCK_SAMPLE_RETRY_EXHAUSTED) {
        if (cpu)
            cpu->local_regressions++;
        clock_stats_add_u64(&state->stats.source_local_regressions, 1);
        if (decision.classification == CLOCK_SAMPLE_RETRY_EXHAUSTED)
            clock_stats_add_u64(&state->stats.source_retry_exhaustions, 1);
        clock_stats_max_u64(&state->stats.max_local_backward_ns,
                            decision.local_backward_ns);
    }

    uint64_t returned = decision.update_global
                            ? clock_publish_max(&state->last_ns,
                                                decision.returned_ns)
                            : global_before;
    uint64_t return_floor = global_before;
    if (cpu && cpu_last_returned_ns > return_floor)
        return_floor = cpu_last_returned_ns;
    if (returned < return_floor) {
        clock_stats_add_u64(&state->stats.api_regressions, 1);
        returned = clock_publish_max(&state->last_ns, return_floor);
    }
    decision.returned_ns = returned;

    if (decision.classification == CLOCK_SAMPLE_CROSS_CPU_LAG) {
        record_event(2, index, lapic_get_id(), &sample, cpu_last_raw_ns,
                     global_before, &decision);
    } else if (decision.classification == CLOCK_SAMPLE_LOCAL_REGRESSION ||
               decision.classification == CLOCK_SAMPLE_RETRY_EXHAUSTED) {
        record_event(1, index, lapic_get_id(), &sample, cpu_last_raw_ns,
                     global_before, &decision);
    }

    if (cpu && decision.update_local_raw) {
        cpu->last_raw_ticks = sample.ticks;
        cpu->last_raw_ns = sample.ns;
        cpu->initialized = 1;
    }
    if (cpu)
        cpu->last_returned_ns = returned;
    irq_restore(flags);
    return returned;
}

uint64_t clock_monotonic_us(void) { return clock_monotonic_ns() / 1000ULL; }
uint64_t clock_monotonic_ms(void) { return clock_monotonic_ns() / 1000000ULL; }

void clock_stats_snapshot(clock_stats_t *out)
{
    if (!out)
        return;
    clock_stats_t *stats = &g_clock_state.stats;
    *out = (clock_stats_t){
        .reads = __atomic_load_n(&stats->reads, __ATOMIC_RELAXED),
        .api_regressions = __atomic_load_n(&stats->api_regressions,
                                           __ATOMIC_RELAXED),
        .source_local_regressions = __atomic_load_n(
            &stats->source_local_regressions, __ATOMIC_RELAXED),
        .cross_cpu_lag_clamps = __atomic_load_n(
            &stats->cross_cpu_lag_clamps, __ATOMIC_RELAXED),
        .source_retry_exhaustions = __atomic_load_n(
            &stats->source_retry_exhaustions, __ATOMIC_RELAXED),
        .saturations = __atomic_load_n(&stats->saturations, __ATOMIC_RELAXED),
        .not_ready = __atomic_load_n(&stats->not_ready, __ATOMIC_RELAXED),
        .unknown_cpu_reads = __atomic_load_n(&stats->unknown_cpu_reads,
                                             __ATOMIC_RELAXED),
        .state_corruptions = __atomic_load_n(&stats->state_corruptions,
                                             __ATOMIC_RELAXED),
        .max_local_backward_ns = __atomic_load_n(
            &stats->max_local_backward_ns, __ATOMIC_RELAXED),
        .max_cross_cpu_lag_ns = __atomic_load_n(
            &stats->max_cross_cpu_lag_ns, __ATOMIC_RELAXED),
        .max_retries = __atomic_load_n(&stats->max_retries, __ATOMIC_RELAXED)
    };
}

void clock_test_stats_reset(void)
{
    clock_stats_t *stats = &g_clock_state.stats;
    __atomic_store_n(&stats->reads, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->api_regressions, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->source_local_regressions, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->cross_cpu_lag_clamps, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->source_retry_exhaustions, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->saturations, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->not_ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->unknown_cpu_reads, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->state_corruptions, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->max_local_backward_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->max_cross_cpu_lag_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats->max_retries, 0, __ATOMIC_RELAXED);

    irq_flags_t flags = spin_lock_irqsave(&g_clock_event_lock);
    memset(g_clock_state.events, 0, sizeof(g_clock_state.events));
    g_clock_state.event_sequence = 0;
    spin_unlock_irqrestore(&g_clock_event_lock, flags);
}

uint32_t clock_events_snapshot(clock_anomaly_event_t *out, uint32_t cap)
{
    if (!out || !cap)
        return 0;
    irq_flags_t flags = spin_lock_irqsave(&g_clock_event_lock);
    uint64_t sequence = g_clock_state.event_sequence;
    uint32_t count = sequence < CLOCK_ANOMALY_LOG_MAX
                         ? (uint32_t)sequence : CLOCK_ANOMALY_LOG_MAX;
    if (count > cap)
        count = cap;
    uint64_t first = sequence >= count ? sequence - count : 0;
    for (uint32_t i = 0; i < count; i++)
        out[i] = g_clock_state.events[(first + i) % CLOCK_ANOMALY_LOG_MAX];
    spin_unlock_irqrestore(&g_clock_event_lock, flags);
    return count;
}

bool clock_monotonic_selftest(void)
{
    uint64_t extended = 0;
    uint64_t hz = hpet_frequency_hz();
    bool guards = hpet_counter_to_ns(0) == 0 && hz &&
                  hpet_counter_to_ns(hz) == 1000000000ULL &&
                  hpet_counter_extend32_test(UINT32_MAX - 1, 0, 2,
                                             &extended) &&
                  extended == (1ULL << 32) + 2;
    bool classifier = clock_sample_classifier_selftest();
    bool publication = clock_publication_selftest();
    serial_write_all(guards ? "[CLOCK][SELFTEST] MONOTONIC_GUARDS_OK\n"
                            : "[CLOCK][SELFTEST] MONOTONIC_GUARDS_FAIL\n");
#if !defined(HOBBYOS_CLOCK_NEGATIVE_GLOBAL_ONLY_REGRESSION) && \
    !defined(HOBBYOS_CLOCK_NEGATIVE_LOCAL_BACKWARD)
    serial_write_all(classifier ? "[CLOCK][SELFTEST] SMP_CLASSIFICATION_OK\n"
                                : "[CLOCK][SELFTEST] SMP_CLASSIFICATION_FAIL\n");
#endif
    serial_write_all(publication
        ? "[CLOCK][SELFTEST] LOCK_FREE_PUBLICATION_OK\n"
        : "[CLOCK][SELFTEST] LOCK_FREE_PUBLICATION_FAIL\n");
    return guards && classifier && publication;
}

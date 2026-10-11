#include "irq_latency_record.h"
#include "irq_latency_layout.h"
#include <stddef.h>

_Static_assert(offsetof(irq_latency_pending_t, witness) + IL_END * 8 == IRQ_TAIL_END_OFFSET,
               "IRQ assembly tail end offset");
_Static_assert(offsetof(irq_latency_pending_t, witness) + IL_OBSERVED_END * 8 == IRQ_TAIL_OBSERVED_END_OFFSET,
               "IRQ assembly tail raw end offset");

static uint64_t get(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void put(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void increment(uint64_t *p)
{
    uint64_t value = get(p);
    if (value != UINT64_MAX) put(p, value + 1);
}

static bool enter(irq_latency_recorder_t *r)
{
    if (!r || !get(&r->epoch)) return false;
    if (__atomic_exchange_n(&r->busy, 1, __ATOMIC_ACQUIRE)) {
        /* An interrupted recorder is never waited on, including from NMI. */
        __atomic_store_n(&r->recursion, 1, __ATOMIC_RELEASE);
        return false;
    }
    uint64_t seq = get(&r->sequence);
    if (seq > UINT64_MAX - 2 || (seq & 1)) {
        put(&r->exhausted, 1); put(&r->busy, 0); return false;
    }
    put(&r->sequence, seq + 1);
    return true;
}

static void leave(irq_latency_recorder_t *r)
{
    put(&r->sequence, get(&r->sequence) + 1);
    put(&r->busy, 0);
}

static void copy(irq_latency_witness_t *to, const irq_latency_witness_t *from)
{
    for (unsigned i = 0; i < IL_FIELDS; ++i) put(&to->cells[i], get(&from->cells[i]));
}

bool irq_latency_record_init(irq_latency_recorder_t *r, uint32_t cpu,
                             uint64_t epoch, uint64_t cycle_limit)
{
    if (!r || !epoch || !cycle_limit || get(&r->epoch)) return false;
    /* Initialization is before publication of this permanently mapped CPU
     * object. There is deliberately no reset API during a boot. */
    put(&r->cpu, cpu); put(&r->cycle_limit, cycle_limit); put(&r->epoch, epoch);
    return true;
}

bool irq_latency_record_begin(irq_latency_recorder_t *r, uint64_t flags,
    uint64_t start, uint64_t site, uint64_t paths, irq_latency_context_t context)
{
    return irq_latency_record_begin_envelope(r, flags, start, site, paths, context,
        start, r ? (uint32_t)get(&r->cpu) : UINT32_MAX, IRQ_BOUNDARY_DIRECT);
}

bool irq_latency_record_begin_envelope(irq_latency_recorder_t *r, uint64_t flags,
    uint64_t start, uint64_t site, uint64_t paths, irq_latency_context_t context,
    uint64_t observed_start, uint32_t observed_cpu, irq_latency_boundary_kind_t kind)
{
    if (paths & ~IRQ_LATENCY_ALL_PATHS || context < IRQ_LATENCY_THREAD ||
        context > IRQ_LATENCY_UNTRACKED || kind < IRQ_BOUNDARY_DIRECT ||
        kind > IRQ_BOUNDARY_AFTER_IRQ || !enter(r)) return false;
    bool enabled = (flags & IRQ_LATENCY_IF) != 0;
    if (get(&r->active)) {
        put(&r->paths, get(&r->paths) | paths);
        if (enabled) { increment(&r->unclosed); increment(&r->invalid); }
        leave(r); return !enabled;
    }
    uint64_t sequence = get(&r->next_sequence);
    if (sequence >= (UINT64_MAX >> 2)) {
        put(&r->exhausted, 1); leave(r); return false;
    }
    put(&r->next_sequence, sequence + 1);
    put(&r->active, 1); put(&r->paths, paths); put(&r->begin, start);
    put(&r->begin_site, site); put(&r->begin_sequence, sequence + 1);
    put(&r->observed_begin, observed_start); put(&r->observed_begin_cpu, observed_cpu);
    put(&r->begin_kind, kind);
    put(&r->wait_cycles, 0); put(&r->held_cycles, 0);
    if (!enabled && context == IRQ_LATENCY_THREAD) {
        context = IRQ_LATENCY_UNTRACKED; increment(&r->missing_begin);
    }
    put(&r->context, context);
    leave(r); return true;
}

bool irq_latency_record_tag(irq_latency_recorder_t *r, uint64_t paths)
{
    if (paths & ~IRQ_LATENCY_ALL_PATHS || !enter(r)) return false;
    bool active = get(&r->active) != 0;
    if (active) {
        put(&r->paths, get(&r->paths) | paths);
        const uint64_t production_memory = IRQ_LATENCY_ALL_PATHS & ~((UINT64_C(1) << 8) | (UINT64_C(1) << 10));
        if ((paths & production_memory) && get(&r->context) == IRQ_LATENCY_TEST_PROBE)
            put(&r->context, IRQ_LATENCY_HARD_IRQ);
    }
    else { increment(&r->missing_begin); increment(&r->invalid); }
    leave(r); return active;
}

bool irq_latency_record_components(irq_latency_recorder_t *r, uint64_t wait, uint64_t held)
{
    if (!enter(r)) return false;
    bool active = get(&r->active) != 0;
    /* These are diagnostic maxima, never a sum purporting to be elapsed time. */
    if (active) {
        if (wait > get(&r->wait_cycles)) put(&r->wait_cycles, wait);
        if (held > get(&r->held_cycles)) put(&r->held_cycles, held);
    } else increment(&r->invalid);
    leave(r); return active;
}

irq_latency_ticket_t irq_latency_record_close(irq_latency_recorder_t *r,
                                              uint64_t flags, uint64_t site)
{
    return irq_latency_record_close_owned(r, flags, site, UINT32_MAX, 0);
}

bool irq_latency_record_ticket(irq_latency_recorder_t *r, uintptr_t lock, uint16_t distance)
{
    if (!lock || !enter(r)) return false;
    bool active = get(&r->active) != 0;
    if (active && (get(&r->context) == IRQ_LATENCY_THREAD || get(&r->context) == IRQ_LATENCY_HARD_IRQ)) {
        uint64_t paths = get(&r->paths);
        for (unsigned path = 0; path < IRQ_LATENCY_PATHS; ++path) {
            if (!(paths & (UINT64_C(1) << path))) continue;
            irq_latency_contention_t *c = &r->contention[path];
            if (get(&c->observations) == UINT64_MAX) put(&r->exhausted, 1);
            increment(&c->observations);
            if (distance) {
                if (get(&c->contended) == UINT64_MAX) put(&r->exhausted, 1);
                increment(&c->contended);
            }
            if (distance > get(&c->maximum_distance)) {
                put(&c->maximum_distance, distance); put(&c->lock, lock);
                put(&c->begin_sequence, get(&r->begin_sequence)); put(&c->paths, paths);
            }
        }
    }
    leave(r); return active;
}

irq_latency_ticket_t irq_latency_record_close_owned(irq_latency_recorder_t *r,
    uint64_t flags, uint64_t site, uint32_t owner_slot, uint64_t owner_generation)
{
    return irq_latency_record_close_calibrated(r, flags, site, owner_slot, owner_generation, 0);
}

irq_latency_ticket_t irq_latency_record_close_calibrated(irq_latency_recorder_t *r,
    uint64_t flags, uint64_t site, uint32_t owner_slot, uint64_t owner_generation,
    uint64_t adjustment)
{
    irq_latency_ticket_t ticket = {0};
    if (!enter(r)) return ticket;
    if (!(flags & IRQ_LATENCY_IF)) { leave(r); return ticket; }
    if (!get(&r->active)) {
        increment(&r->missing_begin); increment(&r->invalid); leave(r); return ticket;
    }
    for (unsigned i = 0; i < IRQ_LATENCY_PENDING; ++i) {
        irq_latency_pending_t *p = &r->pending[i];
        if (get(&p->ready)) continue;
        irq_latency_witness_t w = {.cells = {
            [IL_BEGIN]=get(&r->begin), [IL_SEQUENCE]=get(&r->begin_sequence),
            [IL_END_SEQUENCE]=get(&r->begin_sequence), [IL_PATHS]=get(&r->paths),
            [IL_BEGIN_SITE]=get(&r->begin_site), [IL_END_SITE]=site,
            [IL_CONTEXT]=get(&r->context), [IL_CPU]=get(&r->cpu),
            [IL_EPOCH]=get(&r->epoch), [IL_WAIT]=get(&r->wait_cycles),
            [IL_HELD]=get(&r->held_cycles), [IL_OBSERVED_BEGIN]=get(&r->observed_begin),
            [IL_OBSERVED_BEGIN_CPU]=get(&r->observed_begin_cpu), [IL_BEGIN_KIND]=get(&r->begin_kind),
            [IL_OWNER_SLOT]=owner_slot, [IL_OWNER_GENERATION]=owner_generation,
            [IL_BOUNDARY_ADJUSTMENT]=adjustment, [IL_CLOSE_ADJUSTMENT]=adjustment}};
        copy(&p->witness, &w); put(&p->ready, (get(&r->begin_sequence) << 2) | 1);
        increment(&r->pending_count);
        ticket = (irq_latency_ticket_t){get(&r->begin_sequence), i, true};
        break;
    }
    if (!ticket.valid) { increment(&r->exhausted); increment(&r->unclosed); }
    put(&r->active, 0); put(&r->paths, 0);
    leave(r); return ticket;
}

uint32_t irq_latency_record_retire_owner(irq_latency_recorder_t *r,
    uint32_t owner_slot, uint64_t owner_generation)
{
    if (!r || !get(&r->epoch) || !owner_generation || owner_slot == UINT32_MAX) return 0;
    uint32_t completed = 0;
    for (unsigned i = 0; i < IRQ_LATENCY_PENDING; ++i) {
        irq_latency_pending_t *p = &r->pending[i];
        uint64_t ready = get(&p->ready);
        if ((ready & 3) != 1 || get(&p->witness.cells[IL_OWNER_SLOT]) != owner_slot ||
            get(&p->witness.cells[IL_OWNER_GENERATION]) != owner_generation) continue;
        if (!__atomic_compare_exchange_n(&p->ready, &ready, (ready & ~UINT64_C(3)) | 3,
                                         false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        /* Called only after the owning stack is off CPU and cannot resume.
         * The first hardware entry with saved IF=1 bounds the completed mask.
         * No timestamp is fabricated if that independent endpoint is absent. */
        bool observed = get(&p->witness.cells[IL_FALLBACK_PRESENT]) != 0;
        put(&p->witness.cells[IL_RETIRE_OWNER_SLOT], owner_slot);
        put(&p->witness.cells[IL_RETIRE_OWNER_GENERATION], owner_generation);
        put(&p->witness.cells[IL_RETIRE_SEQUENCE], ready >> 2);
        put(&p->witness.cells[IL_END], observed ? get(&p->witness.cells[IL_FALLBACK_END]) : 0);
        put(&p->witness.cells[IL_END_CPU], get(&r->cpu));
        put(&p->witness.cells[IL_OBSERVED_END_CPU], UINT32_MAX);
        put(&p->witness.cells[IL_END_KIND], IRQ_BOUNDARY_RETIRED_PUBLISHER);
        put(&p->witness.cells[IL_VALIDITY], observed ? IRQ_LATENCY_VALID : IRQ_LATENCY_BOUNDARY_UNCERTAIN);
        put(&p->ready, (ready & ~UINT64_C(3)) | 2);
        ++completed;
    }
    return completed;
}

bool irq_latency_record_complete(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity)
{
    return irq_latency_record_complete_adjusted(r, ticket, end, end_cpu, validity, 0);
}

bool irq_latency_record_complete_adjusted(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity, uint64_t adjustment)
{
    return irq_latency_record_complete_envelope(r, ticket, end, end_cpu, validity, adjustment,
        adjustment ? IRQ_BOUNDARY_IRQ_ADJACENT : IRQ_BOUNDARY_DIRECT);
}

bool irq_latency_record_complete_envelope(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity, uint64_t adjustment,
    irq_latency_boundary_kind_t kind)
{
    if (!r || !ticket.valid || !ticket.sequence || ticket.sequence > (UINT64_MAX >> 2) ||
        ticket.slot >= IRQ_LATENCY_PENDING ||
        validity < IRQ_LATENCY_VALID || validity > IRQ_LATENCY_OVERFLOW ||
        kind < IRQ_BOUNDARY_DIRECT || kind > IRQ_BOUNDARY_BEFORE_IRQ) return false;
    irq_latency_pending_t *p = &r->pending[ticket.slot];
    uint64_t ready = (ticket.sequence << 2) | 1;
    if (!__atomic_compare_exchange_n(&p->ready, &ready, (ticket.sequence << 2) | 3, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    put(&p->witness.cells[IL_OBSERVED_END], end);
    put(&p->witness.cells[IL_OBSERVED_END_CPU], end_cpu);
    if (get(&p->witness.cells[IL_FALLBACK_PRESENT]) &&
        (validity == IRQ_LATENCY_CPU_CHANGED || validity == IRQ_LATENCY_BOUNDARY_UNCERTAIN ||
         (validity == IRQ_LATENCY_VALID && end > get(&p->witness.cells[IL_BEGIN])))) {
        /* A hardware entry with saved IF=1 proves this mask was already over.
         * Its same-CPU timestamp remains usable if the suspended publisher
         * later resumes on another CPU. Preserve both raw observations. */
        end = get(&p->witness.cells[IL_FALLBACK_END]);
        end_cpu = (uint32_t)get(&p->witness.cells[IL_CPU]);
        kind = IRQ_BOUNDARY_BEFORE_IRQ;
        validity = IRQ_LATENCY_VALID;
    }
    put(&p->witness.cells[IL_END], end); put(&p->witness.cells[IL_END_CPU], end_cpu);
    put(&p->witness.cells[IL_END_KIND], kind);
    if (end_cpu != get(&p->witness.cells[IL_CPU])) validity = IRQ_LATENCY_CPU_CHANGED;
    else if (end <= get(&p->witness.cells[IL_BEGIN])) validity = IRQ_LATENCY_CLOCK_ORDER;
    else if (adjustment > UINT64_MAX - (end - get(&p->witness.cells[IL_BEGIN])))
        validity = IRQ_LATENCY_OVERFLOW;
    put(&p->witness.cells[IL_BOUNDARY_ADJUSTMENT], adjustment);
    put(&p->witness.cells[IL_VALIDITY], validity);
    put(&p->ready, (ticket.sequence << 2) | 2);
    return true;
}

bool irq_latency_record_interrupt(irq_latency_recorder_t *r, uint64_t entry_tsc, uint32_t cpu)
{
    if (!r || !get(&r->epoch) || get(&r->cpu) != cpu) return false;
    for (unsigned i = 0; i < IRQ_LATENCY_PENDING; ++i) {
        irq_latency_pending_t *p = &r->pending[i];
        uint64_t ready = get(&p->ready);
        if ((ready & 3) != 1 || get(&p->witness.cells[IL_FALLBACK_PRESENT])) continue;
        if (!__atomic_compare_exchange_n(&p->ready, &ready, (ready & ~UINT64_C(3)) | 3,
                                         false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        put(&p->witness.cells[IL_FALLBACK_END], entry_tsc);
        put(&p->witness.cells[IL_FALLBACK_PRESENT], 1);
        put(&p->ready, (ready & ~UINT64_C(3)) | 1);
    }
    return true;
}

irq_latency_pending_t *irq_latency_record_prepare_tail(irq_latency_recorder_t *r,
    irq_latency_ticket_t ticket, uint32_t cpu, uint64_t adjustment)
{
    if (!r || !ticket.valid || !ticket.sequence || ticket.sequence > (UINT64_MAX >> 2) ||
        ticket.slot >= IRQ_LATENCY_PENDING || cpu != get(&r->cpu)) return NULL;
    irq_latency_pending_t *p = &r->pending[ticket.slot];
    uint64_t ready = (ticket.sequence << 2) | 1;
    if (!__atomic_compare_exchange_n(&p->ready, &ready, (ticket.sequence << 2) | 3,
                                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return NULL;
    put(&p->witness.cells[IL_END_CPU], cpu);
    put(&p->witness.cells[IL_OBSERVED_END_CPU], cpu);
    put(&p->witness.cells[IL_END_KIND], IRQ_BOUNDARY_IRQ_ADJACENT);
    put(&p->witness.cells[IL_VALIDITY], IRQ_LATENCY_VALID);
    put(&p->witness.cells[IL_BOUNDARY_ADJUSTMENT], adjustment);
    put(&p->witness.cells[IL_CLOSE_ADJUSTMENT], adjustment);
    /* The IF=0 assembly tail supplies both endpoint cells, then publishes
     * READY=sequence:2. Only permanently mapped recorder storage is returned. */
    return p;
}

bool irq_latency_record_drain(irq_latency_recorder_t *r)
{
    if (!enter(r)) return false;
    for (unsigned i = 0; i < IRQ_LATENCY_PENDING; ++i) {
        irq_latency_pending_t *p = &r->pending[i];
        if ((get(&p->ready) & 3) != 2) continue;
        const irq_latency_witness_t *w = &p->witness;
        uint64_t begin = get(&w->cells[IL_BEGIN]), end = get(&w->cells[IL_END]);
        if (get(&w->cells[IL_END_CPU]) != get(&w->cells[IL_CPU]))
            put(&p->witness.cells[IL_VALIDITY], IRQ_LATENCY_CPU_CHANGED);
        else if (end <= begin) put(&p->witness.cells[IL_VALIDITY], IRQ_LATENCY_CLOCK_ORDER);
        else if (get(&w->cells[IL_BOUNDARY_ADJUSTMENT]) > UINT64_MAX - (end - begin))
            put(&p->witness.cells[IL_VALIDITY], IRQ_LATENCY_OVERFLOW);
        uint64_t context = get(&w->cells[IL_CONTEXT]);
        bool valid = get(&w->cells[IL_VALIDITY]) == IRQ_LATENCY_VALID &&
            context != IRQ_LATENCY_UNTRACKED;
        uint64_t cycles = valid ? get(&w->cells[IL_END]) - get(&w->cells[IL_BEGIN]) +
            get(&w->cells[IL_BOUNDARY_ADJUSTMENT]) : 0;
        bool runtime = context == IRQ_LATENCY_THREAD || context == IRQ_LATENCY_HARD_IRQ ||
            context == IRQ_LATENCY_UNTRACKED;
        uint64_t paths = get(&w->cells[IL_PATHS]);
        if (!valid) increment(&r->invalid);
        if (context == IRQ_LATENCY_EARLY) increment(&r->early);
        if (context == IRQ_LATENCY_TEST_PROBE || context == IRQ_LATENCY_PANIC) increment(&r->probes);
        if (!runtime) {
            unsigned role = context == IRQ_LATENCY_EARLY ? 0 : context == IRQ_LATENCY_TEST_PROBE ? 1 : 2;
            irq_latency_path_record_t *row = &r->excluded[role];
            if (get(&row->count) == UINT64_MAX) put(&r->exhausted, 1);
            increment(&row->count);
            if (!valid) increment(&row->invalid);
            else if (cycles > get(&row->maximum_cycles)) {
                copy(&row->maximum, w); put(&row->maximum_cycles, cycles);
            }
        }
        if (runtime) for (unsigned path = 0; path < IRQ_LATENCY_PATHS; ++path) {
            if (!(paths & (UINT64_C(1) << path))) continue;
            irq_latency_path_record_t *row = &r->path[path];
            if (get(&row->count) == UINT64_MAX) put(&r->exhausted, 1);
            increment(&row->count);
            if (!valid) increment(&row->invalid);
            if (valid && cycles > get(&r->cycle_limit)) {
                if (get(&row->violations) == UINT64_MAX) put(&r->exhausted, 1);
                increment(&row->violations);
            }
            if (valid && cycles > get(&row->maximum_cycles)) {
                copy(&row->maximum, w); put(&row->maximum_cycles, cycles);
            }
        }
        if (!valid || (runtime && paths && cycles > get(&r->cycle_limit))) {
#if HOBBYOS_IRQ_LATENCY_DETAILS
            uint64_t n = get(&r->failure_count);
            if (n < IRQ_LATENCY_FAILURES) {
                copy(&r->failures[n], w); put(&r->failure_count, n + 1);
            } else increment(&r->failure_overflow);
#else
            increment(&r->failure_overflow);
#endif
        }
        put(&p->ready, 0); put(&r->pending_count, get(&r->pending_count) - 1);
    }
    leave(r); return true;
}

bool irq_latency_record_snapshot(const irq_latency_recorder_t *r, irq_latency_snapshot_t *out)
{
    if (!r || !out || !get(&r->epoch)) return false;
    uint64_t sequence = get(&r->sequence);
    if ((sequence & 1) || get(&r->busy)) return false;
    out->sequence = sequence;
#define READ(field) out->field = get(&r->field)
    READ(epoch); READ(cpu); READ(active); READ(invalid); READ(exhausted); READ(recursion);
    READ(missing_begin); READ(unclosed); READ(early); READ(probes); READ(failure_count);
    READ(failure_overflow);
#undef READ
    out->pending = get(&r->pending_count);
    for (unsigned i = 0; i < IRQ_LATENCY_PATHS; ++i) {
        out->path[i].count = get(&r->path[i].count);
        out->path[i].invalid = get(&r->path[i].invalid);
        out->path[i].violations = get(&r->path[i].violations);
        out->path[i].maximum_cycles = get(&r->path[i].maximum_cycles);
        copy(&out->path[i].maximum, &r->path[i].maximum);
        const irq_latency_contention_t *c = &r->contention[i];
        out->contention[i] = (irq_latency_contention_t){get(&c->observations), get(&c->contended),
            get(&c->maximum_distance), get(&c->lock), get(&c->begin_sequence), get(&c->paths)};
    }
    for (unsigned i = 0; i < 3; ++i) {
        out->excluded[i].count = get(&r->excluded[i].count);
        out->excluded[i].invalid = get(&r->excluded[i].invalid);
        out->excluded[i].violations = get(&r->excluded[i].violations);
        out->excluded[i].maximum_cycles = get(&r->excluded[i].maximum_cycles);
        copy(&out->excluded[i].maximum, &r->excluded[i].maximum);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return get(&r->sequence) == sequence && !get(&r->busy);
}

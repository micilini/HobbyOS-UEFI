#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main

static irq_latency_recorder_t before_nested, reference_nested;
_Static_assert(sizeof(irq_nested_counter_t) == 64 && _Alignof(irq_nested_counter_t) == 64,
    "per-CPU counters must not share a cache line");

static void nested_precheck(void)
{
    reset();
    irq_flags_t outer = irq_save();
    CHECK(irq_latency_record_tag(&g_irq_latency[0], 3));
    before_nested = g_irq_latency[0];
    irq_latency_boundary_t inner = {.flags=host_flags,.observed_flags=host_flags,
        .site=0xffffffff80001000};
    CHECK(!irq_latency_before_mask(&inner));
    CHECK(!inner.ready && !irq_is_enabled());
    CHECK(memcmp(&before_nested, &g_irq_latency[0], sizeof(before_nested)) == 0);
    CHECK(g_irq_nested_elided[0].count == 1);
    irq_restore(outer);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[0].count == 1 && g_irq_latency[0].path[1].count == 1);
    CHECK(!g_irq_invalid_boundaries && !g_irq_latency[0].pending_count);
}

static void precheck_refusals(void)
{
    for (unsigned mutation = 0; mutation < 20; ++mutation) {
        reset();
        (void)irq_save();
        irq_latency_boundary_t b = {.flags=0,.observed_flags=0,.site=0xffffffff80001000};
        irq_latency_recorder_t *r = &g_irq_latency[0];
        if (mutation == 0) r->pending_count = 1;
        if (mutation == 1) g_irq_scopes[host_stack].paths = 1;
        if (mutation == 2) g_irq_mask_identity[host_stack].valid = false;
        if (mutation == 3) ++owner_generation;
        if (mutation == 4) ++g_irq_switch_sequence[0];
        if (mutation == 5) ++g_irq_entry_sequence[0];
        if (mutation == 6) ++g_irq_interior_fault_sequence;
        if (mutation == 7) ++r->epoch;
        if (mutation == 8) ++g_irq_mask_segment[0];
        if (mutation == 9) r->busy = 1;
        if (mutation == 10) r->sequence |= 1;
        if (mutation == 11) r->recursion = 1;
        if (mutation == 12) r->exhausted = 1;
        if (mutation == 13) g_irq_nested_elided[0].count = UINT64_MAX;
        if (mutation == 14) g_irq_nested_elided[0].prechecked = UINT64_MAX;
        if (mutation == 15) g_irq_started[0] = 0;
        if (mutation == 16) b.observed_flags = IRQ_LATENCY_IF;
        if (mutation == 17) host_flags = IRQ_LATENCY_IF;
        if (mutation == 18) r->active = 0;
        if (mutation == 19) r->sequence = UINT64_MAX - 3;
        CHECK(irq_latency_before_mask(&b) && b.ready);
        CHECK(g_irq_nested_elided[0].prechecked == (mutation == 14 ? UINT64_MAX : 0));
    }
    reset();
    uint64_t value = UINT64_MAX;
    CHECK(!irq_latency_nested_prechecks(0, &value) && value == UINT64_MAX);
    g_irq_frozen[0] = 1;
    CHECK(irq_latency_nested_prechecks(0, &value) && value == 0);
    CHECK(!irq_latency_nested_prechecks(g_cpu_count, &value));
    CHECK(!irq_latency_nested_prechecks(HOBBYOS_MAX_CPUS, &value));
    CHECK(!irq_latency_nested_prechecks(0, NULL));
    g_irq_frozen[0] = 0;
}

static void scoped_identity_reuse(void)
{
    reset();
    irq_flags_t flags = irq_save();
    uint64_t queries = identity_queries;
    irq_latency_scope_token_t scope = irq_latency_scope_enter(1);
    CHECK(scope.active && identity_queries == queries);
    irq_latency_callback_token_t callback = irq_latency_callback_begin(1, 9, 4096);
    CHECK(callback.active && identity_queries == queries);
    irq_latency_callback_end(callback, true);
    CHECK(identity_queries == queries && g_irq_callbacks[0][0].count == 1);
    irq_latency_scope_exit(&scope);
    CHECK(identity_queries == queries && !g_irq_invalid_boundaries);
    irq_restore(flags);

    reset();
    /* The request-result hook's own save performs the two required outer
     * identities; its body must not repeat the same masked lookup. */
    queries = identity_queries;
    irq_latency_request_result(1, 9, 4096, 10, 20, true);
    CHECK(identity_queries == queries + 3); /* two at save, one after restore */
    CHECK(g_irq_requests[0][0].count == 1 && !g_irq_invalid_boundaries);

    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        reset();
        (void)irq_save();
        if (mutation == 0) ++g_irq_interior_fault_sequence;
        if (mutation == 1) ++owner_generation;
        if (mutation == 2) ++g_irq_entry_sequence[0];
        if (mutation == 3) ++g_irq_switch_sequence[0];
        if (mutation == 4) ++g_irq_latency[0].epoch;
        queries = identity_queries;
        callback = irq_latency_callback_begin(1, 9, 4096);
        CHECK(callback.active && identity_queries == queries + 1);
    }
    memset(g_irq_callbacks, 0, sizeof(g_irq_callbacks));
    memset(g_irq_requests, 0, sizeof(g_irq_requests));
}

static void unchanged_nested_record(void)
{
    reset();
    irq_flags_t outer = irq_save();
    CHECK(irq_latency_record_tag(&g_irq_latency[0], 3));
    before_nested = g_irq_latency[0];
    irq_flags_t inner = irq_save();
    CHECK(!(inner & IRQ_LATENCY_IF) && !irq_is_enabled());
    CHECK(memcmp(&before_nested, &g_irq_latency[0], sizeof(before_nested)) == 0);
    irq_restore(inner);
    CHECK(memcmp(&before_nested, &g_irq_latency[0], sizeof(before_nested)) == 0);
    irq_restore(outer);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[0].count == 1 && g_irq_latency[0].path[1].count == 1);
    CHECK(!g_irq_invalid_boundaries && !g_irq_latency[0].pending_count);
}

static void new_paths_still_recorded(void)
{
    reset();
    (void)irq_save();
    CHECK(irq_latency_record_tag(&g_irq_latency[0], 1));
    g_irq_scopes[host_stack].paths = 2;
    uint64_t sequence = g_irq_latency[0].sequence;
    (void)irq_save();
    CHECK(g_irq_latency[0].paths == 3 && g_irq_latency[0].sequence == sequence + 4);
    CHECK(!g_irq_invalid_boundaries);
}

static void pending_publication_still_consumed(void)
{
    reset();
    irq_latency_recorder_t *r = &g_irq_latency[0];
    CHECK(irq_latency_record_begin(r, IRQ_LATENCY_IF, 10, 1, 1, IRQ_LATENCY_HARD_IRQ));
    irq_latency_ticket_t t = irq_latency_record_close_calibrated(r, IRQ_LATENCY_IF, 2, host_stack, 1, 40);
    CHECK(t.valid && irq_latency_record_complete_adjusted(r, t, 100, 0, IRQ_LATENCY_VALID, 40));
    CHECK(irq_latency_record_begin(r, IRQ_LATENCY_IF, 200, 3, 2, IRQ_LATENCY_THREAD));
    host_flags = 0;
    uint64_t sequence = r->sequence;
    (void)irq_save();
    CHECK(r->sequence == sequence + 4 && !r->pending_count && r->active);
    CHECK(r->path[0].count == 1 && r->path[0].maximum_cycles == 130);
    CHECK(r->path[0].maximum.cells[IL_CLOSE_ADJUSTMENT] == 40);
    CHECK(r->paths == 2 && r->begin == 200 && !g_irq_invalid_boundaries);
}

static void incomplete_publisher_still_retained(void)
{
    reset();
    irq_latency_recorder_t *r = &g_irq_latency[0];
    CHECK(irq_latency_record_begin(r, IRQ_LATENCY_IF, 10, 1, 1, IRQ_LATENCY_HARD_IRQ));
    irq_latency_ticket_t t = irq_latency_record_close_calibrated(r, IRQ_LATENCY_IF, 2, host_stack, 1, 40);
    CHECK(t.valid && irq_latency_record_interrupt(r, 100, 0));
    CHECK(irq_latency_record_begin(r, IRQ_LATENCY_IF, 200, 3, 2, IRQ_LATENCY_THREAD));
    host_flags = 0;
    uint64_t sequence = r->sequence;
    (void)irq_save();
    CHECK(r->sequence == sequence + 4 && r->pending_count == 1 && !r->path[0].count);
    CHECK(irq_latency_record_retire_owner(r, host_stack, 2) == 0);
    CHECK(irq_latency_record_retire_owner(r, host_stack, 1) == 1);
    CHECK(irq_latency_record_drain(r));
    CHECK(!r->pending_count && r->path[0].maximum_cycles == 130);
    CHECK(r->path[0].maximum.cells[IL_RETIRE_SEQUENCE] == t.sequence);
    CHECK(r->path[0].maximum.cells[IL_CLOSE_ADJUSTMENT] == 40);
}

static void invalid_state_is_not_elided(void)
{
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        reset();
        (void)irq_save();
        irq_latency_recorder_t *r = &g_irq_latency[0];
        if (mutation == 0) r->busy = 1;
        if (mutation == 1) r->sequence = UINT64_MAX - 1;
        if (mutation == 2) r->sequence |= 1;
        if (mutation == 3) g_irq_scopes[host_stack].paths = UINT64_C(1) << IRQ_LATENCY_PATHS;
        (void)irq_save();
        CHECK(g_irq_invalid_sites[3] == 1);
        if (mutation == 0) CHECK(r->recursion);
        if (mutation == 1 || mutation == 2) CHECK(r->exhausted);
    }
    reset();
    host_flags = 0;
    (void)irq_save();
    CHECK(g_irq_latency[0].active && g_irq_latency[0].context == IRQ_LATENCY_EARLY);
}

static void original_helper_semantics(void)
{
    for (unsigned context = IRQ_LATENCY_THREAD; context <= IRQ_LATENCY_UNTRACKED; ++context) {
        reset();
        (void)irq_save();
        g_irq_latency[0].context = context;
        CHECK(irq_latency_record_tag(&g_irq_latency[0], 3));
        g_irq_scopes[host_stack].paths = 1;
        reference_nested = g_irq_latency[0];
        CHECK(irq_latency_record_drain(&reference_nested));
        CHECK(irq_latency_record_begin_envelope(&reference_nested, 0, 200, 5, 1,
            IRQ_LATENCY_THREAD, 200, 0, IRQ_BOUNDARY_DIRECT));
        (void)irq_save();
        CHECK(reference_nested.sequence == g_irq_latency[0].sequence + 4);
        reference_nested.sequence = g_irq_latency[0].sequence;
        CHECK(memcmp(&reference_nested, &g_irq_latency[0], sizeof(reference_nested)) == 0);
    }
}

static void eligibility_and_counter(void)
{
    reset();
    (void)irq_save();
    irq_latency_recorder_t *r = &g_irq_latency[0];
    CHECK(nested_record_unchanged(r, 0, 0, true));
    CHECK(!nested_record_unchanged(r, 0, 0, false));
    CHECK(!nested_record_unchanged(r, IRQ_LATENCY_IF, 0, true));
    host_flags = IRQ_LATENCY_IF;
    CHECK(!nested_record_unchanged(r, 0, 0, true));
    host_flags = 0;
    uint64_t sequence = r->sequence;
    r->sequence = UINT64_MAX - 3;
    CHECK(!nested_record_unchanged(r, 0, 0, true));
    r->sequence = UINT64_MAX - 5;
    CHECK(nested_record_unchanged(r, 0, 0, true));
    r->sequence = sequence;
    for (unsigned field = 0; field < 3; ++field) {
        uint64_t *p = field == 0 ? &r->epoch : field == 1 ? &r->active : &r->recursion;
        uint64_t before = *p;
        *p = field == 2 ? 1 : 0;
        CHECK(!nested_record_unchanged(r, 0, 0, true));
        *p = before;
    }
    uint64_t value = UINT64_MAX;
    CHECK(!irq_latency_nested_elisions(0, &value) && value == UINT64_MAX);
    (void)irq_save();
    CHECK(g_irq_nested_elided[0].count == 1);
    g_irq_frozen[0] = 1;
    CHECK(irq_latency_nested_elisions(0, &value) && value == 1);
    CHECK(!irq_latency_nested_elisions(0, NULL));
    CHECK(!irq_latency_nested_elisions(g_cpu_count, &value));
    g_irq_frozen[0] = 0;
    g_irq_nested_elided[0].count = UINT64_MAX;
    (void)irq_save();
    CHECK(g_irq_nested_elided[0].count == UINT64_MAX && g_irq_invalid_sites[31] == 1);
    CHECK(r->sequence == sequence + 4);
}

int main(void)
{
    nested_precheck();
    precheck_refusals();
    scoped_identity_reuse();
    unchanged_nested_record();
    new_paths_still_recorded();
    pending_publication_still_consumed();
    incomplete_publisher_still_retained();
    invalid_state_is_not_elided();
    original_helper_semantics();
    eligibility_and_counter();
    puts("IRQ latency actual-C nested no-op equivalence and required publication: PASS hardware=HOSTED");
    return inherited_controller_main();
}

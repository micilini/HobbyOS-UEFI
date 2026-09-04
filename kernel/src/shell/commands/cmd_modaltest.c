#include "cmd_modaltest.h"
#include "cmd_inputtest.h"

#include "../../core/modal_ui.h"
#include "../../core/modal_session.h"
#include "../../core/input_router.h"
#include "../../core/scheduler.h"
#include "../../core/clock.h"
#include "../../core/timers.h"
#include "../../drivers/timer.h"
#include "../../drivers/serial.h"
#include "../../libc/string.h"
#include "../../libc/memory.h"
#include "../../memory/heap.h"

#define MODALTEST_OPEN_CLOSE_MAX 1000u
#define MODALTEST_OPEN_CLOSE_TIMEOUT_MS 30000u
#define MODALTEST_OPEN_CLOSE_RECORD_MAX 1792u

_Static_assert(
    MODALTEST_OPEN_CLOSE_MAX >= 1000u,
    "modal soak requires 1000 handles");

typedef enum
{
    MODALTEST_HEAP_DIRECTION_ZERO = 0,
    MODALTEST_HEAP_DIRECTION_UP,
    MODALTEST_HEAP_DIRECTION_DOWN
} modaltest_heap_direction_t;

typedef struct
{
    uint32_t requested_cycles;
    uint32_t passed_cycles;

    uint64_t cleanup_total;

    task_handle_t warmup_worker;
    uint8_t warmup_gone;

    uint32_t handles_recorded;
    uint32_t handles_gone;
    task_handle_t first_live_handle;

    HeapStats baseline;
    HeapStats final;

    modal_ui_stats_t stats_before;
    modal_ui_stats_t stats_after;
    timer_stats_t timers_before;
    timer_stats_t timers_after;

    uint64_t runs_delta;
    uint64_t workers_delta;
    uint64_t normal_delta;
    uint64_t killed_delta;
    uint64_t cleanup_closes_delta;
    uint64_t completion_signals_delta;
    uint64_t completion_duplicates_delta;
    uint64_t context_alloc_fail_delta;
    uint64_t worker_create_fail_delta;
    uint64_t begin_fail_delta;
    uint64_t lifecycle_recovery_delta;
    uint64_t second_session_reject_delta;

    int64_t signed_drift;
    modaltest_heap_direction_t direction;

    uint32_t baseline_used_blocks;
    uint32_t final_used_blocks;

    uint32_t reaper_zombies;
    uint32_t reaper_free_inflight;

    uint64_t modal_violations;
    uint64_t session_violations;

    uint32_t timer_nodes_before;
    uint32_t timer_nodes_after;

    uint8_t baseline_heap_stable;
    uint8_t heap_stable;
    uint8_t reaper_idle;
    uint8_t ownership_pass;
} modaltest_open_close_result_t;

typedef struct
{
    char bytes[MODALTEST_OPEN_CLOSE_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} modaltest_open_close_record_t;

static task_handle_t
    g_modaltest_open_close_handles[MODALTEST_OPEN_CLOSE_MAX];
static modaltest_open_close_record_t g_modaltest_open_close_record;

typedef struct {
    volatile uint64_t cases;
    volatile uint64_t failures;
    volatile uint64_t cleanup_calls;
    volatile uint8_t gate;
    volatile uint8_t helper_done;
    task_handle_t caller;
    modal_session_token_t token;
    modal_end_result_t helper_end;
    modal_input_result_t helper_input;
    modal_ui_run_status_t helper_status;
    modal_ui_run_result_t helper_result;
} modaltest_context_t;

typedef struct {
    uint32_t target;
    volatile uint64_t transitions, snapshots, violations;
    volatile uint8_t transition_done, snapshot_done;
} modal_snapshot_race_ctx_t;

typedef struct {
    volatile uint8_t start, release_ui;
    volatile uint32_t done;
    volatile uint64_t cleanup_calls;
    modal_ui_run_status_t status[2];
} modal_concurrent_ctx_t;

typedef struct {
    modal_concurrent_ctx_t *shared;
    uint32_t slot;
} modal_concurrent_arg_t;

static void put_u64(uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(digits[--count]);
}

static bool parse_u32(const char *text, uint32_t *out)
{
    uint64_t value = 0;
    if (!text || !*text || !out)
        return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            return false;
        value = value * 10u + (uint64_t)(*text - '0');
        if (value > UINT32_MAX)
            return false;
    }
    *out = (uint32_t)value;
    return true;
}

static void cleanup_count(void *arg, int result, bool completed)
{
    modaltest_context_t *ctx = arg;
    (void)result;
    (void)completed;
    __atomic_add_fetch(&ctx->cleanup_calls, 1, __ATOMIC_ACQ_REL);
}

static int return_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    ctx->token = token;
    return 0;
}

static int false_token_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    input_event_t event;
    modal_session_token_t bad[4] = {token, token, token, token};
    bad[0].session_generation++;
    bad[1].route_generation++;
    bad[2].owner_task_id++;
    bad[3].owner_lifecycle_generation++;
    for (uint32_t i = 0; i < 4; i++) {
        if (modal_session_end(bad[i]) != MODAL_END_INVALID_TOKEN)
            ctx->failures++;
        modal_input_result_t r = modal_session_try_pop(bad[i], &event);
        if (r != MODAL_INPUT_INVALID_TOKEN)
            ctx->failures++;
        r = modal_session_wait(bad[i], &event);
        if (r != MODAL_INPUT_INVALID_TOKEN)
            ctx->failures++;
        ctx->cases++;
    }
    modal_session_snapshot_t snap;
    if (!modal_session_snapshot(&snap) ||
        snap.state != MODAL_SESSION_ACTIVE)
        ctx->failures++;
    return 0;
}

static int input_token_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    input_event_t expected = {.type = INPUT_EVENT_CHAR, .value = 'M'};
    input_event_t actual;
    modal_session_token_t bad = token;
    bad.session_generation++;
    input_router_dispatch_event(expected);
    if (modal_session_try_pop(bad, &actual) != MODAL_INPUT_INVALID_TOKEN)
        ctx->failures++;
    if (modal_session_try_pop(token, &actual) != MODAL_INPUT_OK ||
        actual.type != expected.type || actual.value != expected.value)
        ctx->failures++;
    ctx->cases = 1;
    return 0;
}

static int stale_token_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    input_event_t event;
    modal_session_token_t old = ctx->token;
    ctx->helper_end = modal_session_end(old);
#ifndef HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN
    if (ctx->helper_end != MODAL_END_INVALID_TOKEN)
        ctx->failures++;
#else
    if (ctx->helper_end != MODAL_END_OK)
        ctx->failures++;
#endif
    modal_input_result_t input_result = modal_session_try_pop(old, &event);
#ifndef HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN
    if (input_result != MODAL_INPUT_INVALID_TOKEN)
        ctx->failures++;
#else
    if (input_result != MODAL_INPUT_SESSION_CLOSED)
        ctx->failures++;
#endif
    modal_session_snapshot_t snap;
    if (!modal_session_snapshot(&snap))
        ctx->failures++;
#ifndef HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN
    else if (snap.state != MODAL_SESSION_ACTIVE ||
             snap.active_generation != token.session_generation)
        ctx->failures++;
#else
    else if (snap.state != MODAL_SESSION_INACTIVE)
        ctx->failures++;
#endif
    ctx->cases = 1;
    return 0;
}

static int shell_blocked_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    task_snapshot_t caller;
    task_handle_t self = TASK_HANDLE_INVALID;
    modal_session_snapshot_t session;
    (void)token;
    if (!scheduler_current_task_handle(&self) ||
        !scheduler_snapshot_task_by_id(ctx->caller.id, &caller) ||
        caller.lifecycle_generation != ctx->caller.lifecycle_generation ||
        caller.state != TASK_BLOCKED ||
        caller.wait_kind != TASK_WAIT_SEMAPHORE ||
        !modal_session_snapshot(&session) ||
        session.owner.id != self.id ||
        session.owner.lifecycle_generation != self.lifecycle_generation)
        ctx->failures++;
    ctx->cases = 1;
    return 0;
}

static void wrong_owner_helper(void *arg)
{
    modaltest_context_t *ctx = arg;
    input_event_t event;
    ctx->helper_end = modal_session_end(ctx->token);
    ctx->helper_input = modal_session_try_pop(ctx->token, &event);
    __atomic_store_n(&ctx->helper_done, 1, __ATOMIC_RELEASE);
}

static int wrong_owner_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    ctx->token = token;
    task_handle_t helper = TASK_HANDLE_INVALID;
    if (!thread_create_named_with_class_flags_handle(
            wrong_owner_helper, ctx, TASK_CLASS_NORMAL, "modal-attacker",
            TASK_FLAG_SYSTEM, &helper)) {
        ctx->failures++;
        return 1;
    }
    uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
    while (!__atomic_load_n(&ctx->helper_done, __ATOMIC_ACQUIRE) &&
           clock_monotonic_ns() < deadline)
        timer_sleep(1);
    if (!ctx->helper_done || ctx->helper_end != MODAL_END_WRONG_OWNER ||
        ctx->helper_input != MODAL_INPUT_WRONG_OWNER)
        ctx->failures++;
    return 0;
}

static int idempotent_owner_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    if (modal_session_end(token) != MODAL_END_OK) {
        ctx->failures++;
        return 1;
    }
    ctx->token = token;
    ctx->helper_done = 0;
    task_handle_t helper = TASK_HANDLE_INVALID;
    if (!thread_create_named_with_class_flags_handle(
            wrong_owner_helper, ctx, TASK_CLASS_NORMAL,
            "modal-closed-attacker", TASK_FLAG_SYSTEM, &helper)) {
        ctx->failures++;
        return 1;
    }
    uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
    while (!__atomic_load_n(&ctx->helper_done, __ATOMIC_ACQUIRE) &&
           clock_monotonic_ns() < deadline)
        timer_sleep(1);
    if (!ctx->helper_done || ctx->helper_end != MODAL_END_WRONG_OWNER)
        ctx->failures++;
    if (modal_session_end(token) != MODAL_END_ALREADY_CLOSED)
        ctx->failures++;
    ctx->cases = 1;
    return 0;
}

static int wait_gate_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    ctx->token = token;
    while (!__atomic_load_n(&ctx->gate, __ATOMIC_ACQUIRE)) {
        task_cancel_point();
        timer_sleep(1);
    }
    return 0;
}

static void contender_helper(void *arg)
{
    modaltest_context_t *ctx = arg;
    modaltest_context_t nested = {0};
    modal_ui_run_result_t result;
    ctx->helper_status = modal_ui_run_sync(
        "modal-contender", return_entry, cleanup_count, &nested, &result);
    __atomic_store_n(&ctx->helper_done, 1, __ATOMIC_RELEASE);
}

static void held_run_helper(void *arg)
{
    modaltest_context_t *ctx = arg;
    modaltest_context_t nested = {0};
    ctx->helper_status = modal_ui_run_sync(
        "modal-held-ui", return_entry, cleanup_count, &nested,
        &ctx->helper_result);
    __atomic_store_n(&ctx->helper_done, 1, __ATOMIC_RELEASE);
}

static int second_entry(modal_session_token_t token, void *arg)
{
    modaltest_context_t *ctx = arg;
    ctx->token = token;
    task_handle_t helper = TASK_HANDLE_INVALID;
    if (!thread_create_named_with_class_flags_handle(
            contender_helper, ctx, TASK_CLASS_INTERACTIVE,
            "modal-contender-ctl",
            TASK_FLAG_SYSTEM, &helper)) {
        ctx->failures++;
        return 1;
    }
    uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
    while (!ctx->helper_done && clock_monotonic_ns() < deadline)
        timer_sleep(1);
    if (!ctx->helper_done || ctx->helper_status != MODAL_UI_RUN_BUSY)
        ctx->failures++;
    return 0;
}

static void killer_helper(void *arg)
{
    modaltest_context_t *ctx = arg;
    uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
    modal_session_snapshot_t snap;
    while (clock_monotonic_ns() < deadline) {
        if (modal_session_snapshot(&snap) &&
            snap.state == MODAL_SESSION_ACTIVE) {
            (void)scheduler_request_kill(snap.owner.id);
            break;
        }
        timer_sleep(1);
    }
    __atomic_store_n(&ctx->helper_done, 1, __ATOMIC_RELEASE);
}

static bool start_helper(void (*entry)(void *), void *arg, const char *name)
{
    task_handle_t handle = TASK_HANDLE_INVALID;
    return thread_create_named_with_class_flags_handle(
        entry, arg, TASK_CLASS_NORMAL, name, TASK_FLAG_SYSTEM, &handle);
}

static bool run_one(modal_ui_entry_fn entry, modaltest_context_t *ctx,
                    modal_ui_run_status_t expected,
                    modal_ui_run_result_t *out)
{
    modal_ui_run_result_t result;
    modal_ui_run_status_t status = modal_ui_run_sync(
        "modaltest-ui", entry, cleanup_count, ctx, &result);
    if (out)
        *out = result;
    return status == expected && result.worker_distinct &&
           result.cleanup_completed;
}

static bool task_handle_valid(task_handle_t handle)
{
    return handle.id != TASK_ID_INVALID && handle.lifecycle_generation != 0;
}

static uint32_t heap_used_blocks(const HeapStats *stats)
{
    if (!stats || stats->blocks_free > stats->blocks_total)
        return 0;
    return stats->blocks_total - stats->blocks_free;
}

static modaltest_heap_direction_t heap_direction(int64_t signed_drift)
{
    if (signed_drift > 0)
        return MODALTEST_HEAP_DIRECTION_UP;
    if (signed_drift < 0)
        return MODALTEST_HEAP_DIRECTION_DOWN;
    return MODALTEST_HEAP_DIRECTION_ZERO;
}

static const char *heap_direction_name(modaltest_heap_direction_t direction)
{
    switch (direction) {
    case MODALTEST_HEAP_DIRECTION_UP: return "UP";
    case MODALTEST_HEAP_DIRECTION_DOWN: return "DOWN";
    default: return "ZERO";
    }
}

static bool reaper_observation_is_idle(uint32_t reaped_this_call,
                                       const task_reaper_stats_t *stats)
{
    (void)reaped_this_call;
    return stats && stats->current_zombies == 0 &&
           stats->free_inflight == 0;
}

static uint64_t timeout_deadline_ns(uint64_t timeout_ms)
{
    uint64_t now = clock_monotonic_ns();
    uint64_t interval = timeout_ms * 1000000ULL;
    return UINT64_MAX - now < interval ? UINT64_MAX : now + interval;
}

static bool wait_task_handle_gone(task_handle_t handle, uint64_t timeout_ms)
{
    if (!task_handle_valid(handle))
        return false;
    uint64_t deadline = timeout_deadline_ns(timeout_ms);
    do {
        task_snapshot_t snapshot;
        if (!scheduler_snapshot_task_by_handle(handle, &snapshot))
            return true;
        (void)scheduler_reap_zombies(0);
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    task_snapshot_t snapshot;
    return !scheduler_snapshot_task_by_handle(handle, &snapshot);
}

static bool wait_reaper_quiescent(uint64_t timeout_ms,
                                  task_reaper_stats_t *out)
{
    uint64_t deadline = timeout_deadline_ns(timeout_ms);
    uint32_t consecutive = 0;
    task_reaper_stats_t stats = {0};
    do {
        uint32_t reaped = scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&stats);
        if (reaper_observation_is_idle(reaped, &stats)) {
            if (++consecutive >= 3) {
                if (out) *out = stats;
                return true;
            }
        } else {
            consecutive = 0;
        }
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    if (out) *out = stats;
    return false;
}

static bool wait_heap_stable(uint64_t timeout_ms, HeapStats *out)
{
    uint64_t deadline = timeout_deadline_ns(timeout_ms);
    HeapStats previous = {0};
    uint32_t consecutive = 0;
    do {
        uint32_t reaped = scheduler_reap_zombies(0);
        task_reaper_stats_t reaper;
        HeapStats current;
        scheduler_reaper_stats_snapshot(&reaper);
        bool got_heap = heap_get_stats(&current);
        bool same = consecutive && got_heap &&
            current.used_bytes == previous.used_bytes &&
            heap_used_blocks(&current) == heap_used_blocks(&previous);
        if (got_heap && reaper_observation_is_idle(reaped, &reaper)) {
            consecutive = same ? consecutive + 1u : 1u;
            previous = current;
            if (consecutive >= 3) {
                if (out) *out = current;
                return true;
            }
        } else {
            consecutive = 0;
            if (got_heap) previous = current;
        }
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    if (out && consecutive) *out = previous;
    return false;
}

static uint32_t u64_to_u32_saturated(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t timer_nodes(const timer_stats_t *stats)
{
    if (!stats)
        return 0;
    return u64_to_u32_saturated((uint64_t)stats->pending +
                                (uint64_t)stats->claimed_now);
}

static bool stats_delta(uint64_t before, uint64_t after, uint64_t *out)
{
    if (!out || after < before)
        return false;
    *out = after - before;
    return true;
}

static bool modal_stats_deltas(
    modaltest_open_close_result_t *result,
    uint32_t cycles)
{
    if (!result)
        return false;
    const modal_ui_stats_t *before = &result->stats_before;
    const modal_ui_stats_t *after = &result->stats_after;
    bool coherent =
        stats_delta(before->runs, after->runs, &result->runs_delta) &&
        stats_delta(before->workers_created, after->workers_created,
                    &result->workers_delta) &&
        stats_delta(before->normal_completions, after->normal_completions,
                    &result->normal_delta) &&
        stats_delta(before->killed_completions, after->killed_completions,
                    &result->killed_delta) &&
        stats_delta(before->cleanup_closes, after->cleanup_closes,
                    &result->cleanup_closes_delta) &&
        stats_delta(before->completion_signals, after->completion_signals,
                    &result->completion_signals_delta) &&
        stats_delta(before->completion_duplicates,
                    after->completion_duplicates,
                    &result->completion_duplicates_delta) &&
        stats_delta(before->context_alloc_failures,
                    after->context_alloc_failures,
                    &result->context_alloc_fail_delta) &&
        stats_delta(before->worker_create_failures,
                    after->worker_create_failures,
                    &result->worker_create_fail_delta) &&
        stats_delta(before->begin_failures, after->begin_failures,
                    &result->begin_fail_delta) &&
        stats_delta(before->lifecycle_recoveries,
                    after->lifecycle_recoveries,
                    &result->lifecycle_recovery_delta) &&
        stats_delta(before->second_session_rejections,
                    after->second_session_rejections,
                    &result->second_session_reject_delta);
    uint64_t expected = (uint64_t)cycles + 1u;
    return coherent && result->runs_delta == expected &&
           result->workers_delta == expected &&
           result->normal_delta == expected && result->killed_delta == 0 &&
           result->cleanup_closes_delta == expected &&
           result->completion_signals_delta == expected &&
           result->completion_duplicates_delta == 0 &&
           result->context_alloc_fail_delta == 0 &&
           result->worker_create_fail_delta == 0 &&
           result->begin_fail_delta == 0 &&
           result->lifecycle_recovery_delta == 0 &&
           result->second_session_reject_delta == 0;
}

static bool modal_internal_ownership_gate(bool ownership_clean,
                                          int64_t global_heap_delta,
                                          int64_t global_block_delta)
{
    (void)global_heap_delta;
    (void)global_block_delta;
    return ownership_clean;
}

static bool modal_outer_heap_gate(bool ownership_clean,
                                  bool outer_heap_measured,
                                  int64_t outer_heap_delta)
{
    return ownership_clean && outer_heap_measured && outer_heap_delta == 0;
}

static void open_close_record_reset(modaltest_open_close_record_t *record)
{
    if (!record) return;
    record->length = 0;
    record->overflow = 0;
    record->bytes[0] = '\0';
}

static bool open_close_record_char(modaltest_open_close_record_t *record,
                                   char value)
{
    if (!record || record->overflow ||
        record->length + 1u >= MODALTEST_OPEN_CLOSE_RECORD_MAX) {
        if (record) record->overflow = 1;
        return false;
    }
    record->bytes[record->length++] = value;
    record->bytes[record->length] = '\0';
    return true;
}

static bool open_close_record_text(modaltest_open_close_record_t *record,
                                   const char *text)
{
    if (!record || !text) return false;
    while (*text)
        if (!open_close_record_char(record, *text++)) return false;
    return true;
}

static bool open_close_record_u64(modaltest_open_close_record_t *record,
                                  uint64_t value)
{
    char reverse[20];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!open_close_record_char(record, reverse[--count])) return false;
    return true;
}

static bool open_close_record_i64(modaltest_open_close_record_t *record,
                                  int64_t value)
{
    if (value >= 0)
        return open_close_record_u64(record, (uint64_t)value);
    if (!open_close_record_char(record, '-'))
        return false;
    uint64_t magnitude = (uint64_t)(-(value + 1)) + 1u;
    return open_close_record_u64(record, magnitude);
}

static uint64_t signed_magnitude(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

static bool build_open_close_record(
    modaltest_open_close_record_t *record,
    const modaltest_open_close_result_t *result,
    bool passed,
    const char *reason,
    const task_snapshot_t *first_live)
{
    if (!record || !result) return false;
    open_close_record_reset(record);
#define TEXT(value_) (void)open_close_record_text(record, (value_))
#define U64(value_) (void)open_close_record_u64(record, (uint64_t)(value_))
    TEXT("\n[MODALTEST][OPEN_CLOSE] "); TEXT(passed ? "PASS" : "FAIL");
    TEXT(" cycles="); U64(result->passed_cycles);
    TEXT(" cleanup="); U64(result->cleanup_total);
    TEXT(" ownership="); TEXT(result->ownership_pass ? "PASS" : "FAIL");
    TEXT(" reason="); TEXT(reason ? reason : "none");
    TEXT(" warmup_id="); U64(result->warmup_worker.id);
    TEXT(" warmup_gone="); U64(result->warmup_gone);
    TEXT(" handles="); U64(result->handles_recorded);
    TEXT(" handles_gone="); U64(result->handles_gone);
    TEXT(" first_live_id="); U64(result->first_live_handle.id);
    TEXT(" baseline_used="); U64(result->baseline.used_bytes);
    TEXT(" final_used="); U64(result->final.used_bytes);
    TEXT(" direction="); TEXT(heap_direction_name(result->direction));
    TEXT(" signed_delta=");
    (void)open_close_record_i64(record, result->signed_drift);
    TEXT(" drift_abs="); U64(signed_magnitude(result->signed_drift));
    TEXT(" baseline_blocks="); U64(result->baseline_used_blocks);
    TEXT(" final_blocks="); U64(result->final_used_blocks);
    TEXT(" heap_scope=GLOBAL_DIAGNOSTIC");
    TEXT(" heap_gate=OUTER_SCENARIO");
    TEXT(" baseline_heap_stable="); U64(result->baseline_heap_stable);
    TEXT(" heap_stable="); U64(result->heap_stable);
    TEXT(" reaper_idle="); U64(result->reaper_idle);
    TEXT(" zombies="); U64(result->reaper_zombies);
    TEXT(" free_inflight="); U64(result->reaper_free_inflight);
    TEXT(" modal_violations="); U64(result->modal_violations);
    TEXT(" session_violations="); U64(result->session_violations);
    TEXT(" runs_delta="); U64(result->runs_delta);
    TEXT(" workers_delta="); U64(result->workers_delta);
    TEXT(" normal_delta="); U64(result->normal_delta);
    TEXT(" killed_delta="); U64(result->killed_delta);
    TEXT(" cleanup_delta="); U64(result->cleanup_closes_delta);
    TEXT(" signals_delta="); U64(result->completion_signals_delta);
    TEXT(" duplicates_delta="); U64(result->completion_duplicates_delta);
    TEXT(" alloc_fail_delta="); U64(result->context_alloc_fail_delta);
    TEXT(" create_fail_delta="); U64(result->worker_create_fail_delta);
    TEXT(" begin_fail_delta="); U64(result->begin_fail_delta);
    TEXT(" recovery_delta="); U64(result->lifecycle_recovery_delta);
    TEXT(" second_session_delta="); U64(result->second_session_reject_delta);
    TEXT(" contexts_live="); U64(result->stats_after.contexts_live);
    TEXT(" contexts_quarantined=");
    U64(result->stats_after.contexts_quarantined);
    TEXT(" timer_pending_before="); U64(result->timers_before.pending);
    TEXT(" timer_pending_after="); U64(result->timers_after.pending);
    TEXT(" timer_claimed_before="); U64(result->timers_before.claimed_now);
    TEXT(" timer_claimed_after="); U64(result->timers_after.claimed_now);
    TEXT(" timer_nodes_before="); U64(result->timer_nodes_before);
    TEXT(" timer_nodes_after="); U64(result->timer_nodes_after);
    if (first_live) {
        TEXT(" first_live_state="); U64(first_live->state);
        TEXT(" first_live_on_cpu="); U64(first_live->on_cpu);
        TEXT(" first_live_queue="); U64(first_live->queue_membership);
        TEXT(" first_live_wait="); U64(first_live->wait_kind);
        TEXT(" first_live_wait_active="); U64(first_live->wait_active);
        TEXT(" first_live_refs="); U64(first_live->task_wake_timer_refs);
        TEXT(" first_live_reap_mask="); U64(first_live->reap_defer_mask_last);
    }
    TEXT("\n");
#undef U64
#undef TEXT
    return !record->overflow && record->length > 2u &&
           record->bytes[0] == '\n' &&
           record->bytes[record->length - 1u] == '\n';
}

bool modaltest_open_close_selftest_case(
    modaltest_open_close_selftest_case_t test_case)
{
    switch (test_case) {
    case MODALTEST_OPEN_CLOSE_SELFTEST_HEAP_DIRECTION:
        return heap_direction(0) == MODALTEST_HEAP_DIRECTION_ZERO &&
               heap_direction(1) == MODALTEST_HEAP_DIRECTION_UP &&
               heap_direction(-1) == MODALTEST_HEAP_DIRECTION_DOWN;
    case MODALTEST_OPEN_CLOSE_SELFTEST_USED_BLOCKS: {
        HeapStats normal = {.blocks_total = 100, .blocks_free = 40};
        HeapStats underflow = {.blocks_total = 40, .blocks_free = 100};
        return heap_used_blocks(&normal) == 60 &&
               heap_used_blocks(&underflow) == 0 &&
               heap_used_blocks(NULL) == 0;
    }
    case MODALTEST_OPEN_CLOSE_SELFTEST_ZERO_IS_NOT_IDLE: {
        task_reaper_stats_t busy = {.current_zombies = 1};
        task_reaper_stats_t idle = {0};
        return !reaper_observation_is_idle(0, &busy) &&
               reaper_observation_is_idle(0, &idle);
    }
    case MODALTEST_OPEN_CLOSE_SELFTEST_RECORD_FIT: {
        modaltest_open_close_result_t result = {
            .passed_cycles = MODALTEST_OPEN_CLOSE_MAX,
            .cleanup_total = UINT64_MAX,
            .warmup_worker = {.id = UINT64_MAX,
                              .lifecycle_generation = UINT64_MAX},
            .warmup_gone = 1,
            .handles_recorded = MODALTEST_OPEN_CLOSE_MAX,
            .handles_gone = MODALTEST_OPEN_CLOSE_MAX,
            .first_live_handle = {.id = UINT64_MAX,
                                  .lifecycle_generation = UINT64_MAX},
            .baseline = {.used_bytes = UINT64_MAX},
            .final = {.used_bytes = UINT64_MAX},
            .signed_drift = INT64_MIN,
            .direction = MODALTEST_HEAP_DIRECTION_DOWN,
            .baseline_used_blocks = UINT32_MAX,
            .final_used_blocks = UINT32_MAX,
            .reaper_zombies = UINT32_MAX,
            .reaper_free_inflight = UINT32_MAX,
            .modal_violations = UINT64_MAX,
            .session_violations = UINT64_MAX,
            .heap_stable = 1,
            .reaper_idle = 1
        };
        task_snapshot_t live = {
            .state = TASK_ZOMBIE,
            .on_cpu = 1,
            .queue_membership = TASK_QUEUE_WAIT,
            .wait_kind = TASK_WAIT_TIMER_SLEEP,
            .wait_active = 1,
            .task_wake_timer_refs = UINT32_MAX,
            .reap_defer_mask_last = UINT64_MAX
        };
        result.stats_after.contexts_live = UINT64_MAX;
        result.stats_after.contexts_quarantined = UINT64_MAX;
        result.runs_delta = UINT64_MAX;
        result.workers_delta = UINT64_MAX;
        result.normal_delta = UINT64_MAX;
        result.killed_delta = UINT64_MAX;
        result.cleanup_closes_delta = UINT64_MAX;
        result.completion_signals_delta = UINT64_MAX;
        result.completion_duplicates_delta = UINT64_MAX;
        result.context_alloc_fail_delta = UINT64_MAX;
        result.worker_create_fail_delta = UINT64_MAX;
        result.begin_fail_delta = UINT64_MAX;
        result.lifecycle_recovery_delta = UINT64_MAX;
        result.second_session_reject_delta = UINT64_MAX;
        result.timers_before.pending = UINT32_MAX;
        result.timers_before.claimed_now = UINT32_MAX;
        result.timers_after.pending = UINT32_MAX;
        result.timers_after.claimed_now = UINT32_MAX;
        result.timer_nodes_before = UINT32_MAX;
        result.timer_nodes_after = UINT32_MAX;
        return build_open_close_record(&g_modaltest_open_close_record,
                                       &result, false, "ownership-stats",
                                       &live);
    }
    case MODALTEST_OPEN_CLOSE_SELFTEST_STATS_DELTA: {
        modaltest_open_close_result_t result = {0};
        result.stats_before = (modal_ui_stats_t){
            .runs = 41, .workers_created = 40,
            .normal_completions = 39, .killed_completions = 3,
            .cleanup_closes = 38, .completion_signals = 37,
            .completion_duplicates = 2, .context_alloc_failures = 4,
            .worker_create_failures = 5, .begin_failures = 6,
            .lifecycle_recoveries = 7, .second_session_rejections = 8
        };
        result.stats_after = result.stats_before;
        result.stats_after.runs += 1001;
        result.stats_after.workers_created += 1001;
        result.stats_after.normal_completions += 1001;
        result.stats_after.cleanup_closes += 1001;
        result.stats_after.completion_signals += 1001;
        return modal_stats_deltas(&result, 1000);
    }
    case MODALTEST_OPEN_CLOSE_SELFTEST_GLOBAL_HEAP_IS_DIAGNOSTIC:
        return modal_internal_ownership_gate(true, 0, 0) &&
               modal_internal_ownership_gate(true, -96, -1) &&
               modal_internal_ownership_gate(true, 96, 1) &&
               !modal_internal_ownership_gate(false, 0, 0);
    case MODALTEST_OPEN_CLOSE_SELFTEST_OUTER_HEAP_REQUIRED:
        return !modal_outer_heap_gate(true, false, 0) &&
               !modal_outer_heap_gate(true, true, 96) &&
               modal_outer_heap_gate(true, true, 0) &&
               !modal_outer_heap_gate(false, true, 0);
    case MODALTEST_OPEN_CLOSE_SELFTEST_TIMER_NODE_NOISE:
        return modal_internal_ownership_gate(true, -96, -1) &&
               !modal_outer_heap_gate(true, true, -96);
    default:
        return false;
    }
}

static int test_open_close(uint32_t cycles)
{
    modaltest_open_close_result_t result = {
        .requested_cycles = cycles,
        .first_live_handle = TASK_HANDLE_INVALID
    };
    task_snapshot_t first_live_snapshot = {0};
    bool first_live_snapshot_valid = false;
    bool start_clean = false;
    bool warmup_ok = false;
    bool cycles_ok = cycles > 0 && cycles <= MODALTEST_OPEN_CLOSE_MAX;
    bool handles_ok = false;
    bool stats_ok = false;
    bool runtime_ok = false;
    bool session_ok = false;
    bool controls_ok = false;
    task_reaper_stats_t reaper = {0};

    memset(g_modaltest_open_close_handles, 0,
           sizeof(g_modaltest_open_close_handles));
    modal_ui_stats_snapshot(&result.stats_before);
    timers_get_stats(&result.timers_before);
    result.timer_nodes_before = timer_nodes(&result.timers_before);
    start_clean = !result.stats_before.active &&
        result.stats_before.contexts_live == 0 &&
        result.stats_before.contexts_quarantined == 0;

    modal_session_test_set_quiet(true);
    scheduler_test_set_lifecycle_log_quiet(true);

    if (cycles_ok && start_clean) {
        modaltest_context_t warmup = {0};
        modal_ui_run_result_t warmup_run = {0};
        warmup_ok = run_one(return_entry, &warmup, MODAL_UI_RUN_OK,
                            &warmup_run) &&
                    task_handle_valid(warmup_run.worker) &&
                    warmup.cleanup_calls == 1;
        result.warmup_worker = warmup_run.worker;
        if (warmup_ok) {
            result.warmup_gone = wait_task_handle_gone(
                result.warmup_worker, MODALTEST_OPEN_CLOSE_TIMEOUT_MS);
            result.reaper_idle = result.warmup_gone &&
                wait_reaper_quiescent(MODALTEST_OPEN_CLOSE_TIMEOUT_MS,
                                      &reaper);
            if (result.reaper_idle) {
                result.baseline_heap_stable = wait_heap_stable(
                    MODALTEST_OPEN_CLOSE_TIMEOUT_MS, &result.baseline);
                if (!result.baseline_heap_stable)
                    (void)heap_get_stats(&result.baseline);
            }
            result.baseline_used_blocks = heap_used_blocks(&result.baseline);
        }
    }

    if (warmup_ok && result.warmup_gone && result.reaper_idle) {
        for (uint32_t i = 0; i < cycles; i++) {
            modaltest_context_t context = {0};
            modal_ui_run_result_t run = {0};
            bool one_ok = run_one(return_entry, &context, MODAL_UI_RUN_OK,
                                  &run);
            if (task_handle_valid(run.worker)) {
                g_modaltest_open_close_handles[result.handles_recorded++] =
                    run.worker;
            }
            result.cleanup_total += context.cleanup_calls;
            if (!one_ok || !task_handle_valid(run.worker) ||
                context.cleanup_calls != 1)
                break;
            result.passed_cycles++;
        }
    }

    for (uint32_t i = 0; i < result.handles_recorded; i++) {
        task_handle_t handle = g_modaltest_open_close_handles[i];
        if (wait_task_handle_gone(handle, MODALTEST_OPEN_CLOSE_TIMEOUT_MS)) {
            result.handles_gone++;
            continue;
        }
        if (!task_handle_valid(result.first_live_handle)) {
            result.first_live_handle = handle;
            first_live_snapshot_valid = scheduler_snapshot_task_by_handle(
                handle, &first_live_snapshot);
        }
    }
    handles_ok = result.handles_recorded == cycles &&
        result.handles_gone == result.handles_recorded;
    if (warmup_ok) {
        result.reaper_idle = wait_reaper_quiescent(
            MODALTEST_OPEN_CLOSE_TIMEOUT_MS, &reaper);
    }
    if (result.reaper_idle) {
        result.heap_stable = wait_heap_stable(
            MODALTEST_OPEN_CLOSE_TIMEOUT_MS, &result.final);
        if (!result.heap_stable)
            (void)heap_get_stats(&result.final);
    } else {
        (void)heap_get_stats(&result.final);
    }

    scheduler_reaper_stats_snapshot(&reaper);
    result.reaper_zombies = u64_to_u32_saturated(reaper.current_zombies);
    result.reaper_free_inflight =
        u64_to_u32_saturated(reaper.free_inflight);
    result.reaper_idle = result.reaper_idle &&
        reaper_observation_is_idle(0, &reaper);
    result.final_used_blocks = heap_used_blocks(&result.final);
    result.signed_drift = (int64_t)result.final.used_bytes -
                          (int64_t)result.baseline.used_bytes;
    result.direction = heap_direction(result.signed_drift);

    modal_ui_runtime_snapshot_t runtime = {0};
    modal_session_snapshot_t session = {0};
    modal_ui_test_controls_snapshot_t controls = {0};
    runtime_ok = modal_ui_validate(&result.modal_violations) &&
        modal_ui_runtime_snapshot(&runtime) && !runtime.active &&
        runtime.contexts_live == 0 && runtime.contexts_quarantined == 0;
    session_ok = modal_session_validate(&result.session_violations) &&
        modal_session_snapshot(&session) &&
        session.state == MODAL_SESSION_INACTIVE &&
        session.router_state == INPUT_ROUTE_DEFAULT &&
        !session.shell_paused;
    controls_ok = modal_ui_test_controls_snapshot(&controls) &&
                  !controls.armed;
    modal_ui_stats_snapshot(&result.stats_after);
    timers_get_stats(&result.timers_after);
    result.timer_nodes_after = timer_nodes(&result.timers_after);
    runtime_ok = runtime_ok && !result.stats_after.active &&
        result.stats_after.contexts_live == 0 &&
        result.stats_after.contexts_quarantined == 0;
    stats_ok = modal_stats_deltas(&result, cycles);

    scheduler_test_set_lifecycle_log_quiet(false);
    modal_session_test_set_quiet(false);

    bool modal_contract_ok = cycles_ok && start_clean && warmup_ok &&
        result.warmup_gone &&
        result.passed_cycles == cycles && result.cleanup_total == cycles &&
        result.handles_recorded == cycles && handles_ok &&
        result.reaper_idle && result.reaper_zombies == 0 &&
        result.reaper_free_inflight == 0 && stats_ok && runtime_ok &&
        session_ok && controls_ok;
    int64_t block_delta = (int64_t)result.final_used_blocks -
                          (int64_t)result.baseline_used_blocks;
    result.ownership_pass = modal_internal_ownership_gate(
        modal_contract_ok, result.signed_drift, block_delta);
    const char *reason = "none";
    if (!start_clean)
        reason = "dirty-start";
    else if (!cycles_ok || !warmup_ok || !result.warmup_gone)
        reason = "warmup";
    else if (result.passed_cycles != cycles ||
             result.cleanup_total != cycles ||
             result.handles_recorded != cycles || !handles_ok)
        reason = "handle";
    else if (!result.reaper_idle || result.reaper_zombies != 0 ||
             result.reaper_free_inflight != 0)
        reason = "reaper";
    else if (!stats_ok)
        reason = "ownership-stats";
    else if (!runtime_ok)
        reason = "modal-runtime";
    else if (!session_ok)
        reason = "modal-session";
    else if (!controls_ok)
        reason = "controls";

    bool ok = result.ownership_pass;
    bool record_ok = build_open_close_record(
        &g_modaltest_open_close_record, &result, ok, reason,
        first_live_snapshot_valid ? &first_live_snapshot : NULL);
    if (record_ok)
        serial_write_all(g_modaltest_open_close_record.bytes);
    else
        serial_write_all("\n[MODALTEST][OPEN_CLOSE] FAIL reason=record-overflow\n");
    ok = ok && record_ok;
    return ok ? 0 : 1;
}

static int test_simple(const char *kind)
{
    modaltest_context_t ctx = {0};
    bool ok = false;
    if (!strcmp(kind, "false-token")) {
        ok = run_one(false_token_entry, &ctx, MODAL_UI_RUN_OK, NULL) &&
             ctx.cases == 4 && ctx.failures == 0;
        serial_write_all(ok ? "[MODALTEST][FALSE_TOKEN] PASS cases=4 state_preserved=1\n" :
                              "[MODALTEST][FALSE_TOKEN] FAIL\n");
    } else if (!strcmp(kind, "wrong-owner")) {
        ok = run_one(wrong_owner_entry, &ctx, MODAL_UI_RUN_OK, NULL) &&
             ctx.failures == 0;
        serial_write_all(ok ? "[MODALTEST][WRONG_OWNER] PASS end=REJECTED input=REJECTED\n" :
                              "[MODALTEST][WRONG_OWNER] FAIL\n");
    } else if (!strcmp(kind, "input-token")) {
        ok = run_one(input_token_entry, &ctx, MODAL_UI_RUN_OK, NULL) &&
             ctx.cases == 1 && ctx.failures == 0;
        serial_write_all(ok ? "[MODALTEST][INPUT_TOKEN] PASS delivered=1 wrong_consumed=0\n" :
                              "[MODALTEST][INPUT_TOKEN] FAIL\n");
    } else if (!strcmp(kind, "second-session")) {
        ok = run_one(second_entry, &ctx, MODAL_UI_RUN_OK, NULL) &&
             ctx.failures == 0;
        serial_write_all(ok ? "[MODALTEST][SECOND] PASS rejected=1 first_preserved=1\n" :
                              "[MODALTEST][SECOND] FAIL\n");
    }
    return ok ? 0 : 1;
}

static int test_failure(const char *kind)
{
    modaltest_context_t ctx = {0};
    modal_ui_run_result_t result;
    modal_ui_run_status_t status;
    if (!strcmp(kind, "alloc-fail"))
        modal_ui_test_fail_next_context_allocation();
    else if (!strcmp(kind, "create-fail"))
        modal_ui_test_fail_next_worker_creation();
    else if (!strcmp(kind, "router-begin-fail"))
        input_router_test_reject_next_begin(true);
    else if (!strcmp(kind, "router-end-fail"))
        input_router_test_reject_next_end(true);
    status = modal_ui_run_sync("modaltest-ui", return_entry, cleanup_count,
                               &ctx, &result);
    modal_session_snapshot_t snap;
    modal_session_snapshot(&snap);
    bool clean = snap.state == MODAL_SESSION_INACTIVE &&
        snap.router_state == INPUT_ROUTE_DEFAULT && !snap.shell_paused;
    bool ok = clean;
    const char *sentinel = "[MODALTEST][FAILURE]";
    if (!strcmp(kind, "alloc-fail")) {
        ok &= status == MODAL_UI_RUN_CONTEXT_ALLOC_FAILED;
        sentinel = "[MODALTEST][ALLOC_FAIL]";
    } else if (!strcmp(kind, "create-fail")) {
        ok &= status == MODAL_UI_RUN_WORKER_CREATE_FAILED;
        sentinel = "[MODALTEST][CREATE_FAIL]";
    } else if (!strcmp(kind, "router-begin-fail")) {
        ok &= status == MODAL_UI_RUN_BEGIN_FAILED;
        sentinel = "[MODALTEST][BEGIN_FAIL]";
    } else {
        ok &= status == MODAL_UI_RUN_OK;
        sentinel = "[MODALTEST][END_FAIL]";
    }
    serial_write_all(sentinel);
    serial_write_all(ok ? " PASS rollback=1 shell_resumed=1\n" : " FAIL\n");
    return ok ? 0 : 1;
}

static int test_owner_exit(void)
{
    modaltest_context_t ctx = {0};
    modal_ui_test_skip_next_cleanup_close();
    modal_ui_run_result_t result;
    modal_ui_run_status_t status = modal_ui_run_sync(
        "modaltest-ui", return_entry, cleanup_count, &ctx, &result);
    bool ok = status == MODAL_UI_RUN_RECOVERED_OWNER_DEATH &&
        result.session_recovered;
    serial_write_all(ok ? "[MODALTEST][OWNER_EXIT] PASS lifecycle_recovered=1 completion=1\n" :
                          "[MODALTEST][OWNER_EXIT] FAIL\n");
    return ok ? 0 : 1;
}

static int test_stale_token(void)
{
    modaltest_context_t first = {0};
    if (!run_one(return_entry, &first, MODAL_UI_RUN_OK, NULL))
        return 1;
    modaltest_context_t second = {.token = first.token};
    bool ok = run_one(stale_token_entry, &second, MODAL_UI_RUN_OK, NULL) &&
              second.cases == 1 && second.failures == 0;
#ifdef HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN
    serial_write_all(ok ? "[MODALTEST][NEGATIVE] STALE_TOKEN_ACCEPTED_DETECTED\n" :
                          "[MODALTEST][NEGATIVE] STALE_TOKEN_ACCEPTED_MISSED\n");
#else
    serial_write_all(ok ? "[MODALTEST][STALE_TOKEN] PASS old_rejected=1 new_preserved=1\n" :
                          "[MODALTEST][STALE_TOKEN] FAIL\n");
#endif
    return ok ? 0 : 1;
}

static int test_shell_blocked(void)
{
    modaltest_context_t ctx = {0};
    if (!scheduler_current_task_handle(&ctx.caller))
        return 1;
    modal_ui_run_result_t result;
    bool ok = run_one(shell_blocked_entry, &ctx, MODAL_UI_RUN_OK,
                      &result) && result.worker_distinct &&
              result.caller_blocked_observed &&
              ctx.cases == 1 && ctx.failures == 0;
    serial_write_all(ok ? "[MODALTEST][SHELL_BLOCKED] PASS distinct=1 shell_blocked=1 owner_worker=1\n" :
                          "[MODALTEST][SHELL_BLOCKED] FAIL\n");
    return ok ? 0 : 1;
}

static int test_kill_before_begin(void)
{
    modaltest_context_t ctx = {0};
    modal_ui_test_kill_next_worker_before_begin();
    modal_ui_run_result_t result;
    modal_ui_run_status_t status = modal_ui_run_sync(
        "modaltest-ui", return_entry, cleanup_count, &ctx, &result);
    modal_session_snapshot_t session;
    modal_session_snapshot(&session);
    bool ok = status == MODAL_UI_RUN_OWNER_KILLED &&
        ctx.cleanup_calls == 1 && result.cleanup_completed &&
        session.state == MODAL_SESSION_INACTIVE &&
        session.router_state == INPUT_ROUTE_DEFAULT &&
        !session.shell_paused;
    serial_write_all(ok ?
        "[MODALTEST][KILL_BEFORE_BEGIN] PASS opened=0 cleanup=1 completion=1\n" :
        "[MODALTEST][KILL_BEFORE_BEGIN] FAIL\n");
    return ok ? 0 : 1;
}

static int test_idempotent_owner(void)
{
    modaltest_context_t ctx = {0};
    bool ok = run_one(idempotent_owner_entry, &ctx, MODAL_UI_RUN_OK,
                      NULL) && ctx.cases == 1 && ctx.failures == 0;
    serial_write_all(ok ?
        "[MODALTEST][IDEMPOTENT_OWNER] PASS owner=ALREADY_CLOSED attacker=WRONG_OWNER\n" :
        "[MODALTEST][IDEMPOTENT_OWNER] FAIL\n");
    return ok ? 0 : 1;
}

static int test_reservation_gap(void)
{
    modaltest_context_t first = {0};
    modal_ui_test_hold_next_worker_after_completion();
    task_handle_t helper = TASK_HANDLE_INVALID;
    if (!thread_create_named_with_class_flags_handle(
            held_run_helper, &first, TASK_CLASS_INTERACTIVE,
            "modal-held-caller", TASK_FLAG_SYSTEM, &helper))
        return 1;
    uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
    modal_ui_runtime_snapshot_t runtime;
    modal_session_snapshot_t before, after;
    do {
        modal_ui_runtime_snapshot(&runtime);
        if (runtime.active && runtime.completion_signaled)
            break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    modal_session_snapshot(&before);
    modaltest_context_t contender = {0};
    modal_ui_run_result_t contender_result;
    modal_ui_run_status_t busy = modal_ui_run_sync(
        "modal-gap-contender", return_entry, cleanup_count, &contender,
        &contender_result);
    modal_session_snapshot(&after);
    modal_ui_test_release_worker_after_completion();
    deadline = clock_monotonic_ns() + 5000000000ULL;
    while (!__atomic_load_n(&first.helper_done, __ATOMIC_ACQUIRE) &&
           clock_monotonic_ns() < deadline)
        timer_sleep(1);
    modaltest_context_t later = {0};
    bool later_ok = run_one(return_entry, &later, MODAL_UI_RUN_OK, NULL);
    bool ok = runtime.active && runtime.completion_signaled &&
        busy == MODAL_UI_RUN_BUSY && contender_result.worker.id == 0 &&
        before.opens == after.opens && first.helper_done &&
        first.helper_status == MODAL_UI_RUN_OK && later_ok;
    serial_write_all(ok ?
        "[MODALTEST][RESERVATION_GAP] PASS busy_before_detach=1 worker_created=0 session_unchanged=1 later_success=1\n" :
        "[MODALTEST][RESERVATION_GAP] FAIL\n");
    return ok ? 0 : 1;
}

static void snapshot_transition_worker(void *arg)
{
    modal_snapshot_race_ctx_t *ctx = arg;
    modal_session_test_set_quiet(true);
    for (uint32_t i = 0; i < ctx->target; i++) {
        if (!modal_session_test_begin("snapshot-race") ||
            !modal_session_test_end()) {
            __atomic_add_fetch(&ctx->violations, 1, __ATOMIC_RELAXED);
            break;
        }
        __atomic_add_fetch(&ctx->transitions, 1, __ATOMIC_RELAXED);
        if ((i & 63u) == 0)
            schedule_voluntary();
    }
    modal_session_test_set_quiet(false);
    __atomic_store_n(&ctx->transition_done, 1, __ATOMIC_RELEASE);
}

static void snapshot_observer_worker(void *arg)
{
    modal_snapshot_race_ctx_t *ctx = arg;
    while (__atomic_load_n(&ctx->snapshots, __ATOMIC_RELAXED) <
           ctx->target) {
        uint64_t violations = 0;
        if (!modal_session_validate(&violations) || violations)
            __atomic_add_fetch(&ctx->violations, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ctx->snapshots, 1, __ATOMIC_RELAXED);
        if ((__atomic_load_n(&ctx->snapshots, __ATOMIC_RELAXED) & 63u) == 0)
            schedule_voluntary();
    }
    __atomic_store_n(&ctx->snapshot_done, 1, __ATOMIC_RELEASE);
}

static int test_snapshot_race(uint32_t iterations)
{
    modal_snapshot_race_ctx_t ctx = {.target = iterations};
    task_handle_t transition = TASK_HANDLE_INVALID;
    task_handle_t observer = TASK_HANDLE_INVALID;
    bool created = thread_create_named_with_class_flags_handle(
        snapshot_transition_worker, &ctx, TASK_CLASS_INTERACTIVE,
        "modal-transition-race", TASK_FLAG_SYSTEM, &transition) &&
        thread_create_named_with_class_flags_handle(
            snapshot_observer_worker, &ctx, TASK_CLASS_INTERACTIVE,
            "modal-snapshot-race", TASK_FLAG_SYSTEM, &observer);
    uint64_t deadline = clock_monotonic_ns() + 120000000000ULL;
    while (created &&
           (!__atomic_load_n(&ctx.transition_done, __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&ctx.snapshot_done, __ATOMIC_ACQUIRE)) &&
           clock_monotonic_ns() < deadline)
        timer_sleep(1);
    bool ok = created && ctx.transition_done && ctx.snapshot_done &&
        ctx.transitions == iterations && ctx.snapshots == iterations &&
        ctx.violations == 0;
    serial_write_all(ok ? "[MODALTEST][SNAPSHOT_RACE] PASS transitions=" :
                          "[MODALTEST][SNAPSHOT_RACE] FAIL transitions=");
    put_u64(ctx.transitions); serial_write_all(" snapshots=");
    put_u64(ctx.snapshots); serial_write_all(" violations=");
    put_u64(ctx.violations); serial_write_all("\n");
    return ok ? 0 : 1;
}

static int concurrent_entry(modal_session_token_t token, void *arg)
{
    modal_concurrent_ctx_t *ctx = arg;
    (void)token;
    while (!__atomic_load_n(&ctx->release_ui, __ATOMIC_ACQUIRE)) {
        task_cancel_point();
        timer_sleep(1);
    }
    return 0;
}

static void concurrent_cleanup(void *arg, int result, bool completed)
{
    modal_concurrent_ctx_t *ctx = arg;
    (void)result;
    (void)completed;
    __atomic_add_fetch(&ctx->cleanup_calls, 1, __ATOMIC_ACQ_REL);
}

static void concurrent_caller(void *arg)
{
    modal_concurrent_arg_t *caller = arg;
    while (!__atomic_load_n(&caller->shared->start, __ATOMIC_ACQUIRE))
        schedule_voluntary();
    modal_ui_run_result_t result;
    caller->shared->status[caller->slot] = modal_ui_run_sync(
        "modal-concurrent", concurrent_entry, concurrent_cleanup,
        caller->shared, &result);
    __atomic_add_fetch(&caller->shared->done, 1, __ATOMIC_ACQ_REL);
}

static bool handle_exited(task_handle_t handle)
{
    task_snapshot_t snapshot;
    return !scheduler_snapshot_task_by_handle(handle, &snapshot) ||
           snapshot.state == TASK_ZOMBIE;
}

static int test_concurrent_callers(uint32_t rounds)
{
    uint64_t accepted = 0, busy = 0, extra_workers = 0;
    modal_session_test_set_quiet(true);
    for (uint32_t round = 0; round < rounds; round++) {
        modal_concurrent_ctx_t ctx = {0};
        modal_concurrent_arg_t args[2] = {
            {.shared = &ctx, .slot = 0},
            {.shared = &ctx, .slot = 1}
        };
        task_handle_t callers[2];
        bool created = thread_create_named_with_class_flags_handle(
            concurrent_caller, &args[0], TASK_CLASS_INTERACTIVE,
            "modal-concurrent-a", TASK_FLAG_SYSTEM, &callers[0]) &&
            thread_create_named_with_class_flags_handle(
            concurrent_caller, &args[1], TASK_CLASS_INTERACTIVE,
            "modal-concurrent-b", TASK_FLAG_SYSTEM, &callers[1]);
        if (!created) {
            extra_workers++;
            break;
        }
        __atomic_store_n(&ctx.start, 1, __ATOMIC_RELEASE);
        uint64_t deadline = clock_monotonic_ns() + 5000000000ULL;
        modal_ui_runtime_snapshot_t runtime;
        while (clock_monotonic_ns() < deadline) {
            modal_ui_runtime_snapshot(&runtime);
            if (runtime.active && ctx.done >= 1)
                break;
            timer_sleep(1);
        }
        __atomic_store_n(&ctx.release_ui, 1, __ATOMIC_RELEASE);
        while (ctx.done != 2 && clock_monotonic_ns() < deadline)
            timer_sleep(1);
        deadline = clock_monotonic_ns() + 5000000000ULL;
        while ((!handle_exited(callers[0]) || !handle_exited(callers[1])) &&
               clock_monotonic_ns() < deadline)
            timer_sleep(1);
        uint32_t ok_count = (ctx.status[0] == MODAL_UI_RUN_OK) +
                            (ctx.status[1] == MODAL_UI_RUN_OK);
        uint32_t busy_count = (ctx.status[0] == MODAL_UI_RUN_BUSY) +
                              (ctx.status[1] == MODAL_UI_RUN_BUSY);
        if (ctx.done != 2 || !handle_exited(callers[0]) ||
            !handle_exited(callers[1]) || ok_count != 1 || busy_count != 1 ||
            ctx.cleanup_calls != 1) {
            extra_workers++;
            break;
        }
        accepted += ok_count;
        busy += busy_count;
        (void)scheduler_reap_zombies(0);
    }
    modal_session_test_set_quiet(false);
    bool ok = accepted == rounds && busy == rounds && extra_workers == 0;
    serial_write_all(ok ? "[MODALTEST][CONCURRENT_CALLERS] PASS rounds=" :
                          "[MODALTEST][CONCURRENT_CALLERS] FAIL rounds=");
    put_u64(rounds); serial_write_all(" accepted="); put_u64(accepted);
    serial_write_all(" busy="); put_u64(busy);
    serial_write_all(" extra_workers="); put_u64(extra_workers);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

typedef enum {
    MK_BLOCKED = 0,
    MK_PREWAIT,
    MK_COMPLETION_READY
} modal_kill_mode_t;

typedef enum {
    MK_STAGE_INIT = 0,
    MK_STAGE_CLAIMED,
    MK_STAGE_CALLER_CREATED,
    MK_STAGE_BOUNDARY_READY,
    MK_STAGE_KILL_ACCEPTED,
    MK_STAGE_HANDLES_ZOMBIE,
    MK_STAGE_SNAPSHOT_VALIDATED,
    MK_STAGE_REAPED,
    MK_STAGE_FAILED
} modal_kill_stage_t;

#define MODAL_KILL_WORKSPACE_CANARY 0x4D4B435746495831ULL

typedef struct {
    uint64_t canary_begin;
    volatile uint32_t claimed;
    uint64_t generation;
    volatile uint32_t active_mode;
    modaltest_context_t context;
    task_handle_t caller;
    task_handle_t worker;
    uint64_t canary_end;
} modal_kill_caller_workspace_t;

typedef struct {
    bool passed;
    modal_kill_mode_t mode;
    modal_kill_stage_t last_completed;
    modal_kill_stage_t first_failure;
    task_handle_t caller;
    task_handle_t worker;
    task_kill_result_t caller_kill_result;
    task_exit_reason_t caller_exit_reason;
    task_exit_reason_t worker_exit_reason;
    modal_ui_test_run_snapshot_t run;
    uint64_t preblock_cancelled_delta;
    uint64_t cleanup_calls;
    bool caller_handle_gone;
    bool worker_handle_gone;
    bool free_inflight_zero;
    bool active_context_zero;
    bool contexts_zero;
    bool quarantine_zero;
    bool session_inactive;
    bool router_default;
    bool shell_unpaused;
    bool test_controls_armed;
    uint64_t residual;
    bool cleanup_ok;
} modal_kill_caller_result_t;

static modal_kill_caller_workspace_t g_kill_caller_workspace;

static const char *modal_kill_mode_name(modal_kill_mode_t mode)
{
    if (mode == MK_PREWAIT)
        return "PREWAIT";
    if (mode == MK_COMPLETION_READY)
        return "COMPLETION_READY";
    return "BLOCKED";
}

static void modal_kill_failure(modal_kill_caller_result_t *result,
                               modal_kill_stage_t stage)
{
    if (result->first_failure == MK_STAGE_INIT)
        result->first_failure = stage;
}

static bool modal_handle_is_gone(task_handle_t handle)
{
    task_snapshot_t snapshot;
    return handle.id == TASK_ID_INVALID ||
           !scheduler_snapshot_task_by_handle(handle, &snapshot);
}

static bool modal_kill_workspace_claim(modal_kill_caller_workspace_t *workspace,
                                       modal_kill_mode_t mode)
{
    uint32_t was_claimed = __atomic_exchange_n(&workspace->claimed, 1,
                                                __ATOMIC_ACQ_REL);
    if (was_claimed) {
        serial_write_all("[MODALTEST][WORKSPACE_CLAIM] FAIL reason=claimed generation=");
        put_u64(workspace->generation);
        serial_write_all("\n");
        return false;
    }

    modal_ui_runtime_snapshot_t runtime = {0};
    bool canaries_ok = !workspace->generation ||
        (workspace->canary_begin == MODAL_KILL_WORKSPACE_CANARY &&
         workspace->canary_end == MODAL_KILL_WORKSPACE_CANARY);
    if (!canaries_ok) {
        serial_write_all("[MODALTEST][WORKSPACE_CLAIM] FAIL reason=canary generation=");
        put_u64(workspace->generation);
        serial_write_all("\n");
        __atomic_store_n(&workspace->claimed, 0, __ATOMIC_RELEASE);
        return false;
    }

    bool runtime_ok = false, old_handles_gone = false;
    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    do {
        runtime_ok = modal_ui_runtime_snapshot(&runtime) &&
            !runtime.active && runtime.contexts_live == 0;
        old_handles_gone = modal_handle_is_gone(workspace->caller) &&
                           modal_handle_is_gone(workspace->worker);
        if (runtime_ok && old_handles_gone)
            break;
        (void)scheduler_reap_zombies(0);
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    if (!runtime_ok || !old_handles_gone) {
        serial_write_all("[MODALTEST][WORKSPACE_CLAIM] FAIL reason=precondition active=");
        put_u64(runtime.active);
        serial_write_all(" contexts="); put_u64(runtime.contexts_live);
        serial_write_all(" caller_id="); put_u64(workspace->caller.id);
        serial_write_all(" worker_id="); put_u64(workspace->worker.id);
        serial_write_all("\n");
        __atomic_store_n(&workspace->claimed, 0, __ATOMIC_RELEASE);
        return false;
    }

    memset(&workspace->context, 0, sizeof(workspace->context));
    workspace->canary_begin = MODAL_KILL_WORKSPACE_CANARY;
    workspace->canary_end = MODAL_KILL_WORKSPACE_CANARY;
    workspace->generation++;
    workspace->caller = TASK_HANDLE_INVALID;
    workspace->worker = TASK_HANDLE_INVALID;
    __atomic_store_n(&workspace->active_mode, (uint32_t)mode,
                     __ATOMIC_RELEASE);
    return true;
}

static void kill_caller_mode_helper(void *arg)
{
    modal_kill_caller_workspace_t *workspace = arg;
    modaltest_context_t *ctx = &workspace->context;
    modal_ui_run_result_t result;
    modal_kill_mode_t mode = (modal_kill_mode_t)__atomic_load_n(
        &workspace->active_mode, __ATOMIC_ACQUIRE);
    modal_ui_entry_fn entry = mode == MK_COMPLETION_READY ?
        return_entry : wait_gate_entry;
    (void)modal_ui_run_sync("modal-caller-owner", entry, cleanup_count, ctx,
                            &result);
    ctx->helper_result = result;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&ctx->helper_done, 1, __ATOMIC_RELEASE);
}

static bool modal_wait_handles_zombie(modal_kill_caller_result_t *result)
{
    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    task_snapshot_t caller = {0}, worker = {0};
    bool caller_zombie = false, worker_zombie = false;
    do {
        caller_zombie = result->caller.id == TASK_ID_INVALID ||
            (scheduler_snapshot_task_by_handle(result->caller, &caller) &&
             caller.state == TASK_ZOMBIE);
        worker_zombie = result->worker.id == TASK_ID_INVALID ||
            (scheduler_snapshot_task_by_handle(result->worker, &worker) &&
             worker.state == TASK_ZOMBIE);
        if (caller_zombie && worker_zombie)
            break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);
    if (caller_zombie && result->caller.id != TASK_ID_INVALID)
        result->caller_exit_reason = caller.exit_reason;
    if (worker_zombie && result->worker.id != TASK_ID_INVALID)
        result->worker_exit_reason = worker.exit_reason;
    return caller_zombie && worker_zombie;
}

static bool modal_kill_caller_cleanup(
    modal_kill_caller_workspace_t *workspace,
    modal_kill_caller_result_t *result)
{
    modal_ui_test_release_caller_before_wait();
    modal_ui_test_release_worker_after_completion();
    modal_ui_test_reset_controls();

    modal_ui_runtime_snapshot_t runtime = {0};
    if (result->worker.id == TASK_ID_INVALID &&
        modal_ui_runtime_snapshot(&runtime) && runtime.active)
        result->worker = runtime.worker;
    workspace->worker = result->worker;

    task_snapshot_t snapshot = {0};
    if (result->caller.id != TASK_ID_INVALID &&
        scheduler_snapshot_task_by_handle(result->caller, &snapshot) &&
        snapshot.state != TASK_ZOMBIE)
        (void)scheduler_request_kill(result->caller.id);
    bool zombies = modal_wait_handles_zombie(result);
    if (!zombies) {
        if (result->worker.id != TASK_ID_INVALID &&
            scheduler_snapshot_task_by_handle(result->worker, &snapshot) &&
            snapshot.state != TASK_ZOMBIE)
            (void)scheduler_request_kill(result->worker.id);
        if (result->worker.id != TASK_ID_INVALID)
            (void)modal_session_recover_owner(result->worker,
                                              MODAL_RECOVERY_TEST);
        zombies = modal_wait_handles_zombie(result);
    }
    if (zombies)
        result->last_completed = MK_STAGE_HANDLES_ZOMBIE;
    else
        modal_kill_failure(result, MK_STAGE_HANDLES_ZOMBIE);
    result->cleanup_calls = __atomic_load_n(
        &workspace->context.cleanup_calls, __ATOMIC_ACQUIRE);

    if (result->caller.id != TASK_ID_INVALID &&
        scheduler_snapshot_task_by_handle(result->caller, &snapshot) &&
        snapshot.state == TASK_ZOMBIE)
        (void)scheduler_test_hold_reap(result->caller.id, false);
    if (result->worker.id != TASK_ID_INVALID &&
        scheduler_snapshot_task_by_handle(result->worker, &snapshot) &&
        snapshot.state == TASK_ZOMBIE)
        (void)scheduler_test_hold_reap(result->worker.id, false);

    task_reaper_stats_t reaper = {0};
    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    do {
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        result->caller_handle_gone = modal_handle_is_gone(result->caller);
        result->worker_handle_gone = modal_handle_is_gone(result->worker);
        result->free_inflight_zero = reaper.free_inflight == 0;
        if (result->caller_handle_gone && result->worker_handle_gone &&
            result->free_inflight_zero)
            break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);

    modal_session_snapshot_t session = {0};
    modal_ui_test_controls_snapshot_t controls = {0};
    bool runtime_valid = modal_ui_runtime_snapshot(&runtime);
    bool session_valid = modal_session_snapshot(&session);
    bool controls_valid = modal_ui_test_controls_snapshot(&controls);
    result->active_context_zero = runtime_valid && !runtime.active;
    result->contexts_zero = runtime_valid && runtime.contexts_live == 0;
    result->quarantine_zero = runtime_valid &&
                              runtime.contexts_quarantined == 0;
    result->session_inactive = session_valid &&
                               session.state == MODAL_SESSION_INACTIVE;
    result->router_default = session_valid &&
                             session.router_state == INPUT_ROUTE_DEFAULT;
    result->shell_unpaused = session_valid && !session.shell_paused;
    result->test_controls_armed = !controls_valid || controls.armed;
    result->residual = !result->caller_handle_gone +
        !result->worker_handle_gone + !result->free_inflight_zero +
        !result->active_context_zero + !result->contexts_zero +
        !result->quarantine_zero + !result->session_inactive +
        !result->router_default + !result->shell_unpaused +
        result->test_controls_armed;

    bool canaries_ok = workspace->canary_begin ==
                           MODAL_KILL_WORKSPACE_CANARY &&
                       workspace->canary_end == MODAL_KILL_WORKSPACE_CANARY;
    bool teardown_safe = zombies && result->caller_handle_gone &&
        result->worker_handle_gone && result->free_inflight_zero &&
        result->active_context_zero && result->contexts_zero &&
        result->session_inactive && result->router_default &&
        result->shell_unpaused && !result->test_controls_armed && canaries_ok;
    result->cleanup_ok = teardown_safe && result->quarantine_zero &&
                         result->cleanup_calls == 1;
    if (teardown_safe) {
        workspace->caller = TASK_HANDLE_INVALID;
        workspace->worker = TASK_HANDLE_INVALID;
        __atomic_store_n(&workspace->claimed, 0, __ATOMIC_RELEASE);
        result->last_completed = MK_STAGE_REAPED;
    } else {
        modal_kill_failure(result, MK_STAGE_REAPED);
    }
    return result->cleanup_ok;
}

static bool test_kill_caller_mode_result(modal_kill_mode_t mode, bool quiet,
                                          modal_kill_caller_result_t *result)
{
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    result->mode = mode;
    result->caller_kill_result = TASK_KILL_ERR_INVALID;
    modal_kill_caller_workspace_t *workspace = &g_kill_caller_workspace;
    modal_ui_test_reset_controls();
    if (!modal_kill_workspace_claim(workspace, mode)) {
        modal_kill_failure(result, MK_STAGE_CLAIMED);
        return false;
    }
    result->last_completed = MK_STAGE_CLAIMED;

    scheduler_wait_stats_t wait_before = {0}, wait_after = {0};
    scheduler_wait_stats_snapshot(&wait_before);
    if (mode != MK_BLOCKED)
        modal_ui_test_hold_next_caller_before_wait();
    if (mode == MK_COMPLETION_READY)
        modal_ui_test_allow_next_worker_before_caller_wait();
    modal_ui_test_hold_next_worker_for_reap();

    task_create_options_t options = {
        .name = "modal-kill-caller",
        .task_class = TASK_CLASS_INTERACTIVE,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .test_reap_hold = 1
    };
    bool created = thread_create_ex_handle(kill_caller_mode_helper, workspace,
                                            &options, &workspace->caller);
    result->caller = workspace->caller;
    if (created)
        result->last_completed = MK_STAGE_CALLER_CREATED;
    else
        modal_kill_failure(result, MK_STAGE_CALLER_CREATED);

    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    modal_ui_runtime_snapshot_t runtime = {0};
    task_snapshot_t caller = {0};
    bool ready = false;
    while (created && clock_monotonic_ns() < deadline) {
        bool runtime_ok = modal_ui_runtime_snapshot(&runtime);
        bool found = scheduler_snapshot_task_by_handle(result->caller,
                                                        &caller);
        if (mode == MK_BLOCKED) {
            ready = runtime_ok && runtime.active && found &&
                caller.state == TASK_BLOCKED && caller.wait_active &&
                caller.wait_kind == TASK_WAIT_SEMAPHORE &&
                runtime.caller_blocked_observed;
        } else if (mode == MK_PREWAIT) {
            ready = runtime_ok && runtime.active &&
                modal_ui_test_caller_before_wait_observed() && found &&
                !caller.wait_active && !runtime.completion_signaled;
        } else {
            ready = runtime_ok && runtime.active &&
                modal_ui_test_caller_before_wait_observed() && found &&
                !caller.wait_active && runtime.completion_signaled &&
                runtime.cleanup_completed;
        }
        if (ready)
            break;
        timer_sleep(1);
    }
    result->worker = runtime.worker;
    workspace->worker = runtime.worker;
    if (ready)
        result->last_completed = MK_STAGE_BOUNDARY_READY;
    else
        modal_kill_failure(result, MK_STAGE_BOUNDARY_READY);
    result->caller_kill_result = ready ? scheduler_request_kill(
        result->caller.id) : TASK_KILL_ERR_INVALID;
    if (result->caller_kill_result == TASK_KILL_ACCEPTED)
        result->last_completed = MK_STAGE_KILL_ACCEPTED;
    else
        modal_kill_failure(result, MK_STAGE_KILL_ACCEPTED);

    bool cleanup = modal_kill_caller_cleanup(workspace, result);
    scheduler_wait_stats_snapshot(&wait_after);
    result->preblock_cancelled_delta = wait_after.preblock_cancelled -
                                       wait_before.preblock_cancelled;
    bool have_run = modal_ui_test_last_run_snapshot(&result->run) &&
        result->run.caller.id == result->caller.id &&
        result->run.caller.lifecycle_generation ==
            result->caller.lifecycle_generation &&
        result->run.worker.id == result->worker.id &&
        result->run.worker.lifecycle_generation ==
            result->worker.lifecycle_generation;
    bool common = ready &&
        result->caller_kill_result == TASK_KILL_ACCEPTED && cleanup &&
        have_run && result->caller_exit_reason == TASK_EXIT_KILLED &&
        result->run.caller_cancel_latched &&
        result->run.cleanup_completed && result->cleanup_calls == 1 &&
        result->caller_handle_gone && result->worker_handle_gone &&
        result->free_inflight_zero && !result->residual;
    bool mode_ok = false;
    if (mode == MK_BLOCKED) {
        mode_ok = result->run.caller_wait_result ==
                TASK_WAIT_RESULT_CANCELLED &&
            result->run.caller_blocked_observed &&
            !result->run.caller_before_wait_observed &&
            !result->run.worker_before_wait_allowed &&
            result->worker_exit_reason == TASK_EXIT_KILLED &&
            result->run.worker_exit_reason == TASK_EXIT_KILLED;
    } else if (mode == MK_PREWAIT) {
        mode_ok = result->run.caller_wait_result ==
                TASK_WAIT_RESULT_CANCELLED &&
            result->run.caller_before_wait_observed &&
            !result->run.caller_blocked_observed &&
            !result->run.worker_before_wait_allowed &&
            !result->run.completion_ready_before_wait &&
            result->preblock_cancelled_delta == 1 &&
            result->worker_exit_reason == TASK_EXIT_KILLED &&
            result->run.worker_exit_reason == TASK_EXIT_KILLED;
    } else {
        mode_ok = result->run.caller_wait_result == TASK_WAIT_RESULT_OK &&
            result->run.caller_before_wait_observed &&
            !result->run.caller_blocked_observed &&
            result->run.worker_before_wait_allowed &&
            result->run.completion_ready_before_wait &&
            result->worker_exit_reason == TASK_EXIT_NORMAL &&
            result->run.worker_exit_reason == TASK_EXIT_NORMAL;
    }
    result->passed = common && mode_ok;
    if (result->passed)
        result->last_completed = MK_STAGE_SNAPSHOT_VALIDATED;
    else
        modal_kill_failure(result, MK_STAGE_SNAPSHOT_VALIDATED);

    if (!quiet) {
        serial_write_all(mode == MK_BLOCKED ?
            "[MODALTEST][KILL_CALLER_BLOCKED] " :
            mode == MK_PREWAIT ? "[MODALTEST][KILL_CALLER_PREWAIT] " :
                                 "[MODALTEST][KILL_CALLER_COMPLETION_READY] ");
        serial_write_all(result->passed ? "PASS" : "FAIL");
        serial_write_all(" caller=KILLED worker=");
        serial_write_all(result->worker_exit_reason == TASK_EXIT_NORMAL ?
                         "NORMAL" : "KILLED");
        serial_write_all(" cleanup="); put_u64(result->cleanup_calls);
        serial_write_all(" caller_handle_gone=");
        put_u64(result->caller_handle_gone);
        serial_write_all(" worker_handle_gone=");
        put_u64(result->worker_handle_gone);
        serial_write_all(" free_inflight=");
        put_u64(result->free_inflight_zero ? 0 : 1);
        serial_write_all(" active_context=");
        put_u64(result->active_context_zero ? 0 : 1);
        serial_write_all(" contexts_live=");
        put_u64(result->contexts_zero ? 0 : 1);
        serial_write_all(" quarantine=");
        put_u64(result->quarantine_zero ? 0 : 1);
        serial_write_all(" session=");
        serial_write_all(result->session_inactive ? "INACTIVE" : "ACTIVE");
        serial_write_all(" router=");
        serial_write_all(result->router_default ? "DEFAULT" : "MODAL");
        serial_write_all(" shell_paused="); put_u64(!result->shell_unpaused);
        serial_write_all(" test_controls_armed=");
        put_u64(result->test_controls_armed);
        serial_write_all(" stage="); put_u64(result->last_completed);
        serial_write_all(" first_failure="); put_u64(result->first_failure);
        serial_write_all(" caller_id="); put_u64(result->caller.id);
        serial_write_all(" worker_id="); put_u64(result->worker.id);
        serial_write_all(" residual="); put_u64(result->residual);
        serial_write_all("\n");
    }
    return result->passed;
}

static int test_kill_caller_mode(modal_kill_mode_t mode, bool quiet)
{
    modal_kill_caller_result_t result;
    return test_kill_caller_mode_result(mode, quiet, &result) ? 0 : 1;
}

static int test_kill_caller_loop(uint32_t count, const char *mode_text)
{
    modal_kill_mode_t mode;
    if (!strcmp(mode_text, "blocked"))
        mode = MK_BLOCKED;
    else if (!strcmp(mode_text, "prewait"))
        mode = MK_PREWAIT;
    else if (!strcmp(mode_text, "completion-ready"))
        mode = MK_COMPLETION_READY;
    else
        return 1;
    uint32_t passed = 0, caller_killed = 0, worker_killed = 0;
    uint32_t worker_normal = 0, cancel_latched = 0, cleanup = 0;
    uint32_t handles_gone = 0, unexpected_exit = 0;
    uint64_t residual = 0;
    scheduler_test_set_lifecycle_log_quiet(true);
    modal_session_test_set_quiet(true);
    while (passed < count) {
        modal_kill_caller_result_t result;
        bool ok = test_kill_caller_mode_result(mode, true, &result);
        caller_killed += result.caller_exit_reason == TASK_EXIT_KILLED;
        worker_killed += result.worker_exit_reason == TASK_EXIT_KILLED;
        worker_normal += result.worker_exit_reason == TASK_EXIT_NORMAL;
        cancel_latched += result.run.caller_cancel_latched;
        cleanup += result.cleanup_calls == 1;
        handles_gone += result.caller_handle_gone +
                        result.worker_handle_gone;
        residual += result.residual;
        unexpected_exit += result.caller_exit_reason != TASK_EXIT_KILLED ||
            (mode == MK_COMPLETION_READY ?
                result.worker_exit_reason != TASK_EXIT_NORMAL :
                result.worker_exit_reason != TASK_EXIT_KILLED);
        if (!ok) {
            serial_write_all("[MODALTEST][KILL_CALLER_CASE] FAIL mode=");
            serial_write_all(modal_kill_mode_name(mode));
            serial_write_all(" iteration="); put_u64(passed + 1u);
            serial_write_all(" last="); put_u64(result.last_completed);
            serial_write_all(" first_failure=");
            put_u64(result.first_failure);
            serial_write_all(" caller_id="); put_u64(result.caller.id);
            serial_write_all(" worker_id="); put_u64(result.worker.id);
            serial_write_all(" caller_reason=");
            put_u64(result.caller_exit_reason);
            serial_write_all(" worker_reason=");
            put_u64(result.worker_exit_reason);
            serial_write_all(" kill="); put_u64(result.caller_kill_result);
            serial_write_all(" wait="); put_u64(result.run.caller_wait_result);
            serial_write_all(" latched=");
            put_u64(result.run.caller_cancel_latched);
            serial_write_all(" before_wait=");
            put_u64(result.run.caller_before_wait_observed);
            serial_write_all(" blocked=");
            put_u64(result.run.caller_blocked_observed);
            serial_write_all(" worker_before_wait=");
            put_u64(result.run.worker_before_wait_allowed);
            serial_write_all(" completion_ready=");
            put_u64(result.run.completion_ready_before_wait);
            serial_write_all(" preblock_delta=");
            put_u64(result.preblock_cancelled_delta);
            serial_write_all(" cleanup="); put_u64(result.cleanup_calls);
            serial_write_all(" gone=");
            put_u64(result.caller_handle_gone +
                    result.worker_handle_gone);
            serial_write_all(" residual="); put_u64(result.residual);
            serial_write_all("\n");
            break;
        }
        passed++;
    }
    modal_session_test_set_quiet(false);
    scheduler_test_set_lifecycle_log_quiet(false);
    bool ok = count && passed == count && caller_killed == count &&
        cleanup == count && handles_gone == count * 2u &&
        !unexpected_exit && !residual &&
        (mode == MK_COMPLETION_READY ?
            (worker_normal == count && !worker_killed &&
             cancel_latched == count) :
            (worker_killed == count && !worker_normal));
    serial_write_all(ok ? "[MODALTEST][KILL_CALLER_LOOP] PASS mode=" :
                          "[MODALTEST][KILL_CALLER_LOOP] FAIL mode=");
    serial_write_all(modal_kill_mode_name(mode));
    serial_write_all(" count="); put_u64(count);
    serial_write_all(" caller_killed="); put_u64(caller_killed);
    serial_write_all(" worker_killed="); put_u64(worker_killed);
    serial_write_all(" worker_normal="); put_u64(worker_normal);
    serial_write_all(" cancel_latched="); put_u64(cancel_latched);
    serial_write_all(" cleanup="); put_u64(cleanup);
    serial_write_all(" handles_gone="); put_u64(handles_gone);
    serial_write_all(" unexpected_exit="); put_u64(unexpected_exit);
    serial_write_all(" residual="); put_u64(residual);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_kill_caller(void)
{
    return test_kill_caller_mode(MK_BLOCKED, false);
}

static int test_kill_owner(void)
{
    modaltest_context_t ctx = {0};
    if (!start_helper(killer_helper, &ctx, "modal-killer"))
        return 1;
    modal_ui_run_result_t result;
    modal_ui_run_status_t status = modal_ui_run_sync(
        "modaltest-ui", wait_gate_entry, cleanup_count, &ctx, &result);
    bool ok = status == MODAL_UI_RUN_OWNER_KILLED &&
        result.cleanup_completed && result.session_recovered;
    serial_write_all(ok ? "[MODALTEST][KILL_OWNER] PASS cleanup=1 completion=1 shell_resumed=1\n" :
                          "[MODALTEST][KILL_OWNER] FAIL\n");
    return ok ? 0 : 1;
}

static int test_stress(uint32_t normal, uint32_t killed,
                       uint32_t recovered, uint32_t busy,
                       uint32_t token, uint32_t failures)
{
    uint32_t normal_done = 0, killed_done = 0, recovered_done = 0;
    uint32_t busy_done = 0, token_done = 0, failure_done = 0;
    modal_session_test_set_quiet(true);
    scheduler_test_set_lifecycle_log_quiet(true);
    for (; normal_done < normal; normal_done++) {
        modaltest_context_t ctx = {0};
        if (!run_one(return_entry, &ctx, MODAL_UI_RUN_OK, NULL) ||
            ctx.cleanup_calls != 1)
            break;
    }
    for (; killed_done < killed; killed_done++) {
        modaltest_context_t ctx = {0};
        if (!start_helper(killer_helper, &ctx, "modal-stress-killer"))
            break;
        modal_ui_run_result_t result;
        if (modal_ui_run_sync("modal-stress-killed", wait_gate_entry,
                              cleanup_count, &ctx, &result) !=
                MODAL_UI_RUN_OWNER_KILLED ||
            ctx.cleanup_calls != 1)
            break;
    }
    for (; recovered_done < recovered; recovered_done++) {
        modaltest_context_t ctx = {0};
        modal_ui_test_skip_next_cleanup_close();
        modal_ui_run_result_t result;
        if (modal_ui_run_sync("modal-stress-recovery", return_entry,
                              cleanup_count, &ctx, &result) !=
                MODAL_UI_RUN_RECOVERED_OWNER_DEATH ||
            !result.session_recovered)
            break;
    }
    for (; busy_done < busy; busy_done++) {
        modaltest_context_t ctx = {0};
        if (!run_one(second_entry, &ctx, MODAL_UI_RUN_OK, NULL) ||
            ctx.failures)
            break;
    }
    for (; token_done < token; token_done++) {
        modaltest_context_t ctx = {0};
        if (!run_one(false_token_entry, &ctx, MODAL_UI_RUN_OK, NULL) ||
            ctx.failures || ctx.cases != 4)
            break;
    }
    for (; failure_done < failures; failure_done++) {
        modaltest_context_t ctx = {0};
        modal_ui_run_result_t result;
        input_router_test_reject_next_begin(true);
        if (modal_ui_run_sync("modal-stress-failure", return_entry,
                              cleanup_count, &ctx, &result) !=
            MODAL_UI_RUN_BEGIN_FAILED)
            break;
    }
    /* Keep lifecycle logging quiet until every stress worker is gone.  This
       prevents a completed stress command from leaving a serial backlog that
       starves the following HMP/input gates. */
    for (uint32_t attempt = 0; attempt < 10000; attempt++) {
        task_reaper_stats_t reaper;
        scheduler_reaper_stats_snapshot(&reaper);
        if (!reaper.current_zombies && !reaper.free_inflight)
            break;
        (void)scheduler_reap_zombies(0);
        timer_sleep(1);
    }
    scheduler_test_set_lifecycle_log_quiet(false);
    modal_session_test_set_quiet(false);
    uint64_t violations = 0;
    bool ok = normal_done == normal && killed_done == killed &&
        recovered_done == recovered && busy_done == busy &&
        token_done == token && failure_done == failures &&
        modal_ui_validate(&violations) && modal_session_validate(&violations);
    serial_write_all(ok ? "[MODALTEST][STRESS] PASS normal=" :
                          "[MODALTEST][STRESS] FAIL normal=");
    put_u64(normal_done); serial_write_all(" killed="); put_u64(killed_done);
    serial_write_all(" recovered="); put_u64(recovered_done);
    serial_write_all(" busy="); put_u64(busy_done);
    serial_write_all(" token="); put_u64(token_done);
    serial_write_all(" failures="); put_u64(failure_done);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_check(void)
{
    uint64_t session_v = 0, runtime_v = 0;
    modal_ui_test_controls_snapshot_t controls = {0};
    bool ok = modal_session_validate(&session_v) &&
              modal_ui_validate(&runtime_v) &&
              modal_ui_test_controls_snapshot(&controls) &&
              !controls.armed;
    serial_write_all(ok ? "[MODALTEST][CHECK] PASS state=INACTIVE contexts=0 quarantine=0 duplicates=0 violations=0\n" :
                          "[MODALTEST][CHECK] FAIL\n");
    return ok ? 0 : 1;
}

static int test_boundary(const char *sub)
{
    char *args[2] = {"inputtest", (char *)sub};
    int rc = cmd_inputtest(2, args);
    uint64_t violations = 0;
    bool ok = rc == 0 && modal_session_validate(&violations);
    if (!strcmp(sub, "begin-boundary"))
        serial_write_all(ok ? "[MODALTEST][OPENING_BOUNDARY] PASS\n" :
                              "[MODALTEST][OPENING_BOUNDARY] FAIL\n");
    else
        serial_write_all(ok ? "[MODALTEST][CLOSING_BOUNDARY] PASS\n" :
                              "[MODALTEST][CLOSING_BOUNDARY] FAIL\n");
    return ok ? 0 : 1;
}

static int test_completion(void)
{
    modal_ui_stats_t before, after;
    modal_ui_stats_snapshot(&before);
    modaltest_context_t normal = {0};
    bool ok = run_one(return_entry, &normal, MODAL_UI_RUN_OK, NULL);
    ok = test_failure("router-begin-fail") == 0 && ok;
    ok = test_failure("router-end-fail") == 0 && ok;
    ok = test_owner_exit() == 0 && ok;
    ok = test_kill_owner() == 0 && ok;
    ok = test_simple("false-token") == 0 && ok;
    modal_ui_stats_snapshot(&after);
    ok = ok && after.completion_signals == before.completion_signals + 6 &&
         after.completion_duplicates == before.completion_duplicates;
    serial_write_all(ok ? "[MODALTEST][COMPLETION] PASS cases=6 duplicates=0\n" :
                          "[MODALTEST][COMPLETION] FAIL\n");
    return ok ? 0 : 1;
}

static int test_stats(void)
{
    modal_ui_stats_t stats;
    modal_ui_stats_snapshot(&stats);
    serial_write_all("[MODALTEST][STATS] runs="); put_u64(stats.runs);
    serial_write_all(" completion="); put_u64(stats.completion_signals);
    serial_write_all(" duplicates="); put_u64(stats.completion_duplicates);
    serial_write_all(" contexts="); put_u64(stats.contexts_live);
    serial_write_all(" quarantine="); put_u64(stats.contexts_quarantined);
    serial_write_all("\n");
    return 0;
}

int cmd_modaltest(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "check";
    if (!strcmp(sub, "check")) return test_check();
    if (!strcmp(sub, "stats")) return test_stats();
    if (!strcmp(sub, "open-close")) {
        uint32_t cycles = 1000;
        if (argc > 2 && !parse_u32(argv[2], &cycles)) return 1;
        return test_open_close(cycles);
    }
    if (!strcmp(sub, "false-token") || !strcmp(sub, "wrong-owner") ||
        !strcmp(sub, "input-token") || !strcmp(sub, "second-session"))
        return test_simple(sub);
    if (!strcmp(sub, "alloc-fail") || !strcmp(sub, "create-fail") ||
        !strcmp(sub, "router-begin-fail") || !strcmp(sub, "router-end-fail"))
        return test_failure(sub);
    if (!strcmp(sub, "kill-owner")) return test_kill_owner();
    if (!strcmp(sub, "kill-caller")) return test_kill_caller();
    if (!strcmp(sub, "kill-caller-blocked")) return test_kill_caller_mode(MK_BLOCKED,false);
    if (!strcmp(sub, "kill-caller-prewait")) return test_kill_caller_mode(MK_PREWAIT,false);
    if (!strcmp(sub, "kill-caller-completion-ready")) return test_kill_caller_mode(MK_COMPLETION_READY,false);
    if (!strcmp(sub,"kill-caller-loop")) {uint32_t n=1000;if(argc>2&&!parse_u32(argv[2],&n))return 1;return test_kill_caller_loop(n,argc>3?argv[3]:"blocked");}
    if (!strcmp(sub, "kill-before-begin")) return test_kill_before_begin();
    if (!strcmp(sub, "reservation-gap")) return test_reservation_gap();
    if (!strcmp(sub, "idempotent-owner")) return test_idempotent_owner();
    if (!strcmp(sub, "snapshot-race")) {
        uint32_t iterations = 100000;
        if (argc > 2 && !parse_u32(argv[2], &iterations)) return 1;
        return test_snapshot_race(iterations);
    }
    if (!strcmp(sub, "concurrent-callers")) {
        uint32_t rounds = 10000;
        if (argc > 2 && !parse_u32(argv[2], &rounds)) return 1;
        return test_concurrent_callers(rounds);
    }
    if (!strcmp(sub, "owner-exit")) return test_owner_exit();
    if (!strcmp(sub, "arm-kill-next-ui")) {
        modal_ui_test_kill_next_worker();
        serial_write_all("[MODALTEST][ARM_KILL_NEXT_UI] OK\n");
        return 0;
    }
    if (!strcmp(sub, "opening-boundary"))
        return test_boundary("begin-boundary");
    if (!strcmp(sub, "closing-boundary"))
        return test_boundary("end-boundary");
    if (!strcmp(sub, "stale-token")) return test_stale_token();
    if (!strcmp(sub, "shell-blocked")) return test_shell_blocked();
    if (!strcmp(sub, "completion-once")) return test_completion();
    if (!strcmp(sub, "stress")) {
        uint32_t values[6] = {5000, 1000, 100, 1000, 1000, 100};
        for (int i = 0; i < 6 && i + 2 < argc; i++)
            if (!parse_u32(argv[i + 2], &values[i])) return 1;
        return test_stress(values[0], values[1], values[2], values[3],
                           values[4], values[5]);
    }
    if (!strcmp(sub, "all")) {
        if (test_open_close(32) || test_simple("false-token") ||
            test_stale_token() || test_simple("wrong-owner") ||
            test_simple("input-token") || test_shell_blocked() ||
            test_simple("second-session") || test_failure("alloc-fail") ||
            test_failure("create-fail") || test_failure("router-begin-fail") ||
            test_failure("router-end-fail") || test_owner_exit() ||
            test_kill_owner() || test_kill_caller() ||
            test_kill_before_begin() || test_reservation_gap() ||
            test_idempotent_owner() || test_snapshot_race(1000) ||
            test_concurrent_callers(100) || test_check())
            return 1;
        serial_write_all("[MODALTEST][ALL] PASS\n");
        return 0;
    }
    serial_write_all("modaltest: unknown subcommand\n");
    return 1;
}

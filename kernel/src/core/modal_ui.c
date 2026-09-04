#include "modal_ui.h"
#include "task_lifecycle.h"
#include "semaphore.h"
#include "clock.h"
#include "panic.h"
#include "../drivers/timer.h"
#include "../memory/heap.h"
#include "../libc/memory.h"
#include "../drivers/serial.h"

#define MODAL_UI_CANARY 0x4D4F44414C554931ULL
#define MODAL_UI_WAIT_MS 30000u

typedef struct modal_ui_context {
    uint64_t canary_begin;
    semaphore_t completion;
    semaphore_t test_caller_cancel_consumed;
    char name[32];
    modal_ui_entry_fn entry;
    modal_ui_cleanup_fn ui_cleanup;
    void *user_ctx;
    task_handle_t caller, worker;
    modal_session_token_t token;
    volatile uint8_t context_state;
    volatile uint8_t worker_published;
    volatile uint8_t begin_completed, begin_succeeded;
    volatile uint8_t telemetry_published;
    volatile uint8_t entry_started, entry_completed;
    volatile uint8_t cleanup_started, cleanup_completed;
    volatile uint8_t completion_claimed, completion_signaled;
    volatile uint8_t caller_cancelled, caller_blocked_observed;
    volatile uint8_t caller_before_wait_observed;
    volatile uint8_t lifecycle_recovered;
    volatile uint8_t test_skip_cleanup_close, test_kill_self;
    volatile uint8_t test_kill_before_begin, test_hold_after_completion;
    volatile uint8_t test_hold_caller_before_wait, test_allow_worker_before_wait;
    int entry_result;
    modal_ui_run_status_t begin_failure_status;
    modal_ui_run_status_t status;
    task_kill_result_t worker_kill_result;
    task_exit_reason_t worker_exit_reason;
    uint64_t canary_end;
} modal_ui_context_t;

typedef struct {
    spinlock_t lock;
    modal_ui_context_t *active_context;
    modal_ui_stats_t stats;
    modal_ui_test_run_snapshot_t last_run;
} modal_ui_runtime_t;

static modal_ui_runtime_t g_ui;
static volatile uint8_t g_fail_alloc, g_fail_create, g_skip_close, g_kill_next;
static volatile uint8_t g_kill_before_begin, g_hold_after_completion;
static volatile uint8_t g_release_after_completion;
static volatile uint8_t g_hold_caller_before_wait, g_release_caller_before_wait;
static volatile uint8_t g_caller_before_wait_observed, g_allow_worker_before_wait;
static volatile uint8_t g_hold_worker_for_reap;

static bool ctx_ok(const modal_ui_context_t *c)
{
    return c && c->canary_begin == MODAL_UI_CANARY &&
           c->canary_end == MODAL_UI_CANARY;
}

static bool handle_eq(task_handle_t a, task_handle_t b)
{
    return a.id == b.id &&
           a.lifecycle_generation == b.lifecycle_generation;
}

static void name_copy(char *dst, const char *src)
{
    uint32_t i = 0;
    if (src)
        for (; i < 31 && src[i]; i++)
            dst[i] = src[i];
    dst[i] = 0;
}

static uint32_t append_text(char *buffer, uint32_t offset, const char *text)
{
    while (*text)
        buffer[offset++] = *text++;
    return offset;
}

static uint32_t append_u64(char *buffer, uint32_t offset, uint64_t value)
{
    char digits[21];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        buffer[offset++] = digits[--count];
    return offset;
}

static bool is_taskman_name(const char *name)
{
    static const char expected[] = "taskman-ui";
    uint32_t i = 0;
    while (expected[i] && name[i] == expected[i])
        i++;
    return expected[i] == 0 && name[i] == 0;
}

static void stat_inc(uint64_t *field)
{
    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    (*field)++;
    spin_unlock_irqrestore(&g_ui.lock, flags);
}

static void context_state_store(modal_ui_context_t *c,
                                modal_ui_context_state_t state)
{
    __atomic_store_n(&c->context_state, (uint8_t)state, __ATOMIC_RELEASE);
}

static bool complete_once(modal_ui_context_t *c,
                          modal_ui_run_status_t status,
                          int entry_result)
{
    uint8_t expected = 0;
    if (!__atomic_compare_exchange_n(&c->completion_claimed, &expected, 1,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        stat_inc(&g_ui.stats.completion_duplicates);
        return false;
    }
    c->status = status;
    c->entry_result = entry_result;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&c->completion_signaled, 1, __ATOMIC_RELEASE);
    stat_inc(&g_ui.stats.completion_signals);
    sem_signal(&c->completion);
    return true;
}

static bool worker_exited(task_handle_t handle)
{
    task_snapshot_t snapshot;
    return !scheduler_snapshot_task_by_handle(handle, &snapshot) ||
           snapshot.state == TASK_ZOMBIE;
}

static modal_ui_run_status_t cleanup_status(modal_ui_context_t *c,
                                             bool entry_done)
{
    task_snapshot_t snapshot;
    if (scheduler_snapshot_task_by_handle(c->worker, &snapshot)) {
        c->worker_exit_reason = snapshot.exit_reason;
        if (snapshot.exit_reason == TASK_EXIT_KILLED || snapshot.kill_pending)
            return MODAL_UI_RUN_OWNER_KILLED;
    }
    if (!__atomic_load_n(&c->begin_succeeded, __ATOMIC_ACQUIRE))
        return c->begin_failure_status;
    if (!entry_done)
        return MODAL_UI_RUN_INTERNAL_ERROR;
    return c->entry_result ? MODAL_UI_RUN_ENTRY_ERROR : MODAL_UI_RUN_OK;
}

static void worker_cleanup(void *arg)
{
    modal_ui_context_t *c = arg;
    if (!ctx_ok(c))
        kpanic("MODAL_UI: cleanup context corrupt");
    uint8_t expected = 0;
    if (!__atomic_compare_exchange_n(&c->cleanup_started, &expected, 1,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    context_state_store(c, MODAL_UI_CONTEXT_COMPLETING);
    bool entry_done = __atomic_load_n(&c->entry_completed,
                                      __ATOMIC_ACQUIRE) != 0;
    if (c->ui_cleanup)
        c->ui_cleanup(c->user_ctx, c->entry_result, entry_done);

    bool closed = false;
    if (modal_session_token_is_valid(c->token) &&
        !c->test_skip_cleanup_close) {
        modal_end_result_t result = modal_session_end(c->token);
        closed = result == MODAL_END_OK ||
                 result == MODAL_END_ALREADY_CLOSED;
        if (!closed)
            closed = modal_session_recover_owner(c->worker,
                                                  MODAL_RECOVERY_CLEANUP);
        if (closed)
            stat_inc(&g_ui.stats.cleanup_closes);
        else
            kpanic("MODAL_UI: unrecoverable session");
    }
    __atomic_store_n(&c->cleanup_completed, 1, __ATOMIC_RELEASE);
    if (c->test_skip_cleanup_close)
        return;

    modal_ui_run_status_t status = cleanup_status(c, entry_done);
    stat_inc(status == MODAL_UI_RUN_OWNER_KILLED ?
             &g_ui.stats.killed_completions :
             &g_ui.stats.normal_completions);
    (void)complete_once(c, status, c->entry_result);

    if (c->test_hold_after_completion)
        while (!__atomic_load_n(&g_release_after_completion,
                                __ATOMIC_ACQUIRE))
            schedule_voluntary();
}

typedef enum {
    MODAL_CALLER_WAITING = 0, MODAL_CALLER_BLOCKED,
    MODAL_CALLER_CANCEL_PENDING, MODAL_CALLER_EXITING, MODAL_CALLER_GONE
} modal_caller_wait_state_t;

static modal_caller_wait_state_t observe_caller(modal_ui_context_t *c)
{
    task_snapshot_t snapshot;
    if (!scheduler_snapshot_task_by_handle(c->caller, &snapshot)) return MODAL_CALLER_GONE;
    if (snapshot.exit_started) return MODAL_CALLER_EXITING;
    if (snapshot.kill_pending) return MODAL_CALLER_CANCEL_PENDING;
    if (snapshot.state == TASK_BLOCKED && snapshot.wait_active &&
        snapshot.wait_kind == TASK_WAIT_SEMAPHORE &&
        snapshot.wait_object_key == (uintptr_t)&c->completion) return MODAL_CALLER_BLOCKED;
    return MODAL_CALLER_WAITING;
}

static void worker_entry(void *arg)
{
    modal_ui_context_t *c = arg;
    while (!__atomic_load_n(&c->worker_published, __ATOMIC_ACQUIRE))
        schedule_voluntary();
    task_handle_t self;
    if (!ctx_ok(c) || !scheduler_current_task_handle(&self) ||
        !handle_eq(self, c->worker))
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    bool reserved = g_ui.active_context == c &&
        c->context_state == MODAL_UI_CONTEXT_WORKER_CREATED;
    if (reserved)
        context_state_store(c, MODAL_UI_CONTEXT_WAITING_CALLER);
    spin_unlock_irqrestore(&g_ui.lock, flags);
    if (!reserved)
        return;

    if (c->test_kill_before_begin) {
        (void)scheduler_request_kill(c->worker.id);
        task_cancel_point();
    }

    modal_begin_result_t begin = modal_session_begin(c->name, &c->token);
    __atomic_store_n(&c->begin_completed, 1, __ATOMIC_RELEASE);
    if (begin != MODAL_BEGIN_OK) {
        c->begin_failure_status = begin == MODAL_BEGIN_BUSY ?
            MODAL_UI_RUN_BUSY : MODAL_UI_RUN_BEGIN_FAILED;
        stat_inc(&g_ui.stats.begin_failures);
        return;
    }
    __atomic_store_n(&c->begin_succeeded, 1, __ATOMIC_RELEASE);
    stat_inc(&g_ui.stats.begins);

    uint64_t deadline = clock_monotonic_ns() +
        (uint64_t)MODAL_UI_WAIT_MS * 1000000ULL;
    modal_caller_wait_state_t caller_state = observe_caller(c);
    while (caller_state == MODAL_CALLER_WAITING && !c->test_allow_worker_before_wait &&
           clock_monotonic_ns() < deadline) {
        task_cancel_point();
        timer_sleep(1);
        caller_state = observe_caller(c);
    }
    if (caller_state == MODAL_CALLER_CANCEL_PENDING &&
        c->test_hold_caller_before_wait &&
        !c->test_allow_worker_before_wait) {
        task_wait_result_t test_wait = sem_wait_interruptible(
            &c->test_caller_cancel_consumed);
        if (test_wait == TASK_WAIT_RESULT_CANCELLED)
            task_cancel_point();
        caller_state = observe_caller(c);
    }
    if (caller_state == MODAL_CALLER_CANCEL_PENDING || caller_state == MODAL_CALLER_EXITING ||
        caller_state == MODAL_CALLER_GONE) {
        (void)modal_session_recover_owner(c->worker, MODAL_RECOVERY_CLEANUP);
        c->worker_kill_result = scheduler_request_kill(c->worker.id);
        task_cancel_point();
    }
    if (caller_state != MODAL_CALLER_BLOCKED && !c->test_allow_worker_before_wait) {
        (void)modal_session_recover_owner(c->worker,
                                          MODAL_RECOVERY_CLEANUP);
        return;
    }
    if (caller_state == MODAL_CALLER_BLOCKED)
        __atomic_store_n(&c->caller_blocked_observed, 1, __ATOMIC_RELEASE);
    context_state_store(c, MODAL_UI_CONTEXT_RUNNING);
    if (is_taskman_name(c->name)) {
        char line[112];
        uint32_t length = append_text(line, 0,
            "[TASKMAN][OWNER] shell_id=");
        length = append_u64(line, length, c->caller.id);
        length = append_text(line, length, " ui_id=");
        length = append_u64(line, length, c->worker.id);
        length = append_text(line, length, " distinct=1\n");
        line[length] = 0;
        serial_write_all(line);
        serial_write_all(
            "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE\n");
    }
    __atomic_store_n(&c->telemetry_published, 1, __ATOMIC_RELEASE);
    if (c->test_kill_self) {
        (void)scheduler_request_kill(c->worker.id);
        task_cancel_point();
    }
    __atomic_store_n(&c->entry_started, 1, __ATOMIC_RELEASE);
    c->entry_result = c->entry(c->token, c->user_ctx);
    __atomic_store_n(&c->entry_completed, 1, __ATOMIC_RELEASE);
}

static void lifecycle_listener(const task_lifecycle_exit_event_t *event,
                               void *unused)
{
    (void)unused;
    modal_ui_context_t *c = NULL;
    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    if (g_ui.active_context &&
        g_ui.active_context->worker.id == event->task_id &&
        g_ui.active_context->worker.lifecycle_generation ==
            event->lifecycle_generation)
        c = g_ui.active_context;
    spin_unlock_irqrestore(&g_ui.lock, flags);
    if (!c || !ctx_ok(c) ||
        __atomic_load_n(&c->completion_signaled, __ATOMIC_ACQUIRE))
        return;
#ifndef HOBBYOS_MODAL_NEGATIVE_DISABLE_OWNER_RECOVERY
    bool recovered = modal_session_recover_owner(
        c->worker, MODAL_RECOVERY_LIFECYCLE);
    if (recovered) {
        __atomic_store_n(&c->lifecycle_recovered, 1, __ATOMIC_RELEASE);
        stat_inc(&g_ui.stats.lifecycle_recoveries);
        (void)complete_once(c, MODAL_UI_RUN_RECOVERED_OWNER_DEATH,
                            c->entry_result);
    }
#else
    modal_session_snapshot_t snapshot;
    if (modal_session_snapshot(&snapshot) &&
        snapshot.state == MODAL_SESSION_ACTIVE &&
        handle_eq(snapshot.owner, c->worker)) {
        serial_write_all("[MODALTEST][NEGATIVE] OWNER_RECOVERY_MISSING_DETECTED\n");
        if (modal_session_recover_owner(c->worker, MODAL_RECOVERY_TEST)) {
            __atomic_store_n(&c->lifecycle_recovered, 1, __ATOMIC_RELEASE);
            (void)complete_once(c, MODAL_UI_RUN_RECOVERED_OWNER_DEATH,
                                c->entry_result);
        }
    }
#endif
}

bool modal_ui_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    spinlock_init(&g_ui.lock);
    modal_ui_test_reset_controls();
    return task_lifecycle_register_exit_listener(lifecycle_listener, NULL);
}

void modal_ui_test_fail_next_context_allocation(void)
{ __atomic_store_n(&g_fail_alloc, 1, __ATOMIC_RELEASE); }
void modal_ui_test_fail_next_worker_creation(void)
{ __atomic_store_n(&g_fail_create, 1, __ATOMIC_RELEASE); }
void modal_ui_test_skip_next_cleanup_close(void)
{ __atomic_store_n(&g_skip_close, 1, __ATOMIC_RELEASE); }
void modal_ui_test_kill_next_worker(void)
{ __atomic_store_n(&g_kill_next, 1, __ATOMIC_RELEASE); }
void modal_ui_test_kill_next_worker_before_begin(void)
{ __atomic_store_n(&g_kill_before_begin, 1, __ATOMIC_RELEASE); }
void modal_ui_test_hold_next_worker_after_completion(void)
{
    __atomic_store_n(&g_release_after_completion, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_after_completion, 1, __ATOMIC_RELEASE);
}
void modal_ui_test_release_worker_after_completion(void)
{ __atomic_store_n(&g_release_after_completion, 1, __ATOMIC_RELEASE); }
void modal_ui_test_hold_next_caller_before_wait(void)
{
    __atomic_store_n(&g_caller_before_wait_observed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release_caller_before_wait, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_caller_before_wait, 1, __ATOMIC_RELEASE);
}
void modal_ui_test_release_caller_before_wait(void)
{ __atomic_store_n(&g_release_caller_before_wait, 1, __ATOMIC_RELEASE); }
bool modal_ui_test_caller_before_wait_observed(void)
{ return __atomic_load_n(&g_caller_before_wait_observed, __ATOMIC_ACQUIRE) != 0; }
void modal_ui_test_allow_next_worker_before_caller_wait(void)
{ __atomic_store_n(&g_allow_worker_before_wait, 1, __ATOMIC_RELEASE); }
void modal_ui_test_hold_next_worker_for_reap(void)
{ __atomic_store_n(&g_hold_worker_for_reap, 1, __ATOMIC_RELEASE); }

void modal_ui_test_reset_controls(void)
{
    __atomic_store_n(&g_fail_alloc, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fail_create, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_skip_close, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kill_next, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kill_before_begin, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_after_completion, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release_after_completion, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_caller_before_wait, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release_caller_before_wait, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_caller_before_wait_observed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_allow_worker_before_wait, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_worker_for_reap, 0, __ATOMIC_RELEASE);
}

bool modal_ui_test_controls_snapshot(modal_ui_test_controls_snapshot_t *out)
{
    if (!out)
        return false;
    out->fail_alloc = __atomic_load_n(&g_fail_alloc, __ATOMIC_ACQUIRE) != 0;
    out->fail_create = __atomic_load_n(&g_fail_create, __ATOMIC_ACQUIRE) != 0;
    out->skip_close = __atomic_load_n(&g_skip_close, __ATOMIC_ACQUIRE) != 0;
    out->kill_next = __atomic_load_n(&g_kill_next, __ATOMIC_ACQUIRE) != 0;
    out->kill_before_begin = __atomic_load_n(&g_kill_before_begin,
                                              __ATOMIC_ACQUIRE) != 0;
    out->hold_after_completion = __atomic_load_n(
        &g_hold_after_completion, __ATOMIC_ACQUIRE) != 0;
    out->release_after_completion = __atomic_load_n(
        &g_release_after_completion, __ATOMIC_ACQUIRE) != 0;
    out->hold_caller_before_wait = __atomic_load_n(
        &g_hold_caller_before_wait, __ATOMIC_ACQUIRE) != 0;
    out->release_caller_before_wait = __atomic_load_n(
        &g_release_caller_before_wait, __ATOMIC_ACQUIRE) != 0;
    out->caller_before_wait_observed = __atomic_load_n(
        &g_caller_before_wait_observed, __ATOMIC_ACQUIRE) != 0;
    out->allow_worker_before_wait = __atomic_load_n(
        &g_allow_worker_before_wait, __ATOMIC_ACQUIRE) != 0;
    out->hold_worker_for_reap = __atomic_load_n(
        &g_hold_worker_for_reap, __ATOMIC_ACQUIRE) != 0;
    out->armed = out->fail_alloc || out->fail_create || out->skip_close ||
        out->kill_next || out->kill_before_begin ||
        out->hold_after_completion || out->hold_caller_before_wait ||
        out->caller_before_wait_observed || out->allow_worker_before_wait ||
        out->hold_worker_for_reap;
    return true;
}

bool modal_ui_test_last_run_snapshot(modal_ui_test_run_snapshot_t *out)
{
    if (!out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    *out = g_ui.last_run;
    spin_unlock_irqrestore(&g_ui.lock, flags);
    return out->generation != 0;
}

static void wait_worker_or_panic(modal_ui_context_t *c)
{
    uint64_t deadline = clock_monotonic_ns() +
        (uint64_t)MODAL_UI_WAIT_MS * 1000000ULL;
    while (!worker_exited(c->worker) && clock_monotonic_ns() < deadline)
        schedule_voluntary();
    if (worker_exited(c->worker))
        return;
    (void)scheduler_request_kill(c->worker.id);
    (void)modal_session_recover_owner(c->worker, MODAL_RECOVERY_CLEANUP);
    deadline = clock_monotonic_ns() +
        (uint64_t)MODAL_UI_WAIT_MS * 1000000ULL;
    while (!worker_exited(c->worker) && clock_monotonic_ns() < deadline)
        schedule_voluntary();
    if (!worker_exited(c->worker)) {
        irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
        context_state_store(c, MODAL_UI_CONTEXT_QUARANTINED);
        g_ui.stats.contexts_quarantined++;
        spin_unlock_irqrestore(&g_ui.lock, flags);
        kpanic("MODAL_UI: unresolved worker lifetime");
    }
}

modal_ui_run_status_t modal_ui_run_sync(const char *name,
    modal_ui_entry_fn entry, modal_ui_cleanup_fn cleanup, void *user,
    modal_ui_run_result_t *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (!entry || !out)
        return MODAL_UI_RUN_INTERNAL_ERROR;
    if (__atomic_exchange_n(&g_fail_alloc, 0, __ATOMIC_ACQ_REL)) {
        stat_inc(&g_ui.stats.context_alloc_failures);
        return MODAL_UI_RUN_CONTEXT_ALLOC_FAILED;
    }
    modal_ui_context_t *c = kmalloc(sizeof(*c));
    if (!c) {
        stat_inc(&g_ui.stats.context_alloc_failures);
        return MODAL_UI_RUN_CONTEXT_ALLOC_FAILED;
    }
    memset(c, 0, sizeof(*c));
    c->canary_begin = c->canary_end = MODAL_UI_CANARY;
    c->status = MODAL_UI_RUN_INTERNAL_ERROR;
    c->begin_failure_status = MODAL_UI_RUN_INTERNAL_ERROR;
    c->worker_kill_result = TASK_KILL_ERR_INVALID;
    sem_init(&c->completion, 0);
    sem_init(&c->test_caller_cancel_consumed, 0);
    name_copy(c->name, name);
    c->entry = entry;
    c->ui_cleanup = cleanup;
    c->user_ctx = user;
    c->test_skip_cleanup_close = __atomic_exchange_n(&g_skip_close, 0,
                                                      __ATOMIC_ACQ_REL);
    c->test_kill_self = __atomic_exchange_n(&g_kill_next, 0,
                                             __ATOMIC_ACQ_REL);
    c->test_kill_before_begin = __atomic_exchange_n(&g_kill_before_begin, 0,
                                                     __ATOMIC_ACQ_REL);
    c->test_hold_after_completion = __atomic_exchange_n(
        &g_hold_after_completion, 0, __ATOMIC_ACQ_REL);
    c->test_hold_caller_before_wait = __atomic_exchange_n(
        &g_hold_caller_before_wait, 0, __ATOMIC_ACQ_REL);
    c->test_allow_worker_before_wait = __atomic_exchange_n(
        &g_allow_worker_before_wait, 0, __ATOMIC_ACQ_REL);
    if (!scheduler_current_task_handle(&c->caller)) {
        kfree(c);
        return MODAL_UI_RUN_INTERNAL_ERROR;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    g_ui.stats.runs++;
    if (g_ui.active_context) {
        g_ui.stats.second_session_rejections++;
        spin_unlock_irqrestore(&g_ui.lock, flags);
        kfree(c);
        return MODAL_UI_RUN_BUSY;
    }
    g_ui.active_context = c;
    g_ui.stats.contexts_live++;
    context_state_store(c, MODAL_UI_CONTEXT_RESERVED);
    spin_unlock_irqrestore(&g_ui.lock, flags);

    task_create_options_t options = {
        .name = name,
        .task_class = TASK_CLASS_INTERACTIVE,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .cleanup_fn = worker_cleanup,
        .cleanup_ctx = c,
        .test_reap_hold = __atomic_exchange_n(&g_hold_worker_for_reap,0,__ATOMIC_ACQ_REL)
    };
    bool created = !__atomic_exchange_n(&g_fail_create, 0,
                                         __ATOMIC_ACQ_REL) &&
        thread_create_ex_handle(worker_entry, c, &options, &c->worker);
    if (!created) {
        flags = spin_lock_irqsave(&g_ui.lock);
        if (g_ui.active_context == c)
            g_ui.active_context = NULL;
        g_ui.stats.worker_create_failures++;
        g_ui.stats.contexts_live--;
        context_state_store(c, MODAL_UI_CONTEXT_DONE);
        spin_unlock_irqrestore(&g_ui.lock, flags);
        memset(c, 0xDD, sizeof(*c));
        kfree(c);
        return MODAL_UI_RUN_WORKER_CREATE_FAILED;
    }
    flags = spin_lock_irqsave(&g_ui.lock);
    context_state_store(c, MODAL_UI_CONTEXT_WORKER_CREATED);
    g_ui.stats.workers_created++;
    spin_unlock_irqrestore(&g_ui.lock, flags);
    __atomic_store_n(&c->worker_published, 1, __ATOMIC_RELEASE);

    if (c->test_hold_caller_before_wait) {
        __atomic_store_n(&c->caller_before_wait_observed, 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_caller_before_wait_observed, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_release_caller_before_wait, __ATOMIC_ACQUIRE))
            schedule_voluntary();
    }

    bool completion_ready_before_wait = __atomic_load_n(
        &c->completion_signaled, __ATOMIC_ACQUIRE) != 0;
    task_wait_result_t wait = sem_wait_interruptible(&c->completion);
    if (c->test_hold_caller_before_wait &&
        !c->test_allow_worker_before_wait)
        (void)sem_signal(&c->test_caller_cancel_consumed);
    bool caller_cancelled = wait == TASK_WAIT_RESULT_CANCELLED ||
                            task_cancel_requested();
    if (caller_cancelled) {
        __atomic_store_n(&c->caller_cancelled, 1, __ATOMIC_RELEASE);
        if (!c->test_hold_caller_before_wait ||
            c->test_allow_worker_before_wait)
            c->worker_kill_result = scheduler_request_kill(c->worker.id);
        (void)modal_session_recover_owner(c->worker,
                                          MODAL_RECOVERY_CLEANUP);
    } else if (wait != TASK_WAIT_RESULT_OK) {
        (void)scheduler_request_kill(c->worker.id);
        (void)modal_session_recover_owner(c->worker,
                                          MODAL_RECOVERY_CLEANUP);
    }
    wait_worker_or_panic(c);
    if (!ctx_ok(c))
        kpanic("MODAL_UI: caller context corrupt");

    modal_session_snapshot_t session;
    (void)modal_session_snapshot(&session);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    out->status = c->status;
    out->entry_result = c->entry_result;
    out->caller = c->caller;
    out->worker = c->worker;
    out->worker_distinct = !handle_eq(c->caller, c->worker);
    out->cleanup_completed = __atomic_load_n(&c->cleanup_completed,
                                             __ATOMIC_ACQUIRE);
    out->caller_blocked_observed = __atomic_load_n(
        &c->caller_blocked_observed, __ATOMIC_ACQUIRE);
    out->session_recovered =
        __atomic_load_n(&c->lifecycle_recovered, __ATOMIC_ACQUIRE) ||
        session.state == MODAL_SESSION_INACTIVE;

    bool cleanup_completed = __atomic_load_n(&c->cleanup_completed,
                                              __ATOMIC_ACQUIRE);
    bool caller_blocked_observed = __atomic_load_n(
        &c->caller_blocked_observed, __ATOMIC_ACQUIRE);
    bool caller_before_wait_observed = __atomic_load_n(
        &c->caller_before_wait_observed, __ATOMIC_ACQUIRE);
    bool caller_cancel_latched = __atomic_load_n(
        &c->caller_cancelled, __ATOMIC_ACQUIRE);
    flags=spin_lock_irqsave(&g_ui.lock);
    g_ui.last_run.generation++;
    g_ui.last_run.caller = c->caller;
    g_ui.last_run.worker = c->worker;
    g_ui.last_run.caller_wait_result = wait;
    g_ui.last_run.caller_cancel_latched = caller_cancel_latched;
    g_ui.last_run.caller_before_wait_observed = caller_before_wait_observed;
    g_ui.last_run.caller_blocked_observed = caller_blocked_observed;
    g_ui.last_run.worker_before_wait_allowed =
        c->test_allow_worker_before_wait;
    g_ui.last_run.completion_ready_before_wait =
        completion_ready_before_wait;
    g_ui.last_run.worker_kill_result = c->worker_kill_result;
    g_ui.last_run.worker_exit_reason = c->worker_exit_reason;
    g_ui.last_run.status = c->status;
    g_ui.last_run.cleanup_completed = cleanup_completed;
    g_ui.last_run.session_recovered = out->session_recovered;
    spin_unlock_irqrestore(&g_ui.lock, flags);

    flags = spin_lock_irqsave(&g_ui.lock);
    if (g_ui.active_context != c)
        kpanic("MODAL_UI: reservation lost");
    g_ui.active_context = NULL;
    g_ui.stats.contexts_live--;
    context_state_store(c, MODAL_UI_CONTEXT_DONE);
    spin_unlock_irqrestore(&g_ui.lock, flags);
    modal_ui_run_status_t status = out->status;
    memset(c, 0xDD, sizeof(*c));
    kfree(c);
    if (caller_cancelled)
        task_cancel_point();
    if (wait != TASK_WAIT_RESULT_OK)
        return MODAL_UI_RUN_INTERNAL_ERROR;
    return status;
}

void modal_ui_stats_snapshot(modal_ui_stats_t *out)
{
    if (!out)
        return;
    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    *out = g_ui.stats;
    out->active = g_ui.active_context != NULL;
    spin_unlock_irqrestore(&g_ui.lock, flags);
}

bool modal_ui_runtime_snapshot(modal_ui_runtime_snapshot_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    irq_flags_t flags = spin_lock_irqsave(&g_ui.lock);
    modal_ui_context_t *c = g_ui.active_context;
    out->active = c != NULL;
    out->contexts_live = g_ui.stats.contexts_live;
    out->contexts_quarantined = g_ui.stats.contexts_quarantined;
    if (c) {
        if (!ctx_ok(c)) {
            spin_unlock_irqrestore(&g_ui.lock, flags);
            return false;
        }
        out->state = (modal_ui_context_state_t)__atomic_load_n(
            &c->context_state, __ATOMIC_ACQUIRE);
        out->caller = c->caller;
        out->worker = c->worker;
        out->completion_claimed = __atomic_load_n(
            &c->completion_claimed, __ATOMIC_ACQUIRE);
        out->completion_signaled = __atomic_load_n(
            &c->completion_signaled, __ATOMIC_ACQUIRE);
        out->cleanup_completed = __atomic_load_n(
            &c->cleanup_completed, __ATOMIC_ACQUIRE);
        out->lifecycle_recovered = __atomic_load_n(
            &c->lifecycle_recovered, __ATOMIC_ACQUIRE);
        out->caller_blocked_observed = __atomic_load_n(
            &c->caller_blocked_observed, __ATOMIC_ACQUIRE);
    }
    spin_unlock_irqrestore(&g_ui.lock, flags);
    return true;
}

bool modal_ui_validate(uint64_t *out_violations)
{
    modal_ui_runtime_snapshot_t runtime;
    modal_session_snapshot_t session;
    uint64_t violations = 0;
    if (!modal_ui_runtime_snapshot(&runtime) ||
        !modal_session_snapshot(&session))
        violations++;
    if (runtime.active != (runtime.contexts_live != 0))
        violations++;
    if (runtime.contexts_quarantined)
        violations++;
    if (runtime.completion_signaled && !runtime.completion_claimed)
        violations++;
    if (runtime.active && runtime.state == MODAL_UI_CONTEXT_DONE)
        violations++;
    if (session.state == MODAL_SESSION_ACTIVE &&
        (!runtime.active || !handle_eq(session.owner, runtime.worker)))
        violations++;
    if (out_violations)
        *out_violations = violations;
    return violations == 0;
}

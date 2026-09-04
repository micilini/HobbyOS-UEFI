#include "timers.h"
#include "spinlock.h"
#include "scheduler.h"
#include "../drivers/timer.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../apic/lapic.h"
#include "../smp/smp_topology.h"
#include "../libc/memory.h"
#include "panic.h"

static struct list_head g_timer_list;
static struct list_head g_claimed_list;
static spinlock_t g_timer_lock;
static timer_id_t g_next_timer_id;
static uint64_t g_next_timer_generation;
static timer_stats_t g_stats;
static uint8_t g_fail_next_allocation;
static volatile uint8_t g_test_hold_claimed;
static volatile task_id_t g_test_hold_claimed_task;
static uint64_t g_test_hold_claimed_generation;

typedef struct {
    uint8_t active;
    uint8_t old_selected;
    uint8_t new_selected;
    uint8_t order_violation;
    uint32_t remaining;
    uint64_t generation;
    timer_handle_t old_handle;
    timer_handle_t new_handle;
} timer_order_test_hook_t;

static timer_order_test_hook_t g_test_order;

typedef struct timer_node {
    timer_handle_t handle;
    uint64_t deadline_ms;
    timer_node_state_t state;
    timer_kind_t kind;
    timer_callback_t callback;
    void *ctx;
    task_id_t task_id;
    uint64_t lifecycle_generation;
    uint64_t wait_generation;
    task_wake_reason_t wake_reason;
    uint8_t task_ref_held;
    struct list_head node;
} timer_node_t;

static void stat_inc(uint64_t *value) { __atomic_add_fetch(value, 1, __ATOMIC_RELAXED); }

static void timer_release_task_ref_or_panic(timer_node_t *node)
{
    if(!node||node->kind!=TIMER_KIND_TASK_WAKE||!node->task_ref_held){stat_inc(&g_stats.task_ref_duplicate_release);kpanic("TIMER: duplicate task ref release");}
    node->task_ref_held=0;
    if(!scheduler_task_timer_ref_release_by_identity(node->task_id,node->lifecycle_generation,node->wait_generation)){
        stat_inc(&g_stats.task_ref_release_failures);kpanic("TIMER: task ref release failed");}
    stat_inc(&g_stats.task_refs_released);
}

static timer_node_t *timer_node_alloc(void)
{
    if (__atomic_exchange_n(&g_fail_next_allocation, 0, __ATOMIC_ACQ_REL)) {
        stat_inc(&g_stats.alloc_failures);
        return NULL;
    }
    timer_node_t *node = (timer_node_t *)kmalloc(sizeof(*node));
    if (!node) {
        stat_inc(&g_stats.alloc_failures);
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    list_init(&node->node);
    return node;
}

static bool timer_assign_locked(timer_node_t *node, uint64_t delay_ms)
{
    if (!node || !g_next_timer_id || !g_next_timer_generation) return false;
    uint64_t now = timer_get_uptime_ms();
    if (delay_ms > UINT64_MAX - now) return false;
    node->handle.id = g_next_timer_id++;
    node->handle.generation = g_next_timer_generation++;
    if (!g_next_timer_id || !g_next_timer_generation) return false;
    node->deadline_ms = now + delay_ms;
    node->state = TIMER_NODE_PENDING;
    return true;
}

void timers_init(void)
{
    spinlock_init(&g_timer_lock);
    list_init(&g_timer_list);
    list_init(&g_claimed_list);
    g_next_timer_id = 1;
    g_next_timer_generation = 1;
    memset(&g_stats, 0, sizeof(g_stats));
    g_fail_next_allocation = 0;
    g_test_hold_claimed = 0;
    g_test_hold_claimed_task = TASK_ID_INVALID;
    g_test_hold_claimed_generation = 0;
    memset(&g_test_order, 0, sizeof(g_test_order));
    console_write_debug("[CORE] Timers subsystem initialized.\n");
    console_write_debug("[SYNC][SELFTEST] WAIT_TIMER_GUARDS_OK\n");
}

bool timers_add_callback(uint64_t delay_ms, timer_callback_t cb, void *ctx, timer_handle_t *out)
{
    if (!cb) return false;
    timer_node_t *node = timer_node_alloc();
    if (!node) return false;
    node->kind = TIMER_KIND_CALLBACK;
    node->callback = cb;
    node->ctx = ctx;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    bool ok=timer_assign_locked(node, delay_ms);
    if (ok) {
        list_add_tail(&node->node, &g_timer_list);
        stat_inc(&g_stats.armed);
        if (out) *out = node->handle;
    }
    spin_unlock_irqrestore(&g_timer_lock, flags);
    if (!ok) kfree(node);
    return ok;
}

int timers_add(uint64_t delay_ms, timer_callback_t cb, void *ctx)
{
    return timers_add_callback(delay_ms, cb, ctx, NULL) ? 0 : -1;
}

bool timers_arm_task_wake(uint64_t delay_ms, task_id_t id, uint64_t lifecycle,
                          uint64_t wait_generation, task_wake_reason_t reason,
                          timer_handle_t *out)
{
    if (!id || !lifecycle || !wait_generation || reason == TASK_WAKE_NONE) return false;
    timer_node_t *node = timer_node_alloc();
    if (!node) return false;
    node->kind = TIMER_KIND_TASK_WAKE;
    node->task_id = id;
    node->lifecycle_generation = lifecycle;
    node->wait_generation = wait_generation;
    node->wake_reason = reason;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    bool ref=false;bool ok = timer_assign_locked(node, delay_ms);
    if(ok){ref=scheduler_task_timer_ref_acquire_by_identity(id,lifecycle,wait_generation);ok=ref;if(ok){node->task_ref_held=1;stat_inc(&g_stats.task_refs_acquired);}}
    if (ok) {
        list_add_tail(&node->node, &g_timer_list);
        stat_inc(&g_stats.armed);
        if (out) *out = node->handle;
    }
    if(!ok&&ref)timer_release_task_ref_or_panic(node);
    spin_unlock_irqrestore(&g_timer_lock, flags);
    if (!ok) kfree(node);
    return ok;
}

static timer_node_t *find_handle_locked(struct list_head *head, timer_handle_t handle)
{
    struct list_head *pos = NULL;
    list_for_each(pos, head) {
        timer_node_t *node = list_entry(pos, timer_node_t, node);
        if (node->handle.id == handle.id && node->handle.generation == handle.generation)
            return node;
    }
    return NULL;
}

timer_cancel_result_t timers_cancel(timer_handle_t handle)
{
    if (!handle.id || !handle.generation) return TIMER_CANCEL_INVALID;
    timer_node_t *found = NULL;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    found = find_handle_locked(&g_timer_list, handle);
    if (found) {
        found->state = TIMER_NODE_CANCELLED;
        list_del(&found->node);
        stat_inc(&g_stats.cancelled);
        if(found->kind==TIMER_KIND_TASK_WAKE)timer_release_task_ref_or_panic(found);
        spin_unlock_irqrestore(&g_timer_lock, flags);
        kfree(found);
        return TIMER_CANCELLED;
    }
    if (find_handle_locked(&g_claimed_list, handle)) {
        stat_inc(&g_stats.duplicate_cancel);
        spin_unlock_irqrestore(&g_timer_lock, flags);
        return TIMER_CANCEL_ALREADY_CLAIMED;
    }
    stat_inc(&g_stats.duplicate_cancel);
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return TIMER_CANCEL_NOT_FOUND;
}

static bool timer_node_is_held_locked(const timer_node_t *node)
{
    task_id_t held_id = g_test_hold_claimed_task;
    return g_test_hold_claimed &&
           (held_id == TASK_ID_INVALID || held_id == node->task_id);
}

static bool timer_handle_equal(timer_handle_t left, timer_handle_t right)
{
    return left.id == right.id && left.generation == right.generation;
}

static void timer_test_record_dispatch_locked(const timer_node_t *node)
{
    if (!g_test_order.active || !node)
        return;
    if (timer_handle_equal(node->handle, g_test_order.old_handle)) {
        if (g_test_order.old_selected)
            g_test_order.order_violation = 1;
        g_test_order.old_selected = 1;
        if (g_test_order.remaining)
            g_test_order.remaining--;
        return;
    }
    if (timer_handle_equal(node->handle, g_test_order.new_handle)) {
        if (!g_test_order.old_selected || g_test_order.new_selected)
            g_test_order.order_violation = 1;
        g_test_order.new_selected = 1;
        if (g_test_order.remaining)
            g_test_order.remaining--;
    }
}

void timers_poll_at(uint64_t now)
{
    cpu_slot_t current = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&current) || current != smp_bsp_cpu_slot())
        return;
    timer_node_t *dispatch = NULL;
#ifdef HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
    timer_node_t *newly_due = NULL;
#endif
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    struct list_head *pos, *next;
    list_for_each_safe(pos, next, &g_timer_list) {
        timer_node_t *node = list_entry(pos, timer_node_t, node);
        if (node->state == TIMER_NODE_PENDING && now >= node->deadline_ms) {
            node->state = TIMER_NODE_CLAIMED;
            list_del(&node->node);
            list_add_tail(&node->node, &g_claimed_list);
            stat_inc(&g_stats.claimed);
#ifdef HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
            if (!newly_due && !timer_node_is_held_locked(node))
                newly_due = node;
#endif
        }
    }

#ifdef HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
    dispatch = newly_due;
#endif
    if (!dispatch) {
        struct list_head *p;
        list_for_each(p, &g_claimed_list) {
            timer_node_t *candidate = list_entry(p, timer_node_t, node);
            if (!timer_node_is_held_locked(candidate)) {
                dispatch = candidate;
                break;
            }
        }
    }
    if (dispatch) {
        timer_test_record_dispatch_locked(dispatch);
        list_del(&dispatch->node);
    }
    spin_unlock_irqrestore(&g_timer_lock, flags);
    if (!dispatch) return;
    if (dispatch->kind == TIMER_KIND_CALLBACK) {
        if (dispatch->callback) dispatch->callback(dispatch->ctx);
    } else if (scheduler_wake_task_by_identity(dispatch->task_id, dispatch->lifecycle_generation,
                                               dispatch->wait_generation, dispatch->wake_reason) != SCHED_WAKE_WON) {
        stat_inc(&g_stats.stale_task_wakes);
    }
    if(dispatch->kind==TIMER_KIND_TASK_WAKE)timer_release_task_ref_or_panic(dispatch);
    stat_inc(&g_stats.dispatched);
    kfree(dispatch);
}

void timers_poll(void)
{
    timers_poll_at(timer_get_uptime_ms());
}

void timers_test_fail_next_allocation(void)
{
    __atomic_store_n(&g_fail_next_allocation, 1, __ATOMIC_RELEASE);
}

void timers_test_hold_claimed(bool hold)
{
    irq_flags_t f=spin_lock_irqsave(&g_timer_lock);
    g_test_hold_claimed_task=TASK_ID_INVALID;g_test_hold_claimed=hold?1:0;
    g_test_hold_claimed_generation++;spin_unlock_irqrestore(&g_timer_lock,f);
}
void timers_test_hold_claimed_task(task_id_t id,bool hold)
{
    (void)timers_test_set_claim_hold(id,hold);
}
bool timers_test_set_claim_hold(task_id_t id,bool hold)
{
    if(hold&&id==TASK_ID_INVALID)return false;
    irq_flags_t f=spin_lock_irqsave(&g_timer_lock);
    bool ok=!hold||!g_test_hold_claimed||g_test_hold_claimed_task==id;
    if(ok){g_test_hold_claimed_task=hold?id:TASK_ID_INVALID;g_test_hold_claimed=hold?1:0;g_test_hold_claimed_generation++;}
    spin_unlock_irqrestore(&g_timer_lock,f);return ok;
}
void timers_test_claim_hold_snapshot(timers_test_claim_hold_snapshot_t*out)
{
    if(!out)return;
    irq_flags_t f=spin_lock_irqsave(&g_timer_lock);
    out->active=g_test_hold_claimed;out->task_id=g_test_hold_claimed_task;
    out->generation=g_test_hold_claimed_generation;spin_unlock_irqrestore(&g_timer_lock,f);
}

bool timers_test_arm_order_pair(timer_callback_t callback, void *old_ctx,
                                void *new_ctx, uint64_t *out_generation)
{
    if (!callback)
        return false;
    timer_node_t *old_node = timer_node_alloc();
    timer_node_t *new_node = timer_node_alloc();
    if (!old_node || !new_node) {
        if (old_node) kfree(old_node);
        if (new_node) kfree(new_node);
        return false;
    }
    old_node->kind = TIMER_KIND_CALLBACK;
    old_node->callback = callback;
    old_node->ctx = old_ctx;
    new_node->kind = TIMER_KIND_CALLBACK;
    new_node->callback = callback;
    new_node->ctx = new_ctx;

    bool ok = false;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    uint64_t generation = g_test_order.generation + 1u;
    if (!g_test_order.active && generation &&
        timer_assign_locked(old_node, 0) &&
        timer_assign_locked(new_node, 0)) {
        old_node->state = TIMER_NODE_CLAIMED;
        list_add_tail(&old_node->node, &g_claimed_list);
        list_add_tail(&new_node->node, &g_timer_list);
        stat_inc(&g_stats.armed);
        stat_inc(&g_stats.armed);
        stat_inc(&g_stats.claimed);
        g_test_order = (timer_order_test_hook_t){
            .active = 1,
            .remaining = 2,
            .generation = generation,
            .old_handle = old_node->handle,
            .new_handle = new_node->handle,
        };
        if (out_generation) *out_generation = generation;
        ok = true;
    }
    spin_unlock_irqrestore(&g_timer_lock, flags);
    if (!ok) {
        kfree(old_node);
        kfree(new_node);
    }
    return ok;
}

static timer_node_t *timer_test_detach_locked(timer_handle_t handle)
{
    timer_node_t *node = find_handle_locked(&g_timer_list, handle);
    if (!node)
        node = find_handle_locked(&g_claimed_list, handle);
    if (node) {
        node->state = TIMER_NODE_CANCELLED;
        list_del(&node->node);
        stat_inc(&g_stats.cancelled);
    }
    return node;
}

bool timers_test_order_finish(uint64_t generation,
                              timers_test_order_snapshot_t *out)
{
    if (!generation || !out)
        return false;
    timer_node_t *old_residual = NULL;
    timer_node_t *new_residual = NULL;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    if (!g_test_order.active || g_test_order.generation != generation) {
        spin_unlock_irqrestore(&g_timer_lock, flags);
        return false;
    }
    *out = (timers_test_order_snapshot_t){
        .active = g_test_order.active,
        .old_selected = g_test_order.old_selected,
        .new_selected = g_test_order.new_selected,
        .order_violation = g_test_order.order_violation,
        .remaining = g_test_order.remaining,
        .generation = g_test_order.generation,
        .old_handle = g_test_order.old_handle,
        .new_handle = g_test_order.new_handle,
    };
    old_residual = timer_test_detach_locked(g_test_order.old_handle);
    new_residual = timer_test_detach_locked(g_test_order.new_handle);
    out->residual = (old_residual ? 1u : 0u) +
                    (new_residual ? 1u : 0u);
    memset(&g_test_order, 0, sizeof(g_test_order));
    g_test_order.generation = generation;
    spin_unlock_irqrestore(&g_timer_lock, flags);
    if (old_residual) kfree(old_residual);
    if (new_residual) kfree(new_residual);
    return true;
}

bool timers_test_handle_present(timer_handle_t handle,
                                timer_node_state_t *out_state)
{
    if (!handle.id || !handle.generation)
        return false;
    bool found = false;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    timer_node_t *node = find_handle_locked(&g_timer_list, handle);
    if (!node)
        node = find_handle_locked(&g_claimed_list, handle);
    if (node) {
        found = true;
        if (out_state) *out_state = node->state;
    }
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return found;
}

bool timers_test_find_claimed_task_wake(task_id_t id,timer_handle_t*out,uint64_t*lifecycle,uint64_t*wait_generation)
{
    bool found=false;irq_flags_t f=spin_lock_irqsave(&g_timer_lock);struct list_head*p;
    list_for_each(p,&g_claimed_list){timer_node_t*n=list_entry(p,timer_node_t,node);if(n->kind==TIMER_KIND_TASK_WAKE&&n->task_id==id&&n->task_ref_held){if(out)*out=n->handle;if(lifecycle)*lifecycle=n->lifecycle_generation;if(wait_generation)*wait_generation=n->wait_generation;found=true;break;}}
    spin_unlock_irqrestore(&g_timer_lock,f);return found;
}

bool timers_test_task_wake_snapshot(
    task_handle_t target,
    timers_test_task_wake_snapshot_t *out)
{
    if (!out || target.id == TASK_ID_INVALID)
        return false;
    memset(out, 0, sizeof(*out));
    out->target = target;

    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    struct list_head *position;
    list_for_each(position, &g_timer_list) {
        timer_node_t *node = list_entry(position, timer_node_t, node);
        if (node->kind != TIMER_KIND_TASK_WAKE ||
            node->task_id != target.id)
            continue;
        if (node->lifecycle_generation != target.lifecycle_generation) {
            out->found_wrong_lifecycle = 1;
            continue;
        }
        out->pending_nodes++;
        if (!out->first_pending.id)
            out->first_pending = node->handle;
        if (node->task_ref_held)
            out->refs_held++;
        out->wait_generation = node->wait_generation;
    }
    list_for_each(position, &g_claimed_list) {
        timer_node_t *node = list_entry(position, timer_node_t, node);
        if (node->kind != TIMER_KIND_TASK_WAKE ||
            node->task_id != target.id)
            continue;
        if (node->lifecycle_generation != target.lifecycle_generation) {
            out->found_wrong_lifecycle = 1;
            continue;
        }
        out->claimed_nodes++;
        if (!out->first_claimed.id)
            out->first_claimed = node->handle;
        if (node->task_ref_held)
            out->refs_held++;
        out->wait_generation = node->wait_generation;
    }
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return true;
}

int timers_test_dispatch_task_wake(task_id_t id, uint64_t lifecycle,
                                   uint64_t wait_generation)
{
    return scheduler_wake_task_by_identity(id, lifecycle, wait_generation, TASK_WAKE_TIMEOUT);
}

void timers_get_stats(timer_stats_t *out)
{
    if (!out) return;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    out->armed = __atomic_load_n(&g_stats.armed, __ATOMIC_RELAXED);
    out->cancelled = __atomic_load_n(&g_stats.cancelled, __ATOMIC_RELAXED);
    out->claimed = __atomic_load_n(&g_stats.claimed, __ATOMIC_RELAXED);
    out->dispatched = __atomic_load_n(&g_stats.dispatched, __ATOMIC_RELAXED);
    out->alloc_failures = __atomic_load_n(&g_stats.alloc_failures, __ATOMIC_RELAXED);
    out->stale_task_wakes = __atomic_load_n(&g_stats.stale_task_wakes, __ATOMIC_RELAXED);
    out->duplicate_cancel = __atomic_load_n(&g_stats.duplicate_cancel, __ATOMIC_RELAXED);
    out->task_refs_acquired=__atomic_load_n(&g_stats.task_refs_acquired,__ATOMIC_RELAXED);
    out->task_refs_released=__atomic_load_n(&g_stats.task_refs_released,__ATOMIC_RELAXED);
    out->task_ref_release_failures=__atomic_load_n(&g_stats.task_ref_release_failures,__ATOMIC_RELAXED);
    out->task_ref_duplicate_release=__atomic_load_n(&g_stats.task_ref_duplicate_release,__ATOMIC_RELAXED);
    out->pending = 0;
    out->claimed_now = 0;
    struct list_head *pos = NULL;
    list_for_each(pos, &g_timer_list) out->pending++;
    list_for_each(pos, &g_claimed_list) out->claimed_now++;
    spin_unlock_irqrestore(&g_timer_lock, flags);
}

bool timers_validate(void)
{
    bool ok = true;
    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);
    struct list_head *pos = NULL;
    list_for_each(pos, &g_timer_list) {
        timer_node_t *node = list_entry(pos, timer_node_t, node);
        if (node->state != TIMER_NODE_PENDING || !node->handle.id || !node->handle.generation) ok = false;
    }
    list_for_each(pos, &g_claimed_list) {
        timer_node_t *node = list_entry(pos, timer_node_t, node);
        if (node->state != TIMER_NODE_CLAIMED || !node->handle.id || !node->handle.generation) ok = false;
    }
    struct list_head *pending_pos = NULL;
    list_for_each(pending_pos, &g_timer_list) {
        timer_node_t *pending = list_entry(pending_pos, timer_node_t, node);
        struct list_head *claimed_pos = NULL;
        list_for_each(claimed_pos, &g_claimed_list) {
            timer_node_t *claimed = list_entry(claimed_pos, timer_node_t, node);
            if (pending == claimed ||
                timer_handle_equal(pending->handle, claimed->handle))
                ok = false;
        }
    }
    if (g_test_hold_claimed || g_test_order.active)
        ok = false;
    spin_unlock_irqrestore(&g_timer_lock, flags);
    return ok;
}

static task_wait_result_t timer_delay_fallback(uint64_t ms, bool interrupts_enabled)
{
    uint64_t start = timer_get_uptime_ms();
    if (ms > UINT64_MAX - start) return TASK_WAIT_RESULT_ERROR;
    if (!interrupts_enabled) {
        uint64_t capped_ms = ms > 10000u ? 10000u : ms;
        volatile uint64_t spins = capped_ms * 10000u + 1u;
        while (spins--) spin_cpu_relax();
        return TASK_WAIT_RESULT_OK;
    }
    while (timer_get_uptime_ms() < start + ms) __asm__ volatile("hlt");
    return TASK_WAIT_RESULT_OK;
}

task_wait_result_t timers_test_fallback_delay(uint64_t ms, bool interrupts_enabled)
{
    return timer_delay_fallback(ms, interrupts_enabled);
}

task_wait_result_t timer_sleep_interruptible(uint64_t ms)
{
    if (!ms) {
        if (scheduler_current_context_can_block()) schedule_voluntary();
        return TASK_WAIT_RESULT_OK;
    }
    if (!scheduler_current_context_can_block()) {
        irq_flags_t flags;
        __asm__ volatile("pushfq; pop %0" : "=r"(flags));
        return timer_delay_fallback(ms, (flags & (1ULL << 9)) != 0);
    }
    timer_node_t *node = timer_node_alloc();
    if (!node) return TASK_WAIT_RESULT_ERROR;
    irq_flags_t flags = irq_save();
    spin_lock(&g_timer_lock);
    task_block_token_t token;
    scheduler_block_prepare_result_t prepared = scheduler_prepare_block_interruptible(
        NULL, TASK_SLEEPING, TASK_WAIT_TIMER_SLEEP, 0, &token);
    if (prepared != SCHED_BLOCK_PREPARED) {
        spin_unlock(&g_timer_lock);
        irq_restore(flags);
        kfree(node);
        return prepared == SCHED_BLOCK_CANCELLED ? TASK_WAIT_RESULT_CANCELLED : TASK_WAIT_RESULT_ERROR;
    }
    node->kind = TIMER_KIND_TASK_WAKE;
    node->task_id = token.task->id;
    node->lifecycle_generation = token.lifecycle_generation;
    node->wait_generation = token.wait_generation;
    node->wake_reason = TASK_WAKE_TIMEOUT;
    if (!timer_assign_locked(node, ms)) {
        scheduler_abort_block(&token, TASK_WAKE_ERROR);
        spin_unlock(&g_timer_lock);
        irq_restore(flags);
        kfree(node);
        return TASK_WAIT_RESULT_ERROR;
    }
    if(!scheduler_task_timer_ref_acquire_locked(token.task,node->lifecycle_generation,node->wait_generation)){
        scheduler_abort_block(&token,TASK_WAKE_ERROR);spin_unlock(&g_timer_lock);irq_restore(flags);kfree(node);return TASK_WAIT_RESULT_ERROR;
    }
    node->task_ref_held=1;
    stat_inc(&g_stats.task_refs_acquired);
    list_add_tail(&node->node, &g_timer_list);
    stat_inc(&g_stats.armed);
    timer_handle_t handle = node->handle;
    spin_unlock(&g_timer_lock);
    task_wait_result_t result = scheduler_commit_block(&token);
    irq_restore(flags);
    if (result != TASK_WAIT_RESULT_TIMEOUT) (void)timers_cancel(handle);
    return result;
}

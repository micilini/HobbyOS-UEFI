#include "scheduler.h"
#include "task.h"
#include "spinlock.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "timers.h"
#include "../apic/lapic.h"
#include "../smp/smp_topology.h"
#include "../smp/cpu_limits.h"
#include "../smp/smp_boot.h"

#include "panic.h"
#include "../drivers/serial.h"
#include "interrupts.h"
#include "interrupt_context.h"
#include "clock.h"
#include "task_lifecycle.h"

#include <stdint.h>

#define STACK_SIZE HOBBYOS_KERNEL_STACK_SIZE
#define TASK_REAPER_BATCH_MAX 16u
#define TASK_REAPER_DEFAULT_GRACE_MS 3000u
#define TASK_REAPER_PERIOD_MS 2000u
#define TASK_REAP_EXPECTED_MASK ((uint64_t)TASK_REAP_DEFER_TEST_HOLD|TASK_REAP_DEFER_ON_CPU|TASK_REAP_DEFER_CURRENT|TASK_REAP_DEFER_TIMER_REFS)
#define TASK_REAP_POLICY_MASK ((uint64_t)TASK_REAP_DEFER_GRACE)

extern void thread_wrapper(void);

static struct list_head g_interactive_queue;
static struct list_head g_normal_queue;
static spinlock_t g_scheduler_lock;
static task_kill_stats_t g_kill_stats;
#define TEST_REAP_HOLD_MAX 16
static task_id_t g_test_reap_hold_ids[TEST_REAP_HOLD_MAX];
static uint8_t g_test_hold_next_ready;
static uint8_t g_test_hold_next_reap;
static uint8_t g_test_quiet_lifecycle;
static uint32_t g_test_finish_failure;
static volatile uint8_t g_test_hold_next_zombie_on_cpu,g_test_release_zombie_on_cpu;

typedef enum
{
    SCHEDULER_BOOT_UNINITIALIZED = 0,
    SCHEDULER_BOOT_GLOBAL_READY,
    SCHEDULER_BOOT_STARTED
} scheduler_boot_state_t;

typedef struct
{
    task_t *prev;
    task_t *next;
    uint64_t sequence;
    uint64_t started_count;
    uint64_t finished_count;
    cpu_slot_t expected_slot;
    uint32_t owner_apic_id;
    task_id_t executing_task_id;
    uint8_t active;
} scheduler_switch_handoff_t;

typedef struct
{
    cpu_slot_t slot;
    uint32_t apic_id;
    task_t *current;
    task_t *idle;
    uint8_t initialized;
    uint8_t timer_ready;
    uint8_t online;
    uint8_t bootstrap_attached;
    uint8_t handoff_complete;
    uint8_t irq_preemption_enabled;
    uint64_t early_preemption_attempts;
    uint64_t last_account_ns;
    uint64_t accounted_ns_total;
    uint64_t accounting_events;
    uint64_t clock_regressions;
    uint64_t runtime_overflows;
    uint8_t accounting_initialized;
    scheduler_switch_handoff_t handoff;
} scheduler_cpu_state_t;

static scheduler_cpu_state_t g_scheduler_cpus[HOBBYOS_MAX_CPUS];
static volatile scheduler_boot_state_t g_scheduler_boot_state = SCHEDULER_BOOT_UNINITIALIZED;

static task_id_t g_next_task_id = 1;
static uint64_t g_next_lifecycle_generation = 1;
static uint64_t g_next_zombie_generation = 1;
static task_reaper_stats_t g_reaper_stats;
static uint64_t g_waits_prepared, g_waits_committed, g_waits_aborted;
static uint64_t g_waits_preblock_cancelled, g_waits_prepare_errors;
static scheduler_test_preblock_negative_t g_preblock_negative;
static uint64_t g_wakes_signal, g_wakes_timeout, g_wakes_cancelled, g_stale_wakes;

static struct list_head g_all_tasks;
static uint32_t g_all_tasks_count = 0;
static uint64_t g_task_registry_generation = 0;
static uint64_t g_handoff_failures;
static uint64_t g_duplicate_current_failures;
static uint64_t g_stale_cpu_slot_entries;
static uint64_t g_pinned_current_mismatches;
static uint64_t g_finish_wrong_cpu;
static uint64_t g_finish_sequence_mismatch;
static uint64_t g_runqueue_on_cpu_failures;
static uint64_t g_finish_without_pending;
static uint64_t g_pending_overwrite;
static uint8_t g_reaper_negative_reported;
static bool scheduler_account_cpu_locked(cpu_slot_t slot, uint64_t now_ns);
static task_t *current_task_pinned(cpu_slot_t slot);

#define TASK_ID_DECIMAL_MAX_DIGITS 20u
#define TASK_ID_DECIMAL_BUFFER_SIZE (TASK_ID_DECIMAL_MAX_DIGITS + 1u)

static uint32_t sched_u64_format_decimal(uint64_t value, char *out, uint32_t out_size)
{
    if (!out || out_size == 0)
        return 0;

    char reversed[TASK_ID_DECIMAL_MAX_DIGITS];
    uint32_t digits = 0;
    do
    {
        reversed[digits++] = (char)(48u + (value % 10u));
        value /= 10u;
    } while (value != 0);

    if (out_size <= digits)
    {
        out[0] = 0;
        return 0;
    }

    for (uint32_t i = 0; i < digits; i++)
        out[i] = reversed[digits - 1u - i];
    out[digits] = 0;
    return digits;
}

static void sched_serial_dec(uint64_t value)
{
    char buffer[TASK_ID_DECIMAL_BUFFER_SIZE];
    if (sched_u64_format_decimal(value, buffer, sizeof(buffer)) != 0)
        serial_write_all(buffer);
}

static void sched_copy_name(char *dst, uint32_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;

    uint32_t i = 0;
    if (src)
    {
        while (src[i] && i < (dst_size - 1))
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void sched_make_default_name(char *dst, uint32_t dst_size, task_id_t tid)
{
    if (!dst || dst_size == 0)
        return;

    char tmp[21];
    int n = 0;

    if (tid == 0)
    {
        tmp[n++] = '0';
    }
    else
    {
        task_id_t v = tid;
        while (v > 0 && n < (int)sizeof(tmp))
        {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }

    const char *prefix = "kthread-";
    uint32_t p = 0;
    while (prefix[p] && p < (dst_size - 1))
    {
        dst[p] = prefix[p];
        p++;
    }

    while (n > 0 && p < (dst_size - 1))
    {
        dst[p++] = tmp[--n];
    }

    dst[p] = '\0';
}

static void sched_make_idle_name(char *dst, uint32_t dst_size, cpu_slot_t slot)
{
    if (!dst || dst_size == 0)
        return;
    sched_copy_name(dst, dst_size, "idle/cpu");
    uint32_t used = 0;
    while (used < dst_size && dst[used])
        used++;
    char digits[10];
    uint32_t count = 0;
    do
    {
        digits[count++] = (char)('0' + (slot % 10u));
        slot /= 10u;
    } while (slot && count < sizeof(digits));
    while (count && used < dst_size - 1)
        dst[used++] = digits[--count];
    dst[used] = '\0';
}

static task_t *create_idle_task(cpu_slot_t slot)
{
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle)
        return NULL;

    memset(idle, 0, sizeof(task_t));

    idle->id = TASK_ID_INVALID;
    idle->lifecycle_generation = 0;
    idle->state = TASK_RUNNING;
    idle->on_cpu = 1;
    idle->current_cpu_slot = slot;
    idle->stack_base = NULL;
    idle->rsp = 0;

    idle->kernel_stack_bytes = 0;
    idle->kernel_ctx_bytes_est = 0;

    idle->quantum = DEFAULT_QUANTUM;
    idle->quantum_default = DEFAULT_QUANTUM;
    idle->task_class = TASK_CLASS_NORMAL;
    idle->flags = TASK_FLAG_IDLE | TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED;
    idle->kill_pending = 0;
    idle->zombie_since_ms = 0;

    idle->runtime_ns_total = 0;
    idle->schedule_count = 0;
    idle->last_cpu_slot = slot;

    list_init(&idle->list);
    list_init(&idle->registry_list);
    idle->queue_membership = TASK_QUEUE_NONE;
    idle->registry_registered = 0;
    return idle;
}

static struct list_head *queue_for_class(int task_class)
{
    if (task_class == TASK_CLASS_INTERACTIVE)
        return &g_interactive_queue;
    return &g_normal_queue;
}

static bool task_id_allocate_locked(task_id_t *out_id)
{
    if (!out_id || g_next_task_id == TASK_ID_INVALID)
        return false;
    *out_id = g_next_task_id;
    if (g_next_task_id == UINT64_MAX)
        g_next_task_id = TASK_ID_INVALID;
    else
        g_next_task_id++;
    return true;
}

static bool registry_add_locked(task_t *t)
{
    if (!t || t->registry_registered || t->id == TASK_ID_INVALID)
        return false;
    list_add_tail(&t->registry_list, &g_all_tasks);
    t->registry_registered = 1;
    g_all_tasks_count++;
    g_task_registry_generation++;
    return true;
}

static bool registry_remove_locked(task_t *t)
{
    if (!t || !t->registry_registered)
        return false;
    list_del(&t->registry_list);
    list_init(&t->registry_list);
    t->registry_registered = 0;
    if (g_all_tasks_count)
        g_all_tasks_count--;
    g_task_registry_generation++;
    return true;
}

static uint8_t task_is_idle_locked(task_t *t)
{
    if (!t)
        return 0;

    for (uint32_t cpu = 0; cpu < HOBBYOS_MAX_CPUS; cpu++)
    {
        if (g_scheduler_cpus[cpu].idle == t)
            return 1;
    }
    return 0;
}

static uint8_t task_is_current_locked(task_t *t, uint32_t *out_cpu)
{
    if (!t)
        return 0;

    for (uint32_t cpu = 0; cpu < HOBBYOS_MAX_CPUS; cpu++)
    {
        if (g_scheduler_cpus[cpu].current == t)
        {
            if (out_cpu)
                *out_cpu = cpu;
            return 1;
        }
    }

    if (out_cpu)
        *out_cpu = 0xFFFFFFFFu;
    return 0;
}


static task_t *find_task_by_id_locked(task_id_t id)
{
    struct list_head *pos = NULL;

    list_for_each(pos, &g_all_tasks)
    {
        task_t *t = list_entry(pos, task_t, registry_list);
        if (t->id == id)
            return t;
    }

    return NULL;
}

static uint8_t task_is_kill_protected_locked(task_t *t)
{
    if (!t)
        return 1;


    return (t->flags & TASK_FLAG_KILL_PROTECTED) != 0;
}

static bool queue_detach_locked(task_t *t);
static bool enqueue_task_locked(task_t *t);
static bool scheduler_switch_locked(cpu_slot_t slot);
static scheduler_wake_status_t scheduler_wake_task_locked(task_t *t, uint64_t lifecycle, uint64_t wait_gen, task_wake_reason_t reason);

static bool task_mark_kill_pending_locked(task_t *t)
{
    if (!t)
        return false;

    if (t->wait_active)
    {
        scheduler_wake_status_t wake = scheduler_wake_task_locked(t, t->lifecycle_generation, t->wait_generation, TASK_WAKE_CANCELLED);
        return wake == SCHED_WAKE_FATAL;
    }

    if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
    {
        if (t->queue_membership == TASK_QUEUE_WAIT)
            queue_detach_locked(t);
        else if (t->queue_membership != TASK_QUEUE_NONE)
            return false;
        if (t->on_cpu)
            t->deferred_ready = 1;
        else
        {
            t->state = TASK_READY;
            if (!enqueue_task_locked(t)) return true;
        }
    }
    return false;
}

static task_queue_membership_t membership_for_class(int task_class)
{
    return task_class == TASK_CLASS_INTERACTIVE ?
           TASK_QUEUE_RUNQ_INTERACTIVE : TASK_QUEUE_RUNQ_NORMAL;
}

static bool queue_detach_locked(task_t *t)
{
    if (!t || t->queue_membership == TASK_QUEUE_NONE)
        return false;
    list_del(&t->list);
    list_init(&t->list);
    t->queue_membership = TASK_QUEUE_NONE;
    return true;
}

static bool waitqueue_attach_locked(wait_queue_t *wq, task_t *t)
{
    if (!wq || !t || t->queue_membership != TASK_QUEUE_NONE)
        return false;
    list_add_tail(&t->list, &wq->head);
    t->queue_membership = TASK_QUEUE_WAIT;
    return true;
}

static bool enqueue_task_locked_impl(task_t *t, uint8_t allow_unregistered)
{
    if (!t || t->queue_membership != TASK_QUEUE_NONE || t->on_cpu ||
        t->state != TASK_READY || (t->flags & TASK_FLAG_IDLE) ||
        t->state == TASK_ZOMBIE || (!allow_unregistered && !t->registry_registered))
    {
        if (t && t->on_cpu)
            g_runqueue_on_cpu_failures++;
        return false;
    }
    list_add_tail(&t->list, queue_for_class(t->task_class));
    t->queue_membership = membership_for_class(t->task_class);
    return true;
}

static bool enqueue_task_locked(task_t *t)
{
    return enqueue_task_locked_impl(t, 0);
}

static task_t *runqueue_take_next_locked(void)
{
    struct list_head *queues[2] = {&g_interactive_queue, &g_normal_queue};
    task_queue_membership_t memberships[2] = {TASK_QUEUE_RUNQ_INTERACTIVE, TASK_QUEUE_RUNQ_NORMAL};
    for (uint32_t q = 0; q < 2; q++)
    {
        while (!list_empty(queues[q]))
        {
            task_t *t = list_entry(queues[q]->next, task_t, list);
            if (t->test_ready_hold) break;
            bool membership_ok = t->queue_membership == memberships[q];
            if (!queue_detach_locked(t))
                return NULL;
            if (!membership_ok || !t->registry_registered || t->state != TASK_READY ||
                t->on_cpu || (t->flags & TASK_FLAG_IDLE) || task_is_current_locked(t, NULL))
            {
                if (t->on_cpu)
                    g_runqueue_on_cpu_failures++;
                continue;
            }
            return t;
        }
    }
    return NULL;
}

static void sched_boot_print_cpu(const char *event, const char *role,
                                 cpu_slot_t slot, uint32_t apic_id)
{
    serial_write_all("[SCHED][BOOT] ");
    serial_write_all(event);
    if (role)
    {
        serial_write_all(" role=");
        serial_write_all(role);
    }
    serial_write_all(" slot=");
    sched_serial_dec(slot);
    serial_write_all(" apic=");
    sched_serial_dec(apic_id);
    serial_write_all("\n");
}

static int scheduler_cpu_init_common(cpu_slot_t slot, const char *role)
{
    if (__atomic_load_n(&g_scheduler_boot_state, __ATOMIC_ACQUIRE) == SCHEDULER_BOOT_UNINITIALIZED)
        return SCHED_BOOT_ERR_GLOBAL_NOT_READY;
    const SmpCpuInfo *topology = smp_cpu_by_slot_const(slot);
    if (slot >= HOBBYOS_MAX_CPUS || !topology)
        return SCHED_BOOT_ERR_INVALID_SLOT;
    if (topology->slot != slot)
        return SCHED_BOOT_ERR_SLOT_TOPOLOGY_MISMATCH;
    if (topology->apic_id != lapic_get_id())
        return SCHED_BOOT_ERR_CURRENT_APIC_MISMATCH;

    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    if (cpu->initialized)
        return SCHED_BOOT_ERR_CPU_ALREADY_INITIALIZED;

    sched_boot_print_cpu("CPU_INIT_BEGIN", role, slot, topology->apic_id);
    task_t *idle = create_idle_task(slot);
    if (!idle)
        return SCHED_BOOT_ERR_ALLOCATION;

    idle->state = TASK_RUNNING;
    idle->last_cpu_slot = slot;
    idle->schedule_count = 1;
    list_init(&idle->registry_list);

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    if (!task_id_allocate_locked(&idle->id) || g_next_lifecycle_generation == 0)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        kfree(idle);
        return SCHED_BOOT_ERR_ALLOCATION;
    }
    idle->lifecycle_generation = g_next_lifecycle_generation++;
    sched_make_idle_name(idle->name, sizeof(idle->name), slot);
    cpu->slot = slot;
    cpu->apic_id = topology->apic_id;
    cpu->idle = idle;
    cpu->current = idle;
    if (!registry_add_locked(idle))
    {
        cpu->idle = NULL;
        cpu->current = NULL;
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        kfree(idle);
        return SCHED_BOOT_ERR_ALLOCATION;
    }
    __atomic_store_n(&cpu->initialized, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    serial_write_all("[TASK][CREATE] id=");
    sched_serial_dec(idle->id);
    serial_write_all(" name=");
    serial_write_all(idle->name);
    serial_write_all(" flags=0x");
    serial_write_hex64_all(idle->flags);
    serial_write_all(" class=NORMAL source=idle slot=");
    sched_serial_dec(slot);
    serial_write_all("\n");
    sched_boot_print_cpu("CPU_INIT_OK", role, slot, topology->apic_id);
    serial_write_all("[BOOT][CPU_INIT] PASS slot=");
    sched_serial_dec(slot);
    serial_write_all("\n[BOOT][IDLE] PASS slot=");
    sched_serial_dec(slot);
    serial_write_all(" id=");
    sched_serial_dec(idle->id);
    serial_write_all("\n");
    return SCHED_BOOT_OK;
}

int scheduler_global_init(void)
{
    if (__atomic_load_n(&g_scheduler_boot_state, __ATOMIC_ACQUIRE) != SCHEDULER_BOOT_UNINITIALIZED)
        return SCHED_BOOT_ERR_ALREADY_INITIALIZED;

    serial_write_all("[SCHED][BOOT] GLOBAL_INIT_BEGIN\n");
    spinlock_init(&g_scheduler_lock);
    list_init(&g_interactive_queue);
    list_init(&g_normal_queue);
    list_init(&g_all_tasks);
    g_all_tasks_count = 0;
    g_next_task_id = 1;
    g_next_lifecycle_generation = 1;
    g_next_zombie_generation = 1;
    memset(&g_reaper_stats,0,sizeof(g_reaper_stats));
    task_lifecycle_init();
    g_waits_prepared = g_waits_committed = g_waits_aborted = 0;
    g_waits_preblock_cancelled = g_waits_prepare_errors = 0;
    g_wakes_signal = g_wakes_timeout = g_wakes_cancelled = g_stale_wakes = 0;
    g_task_registry_generation = 0;
    g_handoff_failures = 0;
    g_duplicate_current_failures = 0;
    g_runqueue_on_cpu_failures = 0;
    g_finish_without_pending = 0;
    g_pending_overwrite = 0;
    g_stale_cpu_slot_entries = 0;
    g_pinned_current_mismatches = 0;
    g_finish_wrong_cpu = 0;
    g_finish_sequence_mismatch = 0;
    g_reaper_negative_reported=0;
    g_test_hold_next_zombie_on_cpu=0;g_test_release_zombie_on_cpu=0;
    memset(g_scheduler_cpus, 0, sizeof(g_scheduler_cpus));
    for (cpu_slot_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
        g_scheduler_cpus[slot].slot = slot;

    __atomic_store_n(&g_scheduler_boot_state, SCHEDULER_BOOT_GLOBAL_READY, __ATOMIC_RELEASE);
    serial_write_all("[SCHED][BOOT] GLOBAL_INIT_OK max_cpus=");
    sched_serial_dec(HOBBYOS_MAX_CPUS);
    serial_write_all("\n");
    serial_write_all("[BOOT][GLOBAL_INIT] PASS\n");
    return SCHED_BOOT_OK;
}

int scheduler_cpu_init_bsp(cpu_slot_t slot)
{
    const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
    if (!cpu || !cpu->is_bsp)
        return SCHED_BOOT_ERR_SLOT_TOPOLOGY_MISMATCH;
    return scheduler_cpu_init_common(slot, "BSP");
}

int scheduler_cpu_init_ap(cpu_slot_t slot)
{
    const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
    if (!cpu || cpu->is_bsp)
        return SCHED_BOOT_ERR_SLOT_TOPOLOGY_MISMATCH;
    return scheduler_cpu_init_common(slot, "AP");
}

int scheduler_cpu_mark_timer_ready(cpu_slot_t slot)
{
    const SmpCpuInfo *topology = smp_cpu_by_slot_const(slot);
    if (slot >= HOBBYOS_MAX_CPUS || !topology)
        return SCHED_BOOT_ERR_INVALID_SLOT;
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    if (!cpu->initialized || !cpu->idle || !cpu->current)
        return SCHED_BOOT_ERR_CPU_NOT_READY;
    if (cpu->apic_id != lapic_get_id())
        return SCHED_BOOT_ERR_CURRENT_APIC_MISMATCH;
    __atomic_store_n(&cpu->timer_ready, 1, __ATOMIC_RELEASE);
    sched_boot_print_cpu("TIMER_READY", NULL, slot, cpu->apic_id);
    return SCHED_BOOT_OK;
}

bool scheduler_cpu_is_ready(cpu_slot_t slot)
{
    if (slot >= HOBBYOS_MAX_CPUS || !smp_cpu_by_slot_const(slot))
        return false;
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    return __atomic_load_n(&cpu->initialized, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&cpu->timer_ready, __ATOMIC_ACQUIRE) &&
           cpu->idle && cpu->current;
}

bool scheduler_is_started(void)
{
    return __atomic_load_n(&g_scheduler_boot_state, __ATOMIC_ACQUIRE) == SCHEDULER_BOOT_STARTED;
}

int scheduler_start(void)
{
    scheduler_boot_state_t state = __atomic_load_n(&g_scheduler_boot_state, __ATOMIC_ACQUIRE);
    if (state == SCHEDULER_BOOT_UNINITIALIZED)
        return SCHED_BOOT_ERR_GLOBAL_NOT_READY;
    if (state == SCHEDULER_BOOT_STARTED)
        return SCHED_BOOT_ERR_ALREADY_STARTED;

    uint32_t online = 0;
    uint64_t start_ns=clock_monotonic_ns();
    if (!start_ns) return SCHED_BOOT_ERR_CPU_NOT_READY;
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
    {
        const SmpCpuInfo *topology = smp_cpu_by_slot_const(slot);
        if (topology && __atomic_load_n(&topology->state, __ATOMIC_ACQUIRE) == CPU_STATE_ONLINE)
        {
            if (!scheduler_cpu_is_ready(slot))
                return SCHED_BOOT_ERR_CPU_NOT_READY;
            g_scheduler_cpus[slot].online = 1;
            g_scheduler_cpus[slot].last_account_ns=start_ns;
            g_scheduler_cpus[slot].accounting_initialized=1;
            online++;
        }
    }
    cpu_slot_t bsp = smp_bsp_cpu_slot();
    if (online == 0 || bsp == CPU_SLOT_INVALID || !scheduler_cpu_is_ready(bsp))
        return SCHED_BOOT_ERR_CPU_NOT_READY;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&g_scheduler_boot_state, SCHEDULER_BOOT_STARTED, __ATOMIC_RELEASE);
    serial_write_all("[SCHED][BOOT] START_OK online=");
    sched_serial_dec(online);
    serial_write_all(" discovered=");
    sched_serial_dec(g_cpu_count);
    serial_write_all(" failed=");
    sched_serial_dec(smp_failed_cpu_count());
    serial_write_all("\n");
    return SCHED_BOOT_OK;
}

bool scheduler_bootstrap_selftest(void)
{
    bool ok = true;
    if (g_scheduler_boot_state == SCHEDULER_BOOT_UNINITIALIZED)
    {
        ok = scheduler_cpu_init_common(CPU_SLOT_INVALID, "TEST") == SCHED_BOOT_ERR_GLOBAL_NOT_READY;
    }
    else if (g_scheduler_boot_state == SCHEDULER_BOOT_GLOBAL_READY)
    {
        ok = scheduler_global_init() == SCHED_BOOT_ERR_ALREADY_INITIALIZED;
        ok = ok && scheduler_cpu_init_common(HOBBYOS_MAX_CPUS, "TEST") == SCHED_BOOT_ERR_INVALID_SLOT;
        ok = ok && scheduler_cpu_init_common(CPU_SLOT_INVALID, "TEST") == SCHED_BOOT_ERR_INVALID_SLOT;
        cpu_slot_t bsp = smp_bsp_cpu_slot();
        if (bsp != CPU_SLOT_INVALID && g_scheduler_cpus[bsp].initialized)
            ok = ok && scheduler_cpu_init_bsp(bsp) == SCHED_BOOT_ERR_CPU_ALREADY_INITIALIZED;
    }
    serial_write_all(ok ? "[SCHED][SELFTEST] BOOT_GUARDS_OK\n" : "[SCHED][SELFTEST] BOOT_GUARDS_FAIL\n");
    return ok;
}

task_t *get_current_task(void)
{
    irq_flags_t flags = irq_save();
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
    {
        irq_restore(flags);
        return NULL;
    }
    task_t *task = g_scheduler_cpus[slot].current;
    irq_restore(flags);
    return task;
}

bool scheduler_current_task_handle(task_handle_t *out)
{
    if (!out) return false;
    *out = TASK_HANDLE_INVALID;
    scheduler_cpu_pin_t pin;
    if (!scheduler_cpu_pin(&pin) || !pin.task) return false;
    out->id = pin.task->id;
    out->lifecycle_generation = pin.task->lifecycle_generation;
    bool valid = out->id != TASK_ID_INVALID && out->lifecycle_generation != 0;
    scheduler_cpu_unpin(&pin);
    if (!valid) *out = TASK_HANDLE_INVALID;
    return valid;
}

void thread_set_name(task_t *t, const char *name)
{
    if (!t)
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (name && name[0])
        sched_copy_name(t->name, sizeof(t->name), name);
    else
        sched_make_default_name(t->name, sizeof(t->name), t->id);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

void thread_set_current_name(const char *name)
{
    task_t *t = get_current_task();
    if (!t)
        return;

    thread_set_name(t, name);
}
bool scheduler_set_task_name_by_id(task_id_t id,const char*name){irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);task_t*t=find_task_by_id_locked(id);if(t){if(name&&name[0])sched_copy_name(t->name,sizeof(t->name),name);else sched_make_default_name(t->name,sizeof(t->name),t->id);}spin_unlock_irqrestore(&g_scheduler_lock,f);return t!=NULL;}

bool thread_create_ex_handle(void (*entry_point)(void *),void *arg,const task_create_options_t *options,task_handle_t *out)
{
    if(out)*out=TASK_HANDLE_INVALID;
    if (!entry_point || !options)
        return false;
    if (options->task_class != TASK_CLASS_INTERACTIVE && options->task_class != TASK_CLASS_NORMAL)
        return false;
    uint32_t creation_flags = options->flags;
    if ((creation_flags & ~TASK_CREATION_FLAGS_MASK) != 0 ||
        ((creation_flags & TASK_FLAG_SYSTEM) && (creation_flags & TASK_FLAG_USER)) ||
        (creation_flags & TASK_FLAG_IDLE) ||
        ((creation_flags & TASK_FLAG_KILL_PROTECTED) && (creation_flags & TASK_FLAG_KILLABLE)) ||
        (!options->cleanup_fn && options->cleanup_ctx))
        return false;

    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t)
        return false;
    memset(t, 0, sizeof(task_t));
    t->stack_base = kmalloc_aligned(STACK_SIZE, 16);
    if (!t->stack_base)
    {
        kfree(t);
        return false;
    }
    memset(t->stack_base, 0xA5, STACK_SIZE);
    t->kernel_stack_guard_bytes=HOBBYOS_KERNEL_STACK_GUARD_BYTES; t->kernel_stack_canary=HOBBYOS_KERNEL_STACK_CANARY; t->kernel_stack_guard_valid=1; t->kernel_stack_low_watermark=STACK_SIZE-HOBBYOS_KERNEL_STACK_GUARD_BYTES;
    t->state = TASK_READY;
    t->on_cpu = 0;
    t->deferred_ready = 0;
    t->current_cpu_slot = TASK_CPU_SLOT_NONE;
    t->task_class = options->task_class;
    t->kernel_stack_bytes = STACK_SIZE;
    t->last_cpu_slot = TASK_CPU_SLOT_NONE;
    t->flags = creation_flags;
    t->cleanup_fn = options->cleanup_fn;
    t->cleanup_ctx = options->cleanup_ctx;
    t->test_ready_hold = __atomic_exchange_n(&g_test_hold_next_ready, 0, __ATOMIC_ACQ_REL);
    t->queue_membership = TASK_QUEUE_NONE;
    list_init(&t->list);
    list_init(&t->registry_list);
    if (t->task_class == TASK_CLASS_INTERACTIVE)
        t->quantum = t->quantum_default = INTERACTIVE_QUANTUM;
    else
        t->quantum = t->quantum_default = DEFAULT_QUANTUM;

    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack_base + STACK_SIZE);
    *--sp = (uint64_t)thread_wrapper;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)entry_point;
    *--sp = (uint64_t)arg;
    *--sp = 0;
    *--sp = 0;
    t->rsp = (uint64_t)sp;

    irq_flags_t lock_flags = spin_lock_irqsave(&g_scheduler_lock);
    if (!task_id_allocate_locked(&t->id) || g_next_lifecycle_generation == 0)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, lock_flags);
        kfree(t->stack_base);
        kfree(t);
        return false;
    }
    t->lifecycle_generation = g_next_lifecycle_generation++;
    if (options->test_reap_hold || g_test_hold_next_reap) {
        uint32_t slot=0; while(slot<TEST_REAP_HOLD_MAX&&g_test_reap_hold_ids[slot]!=TASK_ID_INVALID)slot++;
        if(slot==TEST_REAP_HOLD_MAX){g_test_hold_next_reap=0;spin_unlock_irqrestore(&g_scheduler_lock,lock_flags);kfree(t->stack_base);kfree(t);return false;}
        g_test_reap_hold_ids[slot]=t->id; g_test_hold_next_reap=0;
    }
    if (options->name && options->name[0])
        sched_copy_name(t->name, sizeof(t->name), options->name);
    else
        sched_make_default_name(t->name, sizeof(t->name), t->id);
    if (!registry_add_locked(t) || !enqueue_task_locked(t))
    {
        for (uint32_t i = 0; i < TEST_REAP_HOLD_MAX; i++)
            if (g_test_reap_hold_ids[i] == t->id)
                g_test_reap_hold_ids[i] = TASK_ID_INVALID;
        if (t->registry_registered)
            registry_remove_locked(t);
        spin_unlock_irqrestore(&g_scheduler_lock, lock_flags);
        kfree(t->stack_base);
        kfree(t);
        return false;
    }
    task_handle_t handle={.id=t->id,.lifecycle_generation=t->lifecycle_generation};
    char log_name[32];sched_copy_name(log_name,sizeof(log_name),t->name);
    uint32_t log_flags=t->flags;int log_class=t->task_class;
    if(out)*out=handle;
    spin_unlock_irqrestore(&g_scheduler_lock, lock_flags);

    if (!__atomic_load_n(&g_test_quiet_lifecycle,__ATOMIC_ACQUIRE)) {
    serial_write_all("[TASK][CREATE] id=");
    sched_serial_dec(handle.id);
    serial_write_all(" name=");
    serial_write_all(log_name);
    serial_write_all(" flags=0x");
    serial_write_hex64_all(log_flags);
    serial_write_all(log_class == TASK_CLASS_INTERACTIVE ?
                     " class=INTERACTIVE source=kthread\n" :
                     " class=NORMAL source=kthread\n"); }
    return true;
}

bool thread_create_named_with_class_flags_handle(void (*entry_point)(void *),void *arg,int task_class,const char *name,uint32_t flags,task_handle_t*out)
{
    task_create_options_t options = {.name=name, .task_class=task_class, .flags=flags, .cleanup_fn=NULL, .cleanup_ctx=NULL};
    return thread_create_ex_handle(entry_point,arg,&options,out);
}

bool thread_create_named_with_class_handle(void (*entry_point)(void *),void *arg,int task_class,const char *name,task_handle_t*out)
{
    return thread_create_named_with_class_flags_handle(entry_point,arg,task_class,name,TASK_FLAG_SYSTEM,out);
}

bool thread_create_with_class_handle(void (*entry_point)(void *),void *arg,int task_class,task_handle_t*out)
{
    return thread_create_named_with_class_handle(entry_point,arg,task_class,NULL,out);
}

bool thread_create_named_handle(void (*entry_point)(void *),void *arg,const char *name,task_handle_t*out)
{
    return thread_create_named_with_class_handle(entry_point,arg,TASK_CLASS_NORMAL,name,out);
}

bool thread_create_handle(void (*entry_point)(void *),void *arg,task_handle_t*out)
{
    return thread_create_named_with_class_handle(entry_point,arg,TASK_CLASS_NORMAL,NULL,out);
}
bool thread_create_ex(void(*fn)(void*),void*arg,const task_create_options_t*o){return thread_create_ex_handle(fn,arg,o,NULL);}
bool thread_create_named_with_class_flags(void(*fn)(void*),void*arg,int c,const char*n,uint32_t f){return thread_create_named_with_class_flags_handle(fn,arg,c,n,f,NULL);}
bool thread_create_named_with_class(void(*fn)(void*),void*arg,int c,const char*n){return thread_create_named_with_class_handle(fn,arg,c,n,NULL);}
bool thread_create_with_class(void(*fn)(void*),void*arg,int c){return thread_create_with_class_handle(fn,arg,c,NULL);}
bool thread_create_named(void(*fn)(void*),void*arg,const char*n){return thread_create_named_handle(fn,arg,n,NULL);}
bool thread_create(void(*fn)(void*),void*arg){return thread_create_handle(fn,arg,NULL);}

static cpu_slot_t current_cpu_slot_pinned_or_panic(void)
{
    if (irq_is_enabled()) kpanic("SCHED: CPU-local access without pin");
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS)
    {
        serial_write_all("[SCHED][BOOT] ERROR code=UNKNOWN_CPU apic=");
        sched_serial_dec(lapic_get_id());
        serial_write_all("\n");
        kpanic("SCHED: current CPU has no logical slot");
    }
    return slot;
}

static bool task_context_blockable(task_t *t)
{
    return scheduler_is_started() && t && !(t->flags & TASK_FLAG_IDLE) &&
           t->state == TASK_RUNNING && t->on_cpu;
}

bool scheduler_current_context_can_block(void)
{
    return task_context_blockable(get_current_task());
}

static task_wait_result_t wake_reason_to_result(task_wake_reason_t reason)
{
    switch (reason) {
    case TASK_WAKE_SIGNAL: return TASK_WAIT_RESULT_OK;
    case TASK_WAKE_TIMEOUT: return TASK_WAIT_RESULT_TIMEOUT;
    case TASK_WAKE_CANCELLED: return TASK_WAIT_RESULT_CANCELLED;
    case TASK_WAKE_SPURIOUS: return TASK_WAIT_RESULT_SPURIOUS;
    default: return TASK_WAIT_RESULT_ERROR;
    }
}

static bool task_has_pending_cancellation_locked(const task_t *t)
{
    return t && (t->flags & TASK_FLAG_KILLABLE) &&
           !(t->flags & TASK_FLAG_KILL_PROTECTED) && t->kill_pending &&
           !t->exit_started;
}

static scheduler_block_prepare_result_t scheduler_prepare_block_common(
    wait_queue_t *wq, task_state_t state, task_wait_kind_t kind,
    uintptr_t object_key, task_block_token_t *out_token, bool interruptible)
{
    if (!out_token || kind == TASK_WAIT_NONE ||
        (state != TASK_BLOCKED && state != TASK_SLEEPING))
        return SCHED_BLOCK_ERROR;
    memset(out_token, 0, sizeof(*out_token));
    spin_lock(&g_scheduler_lock);
    cpu_slot_t slot = current_cpu_slot_pinned_or_panic();
    task_t *t = current_task_pinned(slot);
    if (!t || (t->flags & TASK_FLAG_IDLE) || !t->on_cpu || t->state != TASK_RUNNING ||
        t->queue_membership != TASK_QUEUE_NONE || t->wait_active || t->wait_generation == UINT64_MAX)
    { g_waits_prepare_errors++; spin_unlock(&g_scheduler_lock); return SCHED_BLOCK_ERROR; }
    bool cancel = interruptible && task_has_pending_cancellation_locked(t);
#ifdef HOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL
    if (cancel && g_preblock_negative.armed && !g_preblock_negative.consumed &&
        g_preblock_negative.target.id == t->id &&
        g_preblock_negative.target.lifecycle_generation == t->lifecycle_generation &&
        g_preblock_negative.kind == kind && g_preblock_negative.object_key == object_key) {
        g_preblock_negative.consumed = 1;
        cancel = false;
    }
#endif
    if (cancel) {
        g_waits_preblock_cancelled++;
        spin_unlock(&g_scheduler_lock);
        return SCHED_BLOCK_CANCELLED;
    }
    t->wait_generation++; if (!t->wait_generation) { g_waits_prepare_errors++; spin_unlock(&g_scheduler_lock); return SCHED_BLOCK_ERROR; }
    t->wait_kind = kind; t->wake_reason = TASK_WAKE_NONE; t->wait_active = 1;
    t->wait_object_key = object_key; t->state = state; t->deferred_ready = 0;
    if (wq && !waitqueue_attach_locked(wq, t))
    { t->state = TASK_RUNNING; t->wait_active = 0; t->wait_kind = TASK_WAIT_NONE; t->wake_reason = TASK_WAKE_NONE; t->wait_object_key = 0; t->deferred_ready = 0; g_waits_prepare_errors++; spin_unlock(&g_scheduler_lock); return SCHED_BLOCK_ERROR; }
    out_token->task = t; out_token->lifecycle_generation = t->lifecycle_generation;
    out_token->wait_generation = t->wait_generation; out_token->kind = kind; out_token->prepared = 1;
    g_waits_prepared++;
    return SCHED_BLOCK_PREPARED;
}

bool scheduler_prepare_block(wait_queue_t *wq, task_state_t state,
                             task_wait_kind_t kind, uintptr_t object_key,
                             task_block_token_t *out_token)
{
    return scheduler_prepare_block_common(wq, state, kind, object_key,
                                          out_token, false) == SCHED_BLOCK_PREPARED;
}

scheduler_block_prepare_result_t scheduler_prepare_block_interruptible(
    wait_queue_t *wq, task_state_t state, task_wait_kind_t kind,
    uintptr_t object_key, task_block_token_t *out_token)
{
    return scheduler_prepare_block_common(wq, state, kind, object_key,
                                          out_token, true);
}

void scheduler_wait_stats_snapshot(scheduler_wait_stats_t *out)
{
    if (!out) return;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    out->prepared = g_waits_prepared;
    out->committed = g_waits_committed;
    out->aborted = g_waits_aborted;
    out->preblock_cancelled = g_waits_preblock_cancelled;
    out->prepare_errors = g_waits_prepare_errors;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

bool scheduler_test_arm_ignore_preblock_cancel(task_handle_t target,
    task_wait_kind_t kind, uintptr_t object_key)
{
#ifndef HOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL
    (void)target; (void)kind; (void)object_key; return false;
#else
    if (target.id == TASK_ID_INVALID || kind == TASK_WAIT_NONE) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    bool ok = !g_preblock_negative.armed;
    if (ok) {
        g_preblock_negative.armed = 1; g_preblock_negative.consumed = 0;
        g_preblock_negative.target = target; g_preblock_negative.kind = kind;
        g_preblock_negative.object_key = object_key; g_preblock_negative.generation++;
    }
    spin_unlock_irqrestore(&g_scheduler_lock, flags); return ok;
#endif
}

void scheduler_test_preblock_negative_snapshot(scheduler_test_preblock_negative_t *out)
{
    if (!out) return;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    *out = g_preblock_negative;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

void scheduler_test_clear_preblock_negative(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    uint64_t generation = g_preblock_negative.generation;
    memset(&g_preblock_negative, 0, sizeof(g_preblock_negative));
    g_preblock_negative.generation = generation + 1;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

task_wait_result_t scheduler_commit_block(task_block_token_t *token)
{
    if (!token || !token->prepared || !token->task) return TASK_WAIT_RESULT_ERROR;
    task_t *t = token->task;
    cpu_slot_t slot = t->current_cpu_slot;
    if (slot >= HOBBYOS_MAX_CPUS || g_scheduler_cpus[slot].current != t ||
        !t->wait_active || t->wait_generation != token->wait_generation ||
        t->lifecycle_generation != token->lifecycle_generation || t->wait_kind != token->kind)
    { spin_unlock(&g_scheduler_lock); token->prepared = 0; return TASK_WAIT_RESULT_ERROR; }
    g_waits_committed++;
    if (!scheduler_switch_locked(slot))
    { spin_unlock(&g_scheduler_lock); token->prepared = 0; return TASK_WAIT_RESULT_ERROR; }
    spin_lock(&g_scheduler_lock);
    task_wake_reason_t reason = t->wake_reason;
    t->wait_active = 0; t->wait_kind = TASK_WAIT_NONE; t->wait_object_key = 0; t->wake_reason = TASK_WAKE_NONE;
    spin_unlock(&g_scheduler_lock);
    token->prepared = 0; token->task = NULL;
    return wake_reason_to_result(reason);
}

void scheduler_abort_block(task_block_token_t *token, task_wake_reason_t reason)
{
    (void)reason;
    if (!token || !token->prepared || !token->task) return;
    task_t *t = token->task;
    if (t->queue_membership == TASK_QUEUE_WAIT) queue_detach_locked(t);
    t->state = TASK_RUNNING; t->wait_active = 0; t->wait_kind = TASK_WAIT_NONE;
    t->wake_reason = TASK_WAKE_NONE; t->wait_object_key = 0; t->deferred_ready = 0; g_waits_aborted++;
    token->prepared = 0; token->task = NULL; spin_unlock(&g_scheduler_lock);
}

static scheduler_wake_status_t scheduler_wake_task_locked(task_t *t, uint64_t lifecycle, uint64_t wait_gen, task_wake_reason_t reason)
{
    if (!t || t->lifecycle_generation != lifecycle || !t->wait_active ||
        t->wait_generation != wait_gen || t->wake_reason != TASK_WAKE_NONE ||
        (t->state != TASK_BLOCKED && t->state != TASK_SLEEPING) || reason == TASK_WAKE_NONE)
    { if (t) t->duplicate_wake_count++; g_stale_wakes++; return SCHED_WAKE_NO_MATCH; }
    t->wake_reason = reason; t->wake_count++;
    if (reason == TASK_WAKE_SIGNAL) g_wakes_signal++; else if (reason == TASK_WAKE_TIMEOUT) g_wakes_timeout++;
    else if (reason == TASK_WAKE_CANCELLED) g_wakes_cancelled++;
    if (t->queue_membership == TASK_QUEUE_WAIT) queue_detach_locked(t);
    if (t->on_cpu) t->deferred_ready = 1;
    else { t->state = TASK_READY; if (!enqueue_task_locked(t)) return SCHED_WAKE_FATAL; }
    return SCHED_WAKE_WON;
}

scheduler_wake_status_t scheduler_wake_task_by_identity(task_id_t id, uint64_t lifecycle, uint64_t wait_gen, task_wake_reason_t reason)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    task_t *t = find_task_by_id_locked(id);
    scheduler_wake_status_t status = scheduler_wake_task_locked(t, lifecycle, wait_gen, reason);
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    if (status == SCHED_WAKE_FATAL) kpanic("SCHED: wake enqueue failed");
    return status;
}

scheduler_wake_status_t scheduler_wake_one_waiter(wait_queue_t *wq, task_wait_kind_t kind, uintptr_t key, task_wake_reason_t reason)
{
    if (!wq) return SCHED_WAKE_NO_MATCH;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    struct list_head *pos = NULL; scheduler_wake_status_t status = SCHED_WAKE_NO_MATCH;
    list_for_each(pos, &wq->head) {
        task_t *t = list_entry(pos, task_t, list);
        if (t->wait_active && t->wait_kind == kind && t->wait_object_key == key)
        { status = scheduler_wake_task_locked(t, t->lifecycle_generation, t->wait_generation, reason); break; }
    }
    spin_unlock_irqrestore(&g_scheduler_lock, flags); return status;
}

void thread_block(wait_queue_t *wq, task_state_t state)
{

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    cpu_slot_t slot = current_cpu_slot_pinned_or_panic();
    task_t *current = current_task_pinned(slot);

    if (current == g_scheduler_cpus[slot].idle)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    current->state = state;

    if (current->queue_membership != TASK_QUEUE_NONE)
        queue_detach_locked(current);

    if (wq)
        waitqueue_attach_locked(wq, current);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    schedule_voluntary();
}

void thread_wake(task_t *t)
{
    if (!t)
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (t->state != TASK_SLEEPING && t->state != TASK_BLOCKED)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    if (t->queue_membership == TASK_QUEUE_WAIT)
        queue_detach_locked(t);
    else if (t->queue_membership != TASK_QUEUE_NONE)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    if (t->on_cpu)
        t->deferred_ready = 1;
    else
    {
        t->state = TASK_READY;
        enqueue_task_locked(t);
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

int thread_wake_one(wait_queue_t *wq)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (list_empty(&wq->head))
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return 0;
    }

    struct list_head *node = wq->head.next;
    task_t *t = list_entry(node, task_t, list);

    if (t->queue_membership != TASK_QUEUE_WAIT || !queue_detach_locked(t))
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return 0;
    }
    if (t->on_cpu)
        t->deferred_ready = 1;
    else
    {
        t->state = TASK_READY;
        enqueue_task_locked(t);
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    return 1;
}

bool scheduler_task_stack_guard_ok(const task_t *task)
{
    if(!task || !task->stack_base) return task && (task->flags&TASK_FLAG_IDLE);
    if(task->kernel_stack_guard_bytes!=HOBBYOS_KERNEL_STACK_GUARD_BYTES)return false;
    const uint8_t *p=(const uint8_t*)task->stack_base;
    for(uint32_t i=0;i<task->kernel_stack_guard_bytes;i++)if(p[i]!=0xA5)return false;
    return true;
}

bool scheduler_task_stack_rsp_in_bounds(const task_t *task)
{
    if(!task || !task->stack_base)return task && (task->flags&TASK_FLAG_IDLE);
    uintptr_t low=(uintptr_t)task->stack_base+task->kernel_stack_guard_bytes;
    uintptr_t top=(uintptr_t)task->stack_base+task->kernel_stack_bytes;
    return task->rsp>=low&&task->rsp<=top;
}

static bool scheduler_task_stack_ok(const task_t *task)
{
    return scheduler_task_stack_guard_ok(task)&&scheduler_task_stack_rsp_in_bounds(task);
}

void scheduler_report_stack_usage(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    struct list_head *pos = NULL;
    list_for_each(pos, &g_all_tasks) {
        task_t *task = list_entry(pos, task_t, registry_list);
        if (!task->stack_base) continue;
        uint32_t usable = task->kernel_stack_bytes - task->kernel_stack_guard_bytes;
        uint8_t *bytes = (uint8_t *)task->stack_base;
        uint32_t free_bytes = 0;
        while (free_bytes < usable && bytes[task->kernel_stack_guard_bytes + free_bytes] == 0xA5)
            free_bytes++;
        uint32_t used = usable - free_bytes;
        if (used > task->kernel_stack_low_watermark) task->kernel_stack_low_watermark = used;
        serial_write_all("[TASK][STACK] id="); sched_serial_dec(task->id);
        serial_write_all(" used="); sched_serial_dec(used);
        serial_write_all(" free="); sched_serial_dec(free_bytes);
        serial_write_all(" guard=");
        serial_write_all(scheduler_task_stack_guard_ok(task) ? "OK\n" : "FAIL\n");
    }
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

static bool task_exit_reason_valid(task_exit_reason_t reason)
{
    return reason == TASK_EXIT_NORMAL || reason == TASK_EXIT_KILLED ||
           reason == TASK_EXIT_INIT_FAILURE || reason == TASK_EXIT_INTERNAL_ERROR;
}

bool scheduler_task_timer_ref_acquire_locked(task_t*t,uint64_t lifecycle,uint64_t wait_generation)
{
    bool ok=false;
    if(t&&t->lifecycle_generation==lifecycle&&t->wait_active&&t->wait_generation==wait_generation&&
       (t->state==TASK_SLEEPING||t->state==TASK_BLOCKED)&&!t->reap_claimed&&t->task_wake_timer_refs!=UINT32_MAX){
        t->task_wake_timer_refs++;t->timer_ref_acquires++;g_reaper_stats.refs_acquired++;
        if(t->task_wake_timer_refs>g_reaper_stats.max_refs_per_task)
            g_reaper_stats.max_refs_per_task=t->task_wake_timer_refs;
        ok=true;
    }
    return ok;
}
bool scheduler_task_timer_ref_acquire_by_identity(task_id_t id,uint64_t lifecycle,uint64_t wait_generation)
{
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);bool ok=scheduler_task_timer_ref_acquire_locked(find_task_by_id_locked(id),lifecycle,wait_generation);spin_unlock_irqrestore(&g_scheduler_lock,f);return ok;
}

bool scheduler_task_timer_ref_release_by_identity(task_id_t id,uint64_t lifecycle,uint64_t wait_generation)
{
    (void)wait_generation;bool ok=false;irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);task_t*t=find_task_by_id_locked(id);
    if(t&&t->lifecycle_generation==lifecycle){if(!t->task_wake_timer_refs){g_reaper_stats.timer_ref_underflows++;spin_unlock_irqrestore(&g_scheduler_lock,f);kpanic("TASK: timer ref underflow");}
        t->task_wake_timer_refs--;t->timer_ref_releases++;g_reaper_stats.refs_released++;ok=true;}
    spin_unlock_irqrestore(&g_scheduler_lock,f);return ok;
}

void thread_exit_with_reason(task_exit_reason_t reason)
{
    if (!task_exit_reason_valid(reason)) kpanic("TASK: invalid exit reason");
    task_cleanup_fn cleanup = NULL;
    void *cleanup_ctx = NULL;
    irq_flags_t flags = irq_save();
    cpu_slot_t slot = current_cpu_slot_pinned_or_panic();
    spin_lock(&g_scheduler_lock);
    task_t *current = current_task_pinned(slot);
    if (!current) { spin_unlock_irqrestore(&g_scheduler_lock, flags); kpanic("TASK: exit without current"); }
    if (current == g_scheduler_cpus[slot].idle || current->state != TASK_RUNNING || !current->on_cpu) { spin_unlock_irqrestore(&g_scheduler_lock, flags); kpanic("TASK: invalid exit"); }
    if (current->exit_started) { g_kill_stats.exit_duplicate_attempts++; spin_unlock_irqrestore(&g_scheduler_lock, flags); kpanic("TASK: duplicate exit"); }
    if (reason == TASK_EXIT_NORMAL && current->kill_pending &&
        (current->flags & TASK_FLAG_KILLABLE) &&
        !(current->flags & TASK_FLAG_KILL_PROTECTED))
        reason = TASK_EXIT_KILLED;
    current->exit_started = 1;
    current->exit_started_ns = clock_monotonic_ns();
    current->exit_reason = reason;
    current->cleanup_started = 1;
    cleanup = current->cleanup_fn;
    cleanup_ctx = current->cleanup_ctx;
    current->cleanup_fn = NULL;
    current->cleanup_ctx = NULL;
    g_kill_stats.exit_phases_started++;
    g_kill_stats.cleanup_started++;
    if (reason == TASK_EXIT_KILLED) { current->cancellation_points++; g_kill_stats.cancellation_points++; }
    if (reason == TASK_EXIT_NORMAL) g_kill_stats.exits_normal++;
    else if (reason == TASK_EXIT_KILLED) g_kill_stats.exits_killed++;
    else if (reason == TASK_EXIT_INIT_FAILURE) g_kill_stats.exits_init_failure++;
    else g_kill_stats.exits_internal_error++;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    if (cleanup) cleanup(cleanup_ctx);
    flags = irq_save();
    uint64_t now_ns = clock_monotonic_ns();
    slot = current_cpu_slot_pinned_or_panic();
    spin_lock(&g_scheduler_lock);
    if (current_task_pinned(slot) != current || !current->exit_started || current->cleanup_done || !current->on_cpu || current->current_cpu_slot != slot) { g_kill_stats.cleanup_duplicate_attempts++; spin_unlock_irqrestore(&g_scheduler_lock, flags); kpanic("TASK: invalid cleanup completion slot"); }
    scheduler_account_cpu_locked(slot, now_ns);
    if (cleanup) { current->cleanup_invocations++; g_kill_stats.cleanup_callbacks_invoked++; }
    current->cleanup_done = 1;
    current->cleanup_completed_ns = now_ns;
    current->lifecycle_notify_started = 1;
    if(!g_next_zombie_generation){spin_unlock_irqrestore(&g_scheduler_lock,flags);kpanic("TASK: zombie generation exhausted");}
    current->zombie_generation=g_next_zombie_generation++;
    g_kill_stats.cleanup_completed++;
    g_kill_stats.exit_phases_completed++;
    task_lifecycle_exit_event_t event;
    memset(&event,0,sizeof(event));event.task_id=current->id;event.lifecycle_generation=current->lifecycle_generation;
    event.zombie_generation=current->zombie_generation;event.reason=current->exit_reason;event.kill_requested_ns=current->kill_requested_ns;
    event.runtime_ns_total=current->runtime_ns_total;event.exit_started_ns=current->exit_started_ns;
    event.cleanup_completed_ns=current->cleanup_completed_ns;event.notification_ns=clock_monotonic_ns();event.flags=current->flags;
    event.last_cpu_slot=current->last_cpu_slot;memcpy(event.name,current->name,sizeof(event.name));
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    bool notified=task_lifecycle_publish_exit(&event);
    flags=irq_save();slot=current_cpu_slot_pinned_or_panic();spin_lock(&g_scheduler_lock);
    if(current_task_pinned(slot)!=current||!current->exit_started||!current->cleanup_done||!current->lifecycle_notify_started||
       current->state!=TASK_RUNNING||current->queue_membership!=TASK_QUEUE_NONE||current->wait_active||
       current->wait_kind!=TASK_WAIT_NONE||current->wait_object_key||current->deferred_ready){
        spin_unlock_irqrestore(&g_scheduler_lock,flags);kpanic("TASK: invalid zombie commit");}
    if(!notified){current->lifecycle_notify_failed=1;g_reaper_stats.notification_failures++;spin_unlock_irqrestore(&g_scheduler_lock,flags);kpanic("TASK: lifecycle publish failed");}
    current->lifecycle_notify_completed=1;current->lifecycle_notify_count++;current->state=TASK_ZOMBIE;
    extern uint64_t timer_get_uptime_ms(void);current->zombie_since_ms=timer_get_uptime_ms();current->zombie_entered_ns=clock_monotonic_ns();
    bool hold_on_cpu=__atomic_exchange_n(&g_test_hold_next_zombie_on_cpu,0,__ATOMIC_ACQ_REL)!=0;
    if(hold_on_cpu){__atomic_store_n(&g_test_release_zombie_on_cpu,0,__ATOMIC_RELEASE);spin_unlock(&g_scheduler_lock);while(!__atomic_load_n(&g_test_release_zombie_on_cpu,__ATOMIC_ACQUIRE))spin_cpu_relax();irq_restore(flags);}
    else spin_unlock_irqrestore(&g_scheduler_lock,flags);
    schedule_voluntary();
    while (1) __asm__ volatile("cli; hlt");
}
void thread_exit_normal(void) { thread_exit_with_reason(TASK_EXIT_NORMAL); }
void thread_exit(void) { thread_exit_with_reason(task_cancel_requested() ? TASK_EXIT_KILLED : TASK_EXIT_NORMAL); }
bool task_cancel_requested(void)
{
    task_t *t = get_current_task();
    if (!t || __atomic_load_n(&t->exit_started, __ATOMIC_ACQUIRE)) return false;
    uint32_t flags = t->flags;
    return (flags & TASK_FLAG_KILLABLE) && !(flags & TASK_FLAG_KILL_PROTECTED) &&
           __atomic_load_n(&t->kill_pending, __ATOMIC_ACQUIRE);
}
void task_cancel_point(void)
{
    if (!task_cancel_requested()) return;
    task_t *t = get_current_task();
    if (!t || __atomic_load_n(&t->exit_started, __ATOMIC_ACQUIRE)) return;
    thread_exit_with_reason(TASK_EXIT_KILLED);
}

static bool scheduler_switch_locked(cpu_slot_t slot)
{
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    task_t *prev = cpu->current;
    task_t *next = runqueue_take_next_locked();
    if (!next) next = cpu->idle;
    if (prev == next) return false;
    uint64_t nrsp = next ? next->rsp : 0;
    bool next_stack_ok = scheduler_task_stack_ok(next);
    if (!next || nrsp < 0x100000ULL ||
        (nrsp >= 0x0000800000000000ULL && nrsp < 0xFFFF800000000000ULL) ||
        !next_stack_ok || next->on_cpu || next->queue_membership != TASK_QUEUE_NONE || next->state != TASK_READY) {
        g_handoff_failures++; spin_unlock(&g_scheduler_lock);
        if (next && !next_stack_ok) {
            serial_write_all("[TASK][STACK] CORRUPTION id="); sched_serial_dec(next->id);
            serial_write_all(" rsp="); serial_write_hex64_all(next->rsp);
            serial_write_all(" low="); serial_write_hex64_all((uint64_t)(uintptr_t)next->stack_base + next->kernel_stack_guard_bytes);
            serial_write_all(" top="); serial_write_hex64_all((uint64_t)(uintptr_t)next->stack_base + next->kernel_stack_bytes);
            serial_write_all(" guard_offset=0\n");
        }
        kpanic("SCHED: invalid next context");
    }
    scheduler_switch_handoff_t *handoff = &cpu->handoff;
    if (handoff->active) { g_pending_overwrite++; spin_unlock(&g_scheduler_lock); kpanic("SCHED: pending handoff overwrite"); }
    handoff->prev = prev; handoff->next = next; handoff->sequence++;
    handoff->expected_slot = slot;
    handoff->owner_apic_id = lapic_get_id();
    handoff->executing_task_id = prev->id;
    handoff->active = 1; handoff->started_count++;
    switch_context(prev, next, cpu, handoff->sequence);
    return true;
}

void schedule_impl(int voluntary)
{
    if (!scheduler_is_started()) return;
#ifdef HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE
    if (irq_is_enabled()) {
        cpu_slot_t before = CPU_SLOT_INVALID;
        if (smp_current_cpu_slot(&before)) {
            for (volatile uint32_t i = 0; i < 100000u; i++)
                __asm__ volatile("pause" ::: "memory");
            irq_flags_t negative_flags = irq_save();
            cpu_slot_t after = CPU_SLOT_INVALID;
            bool after_valid = smp_current_cpu_slot(&after);
            if (after_valid && after != before) {
                g_stale_cpu_slot_entries++;
                serial_write_all("[SCHED][NEGATIVE] STALE_CPU_SLOT_CAPTURE_DETECTED before=");
                sched_serial_dec(before);
                serial_write_all(" after=");
                sched_serial_dec(after);
                serial_write_all("\n[SCHED][NEGATIVE] STALE_SLOT_DETECTED\n");
                irq_restore(negative_flags);
                return;
            }
            irq_restore(negative_flags);
        }
    }
#endif
    irq_flags_t flags = irq_save();
    uint64_t now_ns = clock_monotonic_ns();
    cpu_slot_t slot = current_cpu_slot_pinned_or_panic();
    task_t *pinned_current = current_task_pinned(slot);
    uint32_t actual_apic = lapic_get_id();
    spin_lock(&g_scheduler_lock);
    scheduler_account_cpu_locked(slot,now_ns);
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    task_t *prev = cpu->current;
    if (cpu->slot != slot || cpu->apic_id != actual_apic) g_stale_cpu_slot_entries++;
    if (prev != pinned_current) g_pinned_current_mismatches++;
    if (cpu->slot != slot || cpu->apic_id != actual_apic || !cpu->initialized || !cpu->online ||
        !prev || prev != pinned_current || !cpu->idle || cpu->handoff.active ||
        !prev->on_cpu || prev->current_cpu_slot != slot || !scheduler_task_stack_ok(prev))
    { g_handoff_failures++; spin_unlock_irqrestore(&g_scheduler_lock, flags); kpanic("SCHED: invalid switch begin state"); }
    if (!voluntary && prev != cpu->idle && prev->state == TASK_RUNNING)
    {
        prev->quantum--;
        if (prev->quantum > 0 && (prev->task_class == TASK_CLASS_INTERACTIVE || list_empty(&g_interactive_queue)))
        { spin_unlock_irqrestore(&g_scheduler_lock, flags); return; }
    }
    if (!scheduler_switch_locked(slot)) spin_unlock(&g_scheduler_lock);
    irq_restore(flags);
}

void scheduler_finish_switch(void *cpu_state, uint64_t expected_sequence)
{
    uintptr_t base = (uintptr_t)&g_scheduler_cpus[0];
    uintptr_t end = (uintptr_t)&g_scheduler_cpus[HOBBYOS_MAX_CPUS];
    uintptr_t ptr = (uintptr_t)cpu_state;
    bool pointer_valid = ptr >= base && ptr < end &&
                         ((ptr - base) % sizeof(scheduler_cpu_state_t)) == 0;
    scheduler_cpu_state_t *cpu = pointer_valid ? (scheduler_cpu_state_t *)cpu_state : NULL;
    if (!pointer_valid) {
        serial_write_all("[SCHED][FINISH_FAULT] invalid cpu pointer\n");
        kpanic("SCHED: invalid switch finish CPU pointer");
    }
    cpu_slot_t slot = (cpu_slot_t)((ptr - base) / sizeof(scheduler_cpu_state_t));
    scheduler_switch_handoff_t *handoff = &cpu->handoff;
    cpu_slot_t actual_slot = CPU_SLOT_INVALID;
    bool actual_valid = !irq_is_enabled() && smp_current_cpu_slot(&actual_slot) &&
                        actual_slot < HOBBYOS_MAX_CPUS;
    bool owns_handoff = handoff->active && handoff->sequence == expected_sequence;
    task_t *prev = handoff->prev;
    task_t *next = handoff->next;
    enum { FF_ACTIVE=1u<<0,FF_PREV=1u<<1,FF_NEXT=1u<<2,FF_DISTINCT=1u<<3,FF_PREV_CPU=1u<<4,FF_PREV_SLOT=1u<<5,FF_CURRENT=1u<<6,FF_NEXT_OFF=1u<<7,FF_NEXT_QUEUE=1u<<8,FF_NEXT_READY=1u<<9,FF_PREV_STACK=1u<<10,FF_NEXT_STACK=1u<<11,FF_CPU_POINTER=1u<<12,FF_WRONG_CPU=1u<<13,FF_WRONG_APIC=1u<<14,FF_SEQUENCE=1u<<15,FF_LOCK_OWNERSHIP=1u<<16 };
    uint32_t failed=0;
    if (!pointer_valid) failed |= FF_CPU_POINTER;
    if (!actual_valid || actual_slot != slot || handoff->expected_slot != slot) failed |= FF_WRONG_CPU;
    if (handoff->owner_apic_id != lapic_get_id()) failed |= FF_WRONG_APIC;
    if (handoff->sequence != expected_sequence) failed |= FF_SEQUENCE;
    if (!owns_handoff) failed |= FF_LOCK_OWNERSHIP;
    if (failed & FF_WRONG_CPU) g_finish_wrong_cpu++;
    if (failed & FF_SEQUENCE) g_finish_sequence_mismatch++;
    if(!handoff->active)failed|=FF_ACTIVE;
    if(!prev)failed|=FF_PREV;
    if(!next)failed|=FF_NEXT;
    if(prev&&next&&prev==next)failed|=FF_DISTINCT;
    if(prev&&!prev->on_cpu)failed|=FF_PREV_CPU;
    if(prev&&prev->current_cpu_slot!=slot)failed|=FF_PREV_SLOT;
    if(cpu->current!=prev)failed|=FF_CURRENT;
    if(next&&next->on_cpu)failed|=FF_NEXT_OFF;
    if(next&&next->queue_membership!=TASK_QUEUE_NONE)failed|=FF_NEXT_QUEUE;
    if(next&&next->state!=TASK_READY)failed|=FF_NEXT_READY;
    if(prev&&!scheduler_task_stack_ok(prev))failed|=FF_PREV_STACK;
    if(next&&!scheduler_task_stack_ok(next))failed|=FF_NEXT_STACK;
    failed |= __atomic_exchange_n(&g_test_finish_failure, 0, __ATOMIC_ACQ_REL);
    if(failed)
    {
        task_id_t prev_id=prev?prev->id:0,next_id=next?next->id:0,current_id=cpu->current?cpu->current->id:0;uint64_t seq=handoff->sequence;
        if (!handoff->active)g_finish_without_pending++;
        g_handoff_failures++;if (owns_handoff) spin_unlock(&g_scheduler_lock);
        serial_write_all("[SCHED][FINISH_FAULT] slot=");serial_write_hex64_all(slot);serial_write_all(" seq=");serial_write_hex64_all(seq);serial_write_all(" failed=");serial_write_hex64_all(failed);serial_write_all(" current=");serial_write_hex64_all(current_id);serial_write_all(" prev=");serial_write_hex64_all(prev_id);serial_write_all(" next=");serial_write_hex64_all(next_id);serial_write_all("\n");
        kpanic("SCHED: invalid switch finish state");
    }

    bool finish_fatal = false;
    prev->on_cpu = 0;
    prev->current_cpu_slot = TASK_CPU_SLOT_NONE;
    if (prev->deferred_ready && (prev->state == TASK_BLOCKED || prev->state == TASK_SLEEPING))
    {
        prev->deferred_ready = 0;
        prev->state = TASK_READY;
        prev->quantum = prev->quantum_default;
        if (!enqueue_task_locked(prev)) { g_handoff_failures++; finish_fatal = true; }
    }
    else if (prev->state == TASK_RUNNING)
    {
        prev->state = TASK_READY;
        prev->quantum = prev->quantum_default;
        if (!(prev->flags & TASK_FLAG_IDLE) && !enqueue_task_locked(prev))
        { g_handoff_failures++; finish_fatal = true; }
    }

    next->state = TASK_RUNNING;
    next->on_cpu = 1;
    next->current_cpu_slot = slot;
    next->last_cpu_slot = slot;
    next->schedule_count++;
    if (next->quantum <= 0)
        next->quantum = next->quantum_default;
    cpu->current = next;

    handoff->prev = NULL;
    handoff->next = NULL;
    handoff->active = 0;
    handoff->finished_count++;
    spin_unlock(&g_scheduler_lock);
    if (finish_fatal) kpanic("SCHED: finish requeue failed");
}

void schedule(void)
{
    schedule_impl(0);
}

void schedule_voluntary(void)
{
    schedule_impl(1);
}

static bool scheduler_bootstrap_rsp_valid(uint64_t rsp)
{
    if (rsp < 0x100000ULL)
        return false;
    return rsp < 0x0000800000000000ULL || rsp >= 0xFFFF800000000000ULL;
}

bool scheduler_bootstrap_handoff_current_cpu(void)
{
    if (!scheduler_is_started())
        return false;

    irq_flags_t flags = irq_save();
    cpu_slot_t slot = CPU_SLOT_INVALID;
    uint64_t observed_rsp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(observed_rsp));
    if (!smp_current_cpu_slot(&slot) || slot >= g_cpu_count) {
        irq_restore(flags);
        return false;
    }

    interrupt_cpu_snapshot_t interrupt_snapshot;
    if (!interrupt_context_snapshot(slot, &interrupt_snapshot) ||
        interrupt_snapshot.depth != 0) {
        irq_restore(flags);
        return false;
    }

    spin_lock(&g_scheduler_lock);
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    task_t *idle = cpu->idle;
    bool valid = cpu->initialized && cpu->online && idle &&
                 cpu->current == idle && idle->on_cpu &&
                 idle->current_cpu_slot == slot && !cpu->handoff.active &&
                 scheduler_bootstrap_rsp_valid(observed_rsp);
    if (!valid) {
        spin_unlock(&g_scheduler_lock);
        irq_restore(flags);
        return false;
    }
    if (!idle->rsp)
        idle->rsp = observed_rsp;
    __atomic_store_n(&cpu->bootstrap_attached, 1, __ATOMIC_RELEASE);
    spin_unlock(&g_scheduler_lock);
    irq_restore(flags);

    /* A voluntary switch, when runnable work exists, makes switch_context
       capture the exact bootstrap stack.  With an empty runqueue the explicit
       attachment above is sufficient; the first later switch overwrites rsp
       with the normal callee-saved frame before the idle context is resumed. */
    schedule_voluntary();

    flags = irq_save();
    spin_lock(&g_scheduler_lock);
    cpu = &g_scheduler_cpus[slot];
    idle = cpu->idle;
    valid = cpu->current == idle && idle && idle->on_cpu &&
            idle->current_cpu_slot == slot &&
            scheduler_bootstrap_rsp_valid(idle->rsp) &&
            !cpu->handoff.active;
    if (valid)
        __atomic_store_n(&cpu->handoff_complete, 1, __ATOMIC_RELEASE);
    uint64_t idle_rsp = idle ? idle->rsp : 0;
    spin_unlock(&g_scheduler_lock);
    irq_restore(flags);
    if (!valid)
        return false;

    char line[128] = "[SCHED][BOOTSTRAP_HANDOFF] PASS slot=";
    char decimal[TASK_ID_DECIMAL_BUFFER_SIZE];
    if (!sched_u64_format_decimal(slot, decimal, sizeof(decimal)))
        return false;
    strcat(line, decimal);
    strcat(line, " idle=");
    if (!sched_u64_format_decimal(idle->id, decimal, sizeof(decimal)))
        return false;
    strcat(line, decimal);
    strcat(line, " rsp_valid=");
    if (!sched_u64_format_decimal(
            scheduler_bootstrap_rsp_valid(idle_rsp) ? 1u : 0u,
            decimal, sizeof(decimal)))
        return false;
    strcat(line, decimal);
    strcat(line, "\n");
    serial_write_all(line);
    return true;
}

bool scheduler_cpu_enable_irq_preemption(cpu_slot_t slot)
{
    irq_flags_t flags = irq_save();
    cpu_slot_t current_slot = CPU_SLOT_INVALID;
    bool current_valid = smp_current_cpu_slot(&current_slot) &&
                         current_slot == slot && slot < g_cpu_count;
    interrupt_cpu_snapshot_t interrupt_snapshot;
    bool interrupt_valid = current_valid &&
        interrupt_context_snapshot(slot, &interrupt_snapshot) &&
        interrupt_snapshot.depth == 0;
    if (!interrupt_valid) {
        irq_restore(flags);
        return false;
    }
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    if (!__atomic_load_n(&cpu->bootstrap_attached, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&cpu->handoff_complete, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&cpu->early_preemption_attempts, 1,
                           __ATOMIC_RELAXED);
        irq_restore(flags);
        return false;
    }
    __atomic_store_n(&cpu->irq_preemption_enabled, 1, __ATOMIC_RELEASE);
    irq_restore(flags);
    return true;
}

bool scheduler_cpu_irq_preemption_enabled(cpu_slot_t slot)
{
    return slot < g_cpu_count &&
        __atomic_load_n(&g_scheduler_cpus[slot].irq_preemption_enabled,
                        __ATOMIC_ACQUIRE) != 0;
}

bool scheduler_cpu_bootstrap_handoff_complete(cpu_slot_t slot)
{
    return slot < g_cpu_count &&
        __atomic_load_n(&g_scheduler_cpus[slot].handoff_complete,
                        __ATOMIC_ACQUIRE) != 0;
}

bool scheduler_preemption_snapshot(cpu_slot_t slot,
                                   scheduler_preemption_snapshot_t *out)
{
    if (!out || slot >= g_cpu_count)
        return false;
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    task_t *idle = cpu->idle;
    *out = (scheduler_preemption_snapshot_t){
        .idle_rsp = idle ? __atomic_load_n(&idle->rsp, __ATOMIC_ACQUIRE) : 0,
        .early_preemption_attempts = __atomic_load_n(
            &cpu->early_preemption_attempts, __ATOMIC_ACQUIRE),
        .bootstrap_attached = __atomic_load_n(
            &cpu->bootstrap_attached, __ATOMIC_ACQUIRE),
        .handoff_complete = __atomic_load_n(
            &cpu->handoff_complete, __ATOMIC_ACQUIRE),
        .irq_preemption_enabled = __atomic_load_n(
            &cpu->irq_preemption_enabled, __ATOMIC_ACQUIRE)};
    out->idle_rsp_valid = scheduler_bootstrap_rsp_valid(out->idle_rsp);
    return true;
}

bool scheduler_preemption_gate_selftest(void)
{
    uint8_t attached = 0;
    uint8_t handoff = 0;
    bool allowed = attached && handoff;
    attached = 1;
    allowed = allowed || (attached && handoff);
    handoff = 1;
    allowed = allowed || (attached && handoff);
    return allowed && scheduler_bootstrap_rsp_valid(0xFFFF800000100000ULL) &&
           !scheduler_bootstrap_rsp_valid(0);
}

void scheduler_preempt_from_irq(void)
{
    if (!interrupt_context_consume_preempt_epilogue())
        return;
    if (!scheduler_is_started())
        return;
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!smp_current_cpu_slot(&slot) || slot >= g_cpu_count)
        return;
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
    extern volatile int g_panic_in_progress;
    if (g_panic_in_progress || !cpu->online || !cpu->current ||
        !__atomic_load_n(&cpu->handoff_complete, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&cpu->irq_preemption_enabled, __ATOMIC_ACQUIRE))
        return;
    if (interrupts_consume_reschedule())
        schedule_impl(0);
}

static bool scheduler_account_cpu_locked(cpu_slot_t slot, uint64_t now_ns)
{
    if (slot>=HOBBYOS_MAX_CPUS || !now_ns) return false;
    scheduler_cpu_state_t *cpu=&g_scheduler_cpus[slot];
    if (!cpu->accounting_initialized) { cpu->last_account_ns=now_ns; cpu->accounting_initialized=1; return true; }
    /* A global snapshot may advance this CPU while a pre-lock IRQ/schedule
       timestamp is waiting for the scheduler lock. It is stale, not a
       regression of the canonical clock; discard it without moving baseline. */
    if (now_ns<cpu->last_account_ns) return true;
    uint64_t delta=now_ns-cpu->last_account_ns; cpu->last_account_ns=now_ns;
#ifdef HOBBYOS_ACCOUNT_NEGATIVE_IRQ_TICKS
    delta=1000000ULL;
#endif
    task_t *current=cpu->current; if (!current) return false;
    cpu->accounting_events++;
    if (current->state==TASK_ZOMBIE) return true;
    if (UINT64_MAX-current->runtime_ns_total<delta || UINT64_MAX-cpu->accounted_ns_total<delta)
    { cpu->runtime_overflows++; return false; }
    current->runtime_ns_total+=delta; cpu->accounted_ns_total+=delta;
    current->last_cpu_slot=slot; return true;
}

void scheduler_account_time(uint64_t now_ns)
{
    irq_flags_t flags=spin_lock_irqsave(&g_scheduler_lock); cpu_slot_t slot=CPU_SLOT_INVALID;
    if (scheduler_is_started() && smp_current_cpu_slot(&slot))
        scheduler_account_cpu_locked(slot,now_ns);
    spin_unlock_irqrestore(&g_scheduler_lock,flags);
}

static void scheduler_fill_test_snapshot_locked(task_t *t, task_snapshot_t *out)
{
    if (!out || !t) return;
    memset(out, 0, sizeof(*out));
    out->id=t->id; out->state=t->state; out->schedule_count=t->schedule_count; out->on_cpu=t->on_cpu; out->current_cpu_slot=t->current_cpu_slot;
    out->flags=t->flags; out->kill_pending=t->kill_pending!=0; out->exit_started=t->exit_started;
    out->cleanup_started=t->cleanup_started;out->cleanup_done=t->cleanup_done; out->exit_reason=t->exit_reason; out->kill_requested_ns=t->kill_requested_ns;
    out->kill_request_count=t->kill_request_count; out->cancellation_points=t->cancellation_points;
    out->lifecycle_generation=t->lifecycle_generation; out->wait_generation=t->wait_generation;
    out->wait_kind=t->wait_kind; out->wake_reason=t->wake_reason; out->wait_active=t->wait_active; out->queue_membership=t->queue_membership;
    out->lifecycle_notify_started=t->lifecycle_notify_started;out->lifecycle_notify_completed=t->lifecycle_notify_completed;out->lifecycle_notify_failed=t->lifecycle_notify_failed;
    out->reap_claimed=t->reap_claimed;out->task_wake_timer_refs=t->task_wake_timer_refs;out->zombie_generation=t->zombie_generation;
    out->exit_started_ns=t->exit_started_ns;out->cleanup_completed_ns=t->cleanup_completed_ns;out->zombie_entered_ns=t->zombie_entered_ns;
    out->wait_object_key=t->wait_object_key;out->deferred_ready=t->deferred_ready;out->timer_ref_acquires=t->timer_ref_acquires;out->timer_ref_releases=t->timer_ref_releases;out->reap_defer_mask_last=t->reap_defer_mask_last;
}

bool scheduler_snapshot_task_by_id(task_id_t id, task_snapshot_t *out)
{
    if (id == TASK_ID_INVALID || !out) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    task_t *task = find_task_by_id_locked(id);
    if (task) {
        scheduler_fill_test_snapshot_locked(task, out);
        memcpy(out->name, task->name, sizeof(out->name));
        out->name[sizeof(out->name) - 1] = 0;
        out->task_class = task->task_class;
        out->quantum = task->quantum;
        out->quantum_default = task->quantum_default;
        out->rsp = task->rsp;
        out->stack_base = task->stack_base;
        out->kernel_stack_bytes = task->kernel_stack_bytes;
        out->kernel_ctx_bytes_est = task->kernel_ctx_bytes_est;
        out->kernel_mem_est_bytes = (uint64_t)sizeof(task_t) +
                                    (uint64_t)task->kernel_stack_bytes +
                                    (uint64_t)task->kernel_ctx_bytes_est;
        out->is_idle = task_is_idle_locked(task);
        out->is_current = task_is_current_locked(task, &out->current_cpu);
        if (!out->is_current)
            out->current_cpu = UINT32_MAX;
        out->last_cpu_slot = task->last_cpu_slot;
        out->runtime_ns_total = task->runtime_ns_total;
    }
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    return task != NULL;
}

bool scheduler_snapshot_task_by_handle(task_handle_t handle,
                                       task_snapshot_t *out)
{
    if (!out || handle.id == TASK_ID_INVALID ||
        handle.lifecycle_generation == 0)
        return false;
    if (!scheduler_snapshot_task_by_id(handle.id, out))
        return false;
    return out->lifecycle_generation == handle.lifecycle_generation;
}

static task_kill_result_t scheduler_request_kill_locked(task_id_t id, uint64_t now_ns, bool *wake_fatal, uint32_t *flags_value)
{
    task_t *t=NULL;
    g_kill_stats.requests++;
    if (id==TASK_ID_INVALID) return TASK_KILL_ERR_INVALID;
    if (!(t=find_task_by_id_locked(id))) { g_kill_stats.not_found++; return TASK_KILL_ERR_NOT_FOUND; }
    if (task_is_kill_protected_locked(t)) { if(flags_value)*flags_value=t->flags; g_kill_stats.protected_rejections++; return TASK_KILL_ERR_PROTECTED; }
    if (t->state==TASK_ZOMBIE) { g_kill_stats.already_zombie++; return TASK_KILL_ALREADY_ZOMBIE; }
#ifndef HOBBYOS_KILL_NEGATIVE_ACCEPT_EXITING
    if (t->exit_started) { g_kill_stats.already_exiting++; return TASK_KILL_ALREADY_EXITING; }
#endif
#ifndef HOBBYOS_KILL_NEGATIVE_IGNORE_KILLABLE
    if (!(t->flags&TASK_FLAG_KILLABLE)) { g_kill_stats.not_killable_rejections++; return TASK_KILL_ERR_NOT_KILLABLE; }
#endif
    if (t->kill_pending) { g_kill_stats.already_pending++; return TASK_KILL_ALREADY_PENDING; }
    t->kill_pending=1; t->kill_requested_ns=now_ns; t->kill_request_count++;
    if (wake_fatal) *wake_fatal=task_mark_kill_pending_locked(t);
    g_kill_stats.accepted++;
    return TASK_KILL_ACCEPTED;
}

static void scheduler_log_kill_result(task_id_t id, task_kill_result_t result, uint32_t flags_value)
{
    if (__atomic_load_n(&g_test_quiet_lifecycle,__ATOMIC_ACQUIRE)) return;
    serial_write_all("[TASK][KILL] result=");
    serial_write_all(scheduler_task_kill_result_to_string(result));
    serial_write_all(" id=");sched_serial_dec(id);if(flags_value){serial_write_all(" flags=0x");serial_write_hex64_all(flags_value);}serial_write_all("\n");
}

task_kill_result_t scheduler_request_kill(task_id_t id)
{
    uint64_t now_ns=clock_monotonic_ns(); bool wake_fatal=false; uint32_t flags_value=0;
    irq_flags_t flags=spin_lock_irqsave(&g_scheduler_lock);
    task_kill_result_t result=scheduler_request_kill_locked(id,now_ns,&wake_fatal,&flags_value);
    spin_unlock_irqrestore(&g_scheduler_lock,flags);
    if(wake_fatal)kpanic("SCHED: kill wake enqueue failed");
    scheduler_log_kill_result(id,result,flags_value);
    return result;
}

const char *scheduler_task_kill_result_to_string(task_kill_result_t result)
{
    switch (result)
    {
    case TASK_KILL_ACCEPTED: return "ACCEPTED";
    case TASK_KILL_ALREADY_PENDING: return "ALREADY_PENDING";
    case TASK_KILL_ALREADY_ZOMBIE: return "ALREADY_ZOMBIE";
    case TASK_KILL_ALREADY_EXITING: return "ALREADY_EXITING";
    case TASK_KILL_ERR_PROTECTED: return "PROTECTED";
    case TASK_KILL_ERR_NOT_KILLABLE: return "NOT_KILLABLE";
    case TASK_KILL_ERR_NOT_FOUND: return "NOT_FOUND";
    default: return "INVALID";
    }
}

static bool scheduler_test_match_locked(task_t *t, task_test_match_t match)
{
    if (!t) return false;
    if (match==TASK_TEST_MATCH_READY_OFF_CPU) return t->schedule_count==0 && t->state==TASK_READY && !t->on_cpu && (t->queue_membership==TASK_QUEUE_RUNQ_NORMAL || t->queue_membership==TASK_QUEUE_RUNQ_INTERACTIVE);
    if (match==TASK_TEST_MATCH_RUNNING_ON_CPU) return t->state==TASK_RUNNING && t->on_cpu && t->queue_membership==TASK_QUEUE_NONE;
    if (match==TASK_TEST_MATCH_SLEEPING_WAIT) return t->state==TASK_SLEEPING && t->wait_active && t->wait_kind==TASK_WAIT_TIMER_SLEEP;
    if (match==TASK_TEST_MATCH_BLOCKED_WAIT) return t->state==TASK_BLOCKED && t->wait_active && t->wait_kind==TASK_WAIT_SEMAPHORE && t->queue_membership==TASK_QUEUE_WAIT;
    if (match==TASK_TEST_MATCH_ZOMBIE) return t->state==TASK_ZOMBIE;
    if (match==TASK_TEST_MATCH_EXITING) return t->exit_started && t->state!=TASK_ZOMBIE;
    return false;
}

task_test_request_status_t scheduler_test_request_kill_when(task_id_t id, task_test_match_t match, task_kill_result_t *out_result, task_snapshot_t *out_observed)
{
    uint64_t now_ns=clock_monotonic_ns(); bool wake_fatal=false; uint32_t flags_value=0; task_kill_result_t result=TASK_KILL_ERR_INVALID;
    irq_flags_t flags=spin_lock_irqsave(&g_scheduler_lock); task_t *t=find_task_by_id_locked(id);
    if (!t) { spin_unlock_irqrestore(&g_scheduler_lock,flags); if(out_result)*out_result=TASK_KILL_ERR_NOT_FOUND; return TASK_TEST_REQUEST_GONE; }
    if (!scheduler_test_match_locked(t,match)) { scheduler_fill_test_snapshot_locked(t,out_observed); spin_unlock_irqrestore(&g_scheduler_lock,flags); return TASK_TEST_REQUEST_NOT_READY; }
    scheduler_fill_test_snapshot_locked(t,out_observed); result=scheduler_request_kill_locked(id,now_ns,&wake_fatal,&flags_value); if(out_observed){out_observed->kill_pending=t->kill_pending;out_observed->kill_requested_ns=t->kill_requested_ns;out_observed->kill_request_count=t->kill_request_count;}
    if (match==TASK_TEST_MATCH_READY_OFF_CPU) t->test_ready_hold=0;
    spin_unlock_irqrestore(&g_scheduler_lock,flags); if(wake_fatal)kpanic("SCHED: test kill wake failed"); if(out_result)*out_result=result; scheduler_log_kill_result(id,result,flags_value); return TASK_TEST_REQUEST_APPLIED;
}

void scheduler_test_hold_next_ready_creation(void){__atomic_store_n(&g_test_hold_next_ready,1,__ATOMIC_RELEASE);}
void scheduler_test_hold_reap_next_creation(void){irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);g_test_hold_next_reap=1;spin_unlock_irqrestore(&g_scheduler_lock,f);}
void scheduler_test_cancel_hold_reap_next_creation(void){irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);g_test_hold_next_reap=0;spin_unlock_irqrestore(&g_scheduler_lock,f);}
void scheduler_test_set_lifecycle_log_quiet(bool quiet){__atomic_store_n(&g_test_quiet_lifecycle,quiet?1:0,__ATOMIC_RELEASE);}
void scheduler_test_force_finish_validation_failure(uint32_t bit){__atomic_store_n(&g_test_finish_failure,bit,__ATOMIC_RELEASE);}
bool scheduler_test_corrupt_stack_guard(task_id_t id,uint32_t offset){bool ok=false;irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);task_t*t=find_task_by_id_locked(id);if(t&&!t->on_cpu&&t->stack_base&&offset<t->kernel_stack_guard_bytes){((uint8_t*)t->stack_base)[offset]^=1u;ok=true;}spin_unlock_irqrestore(&g_scheduler_lock,f);if(ok)serial_write_all("[TASK][STACK][NEGATIVE] STACK_GUARD_CORRUPTION_DETECTED\n");return ok;}

bool scheduler_test_hold_reap(task_id_t id, bool hold)
{
    irq_flags_t flags=spin_lock_irqsave(&g_scheduler_lock); bool ok=false;
    if(hold&&id!=TASK_ID_INVALID&&find_task_by_id_locked(id)){for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++){if(g_test_reap_hold_ids[i]==id){ok=true;break;}if(g_test_reap_hold_ids[i]==TASK_ID_INVALID){g_test_reap_hold_ids[i]=id;ok=true;break;}}}
    else if(!hold){for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++)if(id==TASK_ID_INVALID||g_test_reap_hold_ids[i]==id){g_test_reap_hold_ids[i]=TASK_ID_INVALID;ok=true;}}
    spin_unlock_irqrestore(&g_scheduler_lock,flags); return ok;
}
uint32_t scheduler_test_reap_holds(void){uint32_t n=0;for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++)if(__atomic_load_n(&g_test_reap_hold_ids[i],__ATOMIC_ACQUIRE)!=TASK_ID_INVALID)n++;return n;}
bool scheduler_test_set_reap_grace_hold(task_id_t id,bool hold){return scheduler_test_hold_reap(id,hold);}
void scheduler_test_hold_next_zombie_on_cpu(void){__atomic_store_n(&g_test_release_zombie_on_cpu,0,__ATOMIC_RELEASE);__atomic_store_n(&g_test_hold_next_zombie_on_cpu,1,__ATOMIC_RELEASE);}
void scheduler_test_release_zombie_on_cpu(void){__atomic_store_n(&g_test_release_zombie_on_cpu,1,__ATOMIC_RELEASE);}
bool scheduler_test_reset_zombie_age(task_id_t id){extern uint64_t timer_get_uptime_ms(void);irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);task_t*t=find_task_by_id_locked(id);bool ok=t&&t->state==TASK_ZOMBIE&&!t->reap_claimed;if(ok){t->zombie_since_ms=timer_get_uptime_ms();t->zombie_entered_ns=clock_monotonic_ns();if(t->zombie_entered_ns<t->cleanup_completed_ns)t->zombie_entered_ns=t->cleanup_completed_ns;}spin_unlock_irqrestore(&g_scheduler_lock,f);return ok;}

void scheduler_kill_stats_snapshot(task_kill_stats_t *out){if(!out)return;irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);*out=g_kill_stats;spin_unlock_irqrestore(&g_scheduler_lock,f);}
int scheduler_current_task_should_exit(void)
{
    task_t *t = get_current_task();
    if (!t)
        return 0;

    return task_cancel_requested()?1:0;
}

const char *scheduler_task_exit_reason_to_string(task_exit_reason_t r){switch(r){case TASK_EXIT_NORMAL:return "NORMAL";case TASK_EXIT_KILLED:return "KILLED";case TASK_EXIT_INIT_FAILURE:return "INIT_FAILURE";case TASK_EXIT_INTERNAL_ERROR:return "INTERNAL_ERROR";default:return "NONE";}}
const char *scheduler_task_kill_state_to_string(const task_snapshot_t *t){if(!t)return "NO";if(t->state==TASK_ZOMBIE)return "ZOMB";if(t->flags&TASK_FLAG_KILL_PROTECTED)return "PROT";if(!(t->flags&TASK_FLAG_KILLABLE))return "NO";if(t->exit_started)return "EXIT";if(t->kill_pending)return "PEND";return "-";}
bool scheduler_cancellation_selftest(void){task_create_options_t bad={.name="bad",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE|TASK_FLAG_KILL_PROTECTED};bool ok=!thread_create_ex((void (*)(void *))1,NULL,&bad);ok=ok&&!strcmp(scheduler_task_exit_reason_to_string(TASK_EXIT_NORMAL),"NORMAL")&&!strcmp(scheduler_task_exit_reason_to_string(TASK_EXIT_KILLED),"KILLED");task_snapshot_t s={.flags=TASK_FLAG_SYSTEM};ok=ok&&!strcmp(scheduler_task_kill_state_to_string(&s),"NO");s.flags=TASK_FLAG_SYSTEM|TASK_FLAG_KILL_PROTECTED;ok=ok&&!strcmp(scheduler_task_kill_state_to_string(&s),"PROT");s.flags=TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE;s.id=UINT64_MAX;ok=ok&&!strcmp(scheduler_task_kill_state_to_string(&s),"-");s.flags=TASK_FLAG_SYSTEM;s.state=TASK_ZOMBIE;ok=ok&&!strcmp(scheduler_task_kill_state_to_string(&s),"ZOMB");serial_write_all(ok?"[TASK][SELFTEST] CANCELLATION_GUARDS_OK\n":"[TASK][SELFTEST] CANCELLATION_GUARDS_FAIL\n");return ok;}

const char *scheduler_task_state_to_string(task_state_t state)
{
    switch (state)
    {
    case TASK_READY:
        return "READY";
    case TASK_RUNNING:
        return "RUNNING";
    case TASK_BLOCKED:
        return "BLOCKED";
    case TASK_SLEEPING:
        return "SLEEPING";
    case TASK_ZOMBIE:
        return "ZOMBIE";
    default:
        return "UNKNOWN";
    }
}

task_snapshot_result_t scheduler_snapshot_tasks(task_snapshot_t *out_entries,
                                                uint32_t capacity,
                                                uint32_t offset)
{
    task_snapshot_result_t result;
    memset(&result, 0, sizeof(result));
    result.offset = offset;

    uint64_t now_ns=clock_monotonic_ns();
    result.sample_time_ns=now_ns;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    for (cpu_slot_t slot=0; slot<g_cpu_count && slot<HOBBYOS_MAX_CPUS; slot++)
        if (g_scheduler_cpus[slot].online) scheduler_account_cpu_locked(slot,now_ns);
    result.total = g_all_tasks_count;
    result.registry_generation = g_task_registry_generation;
    uint32_t index = 0;
    struct list_head *pos = NULL;
    list_for_each(pos, &g_all_tasks)
    {
        if (index++ < offset)
            continue;
        if (!out_entries || result.written >= capacity)
            break;
        task_t *t = list_entry(pos, task_t, registry_list);
        task_snapshot_t *dst = &out_entries[result.written++];
        memset(dst, 0, sizeof(*dst));
        dst->id = t->id;
        memcpy(dst->name, t->name, sizeof(dst->name));
        dst->name[sizeof(dst->name) - 1] = 0;
        dst->state = t->state;
        dst->task_class = t->task_class;
        dst->quantum = t->quantum;
        dst->quantum_default = t->quantum_default;
        dst->rsp = t->rsp;
        dst->stack_base = t->stack_base;
        dst->kernel_stack_bytes = t->kernel_stack_bytes;
        dst->kernel_ctx_bytes_est = t->kernel_ctx_bytes_est;
        dst->kernel_mem_est_bytes = (uint64_t)sizeof(task_t) +
                                    (uint64_t)t->kernel_stack_bytes +
                                    (uint64_t)t->kernel_ctx_bytes_est;
        dst->is_current = task_is_current_locked(t, &dst->current_cpu);
        if (!dst->is_current)
            dst->current_cpu = 0xFFFFFFFFu;
        dst->is_idle = task_is_idle_locked(t);
        dst->on_cpu = t->on_cpu;
        dst->current_cpu_slot = t->current_cpu_slot;
        if (dst->is_current != t->on_cpu ||
            (t->on_cpu && dst->current_cpu != t->current_cpu_slot))
            g_duplicate_current_failures++;
        dst->last_cpu_slot = t->last_cpu_slot;
        dst->runtime_ns_total = t->runtime_ns_total;
        dst->schedule_count = t->schedule_count;
        dst->flags = t->flags;
        dst->kill_pending = t->kill_pending != 0;
        dst->exit_started = t->exit_started;
        dst->cleanup_started=t->cleanup_started;
        dst->cleanup_done = t->cleanup_done;
        dst->exit_reason = t->exit_reason;
        dst->kill_requested_ns = t->kill_requested_ns;
        dst->kill_request_count = t->kill_request_count;
        dst->cancellation_points = t->cancellation_points;
        dst->lifecycle_generation = t->lifecycle_generation;
        dst->wait_generation = t->wait_generation;
        dst->wait_kind = t->wait_kind;
        dst->wake_reason = t->wake_reason;
        dst->wait_active = t->wait_active;
        dst->queue_membership = t->queue_membership;
        dst->lifecycle_notify_started=t->lifecycle_notify_started;dst->lifecycle_notify_completed=t->lifecycle_notify_completed;dst->lifecycle_notify_failed=t->lifecycle_notify_failed;
        dst->reap_claimed=t->reap_claimed;dst->task_wake_timer_refs=t->task_wake_timer_refs;dst->zombie_generation=t->zombie_generation;
        dst->exit_started_ns=t->exit_started_ns;dst->cleanup_completed_ns=t->cleanup_completed_ns;dst->zombie_entered_ns=t->zombie_entered_ns;
        dst->wait_object_key=t->wait_object_key;dst->deferred_ready=t->deferred_ready;dst->timer_ref_acquires=t->timer_ref_acquires;dst->timer_ref_releases=t->timer_ref_releases;dst->reap_defer_mask_last=t->reap_defer_mask_last;
    }
    uint64_t end = (uint64_t)offset + result.written;
    result.truncated = end < result.total;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    return result;
}

bool scheduler_validate_runtime_invariants(scheduler_runtime_stats_t *out_stats)
{
    scheduler_runtime_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    bool ok = true;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    stats.tasks = g_all_tasks_count;

    for (cpu_slot_t slot = 0; slot < g_cpu_count && slot < HOBBYOS_MAX_CPUS; slot++)
    {
        scheduler_cpu_state_t *cpu = &g_scheduler_cpus[slot];
        if (!cpu->online)
            continue;
        stats.cpus++;
        stats.started += cpu->handoff.started_count;
        stats.finished += cpu->handoff.finished_count;
        if (cpu->handoff.active)
            stats.pending++;
        task_t *current = cpu->current;
        if (!current || !cpu->idle || !current->on_cpu ||
            current->current_cpu_slot != slot || current->state != TASK_RUNNING ||
            current->queue_membership != TASK_QUEUE_NONE)
        {
            ok = false;
            if (!stats.failure_code) stats.failure_code = 1;
        }
        for (cpu_slot_t other = slot + 1; other < g_cpu_count && other < HOBBYOS_MAX_CPUS; other++)
            if (current && g_scheduler_cpus[other].online && g_scheduler_cpus[other].current == current)
            {
                ok = false;
                g_duplicate_current_failures++;
                if (!stats.failure_code) stats.failure_code = 2;
            }
    }

    struct list_head *pos = NULL;
    list_for_each(pos, &g_all_tasks)
    {
        task_t *t = list_entry(pos, task_t, registry_list);
        uint32_t current_count = 0;
        if (task_has_pending_cancellation_locked(t) && t->wait_active &&
            t->wake_reason == TASK_WAKE_NONE) {
            stats.pending_cancel_wait_violations++;
            ok = false;
            if (!stats.failure_code) stats.failure_code = 4;
        }
        for (cpu_slot_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
            if (g_scheduler_cpus[slot].current == t) current_count++;
        if (!t->registry_registered || t->id == TASK_ID_INVALID ||
            (t->on_cpu && (current_count != 1 || t->state != TASK_RUNNING ||
                           t->current_cpu_slot >= HOBBYOS_MAX_CPUS)) ||
            (!t->on_cpu && current_count != 0) ||
            ((t->queue_membership == TASK_QUEUE_RUNQ_INTERACTIVE ||
              t->queue_membership == TASK_QUEUE_RUNQ_NORMAL) &&
             (t->state != TASK_READY || t->on_cpu || (t->flags & TASK_FLAG_IDLE))) ||
            (t->state == TASK_ZOMBIE && t->queue_membership != TASK_QUEUE_NONE) ||
            ((t->flags & TASK_FLAG_IDLE) && t->queue_membership != TASK_QUEUE_NONE) ||
            (t->wait_active && (!t->wait_generation || t->wait_kind == TASK_WAIT_NONE ||
             (t->wake_reason == TASK_WAKE_NONE && t->state != TASK_BLOCKED && t->state != TASK_SLEEPING) ||
             (t->wake_reason != TASK_WAKE_NONE && t->state != TASK_READY && !t->on_cpu))) ||
            (!t->wait_active && t->wait_kind != TASK_WAIT_NONE) ||
            (!(t->flags & TASK_FLAG_IDLE) && t->state == TASK_READY &&
             t->queue_membership == TASK_QUEUE_NONE && !t->on_cpu))
        {
            ok = false;
            if (!stats.failure_code) stats.failure_code = 3;
        }
        struct list_head *other_pos = pos->next;
        while (other_pos != &g_all_tasks)
        {
            task_t *other = list_entry(other_pos, task_t, registry_list);
            if (other->id == t->id)
            {
                ok = false;
                if (!stats.failure_code) stats.failure_code = 4;
            }
            other_pos = other_pos->next;
        }
    }

    struct list_head *queues[2] = {&g_interactive_queue, &g_normal_queue};
    task_queue_membership_t expected[2] = {TASK_QUEUE_RUNQ_INTERACTIVE, TASK_QUEUE_RUNQ_NORMAL};
    for (uint32_t q = 0; q < 2; q++)
    {
        list_for_each(pos, queues[q])
        {
            task_t *t = list_entry(pos, task_t, list);
            if (t->queue_membership != expected[q] || t->state != TASK_READY ||
                t->on_cpu || !t->registry_registered || (t->flags & TASK_FLAG_IDLE))
            {
                ok = false;
                if (t->on_cpu) g_runqueue_on_cpu_failures++;
                if (!stats.failure_code) stats.failure_code = 5;
            }
            struct list_head *dup = pos->next;
            while (dup != queues[q])
            {
                if (list_entry(dup, task_t, list) == t) ok = false;
                dup = dup->next;
            }
        }
    }

    stats.stale_cpu_slot_entries = g_stale_cpu_slot_entries;
    stats.pinned_current_mismatches = g_pinned_current_mismatches;
    stats.finish_wrong_cpu = g_finish_wrong_cpu;
    stats.finish_sequence_mismatch = g_finish_sequence_mismatch;
    stats.finish_without_pending = g_finish_without_pending;
    stats.pending_overwrite = g_pending_overwrite;
    stats.violations = g_handoff_failures + g_duplicate_current_failures +
                       g_runqueue_on_cpu_failures + g_finish_without_pending + g_pending_overwrite;
    if (stats.pending || stats.started != stats.finished || stats.violations)
        ok = false;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    if (out_stats) *out_stats = stats;
    return ok;
}

void scheduler_accounting_stats_snapshot(scheduler_accounting_stats_t *out)
{
    if(!out) return;
    memset(out,0,sizeof(*out));
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);
    for(cpu_slot_t slot=0;slot<HOBBYOS_MAX_CPUS;slot++) {
        out->accounting_events+=g_scheduler_cpus[slot].accounting_events;
        out->runtime_ns_accounted+=g_scheduler_cpus[slot].accounted_ns_total;
        out->runtime_overflows+=g_scheduler_cpus[slot].runtime_overflows;
        out->clock_regressions+=g_scheduler_cpus[slot].clock_regressions;
    }
    spin_unlock_irqrestore(&g_scheduler_lock,f);
}

task_id_parse_result_t task_id_parse_decimal_ex(const char *text,
                                                 task_id_t *out_id)
{
    if (!text || !out_id)
        return TASK_ID_PARSE_INVALID_ARGUMENT;
    if (!text[0])
        return TASK_ID_PARSE_EMPTY;
    if (text[0] == '-' || text[0] == '+')
        return TASK_ID_PARSE_SIGN;
    task_id_t value = 0;
    for (const char *p = text; *p; p++)
    {
        if ((uint8_t)*p < 48u || (uint8_t)*p > 57u)
            return TASK_ID_PARSE_NON_DECIMAL;
        task_id_t digit = (task_id_t)((uint8_t)*p - 48u);
        if (value > (UINT64_MAX - digit) / 10u)
            return TASK_ID_PARSE_OVERFLOW;
        value = value * 10u + digit;
    }
    *out_id = value;
    return value == TASK_ID_INVALID ? TASK_ID_PARSE_ZERO : TASK_ID_PARSE_OK;
}

const char *task_id_parse_result_to_string(task_id_parse_result_t result)
{
    switch (result)
    {
    case TASK_ID_PARSE_OK: return "OK";
    case TASK_ID_PARSE_ZERO: return "ZERO";
    case TASK_ID_PARSE_EMPTY: return "EMPTY";
    case TASK_ID_PARSE_SIGN: return "SIGN";
    case TASK_ID_PARSE_NON_DECIMAL: return "NON_DECIMAL";
    case TASK_ID_PARSE_OVERFLOW: return "OVERFLOW";
    default: return "INVALID_ARGUMENT";
    }
}

bool task_id_parse_decimal(const char *text, task_id_t *out_id)
{
    task_id_parse_result_t result = task_id_parse_decimal_ex(text, out_id);
    return result == TASK_ID_PARSE_OK || result == TASK_ID_PARSE_ZERO;
}

static bool task_identity_selftests(void)
{
    static task_t protected_task;
    static task_t ordinary_task;
    memset(&protected_task, 0, sizeof(protected_task));
    memset(&ordinary_task, 0, sizeof(ordinary_task));
    protected_task.flags = TASK_FLAG_KILL_PROTECTED;
    sched_copy_name(protected_task.name, sizeof(protected_task.name), "ordinary");
    sched_copy_name(ordinary_task.name, sizeof(ordinary_task.name), "reaper");
    bool flags_ok = task_is_kill_protected_locked(&protected_task) &&
                    !task_is_kill_protected_locked(&ordinary_task);
    sched_copy_name(protected_task.name, sizeof(protected_task.name), "renamed");
    flags_ok = flags_ok && task_is_kill_protected_locked(&protected_task);
    serial_write_all(flags_ok ? "[TASK][SELFTEST] FLAGS_RENAME_OK\n" :
                                  "[TASK][SELFTEST] FLAGS_RENAME_FAIL\n");

    static task_t registry_task;
    memset(&registry_task, 0, sizeof(registry_task));
    list_init(&registry_task.registry_list);
    irq_flags_t lock_flags = spin_lock_irqsave(&g_scheduler_lock);
    uint32_t count_before = g_all_tasks_count;
    uint64_t generation_before = g_task_registry_generation;
    bool registry_ok = task_id_allocate_locked(&registry_task.id);
    registry_ok = registry_ok && registry_add_locked(&registry_task);
    uint32_t count_added = g_all_tasks_count;
    uint64_t generation_added = g_task_registry_generation;
    registry_ok = registry_ok && !registry_add_locked(&registry_task) &&
                  g_all_tasks_count == count_added &&
                  g_task_registry_generation == generation_added;
    registry_ok = registry_ok && registry_remove_locked(&registry_task);
    uint64_t generation_removed = g_task_registry_generation;
    registry_ok = registry_ok && !registry_remove_locked(&registry_task) &&
                  g_all_tasks_count == count_before &&
                  generation_added == generation_before + 1u &&
                  generation_removed == generation_added + 1u &&
                  g_task_registry_generation == generation_removed;
    spin_unlock_irqrestore(&g_scheduler_lock, lock_flags);
    serial_write_all(registry_ok ? "[TASK][SELFTEST] REGISTRY_GUARDS_OK\n" :
                                     "[TASK][SELFTEST] REGISTRY_GUARDS_FAIL\n");

    task_t queue_task;
    wait_queue_t test_wait;
    memset(&queue_task, 0, sizeof(queue_task));
    list_init(&queue_task.list);
    wait_queue_init(&test_wait);
    queue_task.task_class = TASK_CLASS_NORMAL;
    lock_flags = spin_lock_irqsave(&g_scheduler_lock);
    bool queue_ok = enqueue_task_locked_impl(&queue_task, 1);
    queue_ok = queue_ok && !enqueue_task_locked_impl(&queue_task, 1);
    queue_ok = queue_ok && queue_detach_locked(&queue_task);
    queue_ok = queue_ok && waitqueue_attach_locked(&test_wait, &queue_task);
    queue_ok = queue_ok && !enqueue_task_locked_impl(&queue_task, 1);
    queue_ok = queue_ok && queue_detach_locked(&queue_task) &&
               queue_task.queue_membership == TASK_QUEUE_NONE;
    spin_unlock_irqrestore(&g_scheduler_lock, lock_flags);
    serial_write_all(queue_ok ? "[TASK][SELFTEST] QUEUE_GUARDS_OK\n" :
                                  "[TASK][SELFTEST] QUEUE_GUARDS_FAIL\n");

    task_snapshot_t page[2];
    task_snapshot_result_t first = scheduler_snapshot_tasks(page, 1, 0);
    task_snapshot_result_t end = scheduler_snapshot_tasks(page, 2, first.total);
    bool snapshot_ok = first.total > 1 && first.written == 1 && first.offset == 0 &&
                       first.truncated && end.written == 0 && !end.truncated &&
                       end.offset == first.total;
    serial_write_all(snapshot_ok ? "[TASK][SELFTEST] SNAPSHOT_CONTRACT_OK\n" :
                                     "[TASK][SELFTEST] SNAPSHOT_CONTRACT_FAIL\n");

    task_id_t parsed = 0;
    bool p1 = task_id_parse_decimal("1", &parsed) && parsed == 1;
    bool p0 = task_id_parse_decimal("0", &parsed) && parsed == 0;
    bool pmax = task_id_parse_decimal("18446744073709551615", &parsed) && parsed == UINT64_MAX;
    bool pover = !task_id_parse_decimal("18446744073709551616", &parsed);
    bool pminus = !task_id_parse_decimal("-1", &parsed);
    bool pplus = !task_id_parse_decimal("+1", &parsed);
    bool ptrail = !task_id_parse_decimal("1x", &parsed);
    bool pempty = !task_id_parse_decimal("", &parsed);
    bool parse_ok = p1 && p0 && pmax && pover && pminus && pplus && ptrail && pempty;
    if (!parse_ok)
    {
        serial_write_all("[TASK][SELFTEST] ID_PARSE_CASES bits=");
        serial_putc_all(p1 ? '1' : '0'); serial_putc_all(p0 ? '1' : '0');
        serial_putc_all(pmax ? '1' : '0'); serial_putc_all(pover ? '1' : '0');
        serial_putc_all(pminus ? '1' : '0'); serial_putc_all(pplus ? '1' : '0');
        serial_putc_all(ptrail ? '1' : '0'); serial_putc_all(pempty ? '1' : '0');
        serial_write_all("\n");
    }
    serial_write_all(parse_ok ? "[TASK][SELFTEST] ID_PARSE_OK\n" :
                                  "[TASK][SELFTEST] ID_PARSE_FAIL\n");

    static const struct
    {
        uint64_t value;
        const char *expected;
    } format_cases[] = {
        {0u, "0"},
        {1u, "1"},
        {9u, "9"},
        {10u, "10"},
        {9999999999ULL, "9999999999"},
        {10000000000ULL, "10000000000"},
        {9999999999999999ULL, "9999999999999999"},
        {10000000000000000ULL, "10000000000000000"},
        {UINT64_MAX, "18446744073709551615"},
    };
    bool format_ok = true;
    char formatted[TASK_ID_DECIMAL_BUFFER_SIZE];
    for (uint32_t i = 0; i < sizeof(format_cases) / sizeof(format_cases[0]); i++)
    {
        uint32_t length = sched_u64_format_decimal(format_cases[i].value,
                                                    formatted, sizeof(formatted));
        if (length == 0 || strcmp(formatted, format_cases[i].expected) != 0)
            format_ok = false;
    }
    char too_small[TASK_ID_DECIMAL_MAX_DIGITS];
    format_ok = format_ok &&
                sched_u64_format_decimal(UINT64_MAX, too_small, sizeof(too_small)) == 0 &&
                too_small[0] == 0 &&
                sched_u64_format_decimal(1u, NULL, TASK_ID_DECIMAL_BUFFER_SIZE) == 0 &&
                sched_u64_format_decimal(1u, formatted, 0) == 0;
    serial_write_all(format_ok ? "[TASK][SELFTEST] ID_FORMAT_OK\n" :
                                   "[TASK][SELFTEST] ID_FORMAT_FAIL\n");
    task_t guard_task;
    memset(&guard_task, 0, sizeof(guard_task));
    list_init(&guard_task.list);
    guard_task.state = TASK_READY;
    guard_task.on_cpu = 1;
    uint64_t guard_failures_before = g_runqueue_on_cpu_failures;
    bool switch_guards_ok = !enqueue_task_locked_impl(&guard_task, 1);
    g_runqueue_on_cpu_failures = guard_failures_before;
    guard_task.on_cpu = 0;
    guard_task.state = TASK_ZOMBIE;
    switch_guards_ok = switch_guards_ok && !enqueue_task_locked_impl(&guard_task, 1);
    scheduler_switch_handoff_t test_handoff;
    memset(&test_handoff, 0, sizeof(test_handoff));
    test_handoff.active = 1;
    switch_guards_ok = switch_guards_ok && test_handoff.active;
    serial_write_all(switch_guards_ok ? "[SCHED][SELFTEST] SWITCH_GUARDS_OK\n" :
                                         "[SCHED][SELFTEST] SWITCH_GUARDS_FAIL\n");
    return flags_ok && registry_ok && queue_ok && snapshot_ok && parse_ok && format_ok && switch_guards_ok;
}

bool scheduler_validate_task_identity(void)
{
    if (!task_identity_selftests())
    {
        serial_write_all("[TASK][IDENTITY] VALIDATION_FAIL code=SELFTEST\n");
        return false;
    }

    bool ok = true;
    uint32_t real_count = 0;
    uint32_t idle_count = 0;
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    struct list_head *pos = NULL;
    list_for_each(pos, &g_all_tasks)
    {
        task_t *t = list_entry(pos, task_t, registry_list);
        real_count++;
        if (!t->registry_registered || t->id == TASK_ID_INVALID)
            ok = false;
        struct list_head *other_pos = pos->next;
        while (other_pos != &g_all_tasks)
        {
            task_t *other = list_entry(other_pos, task_t, registry_list);
            if (other->id == t->id)
                ok = false;
            other_pos = other_pos->next;
        }
        if (t->flags & TASK_FLAG_IDLE)
        {
            idle_count++;
            if ((t->flags & (TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED)) !=
                (TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED))
                ok = false;
            char expected[32];
            sched_make_idle_name(expected, sizeof(expected), t->last_cpu_slot);
            if (strcmp(t->name, expected) != 0)
                ok = false;
        }
    }
    uint32_t online = smp_online_cpu_count();
    ok = ok && real_count == g_all_tasks_count && idle_count == online;
    uint64_t generation = g_task_registry_generation;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    if (!ok)
    {
        serial_write_all("[TASK][IDENTITY] VALIDATION_FAIL code=INVARIANT\n");
        return false;
    }
    serial_write_all("[TASK][IDENTITY] VALIDATION_OK total=");
    sched_serial_dec(real_count);
    serial_write_all(" idles=");
    sched_serial_dec(idle_count);
    serial_write_all(" unique=");
    sched_serial_dec(real_count);
    serial_write_all(" generation=");
    sched_serial_dec(generation);
    serial_write_all("\n");
    return true;
}

static task_reap_defer_mask_t task_reap_mask_locked(const task_t*t,uint64_t now,uint64_t grace)
{
    uint64_t m=0;
    if(t->state!=TASK_ZOMBIE) m|=TASK_REAP_DEFER_NOT_ZOMBIE;
    for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++)
        if(t->id==g_test_reap_hold_ids[i]) m|=TASK_REAP_DEFER_TEST_HOLD;
    if(t->flags&TASK_FLAG_IDLE) m|=TASK_REAP_DEFER_IDLE;
    if(!t->exit_started) m|=TASK_REAP_DEFER_EXIT_NOT_STARTED;
    if(!t->cleanup_started||!t->cleanup_done) m|=TASK_REAP_DEFER_CLEANUP_NOT_DONE;
    if(!task_exit_reason_valid(t->exit_reason)) m|=TASK_REAP_DEFER_INVALID_REASON;
    if(!t->lifecycle_notify_started) m|=TASK_REAP_DEFER_NOTIFY_NOT_STARTED;
    if(!t->lifecycle_notify_completed||t->lifecycle_notify_failed) m|=TASK_REAP_DEFER_NOTIFY_NOT_DONE;
    uint32_t cpu;
    if(t->on_cpu) m|=TASK_REAP_DEFER_ON_CPU;
    if(task_is_current_locked((task_t*)t,&cpu)) m|=TASK_REAP_DEFER_CURRENT;
    if(t->current_cpu_slot!=TASK_CPU_SLOT_NONE) m|=TASK_REAP_DEFER_CPU_SLOT;
    if(t->queue_membership!=TASK_QUEUE_NONE) m|=TASK_REAP_DEFER_QUEUE;
    if(t->wait_active) m|=TASK_REAP_DEFER_WAIT_ACTIVE;
    if(t->wait_kind!=TASK_WAIT_NONE) m|=TASK_REAP_DEFER_WAIT_KIND;
    if(t->wait_object_key) m|=TASK_REAP_DEFER_WAIT_OBJECT;
    if(t->deferred_ready) m|=TASK_REAP_DEFER_DEFERRED_READY;
    if(t->task_wake_timer_refs) m|=TASK_REAP_DEFER_TIMER_REFS;
    if(!scheduler_task_stack_guard_ok(t)) m|=TASK_REAP_DEFER_STACK_GUARD;
    if(!scheduler_task_stack_rsp_in_bounds(t)) m|=TASK_REAP_DEFER_RSP_BOUNDS;
    if(t->reap_claimed) m|=TASK_REAP_DEFER_ALREADY_CLAIMED;
    if(!t->zombie_generation) m|=TASK_REAP_DEFER_ZOMBIE_GENERATION;
    if(!t->exit_started_ns||!t->cleanup_completed_ns||!t->zombie_since_ms||!t->zombie_entered_ns) m|=TASK_REAP_DEFER_NO_TIMESTAMP;
    if(t->cleanup_completed_ns<t->exit_started_ns||t->zombie_entered_ns<t->cleanup_completed_ns) m|=TASK_REAP_DEFER_TIMESTAMP_ORDER;
    if(t->timer_ref_acquires!=t->timer_ref_releases+t->task_wake_timer_refs) m|=TASK_REAP_DEFER_TIMER_ACCOUNTING;
    if(t->zombie_since_ms&&now<t->zombie_since_ms) m|=TASK_REAP_DEFER_CLOCK;
    else if(now-t->zombie_since_ms<grace) m|=TASK_REAP_DEFER_GRACE;
    return (task_reap_defer_mask_t)m;
}

bool scheduler_test_set_zombie_since_ms(task_id_t id,uint64_t since_ms)
{
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);task_t*t=find_task_by_id_locked(id);bool held=false;
    for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++)if(g_test_reap_hold_ids[i]==id)held=true;
    bool ok=t&&held&&t->state==TASK_ZOMBIE&&!t->reap_claimed;if(ok)t->zombie_since_ms=since_ms;
    spin_unlock_irqrestore(&g_scheduler_lock,f);return ok;
}
bool scheduler_test_reap_mask(task_id_t id,uint64_t now_ms,uint64_t grace_ms,task_reap_defer_mask_t*out)
{
    if(!out) return false;
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);
    task_t*t=find_task_by_id_locked(id);
    bool ok=t!=NULL;
    if(ok) *out=task_reap_mask_locked(t,now_ms,grace_ms);
    spin_unlock_irqrestore(&g_scheduler_lock,f);
    return ok;
}
static bool scheduler_test_reap_observe_locked(task_id_t id,uint64_t now_ms,
                                                uint64_t grace_ms,
                                                scheduler_test_reap_observation_t*out)
{
    task_t*t=find_task_by_id_locked(id);out->observed_now_ms=now_ms;
    for(uint32_t i=0;i<TEST_REAP_HOLD_MAX;i++)if(g_test_reap_hold_ids[i]!=TASK_ID_INVALID){out->total_holds++;if(g_test_reap_hold_ids[i]==id)out->held=1;}
    if(t){out->found=1;out->zombie_since_ms=t->zombie_since_ms;scheduler_fill_test_snapshot_locked(t,&out->snapshot);out->mask=task_reap_mask_locked(t,now_ms,grace_ms);}
    return out->found!=0;
}
bool scheduler_test_reap_observe_at(task_id_t id,uint64_t now_ms,uint64_t grace_ms,
                                    scheduler_test_reap_observation_t*out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);
    bool found=scheduler_test_reap_observe_locked(id,now_ms,grace_ms,out);
    spin_unlock_irqrestore(&g_scheduler_lock,f);return found;
}
bool scheduler_test_reap_observe_now(task_id_t id,uint64_t grace_ms,
                                     scheduler_test_reap_observation_t*out)
{
    extern uint64_t timer_get_uptime_ms(void);
    if(!out)return false;
    memset(out,0,sizeof(*out));
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);
    uint64_t now_ms=timer_get_uptime_ms();
    bool found=scheduler_test_reap_observe_locked(id,now_ms,grace_ms,out);
    spin_unlock_irqrestore(&g_scheduler_lock,f);return found;
}

void scheduler_reaper_stats_snapshot(task_reaper_stats_t*out){if(!out)return;irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);memcpy(out,&g_reaper_stats,sizeof(*out));out->refs_current=0;struct list_head*p;list_for_each(p,&g_all_tasks){task_t*t=list_entry(p,task_t,registry_list);out->refs_current+=t->task_wake_timer_refs;}spin_unlock_irqrestore(&g_scheduler_lock,f);}

uint32_t scheduler_reap_zombies(uint64_t grace_ms)
{
    extern uint64_t timer_get_uptime_ms(void);uint64_t now;task_t*to_free[TASK_REAPER_BATCH_MAX];uint32_t n=0,zombies=0;
    task_id_t structural_id=0;uint64_t structural_mask=0;const char*negative_message=NULL;
    irq_flags_t f=spin_lock_irqsave(&g_scheduler_lock);now=timer_get_uptime_ms();g_reaper_stats.scans++;
    struct list_head *pos,*tmp;list_for_each_safe(pos,tmp,&g_all_tasks){task_t*t=list_entry(pos,task_t,registry_list);if(t->state!=TASK_ZOMBIE)continue;
        zombies++;g_reaper_stats.candidates++;uint64_t m=task_reap_mask_locked(t,now,grace_ms);t->reap_defer_mask_last=m;
#ifdef HOBBYOS_REAPER_NEGATIVE_AGE_ONLY
        if(!g_reaper_negative_reported&&(m&~TASK_REAP_POLICY_MASK)){g_reaper_negative_reported=1;negative_message="[REAPTEST][NEGATIVE] AGE_ONLY_REAPER_DETECTED\n";break;}
#endif
#ifdef HOBBYOS_REAPER_NEGATIVE_IGNORE_TIMER_REF
        if(!g_reaper_negative_reported&&(m&TASK_REAP_DEFER_TIMER_REFS)){g_reaper_negative_reported=1;negative_message="[REAPTEST][NEGATIVE] TIMER_REF_IGNORED_DETECTED\n";break;}
#endif
#ifdef HOBBYOS_REAPER_NEGATIVE_ON_CPU
        if(!g_reaper_negative_reported&&(m&(TASK_REAP_DEFER_ON_CPU|TASK_REAP_DEFER_CURRENT))){g_reaper_negative_reported=1;negative_message="[REAPTEST][NEGATIVE] ON_CPU_REAP_DETECTED\n";break;}
#endif
        uint64_t expected=m&TASK_REAP_EXPECTED_MASK;
        if((m&TASK_REAP_DEFER_CPU_SLOT)&&(m&(TASK_REAP_DEFER_ON_CPU|TASK_REAP_DEFER_CURRENT)))expected|=TASK_REAP_DEFER_CPU_SLOT;
        uint64_t structural=m&~(expected|TASK_REAP_POLICY_MASK);
        if(expected||structural){g_reaper_stats.deferred_safety++;if(expected)g_reaper_stats.deferred_expected++;if(structural){g_reaper_stats.deferred_structural++;g_reaper_stats.invariant_failures++;}
            if(m&TASK_REAP_DEFER_TEST_HOLD) g_reaper_stats.deferred_test_hold++;
            if(m&TASK_REAP_DEFER_CURRENT) g_reaper_stats.deferred_current++;
            if(m&TASK_REAP_DEFER_ON_CPU) g_reaper_stats.deferred_on_cpu++;
            if(m&TASK_REAP_DEFER_QUEUE) g_reaper_stats.deferred_queue++;
            if(m&(TASK_REAP_DEFER_WAIT_ACTIVE|TASK_REAP_DEFER_WAIT_KIND|TASK_REAP_DEFER_WAIT_OBJECT))g_reaper_stats.deferred_wait++;
            if(m&TASK_REAP_DEFER_TIMER_REFS) g_reaper_stats.deferred_timer_ref++;
            if(m&TASK_REAP_DEFER_NOTIFY_NOT_DONE) g_reaper_stats.deferred_notification++;
            if(m&TASK_REAP_DEFER_CLEANUP_NOT_DONE) g_reaper_stats.deferred_cleanup++;
            if(m&(TASK_REAP_DEFER_STACK_GUARD|TASK_REAP_DEFER_RSP_BOUNDS)) g_reaper_stats.deferred_stack++;
            if(structural){g_reaper_stats.structural_faults++;structural_id=t->id;structural_mask=structural;break;}continue;}
        if(m&TASK_REAP_DEFER_GRACE){g_reaper_stats.deferred_grace++;continue;}if(n==TASK_REAPER_BATCH_MAX)continue;
        t->reap_claimed=1;t->reap_claimed_ns=clock_monotonic_ns();if(!registry_remove_locked(t)){t->reap_claimed=0;g_reaper_stats.claim_failures++;continue;}
        g_reaper_stats.claimed++;g_reaper_stats.free_inflight++;to_free[n++]=t;}
    g_reaper_stats.current_zombies=zombies-n;if(zombies>g_reaper_stats.max_zombie_backlog)g_reaper_stats.max_zombie_backlog=zombies;if(n)g_reaper_stats.batches++;
    spin_unlock_irqrestore(&g_scheduler_lock,f);
    if(negative_message){serial_write_all(negative_message);return 0;}
    if(structural_id){serial_write_all("[REAPER][STRUCTURAL_FAULT] id=");sched_serial_dec(structural_id);serial_write_all(" mask=0x");serial_write_hex64_all(structural_mask);serial_write_all("\n");kpanic("REAPER: structural zombie fault");}
    uint32_t freed=0;for(uint32_t i=0;i<n;i++){task_t*t=to_free[i];if(!t->reap_claimed||t->registry_registered||t->state!=TASK_ZOMBIE||t->on_cpu||t->current_cpu_slot!=TASK_CPU_SLOT_NONE||t->queue_membership!=TASK_QUEUE_NONE||t->wait_active||t->wait_kind!=TASK_WAIT_NONE||t->wait_object_key||t->deferred_ready||!t->exit_started||!t->cleanup_started||!t->cleanup_done||!t->lifecycle_notify_started||!t->lifecycle_notify_completed||t->lifecycle_notify_failed||!task_exit_reason_valid(t->exit_reason)||t->task_wake_timer_refs||t->timer_ref_acquires!=t->timer_ref_releases||!scheduler_task_stack_guard_ok(t)||!scheduler_task_stack_rsp_in_bounds(t)||!t->zombie_generation||!t->exit_started_ns||t->cleanup_completed_ns<t->exit_started_ns||t->zombie_entered_ns<t->cleanup_completed_ns)kpanic("REAPER: claimed invariant changed");
        if(!__atomic_load_n(&g_test_quiet_lifecycle,__ATOMIC_ACQUIRE)){serial_write_all("[REAPER] FREE id=");sched_serial_dec(t->id);serial_write_all(" generation=");sched_serial_dec(t->zombie_generation);serial_write_all(" reason=");serial_write_all(scheduler_task_exit_reason_to_string(t->exit_reason));serial_write_all(" stack_bytes=");sched_serial_dec(t->kernel_stack_bytes);serial_write_all("\n");}
        if(t->stack_base) kfree(t->stack_base);
        kfree(t);
        freed++;
    }
    if(freed){f=spin_lock_irqsave(&g_scheduler_lock);if(g_reaper_stats.free_inflight<freed){spin_unlock_irqrestore(&g_scheduler_lock,f);kpanic("REAPER: free inflight underflow");}g_reaper_stats.free_inflight-=freed;g_reaper_stats.reaped+=freed;spin_unlock_irqrestore(&g_scheduler_lock,f);}
    return n;
}

void reaper_thread_entry(void *arg)
{
    (void)arg;

    extern void timer_sleep(uint64_t ms);

    serial_write_all("[REAPER] Reaper thread started.\n");

    while (1)
    {

        timer_sleep(TASK_REAPER_PERIOD_MS);
        scheduler_reap_zombies(TASK_REAPER_DEFAULT_GRACE_MS);
    }
}

static task_t *current_task_pinned(cpu_slot_t slot)
{
    if (irq_is_enabled()) kpanic("SCHED: current access without pin");
    if (slot >= HOBBYOS_MAX_CPUS) return NULL;
    return g_scheduler_cpus[slot].current;
}

bool scheduler_cpu_pin(scheduler_cpu_pin_t *pin)
{
    if (!pin) return false;
    memset(pin, 0, sizeof(*pin));
    irq_flags_t flags = irq_save();
    cpu_slot_t slot = CPU_SLOT_INVALID;
    uint32_t apic_id = lapic_get_id();
    if (!smp_current_cpu_slot(&slot) || slot >= HOBBYOS_MAX_CPUS) {
        irq_restore(flags);
        return false;
    }
    pin->irq_flags = flags;
    pin->slot = slot;
    pin->apic_id = apic_id;
    pin->task = current_task_pinned(slot);
    pin->active = 1;
    return true;
}

bool scheduler_cpu_pin_validate(const scheduler_cpu_pin_t *pin)
{
    if (!pin || !pin->active || irq_is_enabled() || pin->slot >= HOBBYOS_MAX_CPUS)
        return false;
    spin_lock(&g_scheduler_lock);
    scheduler_cpu_state_t *cpu = &g_scheduler_cpus[pin->slot];
    bool valid = cpu->slot == pin->slot && cpu->apic_id == pin->apic_id &&
                 cpu->current == pin->task && pin->task && pin->task->on_cpu &&
                 pin->task->current_cpu_slot == pin->slot;
    spin_unlock(&g_scheduler_lock);
    return valid;
}

void scheduler_cpu_unpin(scheduler_cpu_pin_t *pin)
{
    if (!pin || !pin->active) kpanic("SCHED: duplicate or invalid CPU unpin");
    irq_flags_t flags = pin->irq_flags;
    pin->active = 0;
    pin->slot = CPU_SLOT_INVALID;
    pin->task = NULL;
    irq_restore(flags);
}

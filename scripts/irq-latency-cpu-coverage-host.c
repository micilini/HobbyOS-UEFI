/* The runner extracts the current scheduler selector; CPU identity and the
 * already locked queue-detach operation are explicit hosted substitutes. */
#include "../kernel/src/core/scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct list_head g_interactive_queue, g_normal_queue;
static uint64_t g_runqueue_on_cpu_failures;
static cpu_slot_t host_cpu;
static bool identified = true;
static unsigned queries;
bool smp_current_cpu_slot(cpu_slot_t *out)
{ ++queries; if (!identified) return false; *out = host_cpu; return true; }
static bool task_is_current_locked(task_t *t, uint32_t *cpu)
{ (void)t; (void)cpu; return false; }
static bool queue_detach_locked(task_t *t)
{
    assert(t->queue_membership != TASK_QUEUE_NONE);
    list_del(&t->list); list_init(&t->list);
    t->queue_membership = TASK_QUEUE_NONE; return true;
}
#include "scheduler-selector.host.c"

static task_t tasks[64];
static void reset(void)
{
    memset(tasks, 0, sizeof(tasks));
    list_init(&g_interactive_queue); list_init(&g_normal_queue);
    queries = 0; identified = true; host_cpu = 0;
}
static void enqueue(unsigned index, unsigned target, bool interactive)
{
    task_t *t = &tasks[index];
    t->id = index + 1; t->state = TASK_READY; t->registry_registered = 1;
    t->test_cpu_plus_one = target;
    t->queue_membership = interactive ? TASK_QUEUE_RUNQ_INTERACTIVE : TASK_QUEUE_RUNQ_NORMAL;
    list_add_tail(&t->list, interactive ? &g_interactive_queue : &g_normal_queue);
}
int main(void)
{
    reset(); enqueue(0, 0, false); enqueue(1, 0, false);
    assert(runqueue_take_next_locked() == &tasks[0]);
    assert(runqueue_take_next_locked() == &tasks[1] && !queries);
    reset();
    for (unsigned cpu = 0; cpu < 24; ++cpu) enqueue(cpu, cpu + 1, false);
    enqueue(24, 0, false);
    for (host_cpu = 23; host_cpu < 24; --host_cpu) {
        assert(runqueue_take_next_locked() == &tasks[host_cpu]);
        assert(tasks[host_cpu].test_cpu_plus_one == host_cpu + 1);
    }
    assert(runqueue_take_next_locked() == &tasks[24]);
    assert(!runqueue_take_next_locked());
    reset(); enqueue(0, 2, true); enqueue(1, 0, false);
    assert(runqueue_take_next_locked() == &tasks[1]);
    assert(!runqueue_take_next_locked() && tasks[0].queue_membership != TASK_QUEUE_NONE);
    host_cpu = 1; assert(runqueue_take_next_locked() == &tasks[0]);
    reset(); enqueue(0, 2, false); enqueue(1, 1, false);
    identified = false;
    assert(!runqueue_take_next_locked());
    assert(tasks[0].queue_membership != TASK_QUEUE_NONE && tasks[1].queue_membership != TASK_QUEUE_NONE);
    identified = true; assert(runqueue_take_next_locked() == &tasks[1]);
    reset(); enqueue(0, 2, false); enqueue(1, 1, false);
    tasks[0].test_ready_hold = 1;
    assert(!runqueue_take_next_locked());
    reset(); enqueue(0, 1, false); enqueue(1, 1, false);
    tasks[0].on_cpu = 1;
    assert(runqueue_take_next_locked() == &tasks[1] && g_runqueue_on_cpu_failures == 1);
    puts("IRQ coverage actual scheduler selection: PASS target_identity=CHECKED ordinary_FIFO=RETAINED");
}

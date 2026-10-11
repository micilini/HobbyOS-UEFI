/* Actual coverage controller with synchronous hosted tasks. This verifies
 * ownership and refusal decisions, not real scheduler execution or IRQ time. */
#include "../kernel/src/memory/irq_latency_stress.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

uint32_t g_cpu_count;
static task_t host_tasks[70];
static task_t *current_task;
static unsigned created, emitted, sleeps;
static uint32_t current_cpu;
static bool host_if = true, wrong_cpu, wrong_generation, bad_clock, fail_create;
static uint64_t now;
static char last_line[1024];

bool irq_is_enabled(void) { return host_if; }
bool scheduler_cpu_pin(scheduler_cpu_pin_t *pin)
{
    assert(host_if && current_task);
    *pin = (scheduler_cpu_pin_t){.irq_flags=512, .slot=current_cpu,
        .task=current_task, .active=1};
    host_if=false; return true;
}
bool scheduler_cpu_pin_validate(const scheduler_cpu_pin_t *pin)
{ return !host_if && pin->task==current_task && pin->slot==current_cpu; }
void scheduler_cpu_unpin(scheduler_cpu_pin_t *pin)
{ assert(!host_if && pin->irq_flags==512); host_if=true; }
bool clock_monotonic_read_ns(uint64_t *out)
{ assert(host_if); if (bad_clock) return false; *out=++now; return true; }
void timer_sleep(uint64_t ms)
{ assert(host_if && ms==1); ++sleeps; now+=UINT64_C(120000000000); }
bool scheduler_snapshot_task_by_handle(task_handle_t handle, task_snapshot_t *out)
{ (void)out; assert(host_if && handle.id && handle.lifecycle_generation==1); return false; }
bool kernel_stack_read(uint32_t slot, stack_owner_view_t *out)
{
    assert(host_if && slot>=128 && slot<198);
    *out=(stack_owner_view_t){.generation=wrong_generation ? 0 : 1,
        .state=STACK_FREE,.retirement_generation=1}; return true;
}
bool thread_create_ex_handle(void (*entry)(void *),void *arg,
    const task_create_options_t *options,task_handle_t *out)
{
    assert(host_if && options->test_cpu_plus_one && options->test_cpu_plus_one<=g_cpu_count);
    assert(options->flags==TASK_FLAG_SYSTEM && options->task_class==TASK_CLASS_NORMAL);
    if (fail_create) return false;
    task_t *previous=current_task;uint32_t cpu=current_cpu;
    assert(created<70);current_task=&host_tasks[created];++created;
    current_task->id=created;current_task->lifecycle_generation=1;
    current_task->stack_owner=(stack_handle_t){.slot=127+created,.generation=1};
    task_handle_t handle={created,1};
    current_cpu=wrong_cpu ? g_cpu_count : options->test_cpu_plus_one-1;
    entry(arg);assert(host_if);
    current_task=previous;current_cpu=cpu;*out=handle;return true;
}
bool stack_test_emit(const char *format,...)
{
    assert(host_if);va_list args;va_start(args,format);
    int length=vsnprintf(last_line,sizeof(last_line),format,args);va_end(args);
    assert(length>0 && (size_t)length<sizeof(last_line));++emitted;return true;
}
static void reset(unsigned cpus)
{
    memset(g_stack_coverage,0,sizeof(g_stack_coverage));memset(host_tasks,0,sizeof(host_tasks));
    g_cpu_count=cpus;created=emitted=sleeps=current_cpu=0;current_task=NULL;
    host_if=true;wrong_cpu=wrong_generation=bad_clock=fail_create=false;now=1;
}
int main(void)
{
    for (unsigned cpus=1;cpus<=32;++cpus) {
        reset(cpus);assert(deterministic_stack_coverage());
        assert(created==2*cpus && emitted==cpus && !sleeps);
        for (unsigned cpu=0;cpu<cpus;++cpu) {
            const stack_coverage_t *r=&g_stack_coverage[cpu];
            assert(r->before_cpu==cpu && r->after_cpu==cpu && r->child.cpu==cpu);
            assert(r->child_retired && coverage_retired(&r->worker));
        }
    }
    reset(8);host_if=false;assert(!deterministic_stack_coverage() && !created && !host_if);
    reset(8);bad_clock=true;assert(!deterministic_stack_coverage() && !created);
    reset(8);fail_create=true;assert(!deterministic_stack_coverage() && !created);
    reset(2);wrong_cpu=true;assert(!deterministic_stack_coverage());
    reset(2);wrong_generation=true;assert(!deterministic_stack_coverage() && sleeps);
    assert(host_if);
    puts("IRQ coverage actual lifecycle controller: PASS CPU1_to_32=HOSTED ownership_and_refusals=CHECKED");
}

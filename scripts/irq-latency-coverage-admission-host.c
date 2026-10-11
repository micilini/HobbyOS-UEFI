#define main inherited_stack_main
#include "stack-scheduler-host.c"
#undef main

uint32_t g_cpu_count = 8;
static bool host_started = true, host_ready = true, host_preemption = true;
bool irq_is_enabled(void) { return host_if; }
bool scheduler_is_started(void) { return host_started; }
bool scheduler_cpu_is_ready(cpu_slot_t slot) { CHECK(slot < g_cpu_count); return host_ready; }
bool scheduler_cpu_irq_preemption_enabled(cpu_slot_t slot) { CHECK(slot < g_cpu_count); return host_preemption; }

int main(void)
{
    inherited_stack_main();
    for (unsigned which=0;which<7;++which) {
        reset();task_create_options_t opt=options();opt.test_cpu_plus_one=1;
        host_started=host_ready=host_preemption=true;
        if(which==0)host_if=false;
        if(which==1)host_started=false;
        if(which==2)host_ready=false;
        if(which==3)host_preemption=false;
        if(which==4)opt.test_cpu_plus_one=9;
        if(which==5)opt.task_class=TASK_CLASS_INTERACTIVE;
        if(which==6)opt.flags|=TASK_FLAG_KILLABLE;
        bool flags=host_if;task_handle_t out;
        CHECK(!thread_create_ex_handle(entry,NULL,&opt,&out));
        CHECK(!allocated && !out.id && host_if==flags);
    }
    host_started=host_ready=host_preemption=true;
    for(unsigned cpu=0;cpu<8;++cpu) {
        reset();task_create_options_t opt=options();opt.test_cpu_plus_one=cpu+1;task_handle_t out;
        CHECK(thread_create_ex_handle(entry,NULL,&opt,&out));
        CHECK(tasks[0].test_cpu_plus_one==cpu+1 && host_if && owners[0].published);
        zombie(&tasks[0]);CHECK(scheduler_reap_zombies(3000)==1 && retired==1 && freed==1);
    }
    puts("IRQ coverage actual task admission: PASS offline_and_masked=REJECTED lifecycle=RETAINED");
}

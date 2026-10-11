/* Real request and clock readers; registration has separate controller tests. */
#define REQUEST_CLOCK_INTEGRATION 1
#define REQUEST_PVCLOCK_INTEGRATION 1
#define main request_fixture_main
#include "request-telemetry-service-host.c"
#undef main
#include "../kernel/src/core/clock.c"
#include "../kernel/src/core/clocksource.c"
#include "../kernel/src/core/pvclock.c"

static unsigned test_case;
static uint64_t test_ns=1000000,test_step=100,admission_ns;
static bool delayed;

bool hpet_is_available(void) {return true;}
uint64_t hpet_frequency_hz(void) {return 100000000;}
uint64_t hpet_period_fs(void) {return 10000000;}
void spinlock_init(spinlock_t *p) {p->locked=0;p->tickets=0;}
int spin_trylock(spinlock_t *p) {(void)p;return 0;}
void spin_unlock(spinlock_t *p) {(void)p;}
__attribute__((noreturn)) void kpanic(const char *s) {fprintf(stderr,"panic %s\n",s);abort();}

bool hpet_read_clock_sample(hpet_clock_sample_t *out)
{
    ++hpet_calls;
    bool valid=test_case!=13;
    *out=(hpet_clock_sample_t){.ticks=test_ns/10,.ns=test_ns,.valid=valid,
        .mode=HPET_COUNTER_READ_SPLIT64_STABLE};
    return valid;
}

static uint64_t pvclock_test_tsc(void)
{
    if(missing && g_source_cpu[actor].reads==2 && !delayed) {
        delayed=true;test_ns+=CPU_REQUEST_DEADLINE_NS;
    }
    test_ns+=test_step;
    if(!admission_ns)admission_ns=test_ns;
    return test_ns+UINT64_C(1000000000000)*actor;
}

static void pvclock_test_after_delivery(void)
{
    if(test_case==4)g_source_pages[actor].page.version=3;
    if(test_case==5)g_source_pages[actor].page.flags=3;
    if(test_case==6)g_source_pages[actor].page.multiplier=1;
    if(test_case==7)test_ns=1;
    if(test_case==8){test_ns=g_source_cpu[actor].last_ns;test_step=0;}
    if(test_case==9 || test_case==10) {
        test_ns=admission_ns+CPU_REQUEST_DEADLINE_NS-100+(test_case==10 ? 1 : 0);
    }
    if(test_case==11)clocksource_invalidate();
    if(test_case==12)g_source_pages[actor].page.ns_anchor+=1000000;
    if(test_case==13)test_ns+=CPU_REQUEST_DEADLINE_NS;
}

static void run_case(unsigned control)
{
    test_case=control;kind=0;g_cpu_count=2;flags=2;
    assert(clock_monotonic_init());
    /* Immutable qualified inputs are injected; reads and deadlines are real. */
    for(unsigned cpu=0;cpu<2;++cpu) {
        g_cpus[cpu]=(SmpCpuInfo){.slot=cpu,.apic_id=cpu+16,.state=CPU_STATE_ONLINE};
        pvclock_page_t page={.version=2,.tsc_anchor=UINT64_C(1000000000000)*cpu,
            .multiplier=0x80000000,.shift=1,.flags=1};
        g_source_pages[cpu].page=page;
        g_source_cpu[cpu]=(source_cpu_t){.status=SOURCE_QUALIFIED,.registered=1,
            .last_tsc=page.tsc_anchor,.anchor=page};
        g_source_cpu[cpu].previous_reference=(pvclock_reference_t){
            .before={.tsc=page.tsc_anchor+990,.ns=990,.version=2,.multiplier=0x80000000,.shift=1,.flags=1},
            .after={.tsc=page.tsc_anchor+1010,.ns=1010,.version=2,.multiplier=0x80000000,.shift=1,.flags=1},
            .ticks=100,.ns=1000,.period_fs=10000000,.mode=1,.valid=true};
    }
    g_source=CLOCKSOURCE_KVM;g_source_epoch=2;g_source_selected=1;
    assert(cpu_request_runtime_init(4096));flags=514;
    if(control==3)actual_root=8192;
    if(control==14)g_source_pages[0].page.version=3;
    if(control==15)migrate=true;
    if(control==16)missing=true;
    tlb_interval_t interval;
    assert(tlb_interval_normalize(0x1ff000,(control==1 ? 65 : 64)*4096,&interval));
    cpu_request_result_t result=cpu_request_run(control==2 ? CPU_REQUEST_CACHE_WRITEBACK : CPU_REQUEST_INVALIDATE,&interval);
    assert(flags==514);
    if(control==14) {
        assert(result.status==CPU_REQUEST_UNAVAILABLE && !invocations[0] && !invocations[1] && !g_request.quarantined);
    } else if(control<=2 || control==15) {
        assert(result.status==CPU_REQUEST_OK && result.acknowledged==3 && !g_request.quarantined);
        assert(result.elapsed_ns>0 && result.elapsed_ns<CPU_REQUEST_DEADLINE_NS);
        for(unsigned cpu=0;cpu<2;++cpu) {
            assert(invocations[cpu]==(control==1 || control==2 ? 0u : 64u));
            assert(reloads[cpu]==(control==1) && writebacks[cpu]==(control==2));
            if(control==1)assert(last_reload==0x1018);
            cpu_request_timing_t t;assert(cpu_request_timing_copy(result.generation,cpu,&t));
            assert(t.local.cpu==cpu && t.local.root==4096 && t.local.generation==result.generation);
            assert(t.local.handler_begin<t.local.operation_begin && t.local.operation_end<t.local.handler_end);
            assert(t.send.cpu<2 && t.send.apic==t.send.cpu+16);
        }
    } else {
        assert(result.status!=CPU_REQUEST_OK && g_request.quarantined);
        if(control!=3 && control!=16) {
            assert(result.status==CPU_REQUEST_TIMEOUT && result.acknowledged==3);
            assert(invocations[0]==64 && invocations[1]==64);
        }
        if(control==9 || control==10)assert(result.elapsed_ns>=CPU_REQUEST_DEADLINE_NS);
        else if(control!=3 && control!=16)assert(result.elapsed_ns==UINT64_MAX);
        if(control==16) {
            uint64_t generation=g_request.spec.generation;
            flags=2;actor=1;cpu_request_irq();flags=514;
            assert(g_request.spec.generation==generation && g_request.quarantined && !invocations[1]);
            assert(cpu_request_run(CPU_REQUEST_INVALIDATE,&interval).status==CPU_REQUEST_QUARANTINED);
        }
    }
    if(control!=9 && control!=10 && control!=13 && control!=16)assert(!hpet_calls);
    if((control>=4 && control<=8) || (control>=11 && control<=14)) {
        assert(clocksource_current()==CLOCKSOURCE_FAILED && clocksource_epoch()==3);
        unsigned before=hpet_calls;uint64_t unchanged=42;
        assert(!clock_monotonic_read_ns(&unchanged) && unchanged==42 && hpet_calls==before);
    }
}

int main(void)
{
    for(unsigned c=0;c<17;++c) {
        pid_t p=fork();assert(p>=0);
        if(!p){run_case(c);_exit(0);}
        int status;assert(waitpid(p,&status,0)==p);
        if(!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"actual pvclock/request case %u failed\n",c);return 1;
        }
    }
    puts("Actual KVM clock/request validity: PASS cases=17 ACK=AFTER_OPERATION quarantine=RETAINED");
    return 0;
}

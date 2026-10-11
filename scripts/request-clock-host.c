/* The real clock and request service share one controlled HPET input. */
#define REQUEST_CLOCK_INTEGRATION 1
#define main request_fixture_main
#include "request-telemetry-service-host.c"
#undef main
#include "../kernel/src/core/clock.c"
#include "../kernel/src/core/clocksource.c"
#include "../kernel/src/core/pvclock.c"

bool hpet_is_available(void) { return true; }
uint64_t hpet_frequency_hz(void) { return 100000000; }
uint64_t hpet_period_fs(void) { return 10000000; }
void spinlock_init(spinlock_t *p) {p->locked=0;p->tickets=0;}
int spin_trylock(spinlock_t *p) {(void)p;return 0;}
void spin_unlock(spinlock_t *p) {(void)p;}
__attribute__((noreturn)) void kpanic(const char *s) {fprintf(stderr,"panic %s\n",s);abort();}

bool hpet_read_clock_sample(hpet_clock_sample_t *out)
{
    ++hpet_calls;
    uint64_t value=hpet_calls*100;
    if (kind==3) value=100;
    if (kind==4 && hpet_calls==3) value=150;
    if (kind==6 && hpet_calls==4) value=UINT64_MAX;
    if (kind==7 && hpet_calls==4) value=CPU_REQUEST_DEADLINE_NS+200;
    bool valid=!((kind==1 && hpet_calls==2) || (kind==2 && hpet_calls>=3) ||
                 (kind==5 && hpet_calls==4));
    *out=(hpet_clock_sample_t){.ticks=value/10,.ns=value,.valid=valid};
    return valid;
}

static void clock_request_case(unsigned control)
{
    kind=control;g_cpu_count=2;
    for(unsigned c=0;c<2;++c)
        g_cpus[c]=(SmpCpuInfo){.slot=c,.apic_id=c+16,.state=CPU_STATE_ONLINE};
    flags=2;assert(clock_monotonic_init());assert(clock_monotonic_ns()==100);
    assert(cpu_request_runtime_init(4096));flags=514;
    tlb_interval_t interval;assert(tlb_interval_normalize(0x1ff000,64*4096,&interval));
    cpu_request_result_t result=cpu_request_run(CPU_REQUEST_INVALIDATE,&interval);
    assert(flags==514);
    if (!control)
        assert(result.status==CPU_REQUEST_OK && result.acknowledged==3 &&
               result.elapsed_ns==200 && !g_request.quarantined);
    else if (control==1)
        assert(result.status==CPU_REQUEST_UNAVAILABLE && !invocations[0] &&
               !invocations[1] && !g_request.quarantined);
    else {
        assert(result.status==CPU_REQUEST_TIMEOUT && result.acknowledged==3 && g_request.quarantined);
        assert(invocations[0]==64 && invocations[1]==64);
        assert(g_request.spec.generation==result.generation);
    }
    if (control==2 || control==3 || control==5 || control==6)
        assert(result.elapsed_ns==UINT64_MAX);
}

int main(void)
{
    for(unsigned c=0;c<8;++c) {
        pid_t p=fork();assert(p>=0);
        if(!p){clock_request_case(c);_exit(0);}
        int status;assert(waitpid(p,&status,0)==p);
        if(!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"actual clock/request case %u failed\n",c);return 1;
        }
    }
    puts("Actual HPET-backed clock/request validity: PASS cases=8 ACK=AFTER_OPERATION quarantine=RETAINED");
    return 0;
}

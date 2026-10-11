#include "../kernel/src/smp/cpu_request.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    cpu_request_sample_t sample = {0};
    sample.timing.local = (cpu_request_local_cycles_t){.handler_begin=10,.operation_begin=20,
        .operation_end=30,.handler_end=40,.generation=1,.root=4096,.epoch=2,.end_epoch=2,.cpu=3,.apic=5};
    cpu_request_local_cycles_t *t=&sample.timing.local;
    assert(cpu_request_local_cycles_check(t,1,4096,2,3,5)==CPU_REQUEST_TIMING_CYCLES);
    assert(cpu_request_local_cycles_check(t,2,4096,2,3,5)==CPU_REQUEST_TIMING_IDENTITY);
    assert(cpu_request_local_cycles_check(t,1,8192,2,3,5)==CPU_REQUEST_TIMING_IDENTITY);
    assert(cpu_request_local_cycles_check(t,1,4096,3,3,5)==CPU_REQUEST_TIMING_IDENTITY);
    assert(cpu_request_local_cycles_check(t,1,4096,2,4,5)==CPU_REQUEST_TIMING_IDENTITY);
    t->end_epoch=3;
    assert(cpu_request_local_cycles_check(t,1,4096,3,3,5)==CPU_REQUEST_TIMING_IDENTITY);
    t->end_epoch=2;
    t->end_flags=512;
    assert(cpu_request_local_cycles_check(t,1,4096,2,3,5)==CPU_REQUEST_TIMING_CONTEXT);
    t->end_flags=0;t->operation_end=41;
    assert(cpu_request_local_cycles_check(t,1,4096,2,3,5)==CPU_REQUEST_TIMING_ORDER);
    t->operation_end=20;
    assert(cpu_request_local_cycles_check(t,1,4096,2,3,5)==CPU_REQUEST_TIMING_ORDER);
    t->operation_end=0;
    assert(cpu_request_local_cycles_check(t,1,4096,2,3,5)==CPU_REQUEST_TIMING_ORDER);
    cpu_request_send_cycles_t s={.begin=10,.end=20,.epoch=2,.end_epoch=2,.cpu=3,.apic=5};
    assert(cpu_request_send_cycles_check(&s,2,3,5)==CPU_REQUEST_TIMING_CYCLES);
    s.end=9;assert(cpu_request_send_cycles_check(&s,2,3,5)==CPU_REQUEST_TIMING_ORDER);
    s.end=20;s.begin_flags=512;
    assert(cpu_request_send_cycles_check(&s,2,3,5)==CPU_REQUEST_TIMING_CONTEXT);
    s.begin_flags=0;assert(cpu_request_send_cycles_check(&s,2,4,5)==CPU_REQUEST_TIMING_IDENTITY);
    puts("Request local-cycle typed identity/order contract: PASS functional_deadline=SEPARATE");
}

#include "../kernel/src/core/irq_latency_record.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef HOBBYOS_IRQ_LATENCY_DETAILS
#define HOBBYOS_IRQ_LATENCY_DETAILS 1
#endif
static irq_latency_recorder_t recorder;
static void samples(unsigned backlog)
{
    irq_latency_ticket_t tickets[IRQ_LATENCY_PENDING];
    for (unsigned i=0;i<backlog;++i) {
        assert(irq_latency_record_begin(&recorder,IRQ_LATENCY_IF,100,1,3,IRQ_LATENCY_THREAD));
        tickets[i]=irq_latency_record_close(&recorder,IRQ_LATENCY_IF,2);
        assert(tickets[i].valid);
    }
    for(unsigned i=0;i<backlog;++i)
        assert(irq_latency_record_complete(&recorder,tickets[i],100000+i,0,IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
}
int main(void)
{
    for(unsigned contents=0;contents<2;++contents) {
        memset(&recorder,0,sizeof(recorder));
        assert(irq_latency_record_init(&recorder,0,1,1000));
        memset(recorder.failures,contents?0xa5:0x5a,sizeof(recorder.failures));
        unsigned total=0;
        for(unsigned i=1;i<=IRQ_LATENCY_PENDING;++i){samples(i);total+=i;}
        assert(total==2080 && !recorder.pending_count);
        assert(recorder.path[0].count==total && recorder.path[1].count==total);
        assert(recorder.path[0].violations==total && recorder.path[1].violations==total);
        assert(recorder.path[0].maximum_cycles==99963 && recorder.path[1].maximum_cycles==99963);
        assert(recorder.path[0].maximum.cells[IL_END]-recorder.path[0].maximum.cells[IL_BEGIN]==99963);
        assert(recorder.failure_count+recorder.failure_overflow==total);
#if HOBBYOS_IRQ_LATENCY_DETAILS
        assert(recorder.failure_count==IRQ_LATENCY_FAILURES && recorder.failure_overflow==32);
#else
        assert(!recorder.failure_count && recorder.failure_overflow==total);
        const unsigned char *bytes=(const unsigned char *)recorder.failures;
        for(unsigned i=0;i<sizeof(recorder.failures);++i)assert(bytes[i]==(contents?0xa5:0x5a));
#endif
        assert(!recorder.invalid && !recorder.exhausted && !recorder.recursion);
        assert(irq_latency_record_begin(&recorder,IRQ_LATENCY_IF,200000,1,1,IRQ_LATENCY_HARD_IRQ));
        irq_latency_ticket_t ticket=irq_latency_record_close_calibrated(&recorder,IRQ_LATENCY_IF,2,128,7,41);
        assert(ticket.valid && recorder.pending[ticket.slot].witness.cells[IL_CLOSE_ADJUSTMENT]==41);
        assert(irq_latency_record_interrupt(&recorder,400000,0));
        assert(irq_latency_record_retire_owner(&recorder,128,8)==0);
        assert(irq_latency_record_retire_owner(&recorder,128,7)==1);
        assert(irq_latency_record_drain(&recorder));
        assert(recorder.path[0].maximum_cycles==200041 && recorder.path[0].count==total+1);
        assert(recorder.path[0].maximum.cells[IL_RETIRE_SEQUENCE]==ticket.sequence);
        assert(recorder.path[0].maximum.cells[IL_CLOSE_ADJUSTMENT]==41);
        assert(recorder.failure_count+recorder.failure_overflow==total+1);
    }
    puts("IRQ latency actual-C compulsory maxima independent of optional detail/backlog: PASS minimum_only=1");
}

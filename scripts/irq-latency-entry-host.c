#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../kernel/src/core/irq_latency_boundary.h"
uint32_t g_irq_latency_identity_ready=1;
uint64_t irq_entry_host_saved_rsp, irq_entry_host_flags, irq_entry_host_values[15];
static uint64_t early_stamp, inner_stamp, tail_stamps[2];
static irq_latency_pending_t pending;
static unsigned entered, dispatched, exited;
extern int irq_entry_host_probe(void);
uint64_t irq_latency_interrupt_enter(uint64_t start,uint64_t vector,uint64_t flags,uint64_t rsp)
{
    assert(start && vector==255 && flags==0x602 && rsp==irq_entry_host_saved_rsp-8);
    inner_stamp=start;++entered;return 0x1234;
}
uint64_t irq_latency_interrupt_enter_checkpoint(uint64_t start,uint64_t vector,uint64_t flags,uint64_t rsp,uint64_t early)
{
    assert(early && early<start);early_stamp=early;
    return irq_latency_interrupt_enter(start,vector,flags,rsp);
}
void irq_external_dispatch(unsigned vector,unsigned df)
{
    assert(vector==255 && df==0);++dispatched;
}
irq_latency_tail_t irq_latency_interrupt_exit(uint64_t vector,uint64_t flags,uint64_t scoped)
{
    assert(vector==255 && flags==0x602 && scoped==(g_irq_latency_identity_ready?0x1234:0));
    ++exited;
    if(!g_irq_latency_identity_ready)return (irq_latency_tail_t){0};
    pending.ready=2;
    return (irq_latency_tail_t){&pending,&tail_stamps[0]};
}
int main(void)
{
    for (unsigned ready=0;ready<2;++ready) {
        g_irq_latency_identity_ready=ready;
        assert(irq_entry_host_probe());
        for(unsigned i=0;i<15;++i)assert(irq_entry_host_values[i]==i+1);
        assert((irq_entry_host_flags&0xe02)==0x602);
    }
    assert(tail_stamps[0]>inner_stamp && tail_stamps[1]>tail_stamps[0]);
    assert(pending.witness.cells[IL_END]==tail_stamps[0]);
    assert(entered==1 && dispatched==2 && exited==2 && early_stamp<inner_stamp);
    puts("IRQ latency actual-assembly entry frame, GPR and DF: PASS iret=HOSTED");
}

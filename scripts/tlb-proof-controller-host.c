#define IO_H
#include <stdint.h>
static void outb(uint16_t, uint8_t);
static uint8_t inb(uint16_t port) { (void)port; return 0xe9; }
#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
#define switch_context fixture_unused_switch_context
#include "../kernel/src/memory/tlb_fixture.c"
#undef switch_context

static unsigned char storage[4096];
static uint64_t wait_delay, copy_delay, port_calls;
static bool trace_held;
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    CHECK(lock==&g_trace_lock && !trace_held);
    irq_flags_t flags=irq_save();
    irq_latency_lock_token_t token=irq_latency_lock_wait((uintptr_t)lock);
    clock_value+=wait_delay;
    irq_latency_lock_acquired((uintptr_t)lock,token);
    trace_held=true;return flags;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags)
{
    CHECK(lock==&g_trace_lock && trace_held && !irq_is_enabled());
    CHECK(g_proof.bytes && g_proof.records && g_trace_bytes==g_proof.bytes);
    clock_value+=copy_delay;
    irq_latency_lock_released((uintptr_t)lock);
    trace_held=false;irq_restore(flags);
}
static void outb(uint16_t port,uint8_t byte)
{
    (void)byte;CHECK(port==TLB_TRACE_PORT && trace_held && !irq_is_enabled());
    ++port_calls;
}
void serial_write_all(const char *line) { (void)line;CHECK(false); }

static uint64_t sample(bool deferred,bool nested,uint64_t wait,uint64_t copy)
{
    reset();memset(&g_proof,0,sizeof(g_proof));
    CHECK(tlb_proof_open(&g_proof,storage,sizeof(storage),1));
    g_proof_deferred=deferred;g_trace_bytes=g_trace_records=g_proof_port_bytes=0;
    g_trace_digest=TLB_TRACE_FNV_OFFSET;wait_delay=wait;copy_delay=copy;port_calls=0;
    irq_flags_t outer=0;if(nested)outer=irq_save();
    uint64_t original_begin=g_irq_latency[0].begin;
    CHECK(emit_line("\n[TLB][HOSTED] exact immutable bytes\n"));
    if(nested) {
        CHECK(!irq_is_enabled() && g_irq_latency[0].begin==original_begin);
        irq_restore(outer);
    }
    CHECK(irq_is_enabled() && !g_irq_invalid_boundaries && !trace_held);
    CHECK(port_calls==(deferred?0:g_trace_bytes));
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    irq_latency_path_record_t *row=&g_irq_latency[0].path[IRQ_PATH_SHOOTDOWN];
    CHECK(row->count && !row->invalid && row->maximum_cycles>=wait+copy);
    CHECK(row->maximum.cells[IL_PATHS]&(UINT64_C(1)<<IRQ_PATH_SHOOTDOWN));
    CHECK(row->maximum.cells[IL_WAIT]>=wait);
    return row->maximum_cycles;
}
int main(void)
{
    for(unsigned mode=0;mode<2;++mode)for(unsigned nested=0;nested<2;++nested) {
        uint64_t base=sample(mode,nested,0,0);
        CHECK(sample(mode,nested,500000,0)==base+500000);
        CHECK(sample(mode,nested,0,500000)==base+500000);
        CHECK(sample(mode,nested,200000,300000)==base+500000);
    }
    puts("TLB actual emitter/controller: PASS capture_wait_copy_in_outer_maximum=1 exact_flags=1");
    return 0;
}

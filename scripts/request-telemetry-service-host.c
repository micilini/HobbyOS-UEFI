/* Actual request dispatcher/state/local operation; privileged effects are hosted. */
#include "smp/cpu_request.h"
#include "smp/smp_topology.h"
#include "smp/smp_boot.h"
#include "apic/lapic.h"
#include "core/clock.h"
#include "core/interrupt_context.h"
#include "core/irq_bootstrap.h"
#include "core/scheduler.h"
#include "cpu/cpu.h"
#include "memory/kernel_tlb.h"
#include "timer/hpet.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count=2,g_bsp_apic_id;
volatile int g_panic_in_progress;
static unsigned actor,kind,hpet_calls,cpuid_calls,reads,invocations[32],reloads[32],writebacks[32];
static uint64_t flags=514,actual_root=0x1018,actual_cr4=0x20,last_reload;
#ifndef REQUEST_PVCLOCK_INTEGRATION
static uint64_t tsc=1000;
#endif

#ifndef REQUEST_CLOCK_INTEGRATION
static uint64_t ns=100;
#endif
static bool missing,unavailable,backward_tsc,failed_clock,migrate;
static uint64_t privileged_cr3(void) { return actual_root; }
static uint64_t privileged_cr4(void) { return actual_cr4; }
static void privileged_invlpg(uint64_t address);
static void privileged_reload(uint64_t root);
static void privileged_writeback(void);
#ifdef REQUEST_PVCLOCK_INTEGRATION
static uint64_t pvclock_test_tsc(void);
static void pvclock_test_after_delivery(void);
#endif
#include REQUEST_DISPATCH_SOURCE
#include REQUEST_LOCAL_SOURCE

uint64_t irq_latency_host_read_tsc(void)
{
    assert(!irq_is_enabled());
#ifdef REQUEST_PVCLOCK_INTEGRATION
    return pvclock_test_tsc();
#else
    return (backward_tsc ? --tsc : (tsc+=10)) + UINT64_C(1000000000000)*actor;
#endif
}
bool cpu_get_cpuid_count(uint32_t leaf,uint32_t subleaf,uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d)
{
    assert(!irq_is_enabled() && leaf<=1 && subleaf==0);++cpuid_calls;
    if (!leaf) {*a=1;*b=0x756e6547;*c=0x6c65746e;*d=0x49656e69;return true;}
    *a=*b=*c=0;*d=unavailable ? 0 : (16|(1u<<26));return true;
}
irq_flags_t irq_save(void) {uint64_t old=flags;flags&=~UINT64_C(512);return old;}
void irq_restore(irq_flags_t old) {flags=old;if(migrate && (old&512))actor=(actor+1)%g_cpu_count;}
bool irq_is_enabled(void) {return (flags&512)!=0;}
bool smp_current_cpu_slot(cpu_slot_t *out) {*out=actor;return true;}
bool smp_cpu_slot_from_apic_id(uint32_t apic,cpu_slot_t *out) {*out=apic-16;return *out<g_cpu_count;}
uint32_t lapic_get_id(void) {return 16+actor;}
uint32_t smp_online_cpu_count(void) {return g_cpu_count;}
uint32_t smp_failed_cpu_count(void) {return 0;}
irq_bootstrap_state_t irq_bootstrap_state(void) {return IRQ_BOOTSTRAP_CPUS_PREPARED;}
bool irq_bootstrap_services_are_ready(void) {return true;}
bool irq_bootstrap_cpu_irq_ready(uint32_t slot) {return slot<g_cpu_count;}
bool scheduler_current_context_can_block(void) {return true;}
#ifndef REQUEST_CLOCK_INTEGRATION
bool clock_monotonic_is_ready(void) {return true;}
#endif
bool interrupt_context_snapshot(cpu_slot_t slot,interrupt_cpu_snapshot_t *out)
{assert(slot==actor);*out=(interrupt_cpu_snapshot_t){.initialized=1};return true;}
#ifndef REQUEST_CLOCK_INTEGRATION
uint64_t clock_monotonic_ns(void)
{
    assert(irq_is_enabled());++reads;
    if (kind==10 && reads==1) return 0;
    if (kind==11 && reads==2) return 50;
    if (kind==12 && reads==3) return CPU_REQUEST_DEADLINE_NS+200;
    if (kind==13) return 100;
    if (kind==14 && reads==2) return CPU_REQUEST_DEADLINE_NS+200;
    if (kind==17 && reads==2) return CPU_REQUEST_DEADLINE_NS+200;
    if (missing && kind!=13) ns+=CPU_REQUEST_DEADLINE_NS;
    return ++ns;
}
bool hpet_read_clock_sample(hpet_clock_sample_t *out)
{
    ++hpet_calls;tsc+=100;
    *out=(hpet_clock_sample_t){.valid=!failed_clock,.ns=hpet_calls*100};return !failed_clock;
}
#ifndef REQUEST_LEGACY
bool clock_monotonic_read_ns(uint64_t *out)
{
    if (!out) return false;
    *out=clock_monotonic_ns();
    return *out!=0 && *out!=UINT64_MAX;
}
#endif
#endif

int ksnprintf(char *dst,size_t size,const char *fmt,...)
{va_list ap;va_start(ap,fmt);int n=vsnprintf(dst,size,fmt,ap);va_end(ap);return n;}
void serial_write_all(const char *s) {assert(s);}
uint64_t paging_kernel_root_physical(void) {return
#ifndef REQUEST_CLOCK_INTEGRATION
        kind==6 ? 8192 :
#endif
        4096;}
bool tlb_fixture_load(const tlb_interval_t *i,cpu_request_sample_t *s)
{(void)i;(void)s;return false;}
bool protection_probe_execute(const cpu_request_spec_t *s,cpu_request_sample_t *p)
{(void)s;(void)p;return false;}
static void before_privileged(void)
{
    assert(!irq_is_enabled());
    assert(g_request.publication[actor]==(g_request.spec.generation<<2|2));
    assert(!g_request.samples[actor].success);
}
static void privileged_invlpg(uint64_t address)
{before_privileged();assert(address>=0x1ff000 && address<0x240000);++invocations[actor];}
static void privileged_reload(uint64_t root)
{before_privileged();last_reload=root;++reloads[actor];}
static void privileged_writeback(void)
{before_privileged();++writebacks[actor];}
lapic_delivery_result_t lapic_send_ipi(uint32_t apic,uint32_t vector)
{
    assert(!irq_is_enabled() && vector==INT_VECTOR_CPU_REQUEST);
    if (kind==15) return LAPIC_DELIVERY_INDETERMINATE;
    if (missing) return LAPIC_DELIVERY_SUBMITTED;
    unsigned old=actor;actor=apic-16;cpu_request_irq();
    assert(g_request.publication[actor]==(g_request.spec.generation<<2|3));
#ifndef REQUEST_LEGACY
    assert(g_request.samples[actor].timing.local.status!=CPU_REQUEST_TIMING_INCOMPLETE);
#endif
    unsigned before=hpet_calls;cpu_request_irq();
#ifdef REQUEST_LEGACY
    assert(hpet_calls==before+1);
#else
    assert(hpet_calls==before);
#endif
    actor=old;
#ifdef REQUEST_PVCLOCK_INTEGRATION
    pvclock_test_after_delivery();
#endif
    return LAPIC_DELIVERY_SUBMITTED;
}

static void execute_case(unsigned which)
{
    kind=which;g_cpu_count=2;
    for(unsigned c=0;c<g_cpu_count;++c)g_cpus[c]=(SmpCpuInfo){.slot=c,.apic_id=c+16,.state=CPU_STATE_ONLINE};
    flags=2;assert(cpu_request_runtime_init(4096));flags=514;
    unavailable=which==4;failed_clock=which==5;backward_tsc=which==8;migrate=which==9;
    missing=which==13 || which==14 || which==15 || which==16;
    if(which==7)actual_cr4=0xa0;
    if(which==17)g_request.owner=1;
    if(which==18)actual_root=8192;
    if(which==19)g_request.next_generation=CPU_REQUEST_AUDIT_CAPACITY;
    tlb_interval_t interval;
    unsigned pages=which==1 ? 65 : 64;
    assert(tlb_interval_normalize(0x1ff000,pages*4096,&interval));
    cpu_request_sample_t samples[32];
    cpu_request_result_t result=cpu_request_run_observed(which==2 || which==3 || which==5 ?
        CPU_REQUEST_CACHE_WRITEBACK : CPU_REQUEST_INVALIDATE,&interval,samples,32);
    assert(flags==514 && (which==17 || !g_request.owner));
    if(which==19) {
        uint64_t admitted,completed;
        assert(result.status==CPU_REQUEST_OK && invocations[0]==64 && invocations[1]==64);
        assert(!cpu_request_audit_snapshot(&admitted,&completed));
#ifndef REQUEST_LEGACY
        cpu_request_timing_t timing;
        assert(!cpu_request_timing_copy(result.generation,0,&timing) && !hpet_calls);
#endif
        return;
    }
    bool fail=which==6 || which==7 || (which>=10 && which<=18);
    if(which==17) { assert(result.status==CPU_REQUEST_BUSY);return; }
    if(fail) {
        assert(result.status!=CPU_REQUEST_OK);
        if(which!=10) assert(g_request.quarantined);
        else assert(result.status==CPU_REQUEST_UNAVAILABLE && !g_request.quarantined);
        if(which==12)assert(result.status==CPU_REQUEST_TIMEOUT && result.acknowledged==3);
        if(which==16){
            uint64_t generation=g_request.spec.generation;
            flags=2;actor=1;cpu_request_irq();flags=514;
            assert(g_request.spec.generation==generation && g_request.quarantined);
            result=cpu_request_run(CPU_REQUEST_INVALIDATE,&interval);
            assert(result.status==CPU_REQUEST_QUARANTINED);
        }
    } else {
        assert(result.status==CPU_REQUEST_OK && result.acknowledged==3 && !g_request.quarantined);
        for(unsigned c=0;c<2;++c){
            if(which==1)assert(reloads[c]==1 && last_reload==0x1018 && !invocations[c]);
            else if(which==2 || which==3 || which==5)assert(writebacks[c]==1 && !invocations[c]);
            else assert(invocations[c]==64 && !reloads[c]);
#ifndef REQUEST_LEGACY
            cpu_request_timing_t t;
            assert(cpu_request_timing_copy(result.generation,c,&t));
            assert(t.local.generation==result.generation && t.local.root==4096 && t.local.cpu==c && t.local.apic==c+16);
            assert(t.local.status==(unavailable ? CPU_REQUEST_TIMING_NO_TSC : backward_tsc ? CPU_REQUEST_TIMING_ORDER : CPU_REQUEST_TIMING_CYCLES));
            assert(t.send.cpu<2 && t.send.apic==16+t.send.cpu);
            if(t.local.status==CPU_REQUEST_TIMING_CYCLES)
                assert(t.local.operation_begin>t.local.handler_begin && t.local.operation_end<t.local.handler_end);
            char raw[512];
            assert(cpu_request_timing_format(raw,sizeof(raw),result.generation,4096,c,&t));
            printf("DATA case=%u values=%s\n",which,raw);fflush(stdout);
#else
            assert(samples[c].handler_body_ns==(failed_clock ? UINT64_MAX : 300));
            assert(samples[c].elapsed_ns==(failed_clock ? UINT64_MAX : 100));
#endif
        }
    }
#ifndef REQUEST_LEGACY
    assert(hpet_calls==0);
    assert(cpuid_calls<=g_cpu_count*(HOBBYOS_IRQ_LOCAL_TSC_ORDER ? 2u : 1u));
#else
    if(!fail)assert(hpet_calls==(failed_clock ? 10u : 12u)+result.notifications);
#endif
    assert(reads>0);
}
int main(void)
{
    for(unsigned c=0;c<20;++c){
        pid_t p=fork();assert(p>=0);
        if(!p){execute_case(c);_exit(0);}
        int status;assert(waitpid(p,&status,0)==p);
        if(!WIFEXITED(status) || WEXITSTATUS(status)){fprintf(stderr,"service case %u failed\n",c);return 1;}
    }
    puts("Request actual dispatcher/state/local operations: PASS cases=20 privileged_operations=SUBSTITUTED");
    return 0;
}

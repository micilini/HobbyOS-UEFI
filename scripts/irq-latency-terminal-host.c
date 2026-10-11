/* Exercise the actual read-only guest audit and refusal paths. */
#include "../kernel/src/memory/irq_latency_test.c"
#define cpu_request_audit_snapshot unused_request_audit_snapshot
#define cpu_request_audit_copy unused_request_audit_copy
#define cpu_request_timing_copy unused_request_timing_copy
#include "../kernel/src/smp/cpu_request.c"
#undef cpu_request_audit_snapshot
#undef cpu_request_audit_copy
#undef cpu_request_timing_copy
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
uint32_t g_cpu_count=8;
static unsigned calls, records, fail_copy, fail_timing, changing;
static uint64_t tick;
static bool enabled=true;
static unsigned finish_step,finish_failure;
bool irq_is_enabled(void){return enabled;}
bool stack_report_flush(void){return true;}
bool stack_report_quiet(void){return true;}
bool warning_report_flush(void){return true;}
bool warning_report_quiet(void){return true;}
bool warning_report_checkpoint(const char *role) { return role!=NULL; }
bool stack_report_checkpoint(const char *role){return role!=NULL;}
uint64_t timer_get_uptime_ms(void){return tick;}
void timer_sleep(uint64_t ms){tick+=ms;}
uint64_t paging_kernel_root_physical(void){return 0x120000;}
bool cpu_request_audit_snapshot(uint64_t *a,uint64_t *c)
{
    ++calls;*a=*c=3;
    if(changing==1)*a=4;
    if(changing==2 && calls>1)*a=*c=4;
    return changing!=3;
}
bool cpu_request_audit_copy(uint64_t gen,cpu_request_audit_t *a)
{
    if(gen==fail_copy)return false;
    memset(a,0,sizeof(*a));a->spec.generation=gen;a->spec.targets=255;
    for(unsigned i=0;i<8;++i)a->spec.apic[i]=i;
    return true;
}
bool cpu_request_timing_copy(uint64_t gen,uint32_t cpu,cpu_request_timing_t *out)
{
    if (fail_timing && gen==3 && cpu==4) return false;
    *out=(cpu_request_timing_t){.local={.cpu=cpu,.generation=gen}};
    return true;
}
int ksnprintf(char *out,size_t size,const char *fmt,...)
{
    va_list ap;va_start(ap,fmt);int n=vsnprintf(out,size,fmt,ap);va_end(ap);return n;
}
bool latency_report(const char *fmt,...)
{
    char out[1024];va_list ap;va_start(ap,fmt);int n=vsnprintf(out,sizeof(out),fmt,ap);va_end(ap);
    assert(n>0 && n<(int)sizeof(out));++records;return true;
}
static void reset(void)
{
    calls=records=fail_copy=fail_timing=changing=0;tick=0;enabled=true;g_terminal_complete=true;g_audit_reported=false;
}

static unsigned host_terminal_freeze(void)
{
    assert(finish_step++==0 && calls==1 && !g_calibration[1].joined);
    return finish_failure==1 ? 0 : g_cpu_count;
}
static bool host_calibration_collect(unsigned role)
{
    assert(finish_step++==1 && role==1);
    g_calibration[role].joined=finish_failure!=2;return g_calibration[role].joined;
}
static bool host_calibration_select(unsigned role)
{
    assert(finish_step++==2 && role==1 && g_calibration[1].joined);return finish_failure!=3;
}
static bool host_calibration_report(unsigned role,bool valid)
{
    assert(role==1 && valid==(finish_failure==0));return true;
}
static bool host_terminal_report(unsigned frozen)
{
    assert(finish_step++==3 && frozen==g_cpu_count && g_calibration[1].joined);return true;
}
#include IRQ_FINISH_SOURCE

static void finish_chronology(void)
{
    for(unsigned state=0;state<6;++state){g_latency_test_state=state;assert(irq_latency_test_reporting()==(state==3));}
    for(finish_failure=0;finish_failure<4;++finish_failure){
        reset();finish_step=0;g_calibration[1].joined=0;g_latency_test_state=2;
        assert(host_irq_latency_test_finish()==(finish_failure!=0));
        assert(g_terminal_complete==(finish_failure==0 || finish_failure==3));
        assert(finish_step==(finish_failure==1 ? 1u : finish_failure==2 ? 2u : 4u));
        assert(host_irq_latency_test_finish()==1);
    }
}
int main(void)
{
    finish_chronology();
    reset();assert(irq_latency_test_audit()==0 && records==29 && calls==2);
    assert(irq_latency_test_audit()==1);
    reset();g_terminal_complete=false;assert(irq_latency_test_audit()==1 && records==0);
    reset();enabled=false;assert(irq_latency_test_audit()==1 && records==0);
    reset();fail_copy=3;assert(irq_latency_test_audit()==1 && records==19);
    reset();fail_timing=1;assert(irq_latency_test_audit()==1);
    reset();changing=1;assert(irq_latency_test_audit()==1 && tick==5000 && records==0);
    reset();changing=2;assert(irq_latency_test_audit()==1 && records==29);
    reset();changing=3;assert(irq_latency_test_audit()==1 && records==0);
    puts("IRQ latency actual-C terminal audit and bounded refusal: PASS services=HOSTED");
}

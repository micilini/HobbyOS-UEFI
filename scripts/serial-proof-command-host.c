#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/shell/commands/cmd_irqlatency.c"
uint32_t g_cpu_count=24;
static char history[32],last[1024];static unsigned used;
static char fail;
static bool context=true,boot_done=true,boot_ready=true,clock_valid=true,clock_stalled,clock_backward,opening_failed,closing_failed,late_boot;
static unsigned waits;static uint64_t now,step=1;
static bool summary_available=true;
static uint32_t uart_failed_ports;
uint32_t serial_proof_failure_mask(void){return uart_failed_ports;}
static irq_latency_test_summary_t test_summary={.cells=264,.maximum_ns=49999};
bool irq_latency_test_summary(irq_latency_test_summary_t *out){*out=test_summary;return summary_available;}
bool cpu_request_context_allowed(void){return context;}
bool runtime_ready_boot_diagnostics_finished(void){return boot_done;}
bool runtime_ready_is_test_ready(void){return boot_ready;}
bool clock_monotonic_read_ns(uint64_t *out){if(!clock_valid)return false;*out=clock_backward?--now:now;if(!clock_stalled)now+=step;return true;}
void schedule(void){++waits;if(waits==3 && !clock_stalled){boot_done=true;if(late_boot)now+=30000000000ULL;}}
void serial_write_all(const char *s){assert(strstr(s,"[PHYSICAL_LATENCY][WAIT]"));}
void serial_proof_aux_totals(uint64_t *records,uint64_t *bytes){*records=*bytes=0;}
bool serial_proof_begin(uint64_t session){assert(session==HOBBYOS_SERIAL_PROOF_ID);return true;}
bool serial_proof_end(uint64_t session){assert(session==HOBBYOS_SERIAL_PROOF_ID);return true;}
static int call(char name) {history[used++]=name;return fail==name;}
bool stack_test_emit_checked(const char *format,...)
{va_list args;va_start(args,format);vsnprintf(last,sizeof(last),format,args);va_end(args);return !(opening_failed && strstr(format,"[BEGIN]")) && !(closing_failed && strstr(format,"aux_records="));}
int irq_latency_test_proof(bool deferred,uint64_t id,unsigned invocations)
{assert(deferred && id==HOBBYOS_SERIAL_PROOF_ID && invocations==1);return call('p');}
int irq_latency_test_begin(void){return call('b');}
int cmd_mem(int argc,char **argv){assert(argc==1 && !strcmp(argv[0],"mem"));return call('m');}
int irq_latency_test_stress(void){return call('s');}
int irq_latency_test_finish(void){return call('f');}
int irq_latency_test_audit(void){return call('a');}
int irq_latency_test_export(void){return call('e');}
int diagnostic_result_complete(const char *name,int status)
{assert(!strcmp(name,"irqlatency serial") || !strcmp(name,"irqlatency reference"));return status;}
int main(void)
{
    char *args[]={"irqlatency","serial"};
    const char cases[]={0,'p','b','m','s','f','a','e'};
    for(unsigned i=0;i<sizeof(cases);++i){
        memset(history,0,sizeof(history));used=0;fail=cases[i];
        int status=cmd_irqlatency(2,args);
        const char *sequence=fail=='p'?"p":fail=='b'?"pb":fail=='a'?"pbmsfa":"pbmsfae";
        assert(!strcmp(history,sequence));
        assert(status==(fail!=0));
        assert(strstr(last,"timing_acceptance=NOT_GRANTED"));
        assert(strstr(last,fail && fail!='f'?"status=INCOMPLETE":"status=CAPTURE_COMPLETE"));
        if(fail=='f')assert(strstr(last,"measurement=REJECTED"));
    }
    fail=0;
    summary_available=false;used=0;assert(cmd_irqlatency(2,args)==1 && strstr(last,"measurement=UNAVAILABLE"));summary_available=true;
    test_summary.over_budget=1;test_summary.maximum_ns=50000;used=0;assert(cmd_irqlatency(2,args)==1 && strstr(last,"measurement=REJECTED"));
    test_summary.over_budget=0;test_summary.maximum_ns=49999;
    test_summary.missing=1;used=0;assert(cmd_irqlatency(2,args)==1);test_summary.missing=0;
    test_summary.invalid=1;used=0;assert(cmd_irqlatency(2,args)==1);test_summary.invalid=0;
    test_summary.unqualified=11;used=0;assert(cmd_irqlatency(2,args)==1);test_summary.unqualified=0;
    uart_failed_ports=1;used=0;assert(cmd_irqlatency(2,args)==1 && strstr(last,"uart_failed_ports=0x1"));uart_failed_ports=0;
    fail=0;boot_ready=false;used=0;assert(cmd_irqlatency(2,args)==1 && !used && strstr(last,"stage=boot_not_ready"));boot_ready=true;
    boot_done=false;waits=used=0;now=0;assert(!cmd_irqlatency(2,args) && waits==3);
    boot_done=false;waits=used=0;now=0;late_boot=true;assert(cmd_irqlatency(2,args)==1 && !used);late_boot=false;
    boot_done=false;waits=used=0;clock_valid=false;assert(cmd_irqlatency(2,args)==1 && !used && strstr(last,"stage=boot_wait"));clock_valid=true;
    clock_stalled=true;waits=used=0;assert(cmd_irqlatency(2,args)==1 && waits<=1024 && !used);clock_stalled=false;
    now=UINT64_MAX;assert(cmd_irqlatency(2,args)==1 && !used);now=10;
    clock_backward=true;step=0;assert(cmd_irqlatency(2,args)==1 && !used);clock_backward=false;
    now=0;step=30000000000ULL;waits=0;assert(cmd_irqlatency(2,args)==1 && !used);step=1;
    boot_done=true;opening_failed=true;assert(cmd_irqlatency(2,args)==1 && !used && strstr(last,"stage=opening_transport"));opening_failed=false;
    context=false;assert(cmd_irqlatency(2,args)==1 && !used);context=true;
    closing_failed=true;used=0;assert(cmd_irqlatency(2,args)==1 && strstr(last,"stage=closing_transport"));
    closing_failed=false;args[1]="reference";used=0;
    test_summary.over_budget=1;test_summary.maximum_ns=50000;
    assert(cmd_irqlatency(2,args)==0 && strstr(last,"version=5") &&
           strstr(last,"measurement=QUALIFIED") && strstr(last,"reference=EXCEEDED"));
    test_summary.maximum_ns=4000000;used=0;assert(cmd_irqlatency(2,args)==0);
    test_summary.unqualified=11;used=0;
    assert(cmd_irqlatency(2,args)==1 && strstr(last,"measurement=UNQUALIFIED"));
    test_summary.unqualified=0;test_summary.missing=1;used=0;assert(cmd_irqlatency(2,args)==1);test_summary.missing=0;
    test_summary.invalid=1;used=0;assert(cmd_irqlatency(2,args)==1);test_summary.invalid=0;
    summary_available=false;used=0;assert(cmd_irqlatency(2,args)==1);summary_available=true;
    uart_failed_ports=1;used=0;assert(cmd_irqlatency(2,args)==1);uart_failed_ports=0;
    for(unsigned i=1;i<sizeof(cases);++i) { used=0;fail=cases[i];assert(cmd_irqlatency(2,args)==1); }
    fail=0;test_summary.over_budget=0;test_summary.maximum_ns=49999;used=0;
    assert(cmd_irqlatency(2,args)==0 && strstr(last,"reference=BELOW"));
    puts("Physical serial actual command: PASS real_work_before_cutoff rejected_postcheck_retained export_required");
}

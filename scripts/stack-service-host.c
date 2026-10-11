/* Hosted request-state executions for the native raw-audit decoder. */
#define HOBBYOS_SELFTEST 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/smp/cpu_request.h"
#include "../kernel/src/smp/smp_topology.h"
#include "../kernel/src/smp/smp_boot.h"
static cpu_request_storage_t request;
static cpu_request_audit_t audit[2];
static bool execute(void *context,const cpu_request_spec_t *spec,cpu_request_sample_t *sample)
{
    (void)context;sample->cr3=spec->root;return true;
}
static void save(const char *directory,const char *name,const void *data,size_t size)
{
    char path[4096];assert(snprintf(path,sizeof(path),"%s/%s.bin",directory,name)>0);
    FILE *file=fopen(path,"wb");assert(file);assert(fwrite(data,1,size,file)==size);assert(fclose(file)==0);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    uint64_t root=0x123000,completed=2;uint32_t count=2,panic=0;bool overflow=false;
    SmpCpuInfo cpus[HOBBYOS_MAX_CPUS]={0};
    for (unsigned cpu=0;cpu<2;++cpu) {
        cpus[cpu].slot=cpu;cpus[cpu].apic_id=cpu+8;cpus[cpu].state=CPU_STATE_ONLINE;
    }
    cpu_request_spec_t spec={.root=root,.targets=3,.operation=CPU_REQUEST_INVALIDATE,
        .interval={0xffffe00000801000,0xffffe00000804000,4,TLB_PAGES},.apic={8,9}};
    for (unsigned index=0;index<2;++index) {
        assert(cpu_request_admit(&request));assert(cpu_request_publish(&request,&spec)==CPU_REQUEST_OK);
        audit[index].spec=request.spec;audit[index].mapping=true;
        assert(cpu_request_handle(&request,0,8,execute,NULL));
        assert(cpu_request_handle(&request,1,9,execute,NULL));
        cpu_request_collect(&request,&audit[index].result);
        audit[index].result.sent=2;audit[index].result.notifications=1;audit[index].result.elapsed_ns=100;
        audit[index].result.timing_schema=CPU_REQUEST_TIMING_SCHEMA;
        assert(cpu_request_retire(&request,true));
    }
    save(argv[1],"g_request",&request,sizeof(request));save(argv[1],"g_audit",audit,sizeof(audit));
    save(argv[1],"g_kernel_root_physical",&root,sizeof(root));save(argv[1],"g_request_root",&root,sizeof(root));
    save(argv[1],"g_audit_admitted",&completed,sizeof(completed));save(argv[1],"g_audit_completed",&completed,sizeof(completed));
    save(argv[1],"g_audit_overflow",&overflow,sizeof(overflow));save(argv[1],"g_panic_in_progress",&panic,sizeof(panic));
    save(argv[1],"g_cpu_count",&count,sizeof(count));save(argv[1],"g_cpus",cpus,sizeof(cpus));
    puts("STACK_SERVICE_HOST actual_publish_handle_collect_retire=PASS hardware_execution=NONE");
    return 0;
}

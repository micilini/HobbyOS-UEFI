/* Actual diagnostic layouts for stopped native-workload service accounting. */
#ifndef HOBBYOS_REQUEST_LAYOUT_NO_SELFTEST
#define HOBBYOS_SELFTEST 1
#endif
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/smp/cpu_request.h"
#include "../kernel/src/smp/smp_topology.h"
int main(void)
{
#define F(type,field) printf("\"" #field "\":%zu,",offsetof(type,field))
    printf("{\"audit\":");
#ifdef HOBBYOS_SELFTEST
    printf("{");
    F(cpu_request_audit_t,spec);F(cpu_request_audit_t,result);F(cpu_request_audit_t,mapping);
    printf("\"size\":%zu}",sizeof(cpu_request_audit_t));
#else
    printf("null");
#endif
    printf(",\"request\":{");
    F(cpu_request_storage_t,active);F(cpu_request_storage_t,next_generation);
    F(cpu_request_storage_t,owner);F(cpu_request_storage_t,quarantined);
    printf("\"size\":%zu},\"cpu\":{",sizeof(cpu_request_storage_t));
    F(SmpCpuInfo,slot);F(SmpCpuInfo,apic_id);F(SmpCpuInfo,is_bsp);F(SmpCpuInfo,state);
    printf("\"size\":%zu},\"result\":{",sizeof(SmpCpuInfo));
    F(cpu_request_result_t,status);F(cpu_request_result_t,requested);
    F(cpu_request_result_t,acknowledged);F(cpu_request_result_t,missing);
    F(cpu_request_result_t,failed);F(cpu_request_result_t,sent);F(cpu_request_result_t,send_failed);
    F(cpu_request_result_t,notifications);F(cpu_request_result_t,generation);F(cpu_request_result_t,elapsed_ns);
    F(cpu_request_result_t,timing_schema);
    printf("\"size\":%zu},\"capacity\":%u}\n",sizeof(cpu_request_result_t),
#ifdef HOBBYOS_SELFTEST
        CPU_REQUEST_AUDIT_CAPACITY
#else
        0u
#endif
    );
    return 0;
}

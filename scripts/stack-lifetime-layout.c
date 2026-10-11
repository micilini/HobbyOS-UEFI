/* Actual service layout for a read-only, post-ACK stack retirement capture. */
#define HOBBYOS_SELFTEST 1
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/smp/cpu_request.h"
int main(void)
{
#define FIELD(type,field) printf("\"" #field "\":%zu,",offsetof(type,field))
    printf("{\"schema\":1,\"audit\":{");
    FIELD(cpu_request_audit_t,spec);FIELD(cpu_request_audit_t,result);FIELD(cpu_request_audit_t,mapping);
    printf("\"size\":%zu},\"spec\":{",sizeof(cpu_request_audit_t));
    FIELD(cpu_request_spec_t,generation);FIELD(cpu_request_spec_t,root);FIELD(cpu_request_spec_t,interval);
    FIELD(cpu_request_spec_t,targets);FIELD(cpu_request_spec_t,apic);FIELD(cpu_request_spec_t,operation);
    printf("\"size\":%zu},\"result\":{",sizeof(cpu_request_spec_t));
    FIELD(cpu_request_result_t,status);FIELD(cpu_request_result_t,requested);FIELD(cpu_request_result_t,acknowledged);
    FIELD(cpu_request_result_t,missing);FIELD(cpu_request_result_t,failed);FIELD(cpu_request_result_t,send_failed);
    FIELD(cpu_request_result_t,generation);
    printf("\"size\":%zu},\"interval\":{",sizeof(cpu_request_result_t));
    FIELD(tlb_interval_t,first);FIELD(tlb_interval_t,last);FIELD(tlb_interval_t,pages);FIELD(tlb_interval_t,strategy);
    printf("\"size\":%zu}}\n",sizeof(tlb_interval_t));
    return 0;
}

/* Compile the actual request state machine without any SELFTEST definition. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/smp/cpu_request.h"
#ifdef HOBBYOS_SELFTEST
#error Production request admission must be compiled without SELFTEST
#endif
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"production admission check %u line %u: %s\n",checks,__LINE__,#x);abort();}} while(0)
static cpu_request_storage_t storage,before;
int main(void)
{
    for(unsigned operation=0;operation<256;++operation) {
        memset(&storage,0,sizeof(storage));
        cpu_request_spec_t spec={.root=0x100000,.targets=3,.apic={1,9},
            .interval={0,0,1,TLB_PAGES},.operation=(cpu_request_operation_t)operation};
        CHECK(cpu_request_admit(&storage));before=storage;
        cpu_request_status_t result=cpu_request_publish(&storage,&spec);
        if(operation==CPU_REQUEST_INVALIDATE || operation==CPU_REQUEST_CACHE_WRITEBACK) {
            CHECK(result==CPU_REQUEST_OK && storage.active && storage.next_generation==1);
        } else {
            CHECK(result==CPU_REQUEST_INVALID);
            CHECK(!memcmp(&storage,&before,sizeof(storage)));
        }
    }
    printf("PROTECTION_PRODUCTION_HOST checks=%u status=PASS test_operations=REJECTED\n",checks);
    return 0;
}

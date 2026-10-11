#include <assert.h>
#include <stdio.h>
#include "../kernel/src/memory/mapping_probe.h"
int main(void)
{
 mapping_probe_state_t s={.address=0xffffd00001000000,.generation=5,.site=0xffffffff80010000,
 .recovery=0xffffffff80010004,.slot=31,.armed=true};
 assert(mapping_probe_match(&s,s.site,s.address,0,8,31,5));
 assert(!mapping_probe_match(&s,s.site+1,s.address,0,8,31,5));
 assert(!mapping_probe_match(&s,s.site,s.address+1,0,8,31,5));
 assert(!mapping_probe_match(&s,s.site,s.address,0,8,30,5));
 assert(!mapping_probe_match(&s,s.site,s.address,0,8,31,4));
 assert(!mapping_probe_match(&s,s.site,s.address,0,11,31,5));
 for(unsigned error=1;error<128;++error)assert(!mapping_probe_match(&s,s.site,s.address,error,8,31,5));
 s.armed=false;assert(!mapping_probe_match(&s,s.site,s.address,0,8,31,5));
 s.armed=true;s.seen=true;assert(!mapping_probe_match(&s,s.site,s.address,0,8,31,5));
 puts("MAPPING_PROBE_HOST exact_site_and_fault_boundaries=PASS");
}

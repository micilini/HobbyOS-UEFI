/* Current scalar layouts for a stopped, read-only guarded-stack capture. */
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/memory/stack_domain.h"
#include "../kernel/src/smp/smp_topology.h"
#include "../kernel/src/cpu/tss.h"
int main(void)
{
    printf("{\"schema\":1,\"records\":%u,\"record_bytes\":%zu,\"sequence\":%zu,\"cells\":%zu,"
           "\"cell_count\":%u,\"cpu_bytes\":%zu,\"tss_bytes\":%zu,\"cpu_max\":%u}\n",
        STACK_OWNER_COUNT,sizeof(stack_owner_record_t),offsetof(stack_owner_record_t,sequence),
        offsetof(stack_owner_record_t,cells),STACK_META_CELLS,sizeof(SmpCpuInfo),sizeof(Tss64),HOBBYOS_MAX_CPUS);
    return 0;
}

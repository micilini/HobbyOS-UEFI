/* Read-only capture offsets from the current owner definitions. */
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/memory/paging_build.h"
#include "../kernel/src/memory/gdt.h"
#include "../kernel/src/cpu/tss.h"
#include "../kernel/src/smp/smp_topology.h"
#include "../kernel/src/smp/smp_boot.h"
#include "../kernel/src/utils/bitmap.h"
#define FIELD(type, member) printf("\"" #member "\":%zu,", offsetof(type, member))
int main(void)
{
    printf("{\"schema\":1,\"cpu\":{");
    FIELD(SmpCpuInfo, slot); FIELD(SmpCpuInfo, apic_id); FIELD(SmpCpuInfo, is_bsp);
    FIELD(SmpCpuInfo, state); FIELD(SmpCpuInfo, stack_top);
    FIELD(SmpCpuInfo, tss_ptr); FIELD(SmpCpuInfo, gdt_ptr);
    printf("\"size\":%zu,\"online\":%u,\"stack_bytes\":%u},\"tss\":{",
           sizeof(SmpCpuInfo), CPU_STATE_ONLINE, SMP_AP_STACK_BYTES);
    FIELD(Tss64, rsp0); FIELD(Tss64, ist1); FIELD(Tss64, ist2); FIELD(Tss64, ist3);
    printf("\"size\":%zu},\"gdt\":{", sizeof(Tss64));
    FIELD(GdtTable, tss); printf("\"size\":%zu},\"bitmap\":{", sizeof(GdtTable));
    FIELD(Bitmap, buffer); FIELD(Bitmap, size); FIELD(Bitmap, total_bits);
    printf("\"size_bytes\":%zu},\"table\":{", sizeof(Bitmap));
    FIELD(mapping_table_t, cpu); FIELD(mapping_table_t, physical);
    printf("\"size\":%zu},\"window\":{", sizeof(mapping_table_t));
    FIELD(mapping_window_t, tables); FIELD(mapping_window_t, allocated); FIELD(mapping_window_t, ready);
    printf("\"size\":%zu,\"capacity\":%u},\"build\":{", sizeof(mapping_window_t), MAPPING_WINDOW_TABLES);
    FIELD(paging_build_t, tables); FIELD(paging_build_t, count); FIELD(paging_build_t, window);
    FIELD(paging_build_t, verified); FIELD(paging_build_t, activated);
    printf("\"size\":%zu,\"capacity\":%u,\"table_stride\":%zu}}\n",
           sizeof(paging_build_t), PAGING_BUILD_TABLES, sizeof(paging_build_table_t));
    return 0;
}

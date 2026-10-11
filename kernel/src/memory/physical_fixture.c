#include "physical_fixture.h"
#include "physical_translation.h"
#include "physical_internal.h"
#include "boot_address.h"
#include "paging_private.h"
#include "pmem.h"
#include "heap.h"
#include "gdt.h"
#include "../cpu/tss.h"
#include "../utils/bitmap.h"
#include "../smp/smp_topology.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "../drivers/serial.h"
#include <stdarg.h>

#define U64(v) ((unsigned long long)(v))
static uint32_t g_fixture_owner;
extern char _kernel_start[];

static bool record(const char *format, ...) __attribute__((format(printf,1,2)));
static bool record(const char *format, ...)
{
    char line[960]; va_list args; va_start(args,format);
    int n=kvsnprintf(line,sizeof(line),format,args); va_end(args);
    if (n<0 || (size_t)n>=sizeof(line)) return false;
    serial_write_all(line); return true;
}

static bool ceiling_cases(void)
{
    static const struct {size_t limit,count,a,b,expected;} cases[] = {
        {65,1,64,100,64},{65,1,65,100,SIZE_MAX},{65,2,63,64,63},
        {65,2,64,65,SIZE_MAX},{65,2,2,3,2},{65,0,2,3,SIZE_MAX},
        {129,1,2,3,SIZE_MAX},{65,SIZE_MAX,2,3,SIZE_MAX},
        {0,1,2,3,SIZE_MAX},{64,1,63,100,63}};
    bool ok=true;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        uint8_t bytes[16], saved[16]; memset(bytes,0xff,sizeof(bytes));
        Bitmap map={bytes,sizeof(bytes),128};
        bitmap_set(&map,cases[i].a,false); bitmap_set(&map,cases[i].b,false);
        memcpy(saved,bytes,sizeof(bytes)); size_t index=SIZE_MAX;
        BitmapSearchStats work={0};
        bool found=bitmap_find_zero_run_below(&map,1,cases[i].limit,cases[i].count,&index,&work);
        uint64_t low,high; memcpy(&low,bytes,8); memcpy(&high,bytes+8,8);
        bool unchanged=memcmp(bytes,saved,sizeof(bytes))==0;
        ok &= found==(cases[i].expected!=SIZE_MAX) && index==cases[i].expected && unchanged;
        ok &= record("\n[PHYSICAL][SEARCH] schema=1 case=%u synthetic=1 bits=128 start=1 "
            "limit=%llu count=%llu allocated_low=0x%llx allocated_high=0x%llx found=%u index=%llu "
            "words=%llu unchanged=%u\n",i,U64(cases[i].limit),U64(cases[i].count),U64(low),U64(high),
            found,U64(index),U64(work.words_examined),unchanged);
    }
    return ok;
}

static bool roundtrip(const char *name, const void *pointer, size_t bytes)
{
    uint64_t physical=0, walked=0;
    bool valid=physical_address_range(pointer,bytes,&physical) &&
        paging_query_range((uintptr_t)pointer,bytes,&walked) && walked==physical;
    return record("\n[PHYSICAL][POINTER] schema=1 name=%s virtual=0x%llx bytes=%llu "
        "physical=0x%llx walked=0x%llx valid=%u\n",name,U64((uintptr_t)pointer),U64(bytes),
        U64(physical),U64(walked),valid) && valid;
}

int physical_fixture_run(void)
{
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&g_fixture_owner,&expected,1,false,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    physical_translation_stats_t before,after;
    physical_translation_stats(&before);
    bool ok=before.initialized && before.bootstrap_sealed && before.normal_mapping;
    ok &= ceiling_cases();
    uint64_t root=paging_kernel_root_physical(); void *root_pointer=NULL;
    ok &= physical_pointer_range(root,4096,PHYSICAL_RAM,&root_pointer);
    if (root_pointer) ok &= roundtrip("active_root",root_pointer,4096);
    ok &= roundtrip("image",_kernel_start,1);
    void *heap=kmalloc(64);
    if (heap) { ok &= roundtrip("heap",heap,64); kfree(heap); } else ok=false;

    void *one=pmm_alloc_frame(), *run=pmm_alloc_contiguous_frames(3);
    void *one_pointer=NULL,*run_pointer=NULL;
    if (one && run && physical_pointer_range((uintptr_t)one,4096,PHYSICAL_RAM,&one_pointer) &&
        physical_pointer_range((uintptr_t)run,3*4096,PHYSICAL_RAM,&run_pointer)) {
        *(volatile uint64_t *)one_pointer=UINT64_C(0x6d656d6f72793131);
        *(volatile uint64_t *)((uint8_t *)run_pointer+3*4096-8)=UINT64_C(0x7461696c33333333);
        ok &= *(volatile uint64_t *)one_pointer==UINT64_C(0x6d656d6f72793131) &&
            *(volatile uint64_t *)((uint8_t *)run_pointer+3*4096-8)==UINT64_C(0x7461696c33333333);
        ok &= roundtrip("single_frame",one_pointer,4096);
        ok &= roundtrip("contiguous_frames",run_pointer,3*4096);
    } else ok=false;
    if (one) pmm_free_frame(one);
    if (run) for(unsigned i=0;i<3;i++) pmm_free_frame((void *)((uintptr_t)run+i*4096));

    for(cpu_slot_t slot=0;slot<g_cpu_count;slot++) {
        const SmpCpuInfo *cpu=smp_cpu_by_slot_const(slot);
        if (!cpu || cpu->is_bsp) continue;
        bool valid=cpu->stack_top>=SMP_AP_STACK_BYTES;
        uint64_t stack=0,tss=0,gdt=0;
        valid = valid && physical_address_range((void *)(cpu->stack_top-SMP_AP_STACK_BYTES),
            SMP_AP_STACK_BYTES,&stack) && physical_address_range(cpu->tss_ptr,sizeof(Tss64),&tss) &&
            physical_address_range(cpu->gdt_ptr,sizeof(GdtTable),&gdt);
        ok &= valid && record("\n[PHYSICAL][CPU] schema=1 slot=%u apic=%u stack_top=0x%llx "
            "stack_physical=0x%llx stack_bytes=%u tss_virtual=0x%llx tss_physical=0x%llx "
            "gdt_virtual=0x%llx gdt_physical=0x%llx valid=%u\n",slot,cpu->apic_id,
            U64(cpu->stack_top),U64(stack),SMP_AP_STACK_BYTES,U64((uintptr_t)cpu->tss_ptr),U64(tss),
            U64((uintptr_t)cpu->gdt_ptr),U64(gdt),valid);
    }
    PmmReachability memory={0};
    ok &= pmm_get_reachability(&memory) && pmm_validate_integrity();
    physical_translation_stats(&after);
    ok &= after.rejected==before.rejected && after.rejected==0;
    ok &= record("\n[PHYSICAL][RESULT] schema=1 synthetic_cases=10 represented_frames=%llu "
        "free_frames=%llu eligible_free_frames=%llu above_ceiling_free_frames=%llu ceiling=%llu minimum=%llu "
        "bitmap_physical=0x%llx bitmap_bytes=%llu root=0x%llx validations=%llu rejected=%llu "
        "owned_frames=4 released_frames=%u status=%s\n",U64(memory.represented_frames),U64(memory.free_frames),
        U64(memory.eligible_free_frames),U64(memory.above_ceiling_free_frames),U64(memory.ceiling),
        U64(memory.minimum),U64(memory.bitmap_physical),U64(memory.bitmap_bytes),U64(root),
        U64(after.validations),U64(after.rejected),(one?1u:0u)+(run?3u:0u),ok?"PASS":"FAIL");
    __atomic_store_n(&g_fixture_owner,0,__ATOMIC_RELEASE);
    return ok?0:1;
}

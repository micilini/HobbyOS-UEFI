#include "ram_map_inspect.h"
#include "physical_translation.h"
#include "pmem.h"
#include "heap.h"
#include "boot_address.h"
#include "../drivers/serial.h"
#include "../core/io.h"
#include "../core/spinlock.h"
#include "../libc/string.h"
#include "../shell/commands/cmd_tasktest.h"
#include <stdarg.h>

#define U64(x) ((unsigned long long)(x))
static unsigned g_owner;
static uint64_t g_generation, g_bytes, g_records, g_digest;

static bool emit(const char *format, ...) __attribute__((format(printf,1,2)));
static bool emit(const char *format, ...)
{
    char line[768]; va_list args; va_start(args,format);
    int n=kvsnprintf(line,sizeof(line),format,args); va_end(args);
    if(n<1 || (size_t)n>=sizeof(line)-32 || line[n-1]!='\n') return false;
    uint32_t crc=tasktest_transport_crc32(line,n-1);
    int tail=ksnprintf(line+n-1,sizeof(line)-(size_t)n+1," crc=%08x\n",crc);
    if(tail<0 || (size_t)tail>=sizeof(line)-(size_t)n+1) return false;
    irq_flags_t flags=irq_save();
    for(const unsigned char *p=(const unsigned char *)line;*p;++p) {
        outb(0x402,*p);g_digest=(g_digest^*p)*UINT64_C(1099511628211);++g_bytes;
    }
    ++g_records;irq_restore(flags);return true;
}
static bool run(void *context, const ram_leaf_run_t *r)
{
    uint64_t *bytes=context; *bytes+=r->bytes;
    return emit("[RAM_MAP][LEAVES] va=0x%llx physical=0x%llx bytes=%llu shift=%u flags=0x%llx\n",
        U64(r->virtual_address),U64(r->physical),U64(r->bytes),r->shift,U64(r->flags));
}
static bool high_allocation(const ram_map_snapshot_t *snapshot)
{
    uint64_t physical=0, table=0, reallocated=0, table_cpu=0, entry=0, inverse=0;
    void *pointer=NULL;
    bool ok=pmm_test_alloc_range(3,1,UINT64_C(0x100000000),snapshot->plan->ceiling,&physical);
    if(ok) ok=pmm_test_alloc_range(1,1,UINT64_C(0x100000000),snapshot->plan->ceiling,&table);
    if(ok) ok=physical_pointer_range(physical,3*4096,PHYSICAL_RAM,&pointer) &&
        physical_address_range(pointer,3*4096,&inverse) && inverse==physical &&
        (uintptr_t)pointer==RAM_DIRECT_BASE+physical;
    uint64_t first=0,last=0;
    if(ok) {
        *(volatile uint64_t *)pointer=UINT64_C(0x686967686672616d);
        *(volatile uint64_t *)((uint8_t *)pointer+3*4096-8)=UINT64_C(0x7461696c61636365);
        first=*(volatile uint64_t *)pointer;
        last=*(volatile uint64_t *)((uint8_t *)pointer+3*4096-8);
        ok=first==UINT64_C(0x686967686672616d) && last==UINT64_C(0x7461696c61636365) &&
            paging_ram_scratch_table(table,physical,&table_cpu,&entry);
    }
    if(table) pmm_free_frame((void *)(uintptr_t)table);
    if(physical) {
        for(unsigned i=0;i<3;++i) pmm_free_frame((void *)(uintptr_t)(physical+i*4096));
        ok=pmm_test_alloc_range(3,1,physical,physical+3*4096,&reallocated) && reallocated==physical && ok;
        if(reallocated) for(unsigned i=0;i<3;++i) pmm_free_frame((void *)(uintptr_t)(reallocated+i*4096));
    }
    bool released=physical && table && pmm_is_frame_free(physical) && pmm_is_frame_free(physical+8192) && pmm_is_frame_free(table);
    return emit("[RAM_MAP][HIGH] physical=0x%llx cpu=0x%llx bytes=12288 inverse=0x%llx first=0x%llx last=0x%llx table=0x%llx table_cpu=0x%llx entry=0x%llx reallocated=0x%llx released=%u valid=%u\n",
        U64(physical),U64((uintptr_t)pointer),U64(inverse),U64(first),U64(last),U64(table),U64(table_cpu),
        U64(entry),U64(reallocated),released,ok) && released && ok;
}

int ram_map_fixture_run(bool high_memory)
{
    unsigned empty=0;
    if(!__atomic_compare_exchange_n(&g_owner,&empty,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    ++g_generation;g_bytes=g_records=0;g_digest=UINT64_C(14695981039346656037);
    bool ok=emit("[RAM_MAP][BEGIN] schema=1 generation=%llu high=%u\n",U64(g_generation),high_memory);
    ram_map_snapshot_t snapshot={0};uint64_t mapped=0;
    ok=paging_ram_snapshot(&snapshot,run,&mapped) && ok;
    if(ok) {
        const ram_map_plan_t *p=snapshot.plan;const ram_mtrr_inputs_t *m=snapshot.mtrrs;
        ok=emit("[RAM_MAP][STATE] root=0x%llx table_cpu=0x%llx base=0x%llx span=0x%llx epoch=%llu ceiling=0x%llx bytes=%llu tables=%llu extended_max=0x%x extended_edx=0x%x address_eax=0x%x nx=%u page1g=%u physical_bits=%u\n",
            U64(snapshot.root),U64(snapshot.table_pointer),U64(RAM_DIRECT_BASE),U64(RAM_DIRECT_BYTES),
            U64(snapshot.epoch),U64(p->ceiling),U64(mapped),U64(snapshot.initial_tables),
            p->features.extended_max,p->features.extended_edx,p->features.address_eax,
            p->features.nx,p->features.page1g,p->features.physical_bits) && mapped==p->bytes;
        ok=emit("[RAM_MAP][MTRR] capability=0x%llx defaults=0x%llx count=%u\n",U64(m->capability),U64(m->defaults),m->count) && ok;
        for(unsigned i=0;ok && i<m->count;++i)
            ok=emit("[RAM_MAP][VARIABLE] index=%u base=0x%llx mask=0x%llx\n",i,U64(m->base[i]),U64(m->mask[i]));
        for(unsigned i=0;ok && i<11;++i)
            ok=emit("[RAM_MAP][FIXED] index=%u value=0x%llx\n",i,U64(m->fixed[i]));
        for(unsigned i=0;ok && i<g_physical_contract.count;++i) {
            const physical_region_t *r=&g_physical_contract.regions[i];
            ok=emit("[RAM_MAP][DESCRIPTOR] index=%u first=0x%llx end=0x%llx type=%u attributes=0x%llx\n",
                i,U64(r->first),U64(r->end),r->type,U64(r->attributes));
        }
        ok=emit("[RAM_MAP][EXCLUSION] framebuffer_first=0x%llx framebuffer_end=0x%llx image_first=0x%llx image_end=0x%llx\n",
            U64(p->framebuffer.first),U64(p->framebuffer.end),U64(p->image_first),U64(p->image_end)) && ok;
        PmmPointerSnapshot bitmap={0};
        ok=pmm_pointer_snapshot(&bitmap) && ok;
        ok=emit("[RAM_MAP][BITMAP] physical=0x%llx bytes=%llu owner=0x%llx reserved_owner=0x%llx member=0x%llx reserved_member=0x%llx\n",
            U64(bitmap.physical),U64(bitmap.bytes),U64(bitmap.bitmap_owner),U64(bitmap.reservation_owner),
            U64(bitmap.bitmap_member),U64(bitmap.reservation_member)) && ok;
        const BootInfo *boot=physical_boot_view();
        const boot_physical_objects_t *origins=physical_boot_objects();
        if (!boot || !origins) ok=false;
        else ok=emit("[RAM_MAP][BOOT] view=0x%llx map=0x%llx descriptors=0x%llx font=0x%llx header=0x%llx glyphs=0x%llx glyph_physical=0x%llx logo=0x%llx pixels=0x%llx pixels_physical=0x%llx rsdp=0x%llx rsdp_physical=0x%llx framebuffer=0x%llx framebuffer_physical=0x%llx\n",
            U64((uintptr_t)boot),U64((uintptr_t)boot->memory_map),U64((uintptr_t)boot->memory_map->Base),
            U64((uintptr_t)boot->font),U64(boot->font?(uintptr_t)boot->font->psf_header:0),
            U64(boot->font?(uintptr_t)boot->font->glyph_buffer:0),U64(origins->glyphs),U64((uintptr_t)boot->logo),
            U64(boot->logo?(uintptr_t)boot->logo->PixelBuffer:0),U64(origins->pixels),U64((uintptr_t)boot->rsdp),
            U64(origins->rsdp),U64((uintptr_t)boot->framebuffer->BaseAddress),U64(origins->framebuffer)) && ok;
        void *heap=kmalloc(64);uint64_t physical=0;
        bool heap_ok=heap && physical_address_range(heap,64,&physical) &&
            (uintptr_t)heap==RAM_DIRECT_BASE+physical;
        if(heap_ok) { *(volatile uint64_t *)heap=UINT64_C(0x6469726563747261);heap_ok=*(volatile uint64_t *)heap==UINT64_C(0x6469726563747261); }
        ok=emit("[RAM_MAP][HEAP] cpu=0x%llx physical=0x%llx bytes=64 accessed=%u\n",U64((uintptr_t)heap),U64(physical),heap_ok) && heap_ok && ok;
        if(heap) kfree(heap);
        if(high_memory) ok=high_allocation(&snapshot) && ok;
    }
    ok=pmm_validate_integrity() && ok;
    ok=emit("[RAM_MAP][END] generation=%llu status=%s\n",U64(g_generation),ok?"PASS":"FAIL") && ok;
    char line[256];int n=ksnprintf(line,sizeof(line),
        "\n[RAM_MAP_TRACE][COMPLETE] generation=%llu channel=debugcon port=0x402 bytes=%llu records=%llu fnv64=0x%llx high=%u status=%s\n",
        U64(g_generation),U64(g_bytes),U64(g_records),U64(g_digest),high_memory,ok?"PASS":"FAIL");
    if(n>0 && (size_t)n<sizeof(line)) serial_write_all(line);else ok=false;
    __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return ok?0:1;
}

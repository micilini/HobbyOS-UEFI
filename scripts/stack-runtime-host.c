/* Actual kernel stack adapter. Only privileged/global PMM, IRQ, TSS and
 * shootdown boundaries are substituted; slot and leaf transactions are real. */
#define HOBBYOS_STACK_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../kernel/src/memory/kernel_stack.c"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"stack runtime check %u line %u: %s\n",checks,__LINE__,#x);exit(1); } } while(0)
static bool host_if,context_allowed=true,install_fail,wait_fail,expect_panic;
static unsigned held_locks,alloc_calls,alloc_fail,freed_pages,waits,local_flushes;
static uint64_t request_generation;
static jmp_buf panic_return;
static const char *panic_reason;
static image_permissions_t image;
static ram_map_plan_t ram;
static PageTable root,tables[STACK_WINDOW_TABLES];
static struct { bool activated; stack_window_t stacks; } g_boot_build;
static spinlock_t g_paging_lock;
static uint64_t g_image_mapping_epoch;
#define window g_boot_build.stacks
static unsigned table_count;
enum { BLOCKS=1600, REQUIRED_TASK_BACKLOG=1000+257+64+32 };
static struct { unsigned char data[32768]; uint32_t pages,freed; bool mapped,ack; } blocks[BLOCKS];
static uint64_t address(unsigned i) {return UINT64_C(0x100000000)+(uint64_t)i*65536;}
static unsigned block_index(uint64_t physical)
{ CHECK(physical>=address(0) && physical<address(BLOCKS));return (unsigned)((physical-address(0))/65536); }

void kpanic(const char *reason)
{ panic_reason=reason;CHECK(expect_panic);longjmp(panic_return,1); }
bool irq_is_enabled(void) {return host_if;}
void spinlock_init(spinlock_t *lock) {memset(lock,0,sizeof(*lock));}
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{CHECK(!lock->locked);irq_flags_t before=host_if;host_if=false;lock->locked=1;++held_locks;return before;}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags)
{CHECK(lock->locked && held_locks);lock->locked=0;--held_locks;host_if=flags;}
void *pmm_alloc_contiguous_frames(size_t pages)
{
    CHECK(pages==4 || pages==8);
    if (++alloc_calls==alloc_fail) return NULL;
    for(unsigned i=0;i<BLOCKS;++i) if(!blocks[i].pages || blocks[i].freed==blocks[i].pages) {
        blocks[i].pages=pages;blocks[i].freed=0;blocks[i].mapped=false;blocks[i].ack=false;
        memset(blocks[i].data,0xcc,sizeof(blocks[i].data));return (void *)(uintptr_t)address(i);
    }
    return NULL;
}
bool physical_pointer_range(uint64_t physical,size_t bytes,physical_kind_t kind,void **out)
{
    CHECK(kind==PHYSICAL_RAM);unsigned i=block_index(physical);
    CHECK(physical==address(i) && bytes==blocks[i].pages*4096);
    *out=blocks[i].data;return true;
}
void pmm_free_frame(void *frame)
{
    uint64_t physical=(uintptr_t)frame;unsigned i=block_index(physical);
    CHECK(physical>=address(i) && physical<address(i)+blocks[i].pages*4096);
    CHECK(!blocks[i].mapped || blocks[i].ack);
    CHECK(blocks[i].freed<blocks[i].pages);++blocks[i].freed;++freed_pages;
}
bool tss_bsp_backing(uint64_t physical[3])
{for(unsigned i=0;i<3;++i)physical[i]=0x330000+i*32768;return true;}
bool tss_install_guarded_ist(const uint64_t tops[3])
{
    for(unsigned i=0;i<3;++i) {stack_owner_view_t v;CHECK(kernel_stack_read(i,&v));
        CHECK(v.end==tops[i] && !v.owned && paging_stack_validate((stack_handle_t){v.generation,i}));}
    return true;
}
bool cpu_request_mapping_context_allowed(void) {return host_if && !held_locks && context_allowed;}
bool kernel_tlb_bootstrap_invalidate(uint64_t first,uint64_t bytes)
{CHECK(!host_if && g_stack_phase==STACK_BOOT_ACTIVE && first>=STACK_REGION_BASE && bytes);++local_flushes;return true;}
cpu_request_result_t kernel_mapping_invalidate(uint64_t first,uint64_t bytes)
{
    CHECK(host_if && !held_locks && g_stack_phase==STACK_RUNTIME);
    unsigned slot=(unsigned)((first-STACK_REGION_BASE)/STACK_SLOT_BYTES);
    stack_owner_view_t v;CHECK(kernel_stack_read(slot,&v) && first==v.base && bytes==v.capacity);
    CHECK(v.state==STACK_ALLOCATED || v.state==STACK_RETIRING);
    unsigned i=block_index(v.physical);CHECK(!blocks[i].freed);
    stack_handle_t stolen;
    CHECK(!stack_owner_reserve(&g_stack_owners[slot],slot,STACK_KIND_TASK,16384,address(BLOCKS-1),true,0,&stolen));
    ++waits;
    cpu_request_result_t result={.status=wait_fail ? CPU_REQUEST_TIMEOUT : CPU_REQUEST_OK,
        .generation=++request_generation,.requested=0xffffff,.acknowledged=wait_fail ? 0x7fffff : 0xffffff,
        .missing=wait_fail ? 0x800000 : 0};
    if (!wait_fail && v.state==STACK_RETIRING) blocks[i].ack=true;
    return result;
}
static bool allocate_table(void *context,mapping_table_t *out)
{
    (void)context;CHECK(table_count<STACK_WINDOW_TABLES);
    *out=(mapping_table_t){&tables[table_count],UINT64_C(0x200000000)+table_count*4096};++table_count;return true;
}
static void release_table(void *context,mapping_table_t table) {(void)context;(void)table;CHECK(false);}
#define paging_stack_install actual_paging_stack_install
#define g_image_permissions image
#define g_ram_plan ram
#include "stack-paging-extracted.h"
#undef paging_stack_install
#undef g_image_permissions
#undef g_ram_plan
bool paging_stack_install(stack_handle_t h,bool prepared)
{
    if (install_fail) return false;
    if (!actual_paging_stack_install(h,prepared)) return false;
    stack_owner_view_t v;CHECK(kernel_stack_read(h.slot,&v) && v.generation==h.generation);
    blocks[block_index(v.physical)].mapped=true;
    return true;
}
bool paging_stack_remove(stack_handle_t h,uint64_t id,uint64_t life,bool unpublished)
{
    stack_owner_view_t v;
    if (!kernel_stack_read(h.slot,&v) || v.generation!=h.generation || !stack_window_check(&window,&root,&v) ||
        !stack_owner_begin_retire(record(h),h,id,life,unpublished)) return false;
    v.state=STACK_RETIRING;return stack_window_remove(&window,&v);
}
bool paging_stack_validate(stack_handle_t h)
{
    stack_owner_view_t v;if (!kernel_stack_read(h.slot,&v) || v.generation!=h.generation) return false;
    return h.slot==STACK_BSP_OWNER || stack_window_check(&window,&root,&v);
}
static void reset(void)
{
    memset(g_stack_owners,0,sizeof(g_stack_owners));memset(blocks,0,sizeof(blocks));
    memset(&root,0,sizeof(root));memset(&window,0,sizeof(window));
    g_boot_build.activated=false;memset(&g_paging_lock,0,sizeof(g_paging_lock));g_image_mapping_epoch=0;
    g_stack_layout=(stack_layout_t){.owners=g_stack_owners};g_stack_phase=STACK_BOOT_COLD;
    g_stack_allocated=g_stack_retired=g_stack_quarantined=g_stack_count_sequence=g_stack_high_water=0;
    host_if=install_fail=wait_fail=expect_panic=false;context_allowed=true;
    held_locks=alloc_calls=alloc_fail=freed_pages=waits=local_flushes=table_count=0;request_generation=0;
    image=(image_permissions_t){.ready=true,.first=0x200000,.end=0x400000,
        .input={.bsp_guard_first=0x3fb000,.bsp_guard_end=0x3fc000,.bsp_stack_first=0x3fc000,.bsp_stack_end=0x400000}};
    image.input.sections[IMAGE_BSS]=(image_section_t){0x300000,0x400000,IMAGE_BSS};
    memset(&ram,0,sizeof(ram));ram.ready=true;ram.count=2;ram.features.physical_bits=40;ram.features.nx=true;
    ram.intervals[0]=(ram_interval_t){.first=0x200000,.end=0x400000};
    ram.intervals[1]=(ram_interval_t){.first=address(0),.end=address(BLOCKS)};
}
static void boot(void)
{
    CHECK(kernel_stack_prepare(&image,&ram)==&g_stack_layout);
    mapping_table_ops_t ops={allocate_table,release_table};CHECK(stack_window_construct(&window,&root,&ram.features,&ops,NULL));
    for(unsigned i=0;i<STACK_REGION_SLOTS;++i) {
        stack_owner_view_t v;CHECK(kernel_stack_read(i,&v));
        if(v.state!=STACK_FREE) {CHECK(stack_window_install(&window,&v));if(v.owned)blocks[block_index(v.physical)].mapped=true;}
    }
    g_boot_build.activated=true;
    CHECK(kernel_stack_activate());CHECK(kernel_stack_bind_bsp(0,9));
    uint64_t top;CHECK(kernel_stack_cpu(STACK_KIND_AP_BOOT,31,117,&top));CHECK(top>=STACK_REGION_BASE && local_flushes==1);
    CHECK(kernel_stack_close_bootstrap());
    CHECK(!kernel_stack_cpu(STACK_KIND_AP_BOOT,30,116,&top));
    CHECK(!kernel_stack_runtime_enable());
}
static void actual_lifetime(void)
{
    reset();boot();stack_handle_t h[STACK_PREPARED_TASKS],replacement;void *base;
    for(unsigned i=0;i<STACK_PREPARED_TASKS;++i) {
        CHECK(kernel_stack_acquire(&h[i],&base));CHECK((uintptr_t)base>=STACK_REGION_BASE);
        CHECK(kernel_stack_bind_task(h[i],20+i,40+i,"early-worker"));
    }
    CHECK(!waits && !kernel_stack_acquire(&replacement,&base));
    kernel_stack_discard(h[0]);CHECK(!waits);
    CHECK(kernel_stack_acquire(&replacement,&base) && replacement.generation>h[0].generation);
    CHECK(!kernel_stack_publish(h[0]));h[0]=replacement;
    CHECK(kernel_stack_bind_task(h[0],20,40,"early-worker"));
    for(unsigned i=0;i<STACK_PREPARED_TASKS;++i)CHECK(kernel_stack_publish(h[i]));
    host_if=true;CHECK(kernel_stack_runtime_enable());
    kernel_stack_retire(h[0],20,40);CHECK(waits==1 && freed_pages==4);
    CHECK(kernel_stack_acquire(&replacement,&base));CHECK(waits==2 && replacement.slot==h[0].slot);
    CHECK(replacement.generation>h[0].generation && !stack_owner_begin_retire(record(replacement),h[0],20,40,false));
    kernel_stack_discard(replacement);CHECK(waits==3 && freed_pages==8);
    install_fail=true;CHECK(!kernel_stack_acquire(&replacement,&base));CHECK(waits==3 && freed_pages==12);install_fail=false;
    context_allowed=false;CHECK(!kernel_stack_acquire(&replacement,&base));context_allowed=true;
    /* A different CPU may be publishing only task identity/state while this
     * allocator validates stable, separately locked backing ownership. The
     * fault-safe full snapshot must still refuse that interrupted writer. */
    stack_owner_record_t *other=record(h[2]);
    uint64_t sequence=__atomic_fetch_add(&other->sequence,1,__ATOMIC_ACQ_REL);
    stack_owner_view_t interrupted;
    CHECK(!(sequence&1) && !stack_owner_read(other,&interrupted));
    CHECK(kernel_stack_acquire(&replacement,&base));
    __atomic_store_n(&other->sequence,sequence+2,__ATOMIC_RELEASE);
    kernel_stack_discard(replacement);
    kernel_stack_stats_t stats;CHECK(kernel_stack_stats(&stats) && !stats.retiring && !stats.quarantined);
    unsigned before=freed_pages;wait_fail=true;expect_panic=true;
    if(!setjmp(panic_return)) {kernel_stack_retire(h[1],21,41);CHECK(false);}
    CHECK(strstr(panic_reason,"quarantined") && freed_pages==before && !held_locks && host_if);
    stack_owner_view_t v;CHECK(kernel_stack_read(h[1].slot,&v) && v.state==STACK_QUARANTINED);
    CHECK(!stack_owner_finish_retire(record(h[1]),h[1],request_generation,true));
    CHECK(!stack_owner_reserve(record(h[1]),h[1].slot,STACK_KIND_TASK,16384,address(31),true,0,&replacement));
}
static void backing_admission(void)
{
    reset();boot();stack_handle_t h;stack_owner_view_t v,out;
    CHECK(allocate_owner(STACK_KIND_TASK,STACK_TASK_BYTES,0,&h));
    CHECK(kernel_stack_reserved_backing(h,&v));
    out=v;out.physical=UINT64_MAX;
    CHECK(!kernel_stack_reserved_backing((stack_handle_t){h.generation+1,h.slot},&out));
    CHECK(out.physical==UINT64_MAX);
    stack_owner_record_t *other=&g_stack_owners[STACK_CPU_SLOTS];
    const unsigned fields[]={STACK_META_PHYSICAL,STACK_META_CAPACITY,STACK_META_STATE,STACK_META_PHYSICAL};
    const uint64_t bad[]={v.physical,UINT64_MAX,STACK_QUARANTINED+1,PHYSICAL_ADDRESS_LIMIT};
    for(unsigned i=0;i<4;++i) {
        uint64_t before=__atomic_exchange_n(&other->cells[fields[i]],bad[i],__ATOMIC_ACQ_REL);
        CHECK(!kernel_stack_reserved_backing(h,&out));CHECK(out.physical==UINT64_MAX);
        __atomic_store_n(&other->cells[fields[i]],before,__ATOMIC_RELEASE);
        CHECK(kernel_stack_reserved_backing(h,&out));out.physical=UINT64_MAX;
    }
    /* A retained FREE record is not still an allocation. Its old extent may
     * match a new PMM reservation after the final acknowledged retirement. */
    uint64_t before_state=other->cells[STACK_META_STATE],before_physical=other->cells[STACK_META_PHYSICAL];
    other->cells[STACK_META_STATE]=STACK_FREE;other->cells[STACK_META_PHYSICAL]=v.physical;
    CHECK(kernel_stack_reserved_backing(h,&out));
    other->cells[STACK_META_STATE]=before_state;other->cells[STACK_META_PHYSICAL]=before_physical;
    CHECK(paging_stack_install(h,false));
}
static void task_capacity(void)
{
    /* Modal workers may all exit before the unchanged reaper grace expires.
     * Add the real TASKMAN peak, two workers per supported CPU and explicit
     * system/controller headroom; handles alone do not retain task owners. */
    static stack_handle_t handles[BLOCKS];
    reset();boot();host_if=true;CHECK(kernel_stack_runtime_enable());
    void *base;
    for(unsigned i=0;i<REQUIRED_TASK_BACKLOG || i<STACK_TASK_SLOTS;++i) {
        CHECK(i<BLOCKS && kernel_stack_acquire(&handles[i],&base));
        CHECK(kernel_stack_bind_task(handles[i],100+i,200+i,"capacity-worker"));
        CHECK(kernel_stack_publish(handles[i]));
        CHECK(paging_stack_validate(handles[i]));
    }
    kernel_stack_stats_t stats;CHECK(kernel_stack_stats(&stats));
    CHECK(stats.live==STACK_TASK_SLOTS+5 && !stats.prepared && !stats.reserved);
    CHECK(stats.owned_bytes==(STACK_TASK_SLOTS+1)*STACK_TASK_BYTES);
    CHECK(stats.free_slots==STACK_CPU_SLOTS-4 && !stats.retiring && !stats.quarantined);
    stack_handle_t extra=STACK_HANDLE_INVALID;
    base=(void *)(uintptr_t)UINT64_C(0x12345678);
    unsigned before_waits=waits,before_freed=freed_pages;
    CHECK(!kernel_stack_acquire(&extra,&base));
    CHECK(extra.slot==UINT32_MAX && !extra.generation && (uintptr_t)base==UINT64_C(0x12345678));
    CHECK(waits==before_waits && freed_pages==before_freed+4 && !held_locks && host_if);
    for(unsigned i=0;i<STACK_TASK_SLOTS;++i) {
        CHECK(paging_stack_validate(handles[i]));
        kernel_stack_retire(handles[i],100+i,200+i);
        CHECK(!kernel_stack_publish(handles[i]));
    }
    CHECK(kernel_stack_stats(&stats));
    CHECK(stats.live==5 && !stats.prepared && !stats.reserved && !stats.retiring && !stats.quarantined);
    CHECK(stats.free_slots==STACK_REGION_SLOTS-4 && stats.owned_bytes==STACK_TASK_BYTES);
    CHECK(stats.borrowed_bytes==3*STACK_IST_BYTES+STACK_TASK_BYTES);
    CHECK(table_count==STACK_WINDOW_TABLES && waits==2*STACK_TASK_SLOTS-STACK_PREPARED_TASKS);
    CHECK(kernel_stack_acquire(&extra,&base) && extra.slot==handles[0].slot &&
          extra.generation>handles[0].generation);
    kernel_stack_discard(extra);
    CHECK(kernel_stack_stats(&stats) && stats.live==5 && !stats.retiring && !stats.quarantined);
}
int main(void)
{
    for(unsigned point=1;point<=STACK_PREPARED_TASKS;++point) {
        reset();alloc_fail=point;CHECK(!kernel_stack_prepare(&image,&ram));
        CHECK(freed_pages==(point-1)*4 && !waits && !held_locks && !host_if);
        for(unsigned i=0;i<STACK_OWNER_COUNT;++i) {stack_owner_view_t v;CHECK(kernel_stack_read(i,&v) && v.state==STACK_FREE);}
    }
    actual_lifetime();backing_admission();task_capacity();
    printf("STACK_RUNTIME_HOST checks=%u status=PASS actual_adapter=1 actual_slot_mapper=1 peak_task_slots=%u required_backlog=%u privileged_boundaries=HOSTED\n",checks,STACK_TASK_SLOTS,REQUIRED_TASK_BACKLOG);return 0;
}

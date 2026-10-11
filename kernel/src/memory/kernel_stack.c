#include "../core/irq_latency.h"
#include "stack_internal.h"
#include "kernel_tlb.h"
#include "physical_translation.h"
#include "../cpu/tss.h"
#include "../core/spinlock.h"
#include "../core/panic.h"
#include "../libc/memory.h"
#ifdef HOBBYOS_SELFTEST
#include "guard_probe.h"
#endif

enum { STACK_BOOT_COLD, STACK_BOOT_PLANNED, STACK_BOOT_ACTIVE, STACK_BOOT_SEALED, STACK_RUNTIME };
static stack_owner_record_t g_stack_owners[STACK_OWNER_COUNT];
static stack_layout_t g_stack_layout={.owners=g_stack_owners};
static spinlock_t g_stack_lock;
static uint32_t g_stack_phase, g_stack_high_water;
static uint64_t g_stack_allocated, g_stack_retired, g_stack_quarantined;
static uint64_t g_stack_count_sequence;
static stack_handle_t g_bsp_ist[3], g_bsp_boot;

/* Called with the stack writer lock, outside the fault lookup path. */
static void counts_begin(void)
{
    uint64_t n=__atomic_load_n(&g_stack_count_sequence,__ATOMIC_RELAXED);
    if ((n&1) || n>UINT64_MAX-2) kpanic("Stack accounting generation exhausted");
    __atomic_store_n(&g_stack_count_sequence,n+1,__ATOMIC_RELEASE);
}
static void counts_end(void)
{ __atomic_add_fetch(&g_stack_count_sequence,1,__ATOMIC_RELEASE); }

static stack_owner_record_t *record(stack_handle_t h)
{ return h.slot<STACK_OWNER_COUNT && h.generation ? &g_stack_owners[h.slot] : NULL; }

bool kernel_stack_reserved_backing(stack_handle_t h,stack_owner_view_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_STACK);
    if (!out || h.slot>=STACK_REGION_SLOTS) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_stack_lock);
    stack_owner_view_t v;
    bool ok=stack_owner_read(record(h),&v) && v.generation==h.generation && v.state==STACK_RESERVED;
    for (unsigned i=0;ok && i<STACK_REGION_SLOTS;++i) {
        if (i==h.slot) continue;
        const stack_owner_record_t *r=&g_stack_owners[i];
        /* Reservation/reclamation hold this lock. Identity publication can
         * proceed concurrently, but cannot change a claimed physical extent
         * or make it FREE. Do not require its unrelated full owner snapshot. */
        uint64_t state=__atomic_load_n(&r->cells[STACK_META_STATE],__ATOMIC_ACQUIRE);
        if (state>STACK_QUARANTINED) {ok=false;break;}
        if (state==STACK_FREE) continue;
        uint64_t physical=__atomic_load_n(&r->cells[STACK_META_PHYSICAL],__ATOMIC_RELAXED);
        uint64_t capacity=__atomic_load_n(&r->cells[STACK_META_CAPACITY],__ATOMIC_RELAXED);
        if ((physical&4095) || physical>=PHYSICAL_ADDRESS_LIMIT ||
            (capacity!=STACK_TASK_BYTES && capacity!=STACK_IST_BYTES) ||
            capacity>PHYSICAL_ADDRESS_LIMIT-physical ||
            (v.physical<physical+capacity && physical<v.physical+v.capacity)) ok=false;
    }
    spin_unlock_irqrestore(&g_stack_lock,flags);
    if (ok) *out=v;
    return ok;
}

static void free_backing(uint64_t physical,uint32_t capacity)
{
    for (uint32_t i=0;i<capacity;i+=4096) pmm_free_frame((void *)(uintptr_t)(physical+i));
}

static bool allocate_owner(stack_kind_t kind,uint32_t capacity,uint64_t borrowed,stack_handle_t *handle)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_STACK);
    bool owned=borrowed==0;
    uint64_t physical=borrowed;
    if (owned) {
        physical=(uintptr_t)pmm_alloc_contiguous_frames(capacity/4096);
        if (!physical) return false;
        void *pointer;
        if (!physical_pointer_range(physical,capacity,PHYSICAL_RAM,&pointer)) {
            free_backing(physical,capacity);return false;
        }
        memset(pointer,0,capacity);
    }
    unsigned first=kind==STACK_KIND_TASK ? STACK_CPU_SLOTS : 0;
    unsigned end=kind==STACK_KIND_TASK ? STACK_REGION_SLOTS : STACK_CPU_SLOTS;
    irq_flags_t flags=spin_lock_irqsave(&g_stack_lock);
    counts_begin();
    bool ok=false;
    for (unsigned i=first;i<end;++i) {
        stack_owner_view_t view;
        if (stack_owner_read(&g_stack_owners[i],&view) && view.state==STACK_FREE &&
            stack_owner_reserve(&g_stack_owners[i],i,kind,capacity,physical,owned,0,handle)) {ok=true;break;}
    }
    if (ok) {
        uint64_t active=__atomic_add_fetch(&g_stack_allocated,1,__ATOMIC_RELAXED)-
            __atomic_load_n(&g_stack_retired,__ATOMIC_RELAXED);
        if (active>__atomic_load_n(&g_stack_high_water,__ATOMIC_RELAXED))
            __atomic_store_n(&g_stack_high_water,(uint32_t)active,__ATOMIC_RELAXED);
    }
    counts_end();
    spin_unlock_irqrestore(&g_stack_lock,flags);
    if (!ok && owned) free_backing(physical,capacity);
    return ok;
}

const stack_layout_t *kernel_stack_prepare(const image_permissions_t *image,const ram_map_plan_t *ram)
{
    if (g_stack_phase!=STACK_BOOT_COLD || irq_is_enabled() || !image || !image->ready ||
        !image->input.bsp_guard_first || !ram || !ram->ready) return NULL;
    spinlock_init(&g_stack_lock);
    g_stack_layout.bsp_guard=image->input.bsp_guard_first+KERNEL_IMAGE_OFFSET;
    if (!tss_bsp_backing(g_stack_layout.borrowed_ist) ||
        !stack_owner_reserve(&g_stack_owners[STACK_BSP_OWNER],STACK_BSP_OWNER,STACK_KIND_BSP_BOOT,
            STACK_TASK_BYTES,image->input.bsp_stack_first,false,g_stack_layout.bsp_guard,&g_bsp_boot)) return NULL;
    for (unsigned i=0;i<3;++i)
        if (!allocate_owner((stack_kind_t)(STACK_KIND_IST1+i),STACK_IST_BYTES,g_stack_layout.borrowed_ist[i],&g_bsp_ist[i])) goto fail;
    for (unsigned i=0;i<STACK_PREPARED_TASKS;++i) {
        stack_handle_t handle;
        if (!allocate_owner(STACK_KIND_TASK,STACK_TASK_BYTES,0,&handle)) goto fail;
    }
    for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(&g_stack_owners[i],&v)) goto fail;
        if (v.state==STACK_FREE) continue;
        if (i!=STACK_BSP_OWNER && !stack_backing_allowed(&g_stack_layout,image,ram,&v)) goto fail;
    }
    for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(&g_stack_owners[i],&v)) return NULL;
        if (v.state==STACK_RESERVED && !stack_owner_mapped(&g_stack_owners[i],
            (stack_handle_t){v.generation,i},v.kind==STACK_KIND_TASK)) return NULL;
    }
    g_stack_phase=STACK_BOOT_PLANNED;return &g_stack_layout;
fail:
    /* No candidate stack table or descriptor has been published at this point. */
    for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(&g_stack_owners[i],&v) || v.state!=STACK_RESERVED) continue;
        if (v.owned) free_backing(v.physical,v.capacity);
        if (!stack_owner_cancel_reservation(&g_stack_owners[i],(stack_handle_t){v.generation,i}))
            kpanic("Stack construction rollback lost ownership");
    }
    return NULL;
}

const stack_layout_t *kernel_stack_layout(void)
{ return g_stack_phase>=STACK_BOOT_PLANNED ? &g_stack_layout : NULL; }

bool kernel_stack_activate(void)
{
    if (g_stack_phase!=STACK_BOOT_PLANNED || irq_is_enabled()) return false;
    uint64_t tops[3];
    for (unsigned i=0;i<3;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(record(g_bsp_ist[i]),&v) || !paging_stack_validate(g_bsp_ist[i])) return false;
        tops[i]=v.end;
    }
    if (!paging_stack_validate(g_bsp_boot) || !tss_install_guarded_ist(tops)) return false;
    __atomic_store_n(&g_stack_phase,STACK_BOOT_ACTIVE,__ATOMIC_RELEASE);return true;
}

bool kernel_stack_bind_bsp(uint32_t cpu,uint32_t apic)
{
    if (g_stack_phase!=STACK_BOOT_ACTIVE || irq_is_enabled()) return false;
    for (unsigned i=0;i<4;++i) {
        stack_handle_t h=i==3 ? g_bsp_boot : g_bsp_ist[i];
        if (!paging_stack_validate(h) || !stack_owner_bind_cpu(record(h),h,cpu,apic) ||
            !stack_owner_publish(record(h),h)) return false;
    }
    return true;
}

bool kernel_stack_cpu(stack_kind_t kind,uint32_t cpu,uint32_t apic,uint64_t *top)
{
    if (!top || g_stack_phase!=STACK_BOOT_ACTIVE || irq_is_enabled() || cpu>=HOBBYOS_MAX_CPUS ||
        (kind!=STACK_KIND_AP_BOOT && (kind<STACK_KIND_IST1 || kind>STACK_KIND_IST3))) return false;
    stack_handle_t handle;stack_owner_view_t v;
    if (!allocate_owner(kind,kind==STACK_KIND_AP_BOOT ? STACK_TASK_BYTES : STACK_IST_BYTES,0,&handle) ||
        !paging_stack_install(handle,false) || !stack_owner_read(record(handle),&v) ||
        !kernel_tlb_bootstrap_invalidate(v.base,v.capacity) || !paging_stack_validate(handle) ||
        !stack_owner_bind_cpu(record(handle),handle,cpu,apic) || !stack_owner_publish(record(handle),handle)) return false;
    *top=v.end;return true;
}

bool kernel_stack_close_bootstrap(void)
{
    if (g_stack_phase!=STACK_BOOT_ACTIVE || irq_is_enabled()) return false;
    __atomic_store_n(&g_stack_phase,STACK_BOOT_SEALED,__ATOMIC_RELEASE);return true;
}
bool kernel_stack_runtime_enable(void)
{
    if (g_stack_phase!=STACK_BOOT_SEALED || !irq_is_enabled()) return false;
    __atomic_store_n(&g_stack_phase,STACK_RUNTIME,__ATOMIC_RELEASE);return true;
}

bool kernel_stack_acquire(stack_handle_t *handle,void **base)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_STACK);
    if (!handle || !base || g_stack_phase<STACK_BOOT_ACTIVE) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_stack_lock);
    for (unsigned i=STACK_CPU_SLOTS;i<STACK_REGION_SLOTS;++i) {
        stack_handle_t h;stack_owner_view_t v;
        if (!stack_owner_take_prepared(&g_stack_owners[i],&h)) continue;
        bool ok=stack_owner_read(&g_stack_owners[i],&v);
        spin_unlock_irqrestore(&g_stack_lock,flags);
        if (!ok || !paging_stack_validate(h)) kpanic("Prepared stack lost its mapping");
        *handle=h;*base=(void *)(uintptr_t)v.base;return true;
    }
    spin_unlock_irqrestore(&g_stack_lock,flags);
    if (__atomic_load_n(&g_stack_phase,__ATOMIC_ACQUIRE)!=STACK_RUNTIME || !cpu_request_mapping_context_allowed()) return false;
    stack_handle_t h;stack_owner_view_t v;
    if (!allocate_owner(STACK_KIND_TASK,STACK_TASK_BYTES,0,&h)) return false;
    if (!paging_stack_install(h,false)) {
        if (!stack_owner_read(record(h),&v) || v.state!=STACK_RESERVED) kpanic("Stack installation failed after publication");
        flags=spin_lock_irqsave(&g_stack_lock);
        counts_begin();
        free_backing(v.physical,v.capacity);
        bool ok=stack_owner_cancel_reservation(record(h),h);
        __atomic_add_fetch(&g_stack_retired,1,__ATOMIC_RELAXED);counts_end();
        spin_unlock_irqrestore(&g_stack_lock,flags);
        if (!ok) kpanic("Unpublished stack rollback lost ownership");
        return false;
    }
    if (!stack_owner_read(record(h),&v)) kpanic("New stack metadata is inconsistent");
    cpu_request_result_t result=kernel_mapping_invalidate(v.base,v.capacity);
    if (result.status!=CPU_REQUEST_OK || !result.generation || result.requested!=result.acknowledged || result.missing || result.failed) {
        if (paging_stack_remove(h,0,0,true)) (void)stack_owner_finish_retire(record(h),h,result.generation,false);
        __atomic_add_fetch(&g_stack_quarantined,1,__ATOMIC_RELAXED);
        kpanic("New stack mapping has uncertain CPU completion");
    }
    if (!paging_stack_validate(h)) kpanic("New stack mapping failed readback");
#ifdef HOBBYOS_SELFTEST
    if (!stack_test_emit("[STACK_GUARD][ALLOC] slot=%u generation=%llu base=%016llx physical=%016llx bytes=%u request=%llu requested=%llx acknowledged=%llx status=COMPLETE\n",
        h.slot,(unsigned long long)h.generation,(unsigned long long)v.base,(unsigned long long)v.physical,
        v.capacity,(unsigned long long)result.generation,(unsigned long long)result.requested,
        (unsigned long long)result.acknowledged)) kpanic("Stack allocation proof record overflow");
#endif
    *handle=h;*base=(void *)(uintptr_t)v.base;return true;
}

bool kernel_stack_bind_task(stack_handle_t h,uint64_t id,uint64_t lifecycle,const char *name)
{ return stack_owner_bind_task(record(h),h,id,lifecycle,name); }
bool kernel_stack_publish(stack_handle_t h)
{ return stack_owner_publish(record(h),h); }

static void release(stack_handle_t h,uint64_t id,uint64_t lifecycle,bool unpublished)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_STACK) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    stack_owner_view_t v;
    if (!stack_owner_read(record(h),&v) || v.generation!=h.generation) kpanic("Stack release has a stale owner");
    if (unpublished && v.prepared_origin && !v.ever_published) {
        if (!stack_owner_return_prepared(record(h),h)) kpanic("Prepared stack return lost ownership");
        return;
    }
    if (__atomic_load_n(&g_stack_phase,__ATOMIC_ACQUIRE)!=STACK_RUNTIME || !cpu_request_mapping_context_allowed())
        kpanic("Stack retirement requires eligible interrupt-enabled context");
    if (!paging_stack_remove(h,id,lifecycle,unpublished)) kpanic("Stack retirement ownership or mapping conflict");
    /* All ordinary locks and IRQ pins are released before the actual wait. */
#ifdef HOBBYOS_SELFTEST
    stack_test_retirement_start(&v);
#endif
    cpu_request_result_t result=kernel_mapping_invalidate(v.base,v.capacity);
    bool complete=result.status==CPU_REQUEST_OK && result.generation &&
        result.requested==result.acknowledged && !result.missing && !result.failed;
    if (!complete) {
        (void)stack_owner_finish_retire(record(h),h,result.generation,false);
        __atomic_add_fetch(&g_stack_quarantined,1,__ATOMIC_RELAXED);
#ifdef HOBBYOS_SELFTEST
        stack_test_retirement_failed(&v,&result);
#endif
        kpanic("Stack retirement quarantined pending CPU completion");
    }
    irq_flags_t flags=spin_lock_irqsave(&g_stack_lock);
    counts_begin();
    free_backing(v.physical,v.capacity);
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    irq_latency_stack_retired(h.slot,h.generation);
#endif
    bool ok=stack_owner_finish_retire(record(h),h,result.generation,true);
    if (ok) __atomic_add_fetch(&g_stack_retired,1,__ATOMIC_RELAXED);
    counts_end();
    spin_unlock_irqrestore(&g_stack_lock,flags);
    if (!ok) kpanic("Completed stack retirement lost ownership");
#ifdef HOBBYOS_SELFTEST
    if (!stack_test_emit("[STACK_GUARD][RETIRE] slot=%u generation=%llu task=%llu lifecycle=%llu base=%016llx physical=%016llx bytes=%u request=%llu requested=%llx acknowledged=%llx status=COMPLETE\n",
        h.slot,(unsigned long long)h.generation,(unsigned long long)v.task_id,
        (unsigned long long)v.task_lifecycle,(unsigned long long)v.base,(unsigned long long)v.physical,
        v.capacity,(unsigned long long)result.generation,(unsigned long long)result.requested,
        (unsigned long long)result.acknowledged)) kpanic("Stack retirement proof record overflow");
#endif
}
void kernel_stack_discard(stack_handle_t h) { release(h,0,0,true); }
void kernel_stack_retire(stack_handle_t h,uint64_t id,uint64_t lifecycle) { release(h,id,lifecycle,false); }

bool kernel_stack_read(uint32_t slot,stack_owner_view_t *owner)
{ return slot<STACK_OWNER_COUNT && stack_owner_read(&g_stack_owners[slot],owner); }
bool kernel_stack_guard_owner(uint64_t address,stack_owner_view_t *owner)
{ return stack_owner_guard_lookup(g_stack_owners,address,owner); }
bool kernel_stack_cpu_owner(uint32_t cpu,stack_kind_t kind,stack_owner_view_t *out)
{
    if (!out || cpu>=HOBBYOS_MAX_CPUS) return false;
    for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(&g_stack_owners[i],&v)) return false;
        if (v.state==STACK_LIVE && v.kind==kind && v.cpu==cpu) {*out=v;return true;}
    }
    return false;
}
bool kernel_stack_physical(uint64_t address,uint64_t bytes,uint64_t *physical)
{
    if (address<STACK_REGION_BASE || address>=STACK_REGION_END) return false;
    unsigned slot=(unsigned)((address-STACK_REGION_BASE)/STACK_SLOT_BYTES);
    stack_owner_view_t v;
    return stack_owner_read(&g_stack_owners[slot],&v) && v.slot==slot && stack_owner_payload(&v,address,bytes,physical);
}
bool kernel_stack_bsp_guard_overlap(uint64_t address,uint64_t bytes)
{
    uint64_t guard=g_stack_layout.bsp_guard;
    return guard && bytes && address<=UINT64_MAX-bytes &&
        address<guard+STACK_GUARD_BYTES && guard<address+bytes;
}
bool kernel_stack_stats(kernel_stack_stats_t *out)
{
    static uint64_t sequences[STACK_OWNER_COUNT];
    static uint32_t observer;
    uint32_t free=0;
    if (!out || !__atomic_compare_exchange_n(&observer,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return false;
    uint64_t before=__atomic_load_n(&g_stack_count_sequence,__ATOMIC_ACQUIRE);
    bool ok=!(before&1);
    kernel_stack_stats_t result={0};
    for (unsigned i=0;ok && i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t v;
        sequences[i]=__atomic_load_n(&g_stack_owners[i].sequence,__ATOMIC_ACQUIRE);
        if ((sequences[i]&1) || !stack_owner_read(&g_stack_owners[i],&v)) {ok=false;break;}
        if (v.state==STACK_FREE) { if(i<STACK_REGION_SLOTS) ++result.free_slots;continue; }
        if (v.owned) result.owned_bytes+=v.capacity;else result.borrowed_bytes+=v.capacity;
        if (v.state==STACK_LIVE) ++result.live;
        else if(v.state==STACK_PREPARED) ++result.prepared;
        else if(v.state==STACK_RETIRING) ++result.retiring;
        else ++result.reserved;
    }
    result.allocated=__atomic_load_n(&g_stack_allocated,__ATOMIC_RELAXED);
    result.retired=__atomic_load_n(&g_stack_retired,__ATOMIC_RELAXED);
    result.quarantined=__atomic_load_n(&g_stack_quarantined,__ATOMIC_RELAXED);
    result.high_water=__atomic_load_n(&g_stack_high_water,__ATOMIC_RELAXED);
    /* All records must share an unchanged observation interval. No IRQ-off
     * scan, writer wait or retry is used; the thread caller can retry later. */
    for (unsigned i=0;ok && i<STACK_OWNER_COUNT;++i)
        ok=sequences[i]==__atomic_load_n(&g_stack_owners[i].sequence,__ATOMIC_ACQUIRE);
    ok=ok && before==__atomic_load_n(&g_stack_count_sequence,__ATOMIC_ACQUIRE);
    if (ok) *out=result;
    __atomic_store_n(&observer,0,__ATOMIC_RELEASE);return ok;
}

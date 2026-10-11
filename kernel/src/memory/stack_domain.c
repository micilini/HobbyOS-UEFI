#include "stack_domain.h"

_Static_assert(STACK_REGION_SLOTS == 1664, "Stack geometry must cover the admitted CPU and task bounds");
_Static_assert(STACK_REGION_END < KERNEL_IMAGE_OFFSET, "Stack region must precede the image");
_Static_assert(STACK_REGION_BASE >= RAM_DIRECT_END, "Stack region must not overlap RAM");

bool stack_slot_geometry(uint32_t slot, uint32_t capacity, uint64_t *guard,
                         uint64_t *base, uint64_t *end)
{
    if (!guard || !base || !end || slot >= STACK_REGION_SLOTS ||
        (capacity != STACK_TASK_BYTES && capacity != STACK_IST_BYTES) ||
        (slot >= STACK_CPU_SLOTS && capacity != STACK_TASK_BYTES)) return false;
    uint64_t first = STACK_REGION_BASE + (uint64_t)slot * STACK_SLOT_BYTES;
    *guard = first; *base = first + STACK_GUARD_BYTES; *end = *base + capacity;
    return true;
}

static uint64_t cell(const stack_owner_record_t *r, unsigned index)
{ return __atomic_load_n(&r->cells[index], __ATOMIC_RELAXED); }
static void set(stack_owner_record_t *r, unsigned index, uint64_t value)
{ __atomic_store_n(&r->cells[index], value, __ATOMIC_RELAXED); }

bool stack_owner_read(const stack_owner_record_t *r, stack_owner_view_t *out)
{
    if (!r || !out) return false;
    uint64_t first = __atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE);
    if (first & 1) return false;
    /* Refuse corrupted scalar representations before narrowing them. */
    for (unsigned i=STACK_META_SLOT;i<=STACK_META_APIC;++i)
        if (cell(r,i)>UINT32_MAX) return false;
    if (cell(r,STACK_META_KIND)>STACK_KIND_PREPARED || cell(r,STACK_META_STATE)>STACK_QUARANTINED ||
        cell(r,STACK_META_OWNED)>1 || cell(r,STACK_META_PREPARED)>1 || cell(r,STACK_META_PUBLISHED)>1) return false;
    for (unsigned i=0;i<STACK_NAME_BYTES;++i) if (cell(r,STACK_META_NAME+i)>UINT8_MAX) return false;
    stack_owner_view_t v = {
        .guard=cell(r,STACK_META_GUARD), .base=cell(r,STACK_META_BASE),
        .end=cell(r,STACK_META_END), .physical=cell(r,STACK_META_PHYSICAL),
        .generation=cell(r,STACK_META_GENERATION), .task_id=cell(r,STACK_META_TASK),
        .task_lifecycle=cell(r,STACK_META_LIFECYCLE), .retirement_generation=cell(r,STACK_META_RETIREMENT),
        .slot=(uint32_t)cell(r,STACK_META_SLOT), .capacity=(uint32_t)cell(r,STACK_META_CAPACITY),
        .cpu=(uint32_t)cell(r,STACK_META_CPU), .apic=(uint32_t)cell(r,STACK_META_APIC),
        .kind=(stack_kind_t)cell(r,STACK_META_KIND), .state=(stack_state_t)cell(r,STACK_META_STATE),
        .owned=cell(r,STACK_META_OWNED)!=0, .prepared_origin=cell(r,STACK_META_PREPARED)!=0,
        .ever_published=cell(r,STACK_META_PUBLISHED)!=0
    };
    for (unsigned i=0;i<STACK_NAME_BYTES;++i) v.name[i]=(char)cell(r,STACK_META_NAME+i);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&r->sequence,__ATOMIC_RELAXED)!=first) return false;
    if (v.state>STACK_QUARANTINED) return false;
    if (v.state!=STACK_FREE) {
        uint64_t guard,base,end;
        if (!v.generation || v.physical>=PHYSICAL_ADDRESS_LIMIT ||
            (v.physical&4095) || v.capacity>PHYSICAL_ADDRESS_LIMIT-v.physical ||
            v.kind==STACK_KIND_NONE || v.kind>STACK_KIND_PREPARED || v.name[STACK_NAME_BYTES-1]) return false;
        if (v.slot==STACK_BSP_OWNER) {
            if (v.kind!=STACK_KIND_BSP_BOOT || v.owned || v.capacity!=STACK_TASK_BYTES ||
                v.guard<KERNEL_IMAGE_OFFSET || (v.guard&4095) ||
                v.guard>UINT64_MAX-STACK_GUARD_BYTES-v.capacity ||
                v.base!=v.guard+STACK_GUARD_BYTES || v.end!=v.base+v.capacity ||
                v.physical!=v.base-KERNEL_IMAGE_OFFSET) return false;
        } else if (!stack_slot_geometry(v.slot,v.capacity,&guard,&base,&end) ||
                   v.guard!=guard || v.base!=base || v.end!=end ||
                   (v.slot>=STACK_CPU_SLOTS && (!v.owned ||
                    (v.kind!=STACK_KIND_TASK && v.kind!=STACK_KIND_PREPARED))) ||
                   (v.slot<STACK_CPU_SLOTS && (v.kind<STACK_KIND_AP_BOOT || v.kind>STACK_KIND_IST3))) return false;
    }
    *out=v; return true;
}

static bool begin(stack_owner_record_t *r, uint64_t *sequence)
{
    if (!r || !sequence) return false;
    uint64_t n=__atomic_load_n(&r->sequence,__ATOMIC_ACQUIRE);
    if ((n&1) || n>UINT64_MAX-2 ||
        !__atomic_compare_exchange_n(&r->sequence,&n,n+1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) return false;
    *sequence=n;return true;
}
static void finish(stack_owner_record_t *r,uint64_t sequence)
{ __atomic_store_n(&r->sequence,sequence+2,__ATOMIC_RELEASE); }
static bool matches(const stack_owner_record_t *r,stack_handle_t h)
{ return h.generation && h.slot<STACK_OWNER_COUNT && cell(r,STACK_META_SLOT)==h.slot && cell(r,STACK_META_GENERATION)==h.generation; }

bool stack_owner_reserve(stack_owner_record_t *r,uint32_t slot,stack_kind_t kind,
                         uint32_t capacity,uint64_t physical,bool owned,
                         uint64_t bsp_guard,stack_handle_t *out)
{
    if (!out || (physical&4095) || physical>=PHYSICAL_ADDRESS_LIMIT ||
        capacity>PHYSICAL_ADDRESS_LIMIT-physical) return false;
    uint64_t guard,base,end;
    if (slot==STACK_BSP_OWNER) {
        if (kind!=STACK_KIND_BSP_BOOT || owned || capacity!=STACK_TASK_BYTES ||
            (bsp_guard&4095) || bsp_guard<KERNEL_IMAGE_OFFSET ||
            bsp_guard>UINT64_MAX-STACK_GUARD_BYTES-capacity) return false;
        guard=bsp_guard;base=guard+STACK_GUARD_BYTES;end=base+capacity;
        if (physical!=base-KERNEL_IMAGE_OFFSET) return false;
    } else {
        if (bsp_guard || !stack_slot_geometry(slot,capacity,&guard,&base,&end) ||
            (slot>=STACK_CPU_SLOTS && (kind!=STACK_KIND_TASK || !owned)) ||
            (slot<STACK_CPU_SLOTS && kind!=STACK_KIND_AP_BOOT &&
             kind!=STACK_KIND_IST1 && kind!=STACK_KIND_IST2 && kind!=STACK_KIND_IST3) ||
            (kind==STACK_KIND_AP_BOOT && !owned) ||
            ((kind==STACK_KIND_AP_BOOT || kind==STACK_KIND_TASK) && capacity!=STACK_TASK_BYTES) ||
            ((kind>=STACK_KIND_IST1 && kind<=STACK_KIND_IST3) && capacity!=STACK_IST_BYTES)) return false;
    }
    uint64_t sequence;
    if (!begin(r,&sequence)) return false;
    uint64_t generation=cell(r,STACK_META_GENERATION);
    bool ok=cell(r,STACK_META_STATE)==STACK_FREE && generation!=UINT64_MAX;
    if (ok) {
        for (unsigned i=0;i<STACK_META_CELLS;++i) set(r,i,0);
        set(r,STACK_META_GENERATION,++generation);set(r,STACK_META_SLOT,slot);
        set(r,STACK_META_GUARD,guard);set(r,STACK_META_BASE,base);set(r,STACK_META_END,end);
        set(r,STACK_META_PHYSICAL,physical);set(r,STACK_META_CAPACITY,capacity);
        set(r,STACK_META_KIND,kind);set(r,STACK_META_OWNED,owned);
        set(r,STACK_META_CPU,UINT32_MAX);set(r,STACK_META_APIC,UINT32_MAX);
        set(r,STACK_META_STATE,STACK_RESERVED);
        *out=(stack_handle_t){generation,slot};
    }
    finish(r,sequence);return ok;
}

bool stack_owner_mapped(stack_owner_record_t *r,stack_handle_t h,bool prepared)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_RESERVED &&
        (!prepared || (cell(r,STACK_META_KIND)==STACK_KIND_TASK && cell(r,STACK_META_OWNED)));
    if (ok) {
        set(r,STACK_META_PREPARED,prepared);
        set(r,STACK_META_STATE,prepared ? STACK_PREPARED : STACK_ALLOCATED);
        if (prepared) set(r,STACK_META_KIND,STACK_KIND_PREPARED);
    }
    finish(r,sequence);return ok;
}

bool stack_owner_take_prepared(stack_owner_record_t *r,stack_handle_t *out)
{
    if (!r || !out || cell(r,STACK_META_STATE)!=STACK_PREPARED) return false;
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    uint64_t generation=cell(r,STACK_META_GENERATION);
    bool ok=cell(r,STACK_META_STATE)==STACK_PREPARED && !cell(r,STACK_META_PUBLISHED) &&
        generation && generation!=UINT64_MAX && cell(r,STACK_META_KIND)==STACK_KIND_PREPARED;
    if (ok) {
        set(r,STACK_META_GENERATION,++generation);set(r,STACK_META_KIND,STACK_KIND_TASK);
        set(r,STACK_META_STATE,STACK_ALLOCATED);
        *out=(stack_handle_t){generation,(uint32_t)cell(r,STACK_META_SLOT)};
    }
    finish(r,sequence);return ok;
}

bool stack_owner_bind_task(stack_owner_record_t *r,stack_handle_t h,uint64_t id,
                          uint64_t lifecycle,const char *name)
{
    if (!id || !lifecycle || !name) return false;
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_ALLOCATED && cell(r,STACK_META_KIND)==STACK_KIND_TASK;
    if (ok) {
        set(r,STACK_META_TASK,id);set(r,STACK_META_LIFECYCLE,lifecycle);
        bool ended=false;
        for (unsigned i=0;i<STACK_NAME_BYTES;++i) {
            unsigned char c=(ended || i==STACK_NAME_BYTES-1) ? 0 : (unsigned char)name[i];
            ended|=c==0;set(r,STACK_META_NAME+i,c);
        }
        set(r,STACK_META_STATE,STACK_BOUND);
    }
    finish(r,sequence);return ok;
}

bool stack_owner_bind_cpu(stack_owner_record_t *r,stack_handle_t h,uint32_t cpu,uint32_t apic)
{
    if (cpu>=HOBBYOS_MAX_CPUS || apic==UINT32_MAX) return false;
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    uint64_t kind=cell(r,STACK_META_KIND);
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_ALLOCATED &&
        kind>=STACK_KIND_BSP_BOOT && kind<=STACK_KIND_IST3;
    if (ok) {set(r,STACK_META_CPU,cpu);set(r,STACK_META_APIC,apic);set(r,STACK_META_STATE,STACK_BOUND);}
    finish(r,sequence);return ok;
}

bool stack_owner_publish(stack_owner_record_t *r,stack_handle_t h)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_BOUND;
    if (ok) {set(r,STACK_META_PUBLISHED,1);set(r,STACK_META_STATE,STACK_LIVE);}
    finish(r,sequence);return ok;
}

bool stack_owner_return_prepared(stack_owner_record_t *r,stack_handle_t h)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    uint64_t state=cell(r,STACK_META_STATE);
    bool ok=matches(r,h) && cell(r,STACK_META_PREPARED) && !cell(r,STACK_META_PUBLISHED) &&
        (state==STACK_ALLOCATED || state==STACK_BOUND) && cell(r,STACK_META_KIND)==STACK_KIND_TASK;
    if (ok) {
        set(r,STACK_META_TASK,0);set(r,STACK_META_LIFECYCLE,0);
        for (unsigned i=0;i<STACK_NAME_BYTES;++i) set(r,STACK_META_NAME+i,0);
        set(r,STACK_META_KIND,STACK_KIND_PREPARED);set(r,STACK_META_STATE,STACK_PREPARED);
    }
    finish(r,sequence);return ok;
}

bool stack_owner_begin_retire(stack_owner_record_t *r,stack_handle_t h,uint64_t id,
                             uint64_t lifecycle,bool unpublished)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    uint64_t state=cell(r,STACK_META_STATE);
    bool ok=matches(r,h) && cell(r,STACK_META_OWNED) && cell(r,STACK_META_KIND)==STACK_KIND_TASK;
    if (unpublished) ok=ok && !cell(r,STACK_META_PUBLISHED) && !cell(r,STACK_META_PREPARED) &&
        (state==STACK_ALLOCATED || state==STACK_BOUND);
    else ok=ok && state==STACK_LIVE && id && lifecycle &&
        id==cell(r,STACK_META_TASK) && lifecycle==cell(r,STACK_META_LIFECYCLE);
    if (ok) set(r,STACK_META_STATE,STACK_RETIRING);
    finish(r,sequence);return ok;
}

bool stack_owner_finish_retire(stack_owner_record_t *r,stack_handle_t h,
                              uint64_t request_generation,bool acknowledged)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_RETIRING && cell(r,STACK_META_OWNED);
    if (ok) {
        set(r,STACK_META_RETIREMENT,request_generation);
        set(r,STACK_META_STATE,acknowledged && request_generation ? STACK_FREE : STACK_QUARANTINED);
    }
    finish(r,sequence);return ok && acknowledged && request_generation;
}

bool stack_owner_cancel_reservation(stack_owner_record_t *r,stack_handle_t h)
{
    uint64_t sequence;if (!begin(r,&sequence)) return false;
    bool ok=matches(r,h) && cell(r,STACK_META_STATE)==STACK_RESERVED && !cell(r,STACK_META_PUBLISHED);
    if (ok) set(r,STACK_META_STATE,STACK_FREE);
    finish(r,sequence);return ok;
}

bool stack_owner_guard_lookup(const stack_owner_record_t *records,uint64_t address,stack_owner_view_t *out)
{
    if (!records || !out) return false;
    unsigned slot=STACK_BSP_OWNER;
    if (address>=STACK_REGION_BASE && address<STACK_REGION_END) slot=(unsigned)((address-STACK_REGION_BASE)/STACK_SLOT_BYTES);
    stack_owner_view_t v;
    if (!stack_owner_read(&records[slot],&v) || v.slot!=slot || !v.generation ||
        v.state==STACK_FREE || v.state==STACK_RESERVED ||
        address<v.guard || address>=v.base || v.base-v.guard!=STACK_GUARD_BYTES) return false;
    *out=v;return true;
}

bool stack_owner_payload(const stack_owner_view_t *v,uint64_t address,uint64_t bytes,uint64_t *physical)
{
    if (!v || !physical || !bytes || !v->generation ||
        (v->state!=STACK_ALLOCATED && v->state!=STACK_BOUND && v->state!=STACK_LIVE && v->state!=STACK_PREPARED) ||
        address<v->base || address>=v->end || bytes>v->end-address ||
        v->physical>=PHYSICAL_ADDRESS_LIMIT || v->capacity>PHYSICAL_ADDRESS_LIMIT-v->physical) return false;
    *physical=v->physical+address-v->base;return true;
}

#include "stack_map.h"
#include "../libc/memory.h"

static bool mapped(stack_state_t state)
{ return state==STACK_PREPARED || state==STACK_ALLOCATED || state==STACK_BOUND || state==STACK_LIVE; }

bool stack_backing_allowed(const stack_layout_t *layout,const image_permissions_t *image,
                           const ram_map_plan_t *ram,const stack_owner_view_t *v)
{
    if (!layout || !layout->owners || !image || !image->ready || !ram || !ram->ready || !v ||
        v->slot>=STACK_REGION_SLOTS || !v->generation || (v->physical&4095) ||
        !ram_map_contains(ram,v->physical,v->capacity)) return false;
    uint64_t guard,base,end;
    if (!stack_slot_geometry(v->slot,v->capacity,&guard,&base,&end) ||
        v->guard!=guard || v->base!=base || v->end!=end) return false;
    if (v->owned) return !image_permissions_overlaps(image,v->physical,v->capacity);
    if (v->kind<STACK_KIND_IST1 || v->kind>STACK_KIND_IST3 || v->capacity!=STACK_IST_BYTES ||
        v->slot>=STACK_CPU_SLOTS || v->physical!=layout->borrowed_ist[v->kind-STACK_KIND_IST1]) return false;
    const image_section_t *bss=&image->input.sections[IMAGE_BSS];
    return v->physical>=bss->first && v->physical<bss->end &&
        v->capacity<=bss->end-v->physical &&
        !(v->physical<layout->bsp_guard-KERNEL_IMAGE_OFFSET+STACK_GUARD_BYTES &&
          layout->bsp_guard-KERNEL_IMAGE_OFFSET<v->physical+v->capacity);
}

bool stack_layout_expected(const stack_layout_t *layout,const image_permissions_t *image,
                           const ram_map_plan_t *ram,uint64_t *bytes)
{
    if (!bytes) return false;
    if (!layout) { *bytes=0; return true; }
    if (!layout->owners || !image || !image->ready || !ram || !ram->ready) return false;
    uint64_t total=0;
    for (unsigned i=0;i<STACK_REGION_SLOTS;++i) {
        stack_owner_view_t v;
        if (!stack_owner_read(&layout->owners[i],&v)) return false;
        if (v.state==STACK_FREE) continue;
        if (v.slot!=i || !stack_backing_allowed(layout,image,ram,&v)) return false;
        if (mapped(v.state)) total+=v.capacity;
        /* Owned extents cannot be shared with another live or uncertain owner. */
        for (unsigned j=0;j<i;++j) {
            stack_owner_view_t previous;
            if (!stack_owner_read(&layout->owners[j],&previous)) return false;
            if (previous.state!=STACK_FREE && v.physical<previous.physical+previous.capacity &&
                previous.physical<v.physical+v.capacity) return false;
        }
    }
    *bytes=total;return true;
}

bool stack_layout_leaf(const stack_layout_t *layout,const image_permissions_t *image,
                       const ram_map_plan_t *ram,uint64_t address,uint64_t physical,
                       uint64_t bytes,bool writable,bool nx)
{
    if (!layout || !layout->owners || address<STACK_REGION_BASE || address>=STACK_REGION_END ||
        bytes!=4096 || (address&4095) || !writable || nx!=ram->features.nx) return false;
    unsigned slot=(unsigned)((address-STACK_REGION_BASE)/STACK_SLOT_BYTES);
    stack_owner_view_t v;uint64_t expected;
    return stack_owner_read(&layout->owners[slot],&v) && v.slot==slot &&
        stack_backing_allowed(layout,image,ram,&v) &&
        stack_owner_payload(&v,address,bytes,&expected) && expected==physical;
}

static void unwind(stack_window_t *w,const mapping_table_ops_t *ops,void *context)
{
    while (w->allocated) {
        unsigned at=--w->allocated;
        ops->release(context,w->tables[at]);w->tables[at]=(mapping_table_t){0};
    }
}

bool stack_window_construct(stack_window_t *w,PageTable *root,const ram_features_t *f,
                            const mapping_table_ops_t *ops,void *context)
{
    const unsigned index=(STACK_REGION_BASE>>39)&511;
    if (!w || !root || !f || !ops || !ops->allocate || !ops->release ||
        w->ready || w->allocated || root->entries[index]) return false;
    w->features=*f;
    for (unsigned i=0;i<STACK_WINDOW_TABLES;++i) {
        mapping_table_t table={0};
        if (!ops->allocate(context,&table)) { unwind(w,ops,context); return false; }
        bool duplicate=false;
        for (unsigned j=0;j<w->allocated;++j)
            duplicate|=table.cpu==w->tables[j].cpu || table.physical==w->tables[j].physical;
        uint64_t encoded;
        if (duplicate || !table.cpu || !table.physical ||
            !paging_entry_encode(f,2,false,table.physical,0,true,f->nx,&encoded)) {
            if (table.cpu && !duplicate) ops->release(context,table);
            unwind(w,ops,context);return false;
        }
        memset(table.cpu,0,sizeof(PageTable));w->tables[w->allocated++]=table;
    }
    if (!paging_entry_encode(f,3,false,w->tables[1].physical,0,true,f->nx,
                             &w->tables[0].cpu->entries[0])) { unwind(w,ops,context);return false; }
    for (unsigned i=0;i<STACK_WINDOW_PTS;++i)
        if (!paging_entry_encode(f,2,false,w->tables[i+2].physical,0,true,f->nx,
                                 &w->tables[1].cpu->entries[i])) { unwind(w,ops,context);return false; }
    uint64_t entry;
    if (!paging_entry_encode(f,4,false,w->tables[0].physical,0,true,f->nx,&entry)) {
        unwind(w,ops,context);return false;
    }
    __atomic_store_n(&root->entries[index],entry,__ATOMIC_RELEASE);
    w->ready=true;return true;
}

static uint64_t *leaf(const stack_window_t *w,uint64_t address)
{
    uint64_t page=(address-STACK_REGION_BASE)/4096;
    return &w->tables[2+page/512].cpu->entries[page%512];
}

static bool transaction(stack_window_t *w,const stack_owner_view_t *v,bool remove)
{
    uint64_t guard,base,end;
    if (!w || !w->ready || !v || !stack_slot_geometry(v->slot,v->capacity,&guard,&base,&end) ||
        v->guard!=guard || v->base!=base || v->end!=end ||
        (remove ? v->state!=STACK_RETIRING : v->state!=STACK_RESERVED && !mapped(v->state))) return false;
    bool empty=true,full=true;
    for (uint64_t address=guard;address<guard+STACK_SLOT_BYTES;address+=4096) {
        uint64_t old=__atomic_load_n(leaf(w,address),__ATOMIC_ACQUIRE),expected=0;
        if (address>=base && address<end &&
            !paging_entry_encode(&w->features,1,true,v->physical+address-base,0,true,w->features.nx,&expected)) return false;
        if (!expected && old) return false;
        if (expected) {
            empty&=old==0;
            full&=(old&~(uint64_t)(PAGE_ACCESSED|PAGE_DIRTY))==expected;
        }
    }
    /* A partially installed slot is a conflict, not a completion opportunity. */
    if (remove ? !full : !empty && !full) return false;
    if (!remove && full) return true;
    for (uint64_t address=base;address<end;address+=4096) {
        uint64_t entry=0;
        if (!remove) (void)paging_entry_encode(&w->features,1,true,v->physical+address-base,0,true,w->features.nx,&entry);
        __atomic_store_n(leaf(w,address),entry,__ATOMIC_RELEASE);
    }
    return true;
}

bool stack_window_install(stack_window_t *w,const stack_owner_view_t *v)
{ return transaction(w,v,false); }
bool stack_window_remove(stack_window_t *w,const stack_owner_view_t *v)
{ return transaction(w,v,true); }

static bool link(const stack_window_t *w,unsigned level,uint64_t actual,uint64_t physical)
{
    uint64_t expected;
    return paging_entry_encode(&w->features,level,false,physical,0,true,w->features.nx,&expected) &&
        (actual&~(uint64_t)PAGE_ACCESSED)==expected;
}

bool stack_window_check(const stack_window_t *w,const PageTable *root,const stack_owner_view_t *v)
{
    uint64_t guard,base,end;
    if (!w || !w->ready || !root || !v || !mapped(v->state) ||
        !stack_slot_geometry(v->slot,v->capacity,&guard,&base,&end) ||
        v->guard!=guard || v->base!=base || v->end!=end ||
        !link(w,4,root->entries[(base>>39)&511],w->tables[0].physical) ||
        !link(w,3,w->tables[0].cpu->entries[(base>>30)&511],w->tables[1].physical) ||
        !link(w,2,w->tables[1].cpu->entries[(base>>21)&511],w->tables[2+(base-STACK_REGION_BASE)/UINT64_C(0x200000)].physical)) return false;
    for (uint64_t va=guard;va<guard+STACK_SLOT_BYTES;va+=4096) {
        uint64_t raw=__atomic_load_n(leaf(w,va),__ATOMIC_ACQUIRE),expected=0;
        if (va>=base && va<end && !paging_entry_encode(&w->features,1,true,v->physical+va-base,0,true,w->features.nx,&expected)) return false;
        if (expected ? (raw&~(uint64_t)(PAGE_ACCESSED|PAGE_DIRTY))!=expected : raw!=0) return false;
    }
    return true;
}

bool stack_window_verify(const stack_window_t *w,const PageTable *root,const stack_layout_t *layout,
                         const image_permissions_t *image,const ram_map_plan_t *ram)
{
    uint64_t expected,total=0;
    if (!w || !w->ready || w->allocated!=STACK_WINDOW_TABLES || !root ||
        w->features.nx!=ram->features.nx || !stack_layout_expected(layout,image,ram,&expected) ||
        !link(w,4,root->entries[(STACK_REGION_BASE>>39)&511],w->tables[0].physical)) return false;
    for (unsigned i=0;i<512;++i) {
        uint64_t a=w->tables[0].cpu->entries[i],b=w->tables[1].cpu->entries[i];
        if (i ? a!=0 : !link(w,3,a,w->tables[1].physical)) return false;
        if (i<STACK_WINDOW_PTS ? !link(w,2,b,w->tables[i+2].physical) : b!=0) return false;
    }
    for (uint64_t va=STACK_REGION_BASE;va<STACK_REGION_END;va+=4096) {
        uint64_t raw=*leaf(w,va);
        if (!raw) continue;
        paging_entry_view_t entry;
        if (!paging_entry_decode(&ram->features,1,raw,&entry) || entry.user || entry.pat_slot ||
            !stack_layout_leaf(layout,image,ram,va,entry.physical,4096,entry.writable,entry.nx)) return false;
        total+=4096;
    }
    return total==expected;
}

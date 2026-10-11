#include "image_walk.h"
#include "../libc/memory.h"

static bool inside(uint64_t address,uint64_t bytes,uint64_t first,uint64_t end)
{ return bytes && address>=first && address<end && bytes<=end-address; }
static bool path(uint64_t address,uint64_t span,uint64_t first,uint64_t end)
{ return first<end && address<=end-1 && (address>=first || first-address<span); }

static bool emit(const image_walk_inputs_t *in,image_walk_workspace_t *w,const image_leaf_run_t *leaf)
{
    image_leaf_run_t *p=&w->pending;
    if (p->bytes && p->address<=UINT64_MAX-p->bytes && p->address+p->bytes==leaf->address &&
        p->physical+p->bytes==leaf->physical && p->flags==leaf->flags && p->shift==leaf->shift &&
        p->writable==leaf->writable && p->nx==leaf->nx) { p->bytes+=leaf->bytes; return true; }
    if (p->bytes && in->consume && !in->consume(in->context,p)) return false;
    *p=*leaf; return true;
}

static bool walk(const image_walk_inputs_t *in,image_walk_workspace_t *w,image_walk_result_t *out,
                 uint64_t frame,unsigned level,uint64_t base,bool write,bool nx)
{
    if (!frame || (frame&4095) || ++w->visits>IMAGE_WALK_TABLES*2u) return false;
    unsigned at=0;
    while (at<w->count && w->frames[at]!=frame) ++at;
    if (at==w->count) {
        if (w->count==IMAGE_WALK_TABLES) return false;
        w->frames[at]=frame;w->levels[at]=level;++w->count;
    } else if (w->levels[at]!=level || w->active[at]) return false;
    const PageTable *table=in->table(in->context,frame,level);
    if (!table) return false;
    w->active[at]=1;
    for (unsigned i=0;i<512;++i) {
        uint64_t raw=__atomic_load_n(&table->entries[i],__ATOMIC_RELAXED);
        if (level==4 && i<256 && raw) return false;
        if (!raw) continue;
        uint64_t va=base|((uint64_t)i<<(12+9*(level-1)));
        if (level==4 && i>=256) va|=UINT64_C(0xffff000000000000);
        paging_entry_view_t entry;
        if (!paging_entry_decode(&in->ram->features,level,raw,&entry) || entry.user) return false;
        bool effective_write=write && entry.writable,effective_nx=nx || entry.nx;
        if (!entry.leaf) {
            uint64_t span=UINT64_C(1)<<(12+9*(level-1));
            bool allowed=path(va,span,RAM_DIRECT_BASE,RAM_DIRECT_END) ||
                path(va,span,in->permissions->linked_first,in->permissions->linked_end) ||
                path(va,span,DEVICE_WINDOW_BASE,DEVICE_WINDOW_BASE+DEVICE_WINDOW_BYTES) ||
                path(va,span,STACK_REGION_BASE,STACK_REGION_END);
            for (unsigned k=0;k<2;++k) allowed|=path(va,span,in->scratch[k].first,in->scratch[k].end);
            if (!allowed || entry.pat_slot || !walk(in,w,out,entry.physical,level-1,va,effective_write,effective_nx)) return false;
            continue;
        }
        bool direct=inside(va,entry.bytes,RAM_DIRECT_BASE,RAM_DIRECT_END);
        bool image=inside(va,entry.bytes,in->permissions->linked_first,in->permissions->linked_end);
        if (direct || image) {
            if (entry.pat_slot || !image_permissions_check_leaf(in->permissions,va,entry.physical,
                entry.bytes,effective_write,effective_nx,in->ram->features.nx)) return false;
            if (direct) {
                const ram_interval_t *r=ram_map_interval(in->ram,entry.physical);
                if (!r || entry.bytes>r->end-entry.physical ||
                    (12+9*(level-1))!=image_permissions_leaf_shift(in->permissions,in->ram,r,entry.physical)) return false;
                out->direct_bytes+=entry.bytes;
            } else out->image_bytes+=entry.bytes;
        } else if (inside(va,entry.bytes,STACK_REGION_BASE,STACK_REGION_END)) {
            if (entry.pat_slot || !stack_layout_leaf(in->stacks,in->permissions,in->ram,va,entry.physical,
                entry.bytes,effective_write,effective_nx)) return false;
            out->stack_bytes+=entry.bytes;
        } else {
            bool managed=inside(va,entry.bytes,DEVICE_WINDOW_BASE,DEVICE_WINDOW_BASE+DEVICE_WINDOW_BYTES);
            bool scratch=false;
            for (unsigned k=0;k<2;++k) scratch|=inside(va,entry.bytes,in->scratch[k].first,in->scratch[k].end);
            if ((!managed && !scratch) || level!=1 ||
                image_permissions_overlaps(in->permissions,entry.physical,entry.bytes) ||
                effective_nx!=in->ram->features.nx || (scratch && (!effective_write || entry.pat_slot)) ||
                (managed && !effective_write && entry.pat_slot)) return false;
            if (managed) out->managed_bytes+=entry.bytes; else out->scratch_bytes+=entry.bytes;
        }
        bool executable=!effective_nx;
        ++out->leaves;out->bytes+=entry.bytes;
        if (effective_write) out->writable_bytes+=entry.bytes;
        if (executable) out->executable_bytes+=entry.bytes;
        if (effective_write && executable) out->rwx_bytes+=entry.bytes;
        image_leaf_run_t leaf={va,entry.physical,entry.bytes,raw & ~(PAGE_ADDR_MASK|PAGE_ACCESSED|PAGE_DIRTY),
            12+9*(level-1),effective_write,effective_nx};
        /* Huge-leaf PAT lives inside the address mask and is retained explicitly. */
        if (level>1) leaf.flags|=raw & UINT64_C(0x1000);
        if (!emit(in,w,&leaf)) return false;
    }
    w->active[at]=0;return true;
}

bool image_walk_main(const image_walk_inputs_t *in,uint64_t root,image_walk_workspace_t *w,image_walk_result_t *out)
{
    if (!in || !in->ram || !in->ram->ready || !in->permissions || !in->permissions->ready ||
        !in->table || !w || !out) return false;
    memset(w,0,sizeof(*w));
    image_walk_result_t result={.root=root,.nx=in->ram->features.nx};
    if (in->stacks) {
        if (!in->stacks->owners) return false;
        for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
            uint64_t sequence=__atomic_load_n(&in->stacks->owners[i].sequence,__ATOMIC_ACQUIRE);
            if (sequence&1) return false;
            w->stack_sequences[i]=sequence;
        }
    }
    uint64_t stack_bytes;
    if (!stack_layout_expected(in->stacks,in->permissions,in->ram,&stack_bytes)) return false;
    if (in->stacks && in->owner_consume) for (unsigned i=0;i<STACK_OWNER_COUNT;++i) {
        stack_owner_view_t owner;
        if (!stack_owner_read(&in->stacks->owners[i],&owner) ||
            (owner.state!=STACK_FREE && !in->owner_consume(in->context,&owner))) return false;
    }
    if (!walk(in,w,&result,root,4,0,true,false) || result.direct_bytes!=in->ram->bytes ||
        result.image_bytes!=image_permissions_linked_bytes(in->permissions) || result.stack_bytes!=stack_bytes ||
        (result.nx && result.rwx_bytes) ||
        (w->pending.bytes && in->consume && !in->consume(in->context,&w->pending))) return false;
    /* No owner may change between coverage planning and the last table read.
     * A bounded rejection also handles a writer interrupted by this observer. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (in->stacks) for (unsigned i=0;i<STACK_OWNER_COUNT;++i)
        if (w->stack_sequences[i]!=__atomic_load_n(&in->stacks->owners[i].sequence,__ATOMIC_RELAXED)) return false;
    result.tables=w->count;result.complete=true;*out=result;return true;
}

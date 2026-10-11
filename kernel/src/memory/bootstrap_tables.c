#include "bootstrap_tables.h"
#include "../libc/memory.h"

static void release_private(bootstrap_tables_t *t)
{
    while (t->count) {
        mapping_table_t owned=t->tables[--t->count];
        t->tables[t->count]=(mapping_table_t){0};
        t->ops.release(t->context,owned);
    }
    t->verified=false; t->main=NULL;
}

bool bootstrap_tables_abort(bootstrap_tables_t *t)
{
    if (!t || t->published) return false;
    release_private(t); return true;
}

bool bootstrap_tables_create(bootstrap_tables_t *t, const paging_build_t *main,
    const bootstrap_claim_t *claim, const mapping_table_ops_t *ops, void *context)
{
    if (!t || t->count || t->published || !main || !main->verified || !main->activated ||
        !main->count || !claim || !claim->generation || claim->owner!=BOOTSTRAP_TRAMPOLINE_OWNER || claim->bytes!=4096 ||
        !claim->physical || claim->physical>=0xa0000 || (claim->physical&4095) ||
        claim->origin_type!=EFI_CONVENTIONAL_MEMORY || claim->physical<claim->origin_first ||
        claim->origin_end<claim->physical+4096 || !ops || !ops->allocate || !ops->release) return false;
    for (unsigned i=0; i<256; ++i) if (main->tables[0].table.cpu->entries[i]) return false;
    t->main=main; t->claim=*claim; t->ops=*ops; t->context=context;
    for (unsigned i=0; i<BOOTSTRAP_PRIVATE_TABLES; ++i) {
        mapping_table_t page={0};
        if (!ops->allocate(context,&page)) goto fail;
        /* Never zero or release a foreign active/shared frame returned by a bad allocator. */
        for (unsigned j=0; j<main->count; ++j)
            if (page.physical==main->tables[j].table.physical || page.cpu==main->tables[j].table.cpu) goto fail;
        for (unsigned j=0; j<t->count; ++j)
            if (page.physical==t->tables[j].physical || page.cpu==t->tables[j].cpu) goto fail;
        if (page.physical==claim->physical) goto fail;
        if (!page.cpu || !page.physical || (page.physical&4095) || page.physical>PHYSICAL_ENTRY_LIMIT-4096 ||
            !ram_map_contains(main->plan,page.physical,4096)) {
            if (page.cpu) ops->release(context,page);
            goto fail;
        }
        t->tables[t->count++]=page;
        memset(page.cpu,0,4096);
    }
    for (unsigned i=256; i<512; ++i)
        t->tables[0].cpu->entries[i]=main->tables[0].table.cpu->entries[i];
    for (unsigned i=0; i<3; ++i)
        if (!paging_entry_encode(&main->plan->features,4-i,false,t->tables[i+1].physical,
            0,true,false,&t->tables[i].cpu->entries[0])) goto fail;
    if (!paging_entry_encode(&main->plan->features,1,true,claim->physical,0,true,false,
        &t->tables[3].cpu->entries[claim->physical/4096])) goto fail;
    if (!bootstrap_tables_verify(t)) goto fail;
    return true;
fail:
    bootstrap_tables_abort(t); return false;
}

bool bootstrap_tables_verify(bootstrap_tables_t *t)
{
    if (t) t->verified=false;
    if (!t || t->count!=BOOTSTRAP_PRIVATE_TABLES || !t->main) return false;
    for (unsigned table=0; table<BOOTSTRAP_PRIVATE_TABLES; ++table) {
        if (!t->tables[table].cpu || !t->tables[table].physical || (t->tables[table].physical&4095) ||
            t->tables[table].physical>PHYSICAL_ENTRY_LIMIT-4096) return false;
        for (unsigned j=0; j<table; ++j)
            if (t->tables[j].physical==t->tables[table].physical || t->tables[j].cpu==t->tables[table].cpu) return false;
        for (unsigned i=0; i<512; ++i) {
            uint64_t raw=t->tables[table].cpu->entries[i];
            if (!table && i>=256) {
                if ((raw^t->main->tables[0].table.cpu->entries[i])&~UINT64_C(0x60)) return false;
                continue;
            }
            bool required=table<3 ? i==0 : i==t->claim.physical/4096;
            if (!required) { if (raw) return false; continue; }
            paging_entry_view_t v;
            if (!paging_entry_decode(&t->main->plan->features,4-table,raw,&v) ||
                v.leaf!=(table==3) || v.nx || v.user || !v.writable || v.pat_slot ||
                v.physical!=(table==3?t->claim.physical:t->tables[table+1].physical)) return false;
        }
    }
    t->verified=true; return true;
}

bool bootstrap_tables_retire(bootstrap_tables_t *t, const bootstrap_lifetime_t *life)
{
    if (!t || !t->published || t->count!=BOOTSTRAP_PRIVATE_TABLES || !life ||
        __atomic_load_n(&life->state,__ATOMIC_ACQUIRE)!=BOOTSTRAP_SEALED ||
        life->root!=t->tables[0].physical || life->main_root!=t->main->tables[0].table.physical) return false;
    release_private(t); return true;
}

#include "bootstrap_memory.h"
#include "physical_layout.h"

_Static_assert(HOBBYOS_MAX_CPUS < 64, "Bootstrap target mask must cover the admitted topology");

static bool high_range(uint64_t first, uint64_t bytes)
{
    return first >= UINT64_C(0xffff800000000000) && bytes && bytes-1 <= UINT64_MAX-first;
}

bool bootstrap_lifetime_not_needed(bootstrap_lifetime_t *l, uint64_t main_root, unsigned count)
{
    if (!l || l->state!=BOOTSTRAP_NEW || l->root || l->intended || l->count ||
        count!=1 || !main_root || (main_root&4095)) return false;
    l->main_root=main_root;
    __atomic_store_n(&l->state,BOOTSTRAP_NOT_NEEDED,__ATOMIC_RELEASE);
    return true;
}

bool bootstrap_lifetime_begin(bootstrap_lifetime_t *l, uint64_t generation,
    uint64_t root, uint64_t main_root, uint64_t intended, const uint32_t *ids, unsigned count)
{
    if (!l || l->state != BOOTSTRAP_NEW || !generation || !root || root >= PHYSICAL_ENTRY_LIMIT ||
        !main_root || root == main_root || ((root|main_root)&4095) || !count ||
        count > HOBBYOS_MAX_CPUS || !ids || !intended || (intended >> count)) return false;
    for (unsigned i=0; i<count; ++i)
        for (unsigned j=0; j<i; ++j) if (ids[i] == ids[j]) return false;
    l->generation=generation; l->root=root; l->main_root=main_root;
    l->intended=intended; l->count=count;
    for (unsigned i=0; i<count; ++i) l->cpus[i].apic_id=ids[i];
    __atomic_store_n(&l->state, BOOTSTRAP_ACTIVE, __ATOMIC_RELEASE);
    return true;
}

static bool target(const bootstrap_lifetime_t *l, unsigned slot, uint32_t id, uint64_t generation)
{
    return l && __atomic_load_n(&l->state,__ATOMIC_ACQUIRE)==BOOTSTRAP_ACTIVE &&
        l->generation==generation && slot<l->count && (l->intended&(UINT64_C(1)<<slot)) &&
        l->cpus[slot].apic_id==id;
}

bool bootstrap_lifetime_start(bootstrap_lifetime_t *l, unsigned slot, uint32_t id, uint64_t generation)
{
    if (!target(l,slot,id,generation)) return false;
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&l->cpus[slot].started,&expected,1,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}

bool bootstrap_lifetime_retry(const bootstrap_lifetime_t *l, unsigned slot, uint32_t id, uint64_t generation)
{
    return target(l,slot,id,generation) && __atomic_load_n(&l->cpus[slot].started,__ATOMIC_ACQUIRE)==1 &&
        !__atomic_load_n(&l->cpus[slot].completed,__ATOMIC_ACQUIRE);
}

bool bootstrap_lifetime_depart(bootstrap_lifetime_t *l, const bootstrap_departure_t *d)
{
    if (!d || !bootstrap_lifetime_retry(l,d->slot,d->apic_id,d->generation) ||
        d->previous_root!=l->root || d->root!=l->main_root || d->cs!=0x08 || d->tr!=0x28 ||
        !high_range(d->stack_first,d->stack_bytes) || d->stack_bytes!=16384 ||
        d->rsp<d->stack_first || d->rsp-d->stack_first>=d->stack_bytes ||
        !high_range(d->gdtr,4096) || !high_range(d->idtr,4096) || !high_range(d->tss,104) ||
        !high_range(d->ist1-32768,32768) || !high_range(d->ist2-32768,32768) ||
        !high_range(d->ist3-32768,32768)) return false;
    /* A target has one admitted AP execution; a duplicate cannot replace its receipt. */
    uint32_t expected=1;
    if (!__atomic_compare_exchange_n(&l->cpus[d->slot].started,&expected,2,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) return false;
    l->cpus[d->slot].departure=*d;
    __atomic_store_n(&l->cpus[d->slot].completed,1,__ATOMIC_RELEASE);
    return true;
}

bool bootstrap_lifetime_seal(bootstrap_lifetime_t *l, uint64_t bsp_root)
{
    if (!l || __atomic_load_n(&l->state,__ATOMIC_ACQUIRE)!=BOOTSTRAP_ACTIVE || bsp_root!=l->main_root) return false;
    for (unsigned i=0; i<l->count; ++i) if (l->intended&(UINT64_C(1)<<i)) {
        if (!__atomic_load_n(&l->cpus[i].completed,__ATOMIC_ACQUIRE) ||
            l->cpus[i].departure.root!=l->main_root ||
            l->cpus[i].departure.generation!=l->generation) return false;
    }
    uint32_t expected=BOOTSTRAP_ACTIVE;
    return __atomic_compare_exchange_n(&l->state,&expected,BOOTSTRAP_SEALED,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);
}

bool bootstrap_lifetime_finish(bootstrap_lifetime_t *l)
{
    if (!l) return false;
    uint32_t expected=BOOTSTRAP_SEALED;
    return __atomic_compare_exchange_n(&l->state,&expected,BOOTSTRAP_RETIRED,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}

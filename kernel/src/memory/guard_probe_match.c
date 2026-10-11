#include "guard_probe.h"
bool guard_probe_match(const guard_probe_state_t *s,const stack_owner_view_t *v,
    uint64_t generation,uint64_t root,uint32_t cpu,uint32_t apic,uint64_t rip,
    uint64_t address,uint64_t error,uint64_t cs,uint64_t site,uint64_t recovery)
{
    return s && v && s->armed && !s->seen && generation && s->generation==generation &&
        s->root && s->root==root && cpu<HOBBYOS_MAX_CPUS && s->cpu==cpu && s->apic==apic &&
        site && recovery && site!=recovery && s->site==site && rip==site && s->recovery==recovery &&
        cs==0x08 && !error && address==s->address &&
        v->state==STACK_LIVE && v->generation && v->generation==s->owner_generation &&
        v->slot==s->owner_slot && v->base-v->guard==STACK_GUARD_BYTES &&
        (address==v->guard || address==v->base-1);
}

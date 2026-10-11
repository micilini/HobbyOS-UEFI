#include "protection_probe.h"

static bool target_valid(const protection_target_t *t,unsigned attempt)
{
    return t && attempt<PROTECTION_ATTEMPTS && t->kind==attempt/2 && t->alias==attempt%2 &&
        t->address>=UINT64_C(0xffff800000000000) && t->site>=UINT64_C(0xffff800000000000) &&
        t->recovery>=UINT64_C(0xffff800000000000) && t->site!=t->recovery &&
        (t->kind!=PROTECTION_FETCH_DATA || t->site==t->address);
}

bool protection_probe_arm(protection_probe_state_t *s,const protection_target_t *t,
                         const protection_fault_t *o,unsigned attempt,bool nx)
{
    if (!s || !o || s->generation || s->armed || s->seen || s->completed ||
        !target_valid(t,attempt) || (t->kind==PROTECTION_FETCH_DATA && !nx) ||
        !o->generation || !o->collection || !o->root || (o->root&4095) ||
        (o->root & ~UINT64_C(0x000ffffffffff000)) || o->slot>=HOBBYOS_MAX_CPUS || o->apic==UINT32_MAX) return false;
    *s=(protection_probe_state_t){.generation=o->generation,.collection=o->collection,.root=o->root,
        .site=t->site,.address=t->address,.recovery=t->recovery,.slot=o->slot,.apic=o->apic,
        .attempt=attempt,.kind=t->kind,.alias=t->alias,.armed=true};
    return true;
}

bool protection_probe_match(const protection_probe_state_t *s,const protection_target_t *t,
                           const protection_fault_t *e)
{
    if (!s || !e || !target_valid(t,s->attempt) || !s->armed || s->seen || s->completed ||
        !s->generation || !s->collection || s->generation!=e->generation || s->collection!=e->collection ||
        s->root!=e->root || s->slot!=e->slot || s->apic!=e->apic || e->cs!=0x08 ||
        s->site!=t->site || s->address!=t->address || s->recovery!=t->recovery ||
        s->kind!=t->kind || s->alias!=t->alias || e->rip!=t->site || e->address!=t->address) return false;
    return e->error==(t->kind==PROTECTION_FETCH_DATA ? 17u : 3u);
}

bool protection_probe_complete(protection_probe_state_t *s,const protection_target_t *t,
                              unsigned returned,uint8_t after)
{
    if (!s || !target_valid(t,s->attempt) || s->armed || !s->seen || s->completed || returned!=1 ||
        !s->generation || !s->collection || !s->root || s->site!=t->site || s->address!=t->address ||
        s->recovery!=t->recovery || s->kind!=t->kind || s->alias!=t->alias ||
        after!=t->original || s->fault_cs!=8 || s->fault_rip!=t->site || s->fault_address!=t->address ||
        s->fault_error!=(t->kind==PROTECTION_FETCH_DATA ? 17u : 3u)) return false;
    s->completed=true;return true;
}

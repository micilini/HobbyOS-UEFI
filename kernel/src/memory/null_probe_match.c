#include "null_probe.h"
bool null_probe_match(const null_probe_state_t *s, uint64_t rip, uint64_t address,
    uint64_t error, uint64_t cs, uint32_t slot, uint64_t generation)
{
    return s && s->armed && !s->seen && s->generation && s->generation==generation &&
        s->slot==slot && s->site && s->site==rip && !address && !error && cs==0x08 &&
        s->recovery && s->recovery!=s->site;
}

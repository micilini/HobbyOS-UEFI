#include "mapping_probe.h"
bool mapping_probe_match(const mapping_probe_state_t *s, uint64_t rip,
    uint64_t address, uint64_t error, uint64_t cs, uint32_t slot, uint64_t generation)
{
    return s && s->armed && !s->seen && s->generation && s->generation == generation &&
        s->slot == slot && s->site == rip && s->address == address && !(cs & 3) &&
        error == 0 && s->recovery && s->recovery != s->site;
}

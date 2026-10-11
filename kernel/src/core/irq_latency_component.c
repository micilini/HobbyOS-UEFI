#include "irq_latency_component.h"

bool irq_latency_component_add(irq_latency_component_t *row, uint64_t begin,
    uint64_t end, uint64_t generation, uint64_t root, uint64_t context,
    bool valid, bool success)
{
    if (!row || row->count == UINT64_MAX) return false;
    ++row->count;
    if (!success) ++row->failures;
    if (!generation || !root || (root & 4095) || !begin || end <= begin || !valid) {
        ++row->invalid;
        return false;
    }
    if (!row->first_generation) row->first_generation = generation;
    row->last_generation = generation;
    if (end-begin > row->maximum) {
        row->maximum = end-begin;
        row->begin = begin; row->end = end; row->generation = generation;
        row->root = root; row->context = context;
    }
    return true;
}

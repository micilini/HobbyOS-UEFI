#include "irq_latency_scope.h"
#include "irq_latency_record.h"

static bool invalid(irq_latency_scope_state_t *s)
{
    if (s && s->invalid != UINT64_MAX) ++s->invalid;
    return false;
}

bool irq_latency_scope_push(irq_latency_scope_state_t *s, uint32_t slot,
    uint64_t generation, uint64_t paths, irq_latency_scope_token_t *token)
{
    if (!s || !token || token->active || !generation || !paths ||
        (paths & ~IRQ_LATENCY_ALL_PATHS) || s->depth >= IRQ_LATENCY_SCOPE_DEPTH)
        return invalid(s);
    if (s->owner_generation != generation) {
        if (s->depth || s->paths) return invalid(s);
        s->owner_generation = generation;
    }
    *token = (irq_latency_scope_token_t){generation, s->paths, s->depth + 1, slot, true};
    s->paths |= paths;
    ++s->depth;
    return true;
}

bool irq_latency_scope_pop(irq_latency_scope_state_t *s, uint32_t slot,
    uint64_t generation, irq_latency_scope_token_t *token)
{
    if (!s || !token || !token->active || token->stack_slot != slot ||
        token->owner_generation != generation || s->owner_generation != generation ||
        !s->depth || token->depth != s->depth) return invalid(s);
    s->paths = token->previous_paths;
    --s->depth;
    token->active = false;
    return true;
}

bool irq_latency_scope_retired(irq_latency_scope_state_t *s, uint64_t generation)
{
    if (!s || !generation || s->depth || s->paths) return invalid(s);
    return true;
}

bool irq_latency_scope_retire_interrupts(irq_latency_scope_state_t *s, uint32_t slot,
    uint64_t generation, irq_latency_scope_token_t *barriers, uint32_t count)
{
    if (!s || !generation || !barriers || count > IRQ_LATENCY_SCOPE_DEPTH || s->paths || s->depth != count)
        return invalid(s);
    for (unsigned i = 0; i < count; ++i)
        if (!barriers[i].active || barriers[i].stack_slot != slot ||
            barriers[i].owner_generation != generation || barriers[i].previous_paths ||
            barriers[i].depth != i+1 || s->owner_generation != generation) return invalid(s);
    /* A terminal task can be switched away from its exit's restored-IF
     * window. Only empty interrupt barriers may then remain on its stack. */
    while (count)
        if (!irq_latency_scope_pop(s, slot, generation, &barriers[--count])) return false;
    return irq_latency_scope_retired(s, generation);
}

bool irq_latency_scope_interrupt(irq_latency_scope_state_t *s, uint32_t slot,
    uint64_t generation, irq_latency_scope_token_t *token)
{
    if (!s || !token || token->active || !generation || s->depth >= IRQ_LATENCY_SCOPE_DEPTH)
        return invalid(s);
    if (s->owner_generation != generation) {
        if (s->depth || s->paths) return invalid(s);
        s->owner_generation = generation;
    }
    *token = (irq_latency_scope_token_t){generation, s->paths, s->depth + 1, slot, true};
    s->paths = 0;
    ++s->depth;
    return true;
}

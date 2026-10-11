#include "stack_fault.h"

stack_fault_kind_t stack_fault_classify(const stack_fault_facts_t *f,
    const stack_owner_view_t *owner,const stack_owner_view_t *handler)
{
    if (!f || !owner || !owner->generation || owner->state!=STACK_LIVE ||
        owner->guard+STACK_GUARD_BYTES!=owner->base ||
        f->address<owner->guard || f->address>=owner->base) return STACK_FAULT_UNKNOWN;
    if (f->vector==14) return STACK_FAULT_GUARD_ACCESS;
    /* DF is an abort: raw RIP is retained by panic output, never used as a
     * precise trigger oracle. CR2 alone cannot establish stack exhaustion. */
    if (f->vector!=8 || f->error || !handler || handler->state!=STACK_LIVE ||
        !handler->generation || handler->kind!=STACK_KIND_IST1 ||
        handler->cpu!=f->cpu || handler->apic!=f->apic || handler->slot==owner->slot ||
        f->handler_rsp<handler->base || f->handler_rsp>=handler->end ||
        owner->kind!=STACK_KIND_TASK || !owner->task_id || !owner->task_lifecycle ||
        f->rsp<owner->guard || f->rsp>owner->base+64 ||
        f->rsp<f->address || f->rsp-f->address>64) return STACK_FAULT_UNKNOWN;
    return STACK_FAULT_OVERFLOW_ASSOCIATED;
}

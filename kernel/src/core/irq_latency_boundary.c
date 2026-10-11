#include "irq_latency_interior.h"
#include "irq_latency_heap.h"
#include "irq_latency.h"
#include "irq_latency_boundary.h"
#include "irq_latency_tsc.h"
#include "clocksource.h"
#include "idt.h"
#include "interrupt_vectors.h"
#include "../cpu/cpu.h"
#include "../memory/kernel_stack.h"
#include "../smp/smp_topology.h"
#include <stddef.h>

_Static_assert(sizeof(irq_latency_boundary_t) == 96, "IRQ boundary assembly size");
_Static_assert(offsetof(irq_latency_boundary_t, observed_flags) == 80, "IRQ boundary flags offset");
_Static_assert(offsetof(irq_latency_boundary_t, tsc) == 24, "IRQ boundary TSC offset");
_Static_assert(offsetof(irq_latency_boundary_t, stack_pointer) == 16, "IRQ boundary stack offset");
_Static_assert(sizeof(irq_latency_writeback_stamps_t) == 32, "Writeback stamp assembly size");
_Static_assert(offsetof(irq_latency_writeback_stamps_t, end) == 8, "Writeback end offset");
_Static_assert(offsetof(irq_latency_writeback_stamps_t, begin_flags) == 16, "Writeback begin flags offset");
_Static_assert(offsetof(irq_latency_writeback_stamps_t, end_flags) == 24, "Writeback end flags offset");

static irq_latency_recorder_t g_irq_latency[HOBBYOS_MAX_CPUS];
static irq_latency_scope_state_t g_irq_scopes[STACK_OWNER_COUNT];
static irq_latency_scope_token_t g_irq_scope_barriers[STACK_OWNER_COUNT][4];
static uint32_t g_irq_scope_depth[STACK_OWNER_COUNT];
static uint64_t g_irq_switch_sequence[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_entry_sequence[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_last_exit[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_last_exit_entry[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_last_exit_switch[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_started[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_freeze_requested, g_irq_frozen[HOBBYOS_MAX_CPUS];
static irq_latency_snapshot_t g_irq_terminal[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_cutoff[HOBBYOS_MAX_CPUS], g_irq_snapshot_cycles[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_ready, g_irq_early_boundaries, g_irq_invalid_boundaries;
uint64_t irq_latency_request_epoch(uint32_t cpu)
{
    return __atomic_load_n(&g_irq_ready,__ATOMIC_ACQUIRE) && cpu < HOBBYOS_MAX_CPUS ? g_irq_latency[cpu].epoch : 0;
}
typedef struct __attribute__((aligned(64))) { uint64_t count, mmio_reused, prechecked; } irq_nested_counter_t;
static irq_nested_counter_t g_irq_nested_elided[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_invalid_sites[32];
static uint64_t g_irq_owner_failure[HOBBYOS_MAX_CPUS][4];
static irq_latency_component_t g_irq_callbacks[HOBBYOS_MAX_CPUS][4];
static irq_latency_component_t g_irq_requests[HOBBYOS_MAX_CPUS][4];
static uint64_t g_irq_freeze_probe[HOBBYOS_MAX_CPUS][14];

static void boundary_failure(unsigned site)
{
    __atomic_add_fetch(&g_irq_invalid_boundaries, 1, __ATOMIC_RELAXED);
    if (site < 32) __atomic_add_fetch(&g_irq_invalid_sites[site], 1, __ATOMIC_RELAXED);
}

static uint32_t g_irq_identity_leaf;
uint32_t g_irq_latency_identity_ready;
static irq_latency_recorder_t g_irq_calibration_records[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_probe_armed[HOBBYOS_MAX_CPUS], g_irq_probe_active[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_probe_begin[HOBBYOS_MAX_CPUS], g_irq_probe_tail[HOBBYOS_MAX_CPUS][2];
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
static uint64_t g_irq_probe_checkpoint[HOBBYOS_MAX_CPUS];
#endif
static uint64_t g_irq_probe_sequence[HOBBYOS_MAX_CPUS], g_irq_boundary_allowance[HOBBYOS_MAX_CPUS];
typedef struct { uint64_t lock, wait_start, acquired; } irq_latency_lock_t;
static irq_latency_lock_t g_irq_locks[HOBBYOS_MAX_CPUS][IRQ_LATENCY_SCOPE_DEPTH];
static uint32_t g_irq_lock_depth[HOBBYOS_MAX_CPUS];
extern char bsp_stack_bottom[], bsp_stack_top[];

static irq_latency_recorder_t *recorder_for(uint32_t cpu)
{
    return g_irq_probe_active[cpu] ? &g_irq_calibration_records[cpu] : &g_irq_latency[cpu];
}

uint64_t g_irq_interior_fault_sequence, g_irq_interior_fault_overflow;
static uint64_t g_irq_mask_segment[HOBBYOS_MAX_CPUS];
typedef struct {
    uint64_t generation, epoch, switches, entries, segment, faults;
    uint32_t cpu, architectural_id;
    bool valid;
} irq_latency_mask_identity_t;
static irq_latency_mask_identity_t g_irq_mask_identity[STACK_OWNER_COUNT];
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
static uint64_t g_irq_identity_queries[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_interior_paths[HOBBYOS_MAX_CPUS][IRQ_LATENCY_SCOPE_DEPTH];
static irq_latency_interior_point_t g_irq_interior_open[HOBBYOS_MAX_CPUS][IRQ_LATENCY_SCOPE_DEPTH];
static irq_latency_interior_record_t g_irq_interior_max[HOBBYOS_MAX_CPUS][IRQ_LATENCY_PATHS];
static irq_latency_writeback_record_t g_irq_writeback[HOBBYOS_MAX_CPUS];
static irq_latency_clock_record_t g_irq_clock_body[HOBBYOS_MAX_CPUS];
static irq_latency_tick_record_t g_irq_tick_body[HOBBYOS_MAX_CPUS];
static irq_latency_tick_sample_t g_irq_tick_sample[HOBBYOS_MAX_CPUS];
_Static_assert(sizeof(irq_latency_tick_sample_t)==64, "tick sample is CPU-local");
static irq_latency_heap_event_t g_irq_heap_open[HOBBYOS_MAX_CPUS];
static irq_latency_heap_record_t g_irq_heap_max[HOBBYOS_MAX_CPUS][IRQ_HEAP_OPERATIONS];
static uint64_t g_irq_heap_sequence[HOBBYOS_MAX_CPUS];
static irq_latency_heap_operation_t g_irq_heap_operation_open[HOBBYOS_MAX_CPUS];
static uint64_t g_irq_heap_operation_state[HOBBYOS_MAX_CPUS];
static irq_latency_heap_operation_record_t g_irq_heap_operation_max[HOBBYOS_MAX_CPUS];
#endif

static bool slot_now(uint32_t *slot)
{
#ifdef IRQ_LATENCY_HOSTED_CONTROLLER
#if !__STDC_HOSTED__
#error IRQ_LATENCY_HOSTED_CONTROLLER requires hosted implementation tests
#endif
    extern bool irq_latency_host_slot(uint32_t *slot);
    bool valid = irq_latency_host_slot(slot);
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    if (valid && !irq_is_enabled() && g_irq_identity_queries[*slot] != UINT64_MAX) ++g_irq_identity_queries[*slot];
#endif
    return valid;
#else
    uint32_t a, b, c, d;
    /* Debug xAPIC MMIO tracing uses irq_save itself. Identity must not enter
     * LAPIC/MMIO helpers from an IRQ boundary observer. */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(g_irq_identity_leaf), "c"(0) : "memory");
    uint32_t architectural_id = g_irq_identity_leaf == 1 ? b >> 24 : d;
    bool valid = smp_cpu_slot_from_apic_id(architectural_id, slot) &&
        *slot < g_cpu_count && *slot < HOBBYOS_MAX_CPUS;
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    if (valid && !irq_is_enabled() && g_irq_identity_queries[*slot] != UINT64_MAX) ++g_irq_identity_queries[*slot];
#endif
    return valid;
#endif
}

static uint32_t stack_slot(uint64_t rsp)
{
#ifdef IRQ_LATENCY_HOSTED_CONTROLLER
    extern uint32_t irq_latency_host_stack(uint64_t rsp);
    return irq_latency_host_stack(rsp);
#else
    if (rsp >= STACK_REGION_BASE && rsp < STACK_REGION_END) {
        uint64_t offset = rsp - STACK_REGION_BASE;
        uint32_t slot = (uint32_t)(offset / STACK_SLOT_BYTES);
        uint64_t within = offset % STACK_SLOT_BYTES;
        uint64_t capacity = slot < STACK_CPU_SLOTS ? STACK_IST_BYTES : STACK_TASK_BYTES;
        if (within >= STACK_GUARD_BYTES && within < STACK_GUARD_BYTES + capacity) return slot;
    }
    if (rsp >= (uintptr_t)bsp_stack_bottom && rsp < (uintptr_t)bsp_stack_top) return STACK_BSP_OWNER;
    return UINT32_MAX;
#endif
}

/* Stack ownership selects storage, never a CPU. Only a real architectural
 * lookup seeds this token, and every serviceable/switch/entry/fault boundary
 * invalidates it before a later masked hook can reuse it. */
static void masked_identity_seed(uint32_t cpu)
{
    if (!g_irq_ready || irq_is_enabled() || cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS) return;
    uint32_t stack = stack_slot((uintptr_t)__builtin_frame_address(0));
    stack_owner_view_t owner;
    if (stack >= STACK_OWNER_COUNT || !kernel_stack_read(stack,&owner) || !owner.generation) return;
    irq_latency_mask_identity_t *token = &g_irq_mask_identity[stack];
    *token = (irq_latency_mask_identity_t){.generation=owner.generation,.cpu=cpu,
        .architectural_id=__atomic_load_n(&g_cpus[cpu].apic_id,__ATOMIC_RELAXED),
        .epoch=g_irq_latency[cpu].epoch,.switches=__atomic_load_n(&g_irq_switch_sequence[cpu],__ATOMIC_ACQUIRE),
        .entries=g_irq_entry_sequence[cpu],.segment=g_irq_mask_segment[cpu],
        .faults=__atomic_load_n(&g_irq_interior_fault_sequence,__ATOMIC_ACQUIRE),.valid=true};
}

static bool masked_token_cpu(uint32_t *cpu)
{
    if (!g_irq_ready || irq_is_enabled()) return false;
    uint32_t stack = stack_slot((uintptr_t)__builtin_frame_address(0));
    stack_owner_view_t owner;
    if (stack < STACK_OWNER_COUNT && kernel_stack_read(stack,&owner) && owner.generation) {
        const irq_latency_mask_identity_t *t = &g_irq_mask_identity[stack];
        uint32_t c = t->cpu;
        if (t->valid && c < g_cpu_count && c < HOBBYOS_MAX_CPUS &&
            __atomic_load_n(&g_cpus[c].slot,__ATOMIC_RELAXED)==c &&
            t->architectural_id==__atomic_load_n(&g_cpus[c].apic_id,__ATOMIC_RELAXED) &&
            t->generation == owner.generation && t->epoch == g_irq_latency[c].epoch &&
            t->switches != UINT64_MAX && t->switches == __atomic_load_n(&g_irq_switch_sequence[c],__ATOMIC_ACQUIRE) &&
            t->entries != UINT64_MAX && t->entries == g_irq_entry_sequence[c] &&
            t->segment != UINT64_MAX && t->segment == g_irq_mask_segment[c] &&
            !__atomic_load_n(&g_irq_interior_fault_overflow,__ATOMIC_ACQUIRE) &&
            t->faults == __atomic_load_n(&g_irq_interior_fault_sequence,__ATOMIC_ACQUIRE)) {
            *cpu=c;return true;
        }
    }
    return false;
}

static bool masked_slot_now(uint32_t *cpu)
{
    if (masked_token_cpu(cpu)) return true;
    if (!slot_now(cpu)) return false;
    masked_identity_seed(*cpu);
    return true;
}

bool irq_latency_mmio_identity(uint32_t *architectural_id)
{
    uint32_t cpu;
    if (!architectural_id || !masked_token_cpu(&cpu) || g_irq_frozen[cpu] ||
        g_irq_nested_elided[cpu].mmio_reused == UINT64_MAX) return false;
    *architectural_id = __atomic_load_n(&g_cpus[cpu].apic_id,__ATOMIC_RELAXED);
    ++g_irq_nested_elided[cpu].mmio_reused;
    return true;
}

static void masked_identity_leave(void)
{
    uint32_t stack = stack_slot((uintptr_t)__builtin_frame_address(0));
    if (stack < STACK_OWNER_COUNT) g_irq_mask_identity[stack].valid=false;
}

static uint64_t paths_at(uint64_t rsp)
{
    uint32_t slot = stack_slot(rsp);
    return slot < STACK_OWNER_COUNT ? g_irq_scopes[slot].paths : 0;
}

bool irq_latency_memory_scope_active(void)
{
    if (!__atomic_load_n(&g_irq_ready,__ATOMIC_ACQUIRE)) return false;
    uint32_t slot=stack_slot((uintptr_t)__builtin_frame_address(0));
    return slot<STACK_OWNER_COUNT && __atomic_load_n(&g_irq_scopes[slot].paths,__ATOMIC_ACQUIRE)!=0;
}

static bool nested_record_unchanged(const irq_latency_recorder_t *r,
    uint64_t flags, uint64_t paths, bool same);

bool irq_latency_before_mask(irq_latency_boundary_t *b)
{
    b->ready = 0;
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_irq_early_boundaries, 1, __ATOMIC_RELAXED);
        return false;
    }
    uint32_t slot;
    bool retained = masked_token_cpu(&slot);
    if (!retained && !masked_slot_now(&slot)) {
        boundary_failure(1);
        return false;
    }
    if (__atomic_load_n(&g_irq_frozen[slot], __ATOMIC_ACQUIRE)) return false;
    /* A retained token proves this same owner has stayed masked. The nested
     * timestamp never becomes an endpoint when both original updates are no-ops.
     * Keep the assembly CLI and exact flag return; omit only observer work. */
    if (retained && b->flags == b->observed_flags && g_irq_started[slot] &&
        nested_record_unchanged(recorder_for(slot), b->flags, paths_at(b->stack_pointer), true) &&
        g_irq_nested_elided[slot].count != UINT64_MAX &&
        g_irq_nested_elided[slot].prechecked != UINT64_MAX) {
        ++g_irq_nested_elided[slot].count;
        ++g_irq_nested_elided[slot].prechecked;
        return false;
    }
    b->cpu = slot;
    b->switch_sequence = __atomic_load_n(&g_irq_switch_sequence[slot], __ATOMIC_ACQUIRE);
    b->end_cpu = __atomic_load_n(&g_irq_entry_sequence[slot], __ATOMIC_ACQUIRE);
    b->ready = 1;
    return true;
}

static bool nested_record_unchanged(const irq_latency_recorder_t *r,
    uint64_t flags, uint64_t paths, bool same)
{
    if (!same || (flags & IRQ_LATENCY_IF) || irq_is_enabled() ||
        (paths & ~IRQ_LATENCY_ALL_PATHS)) return false;
    uint64_t sequence = __atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->epoch, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&r->active, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&r->busy, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&r->exhausted, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&r->recursion, __ATOMIC_ACQUIRE) &&
        !(sequence & 1) && sequence <= UINT64_MAX - 4 &&
        !__atomic_load_n(&r->pending_count, __ATOMIC_ACQUIRE) &&
        !(paths & ~__atomic_load_n(&r->paths, __ATOMIC_ACQUIRE));
}

void irq_latency_after_mask(irq_latency_boundary_t *b)
{
    uint32_t slot;
    bool identified = (b->flags & IRQ_LATENCY_IF) ? slot_now(&slot) : masked_slot_now(&slot);
    if (!identified) { boundary_failure(2); return; }
    masked_identity_seed(slot);
    uint64_t switches = __atomic_load_n(&g_irq_switch_sequence[slot], __ATOMIC_ACQUIRE);
    uint64_t entries = __atomic_load_n(&g_irq_entry_sequence[slot], __ATOMIC_ACQUIRE);
    bool same = b->cpu == slot && b->switch_sequence == switches && b->end_cpu == entries;
    uint64_t start = b->tsc;
    irq_latency_boundary_kind_t kind = IRQ_BOUNDARY_DIRECT;
    bool enveloped = !same && (b->flags & IRQ_LATENCY_IF) && g_irq_last_exit[slot] &&
        g_irq_last_exit_entry[slot] == entries && g_irq_last_exit_switch[slot] == switches;
    if (enveloped) { start = g_irq_last_exit[slot]; kind = IRQ_BOUNDARY_AFTER_IRQ; }
    irq_latency_context_t context = same || enveloped ? IRQ_LATENCY_THREAD : IRQ_LATENCY_UNTRACKED;
    if (__atomic_load_n(&g_irq_frozen[slot], __ATOMIC_ACQUIRE)) return;
    if (!g_irq_started[slot]) {
        if (b->flags & IRQ_LATENCY_IF) g_irq_started[slot] = 1;
        else context = IRQ_LATENCY_EARLY;
    }
    irq_latency_recorder_t *r = recorder_for(slot);
    uint64_t paths = paths_at(b->stack_pointer);
    /* Only this masked CPU creates pending slots. Remote completion/retirement
     * cannot create one when pending_count is zero. Both original calls then
     * change no witness or accounting state for this already open path set. */
    if (nested_record_unchanged(r, b->flags, paths, same)) {
        if (g_irq_nested_elided[slot].count != UINT64_MAX) {
            ++g_irq_nested_elided[slot].count;
            return;
        }
        boundary_failure(31);
    }
    (void)irq_latency_record_drain(r);
    if (!irq_latency_record_begin_envelope(r, b->flags, start,
            b->site, paths, context, b->tsc,
            same ? slot : UINT32_MAX, kind))
        boundary_failure(3);
}

bool irq_latency_before_restore(irq_latency_boundary_t *b)
{
    b->ready = 0;
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return false;
    uint32_t slot;
    if (!masked_slot_now(&slot)) {
        boundary_failure(4); return false;
    }
    if (b->flags & IRQ_LATENCY_IF) {
        masked_identity_leave();
        if (!(b->observed_flags & IRQ_LATENCY_IF) && g_irq_mask_segment[slot] != UINT64_MAX)
            ++g_irq_mask_segment[slot];
    }
    if (__atomic_load_n(&g_irq_frozen[slot], __ATOMIC_ACQUIRE)) return false;
    if (b->observed_flags & IRQ_LATENCY_IF) {
        if (!(b->flags & IRQ_LATENCY_IF))
            boundary_failure(5);
        return false;
    }
    if (!(b->flags & IRQ_LATENCY_IF)) return false;
    if (!g_irq_started[slot] && !g_irq_latency[slot].active) return false;
    if (g_irq_lock_depth[slot])
        boundary_failure(6);
    b->cpu = slot;
    b->switch_sequence = __atomic_load_n(&g_irq_switch_sequence[slot], __ATOMIC_ACQUIRE);
    irq_latency_recorder_t *r = recorder_for(slot);
    b->end_cpu = r->context == IRQ_LATENCY_HARD_IRQ ? g_irq_boundary_allowance[slot] : 0;
    uint32_t owner_slot = stack_slot(b->stack_pointer);
    stack_owner_view_t owner;
    uint64_t generation = owner_slot < STACK_OWNER_COUNT && kernel_stack_read(owner_slot, &owner) ? owner.generation : 0;
    if (!generation) {
        boundary_failure(24);
        ++g_irq_owner_failure[slot][0];
        g_irq_owner_failure[slot][1] = b->stack_pointer;
        g_irq_owner_failure[slot][2] = b->site;
        g_irq_owner_failure[slot][3] = owner_slot;
    }
    /* Store the frozen allowance before POPFQ. A nonreturning publisher must
     * retain it when the exact stack generation is later retired. */
    irq_latency_ticket_t ticket = irq_latency_record_close_calibrated(r, b->flags,
        b->site, owner_slot, generation, b->end_cpu);
    b->ticket_sequence = ticket.sequence; b->ticket_slot = ticket.slot; b->ready = ticket.valid;
    return ticket.valid;
}

void irq_latency_after_restore(irq_latency_boundary_t *b)
{
    uint32_t slot;
    bool same = slot_now(&slot) && slot == b->cpu && b->switch_sequence ==
        __atomic_load_n(&g_irq_switch_sequence[b->cpu], __ATOMIC_ACQUIRE);
    irq_latency_ticket_t ticket = {b->ticket_sequence, (uint32_t)b->ticket_slot, b->ready != 0};
    if (!irq_latency_record_complete_envelope(&g_irq_latency[b->cpu], ticket, b->tsc,
        same ? (uint32_t)b->cpu : UINT32_MAX, same ? IRQ_LATENCY_VALID : IRQ_LATENCY_BOUNDARY_UNCERTAIN,
        b->end_cpu, IRQ_BOUNDARY_DIRECT))
        boundary_failure(7);
}

void irq_latency_context_switch(void)
{
    if (!__atomic_load_n(&g_irq_latency_identity_ready, __ATOMIC_ACQUIRE)) return;
    uint32_t slot;
    if (!slot_now(&slot)) {
        boundary_failure(8); return;
    }
    uint64_t sequence = __atomic_load_n(&g_irq_switch_sequence[slot], __ATOMIC_RELAXED);
    if (sequence == UINT64_MAX) {
        boundary_failure(9); return;
    }
    __atomic_store_n(&g_irq_switch_sequence[slot], sequence + 1, __ATOMIC_RELEASE);
}

bool irq_latency_identity_prepare(void)
{
    if (__atomic_load_n(&g_irq_latency_identity_ready, __ATOMIC_ACQUIRE)) return true;
    if (!irq_is_enabled() || !g_cpu_count || g_cpu_count > HOBBYOS_MAX_CPUS) return false;
    uint32_t a, b, c, d;
    if (!irq_latency_tsc_available()) return false;
    uint32_t identity_leaf = 1;
    if (cpu_get_cpuid_count(0x1f, 0, &a, &b, &c, &d) && b) identity_leaf = 0x1f;
    else if (cpu_get_cpuid_count(0xb, 0, &a, &b, &c, &d) && b) identity_leaf = 0xb;
    if (identity_leaf == 1)
        for (uint32_t slot = 0; slot < g_cpu_count; ++slot)
            if (g_cpus[slot].apic_id > 255) return false;
    g_irq_identity_leaf = identity_leaf;
    for (uint32_t slot = 0; slot < g_cpu_count; ++slot)
        (void)irq_latency_record_init(&g_irq_calibration_records[slot], slot, 1, UINT64_MAX);
    __atomic_store_n(&g_irq_latency_identity_ready, 1, __ATOMIC_RELEASE);
    return true;
}

bool irq_latency_identity_sample(uint32_t *cpu, uint64_t *switch_sequence)
{
    if (!cpu || !switch_sequence || !__atomic_load_n(&g_irq_latency_identity_ready, __ATOMIC_ACQUIRE) ||
        !slot_now(cpu)) return false;
    *switch_sequence = __atomic_load_n(&g_irq_switch_sequence[*cpu], __ATOMIC_ACQUIRE);
    return *switch_sequence != UINT64_MAX;
}

bool irq_latency_activate(const irq_latency_timebase_t *timebases, uint32_t count)
{
    if (!timebases || !irq_is_enabled() || !count || count != g_cpu_count ||
        count > HOBBYOS_MAX_CPUS || __atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE) ||
        !irq_latency_identity_prepare()) return false;
    for (uint32_t slot = 0; slot < count; ++slot) {
        uint64_t limit;
        if (timebases[slot].cpu != slot || !timebases[slot].conversion_hz || !g_irq_boundary_allowance[slot] ||
            !irq_latency_muldiv(IRQ_LATENCY_BUDGET_NS - 1, timebases[slot].conversion_hz,
                                UINT64_C(1000000000), false, &limit) || !limit) return false;
    }
    uint64_t flags = irq_save();
    for (uint32_t slot = 0; slot < count; ++slot) {
        uint64_t limit;
        (void)irq_latency_muldiv(IRQ_LATENCY_BUDGET_NS - 1, timebases[slot].conversion_hz,
                                UINT64_C(1000000000), false, &limit);
        (void)irq_latency_record_init(&g_irq_latency[slot], slot, 1, limit);
        g_irq_mask_segment[slot] = 1;
    }
    __atomic_store_n(&g_irq_ready, 1, __ATOMIC_RELEASE);
    irq_restore(flags);
    return true;
}

irq_latency_scope_token_t irq_latency_scope_enter(uint64_t paths)
{
    irq_latency_scope_token_t token = {0};
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return token;
    uint64_t flags = irq_save();
    uint32_t stack = stack_slot((uintptr_t)&flags), cpu;
    stack_owner_view_t owner;
    bool valid = stack < STACK_OWNER_COUNT && kernel_stack_read(stack, &owner) &&
        (uintptr_t)&flags >= owner.base && (uintptr_t)&flags < owner.end && masked_slot_now(&cpu);
    if (valid) {
        if (!__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) {
            (void)irq_latency_record_tag(&g_irq_latency[cpu], paths);
            valid = irq_latency_scope_push(&g_irq_scopes[stack], stack, owner.generation, paths, &token);
        }
    }
    if (!valid) boundary_failure(10);
    irq_restore(flags);
    return token;
}

void irq_latency_scope_exit(irq_latency_scope_token_t *token)
{
    if (!token || !token->active) return;
    uint64_t flags = irq_save();
    uint32_t stack = stack_slot((uintptr_t)&flags);
    stack_owner_view_t owner;
    bool valid = stack < STACK_OWNER_COUNT && kernel_stack_read(stack, &owner) &&
        irq_latency_scope_pop(&g_irq_scopes[stack], stack, owner.generation, token);
    if (!valid) boundary_failure(11);
    irq_restore(flags);
}

void irq_latency_stack_retired(uint32_t slot, uint64_t generation)
{
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return;
    if (slot >= STACK_OWNER_COUNT || g_irq_scope_depth[slot] > 4 ||
        !irq_latency_scope_retire_interrupts(&g_irq_scopes[slot], slot, generation,
            g_irq_scope_barriers[slot], g_irq_scope_depth[slot])) {
        boundary_failure(12);
        return;
    }
    g_irq_scope_depth[slot] = 0;
    g_irq_mask_identity[slot].valid = false;
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu)
        (void)irq_latency_record_retire_owner(&g_irq_latency[cpu], slot, generation);
}

irq_latency_lock_token_t irq_latency_lock_wait(uintptr_t lock)
{
    irq_latency_lock_token_t token = {0};
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return token;
    uint32_t cpu;
    if (!masked_slot_now(&cpu)) {
        boundary_failure(13); return token;
    }
    if (__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return token;
    if (!g_irq_latency[cpu].active) {
        boundary_failure(14); return token;
    }
    if (!g_irq_latency[cpu].paths) return token;
    uint32_t depth = g_irq_lock_depth[cpu];
    if (depth >= IRQ_LATENCY_SCOPE_DEPTH) {
        boundary_failure(15); return token;
    }
    g_irq_locks[cpu][depth] = (irq_latency_lock_t){lock, irq_latency_read_tsc(), 0};
    g_irq_lock_depth[cpu] = depth + 1;
    token = (irq_latency_lock_token_t){g_irq_latency[cpu].begin_sequence,
        g_irq_switch_sequence[cpu], g_irq_entry_sequence[cpu], g_irq_latency[cpu].epoch,
        cpu, depth + 1, true};
    return token;
}

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
static irq_latency_interior_point_t interior_point(uint32_t cpu, uintptr_t lock)
{
    irq_latency_interior_point_t p = {0};
    uint64_t flags;
#ifdef IRQ_LATENCY_HOSTED_CONTROLLER
    flags = irq_is_enabled() ? IRQ_LATENCY_IF : 0;
#else
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
#endif
    uint32_t stack = stack_slot((uintptr_t)&p);
    stack_owner_view_t owner;
    if (cpu >= g_cpu_count || stack >= STACK_OWNER_COUNT || !kernel_stack_read(stack, &owner)) return p;
    p = (irq_latency_interior_point_t){.flags=flags,.cpu=cpu,.epoch=g_irq_latency[cpu].epoch,
        .segment=g_irq_mask_segment[cpu],.switches=g_irq_switch_sequence[cpu],.entries=g_irq_entry_sequence[cpu],
        .faults=__atomic_load_n(&g_irq_interior_fault_sequence,__ATOMIC_ACQUIRE),.owner=stack,
        .generation=owner.generation,.depth=g_irq_lock_depth[cpu],.lock=lock,
        .queries=g_irq_identity_queries[cpu],.recorder_sequence=g_irq_latency[cpu].sequence};
    if (__atomic_load_n(&g_irq_interior_fault_overflow,__ATOMIC_ACQUIRE)) p.faults=UINT64_MAX;
    return p;
}

static bool heap_slot(uintptr_t lock, uint32_t *cpu)
{
    if (!g_irq_ready || irq_is_enabled() || !masked_slot_now(cpu) || g_irq_frozen[*cpu]) return false;
    uint32_t depth=g_irq_lock_depth[*cpu];
    return depth && depth<=IRQ_LATENCY_SCOPE_DEPTH && g_irq_locks[*cpu][depth-1].lock==lock &&
        g_irq_locks[*cpu][depth-1].acquired && g_irq_latency[*cpu].active;
}

irq_latency_clock_token_t irq_latency_clock_prepare(uint64_t flags, uint64_t site)
{
    irq_latency_clock_token_t t={0};
    uint32_t cpu;
    if (!g_irq_ready || irq_is_enabled() || !masked_slot_now(&cpu) || g_irq_frozen[cpu]) return t;
    irq_latency_recorder_t *r=&g_irq_latency[cpu];
    if (!r->active || !r->paths) return t;
    irq_latency_clock_record_t *record=&g_irq_clock_body[cpu];
    if (record->pending || record->started>=UINT64_MAX-1) {
        if (record->invalid!=UINT64_MAX) ++record->invalid;
        return t;
    }
    t.sequence=++record->started;record->pending=t.sequence;
    t.begin=interior_point(cpu,0);t.incoming_flags=flags;t.site=site;t.paths=r->paths;t.phase=3;t.active=true;
    return t;
}

void irq_latency_clock_complete(irq_latency_clock_token_t t, uint64_t end)
{
    if (!t.active || t.begin.cpu >= g_cpu_count) return;
    irq_latency_clock_record_t *r=&g_irq_clock_body[t.begin.cpu];
    if (!t.sequence || r->pending!=t.sequence) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return;
    }
    r->pending=0;++r->retired;
    irq_latency_clock_witness_t w={.begin=t.begin,.incoming_flags=t.incoming_flags,
        .site=t.site,.paths=t.paths,.sequence=t.sequence,.phase=t.phase};
    uint32_t cpu;
    if (slot_now(&cpu) && !g_irq_frozen[cpu] && g_irq_latency[cpu].active) w.end=interior_point(cpu,0);
    w.end.tsc=end;
    if (!irq_latency_clock_valid(&w) || r->count==UINT64_MAX) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return;
    }
    ++r->count;
    uint64_t cycles=end-t.begin.tsc;
    if (cycles>r->maximum_cycles) { r->maximum_cycles=cycles;r->maximum=w; }
}

const irq_latency_clock_record_t *irq_latency_clock_read(uint32_t cpu)
{
    return cpu<g_cpu_count && g_irq_frozen[cpu] ? &g_irq_clock_body[cpu] : 0;
}

irq_latency_tick_token_t irq_latency_tick_prepare(void)
{
    irq_latency_tick_token_t t={0};uint32_t cpu;
    if (!g_irq_ready || irq_is_enabled() || !masked_slot_now(&cpu) || g_irq_frozen[cpu]) return t;
    irq_latency_recorder_t *r=&g_irq_latency[cpu];
    if (!r->active || r->context!=IRQ_LATENCY_HARD_IRQ || r->begin_site!=INT_VECTOR_LAPIC_TIMER) return t;
    irq_latency_tick_record_t *record=&g_irq_tick_body[cpu];
    if (record->pending || record->started>=UINT64_MAX-1) {
        if (record->invalid!=UINT64_MAX) ++record->invalid;
        return t;
    }
    t.sequence=++record->started;record->pending=t.sequence;
    t.begin=interior_point(cpu,0);t.outer_sequence=r->begin_sequence;t.active=true;
    g_irq_tick_sample[cpu]=(irq_latency_tick_sample_t){.outer_sequence=t.outer_sequence};
    return t;
}

irq_latency_tick_sample_t *irq_latency_tick_clock_window(uint32_t cpu)
{
    if (!g_irq_ready || irq_is_enabled() || cpu>=g_cpu_count || g_irq_frozen[cpu] ||
        !g_irq_tick_body[cpu].pending || g_irq_tick_sample[cpu].claimed ||
        !g_irq_latency[cpu].active || g_irq_latency[cpu].begin_sequence!=g_irq_tick_sample[cpu].outer_sequence) return 0;
    g_irq_tick_sample[cpu].claimed=true;
    return &g_irq_tick_sample[cpu];
}

void irq_latency_tick_complete(const irq_latency_tick_token_t *t, uint64_t end)
{
    if (!t || !t->active || t->begin.cpu>=g_cpu_count) return;
    irq_latency_tick_record_t *r=&g_irq_tick_body[t->begin.cpu];
    if (!t->sequence || r->pending!=t->sequence) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return;
    }
    r->pending=0;++r->retired;
    const irq_latency_tick_sample_t *sample=&g_irq_tick_sample[t->begin.cpu];
    irq_latency_tick_witness_t w={.begin=t->begin,.after_clock=t->after_clock,
        .after_work=t->after_work,.sequence=t->sequence,.outer_sequence=t->outer_sequence,
        .sample_begin=sample->begin,.sample_end=sample->end,.counter_end=sample->counter_end,
        .sample_ticks=sample->ticks,.sample_mode=sample->mode,.sample_retries=sample->retries,
        .sample_source=sample->source,.source_epoch=sample->source_epoch,
        .sample_valid=sample->valid};
    uint32_t cpu;
    if (slot_now(&cpu) && cpu==t->begin.cpu && !g_irq_frozen[cpu] && g_irq_latency[cpu].active &&
        g_irq_latency[cpu].begin_sequence==t->outer_sequence) {
        w.end=interior_point(cpu,0);w.paths=g_irq_latency[cpu].paths;
    }
    w.end.tsc=end;
    if (!irq_latency_tick_valid(&w)) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return;
    }
    if (!w.paths) { ++r->without_memory;return; }
    ++r->count;
    uint64_t cycles=end-t->begin.tsc;
    if (cycles>r->maximum_cycles) { r->maximum_cycles=cycles;r->maximum=w; }
}

const irq_latency_tick_record_t *irq_latency_tick_read(uint32_t cpu)
{
    return cpu<g_cpu_count && g_irq_frozen[cpu] ? &g_irq_tick_body[cpu] : 0;
}

void irq_latency_heap_begin(uintptr_t lock, unsigned operation, uint64_t pages)
{
    uint32_t cpu;
    if (!heap_slot(lock,&cpu)) return;
    irq_latency_heap_event_t *e=&g_irq_heap_open[cpu];
    if (e->operation || !operation || operation>IRQ_HEAP_OPERATIONS ||
        g_irq_heap_sequence[cpu]>=UINT64_MAX-1) { boundary_failure(31); return; }
    e->operation=operation;e->sequence=++g_irq_heap_sequence[cpu];e->marks=0;
    e->body.begin=interior_point(cpu,lock);e->body.paths=UINT64_C(1)<<IRQ_PATH_HEAP;
    e->wait_start=g_irq_locks[cpu][g_irq_lock_depth[cpu]-1].wait_start;
    e->acquired=g_irq_locks[cpu][g_irq_lock_depth[cpu]-1].acquired;
    e->pending_begin=g_irq_latency[cpu].pending_count;e->requested_pages=pages;
    e->body.begin.tsc=irq_latency_read_tsc();
}

void irq_latency_heap_mark(uintptr_t lock, unsigned site, uint64_t work, uint64_t bytes)
{
    uint64_t now=irq_latency_read_tsc();uint32_t cpu;
    if (!heap_slot(lock,&cpu)) return;
    irq_latency_heap_event_t *e=&g_irq_heap_open[cpu];
    if (!e->operation || e->body.begin.lock!=lock || e->marks>=IRQ_HEAP_MARKS) {
        boundary_failure(31);return;
    }
    e->mark[e->marks++]=(irq_latency_heap_mark_t){now,site,work,bytes};
}

void irq_latency_heap_end(uintptr_t lock)
{
    uint64_t now=irq_latency_read_tsc();uint32_t cpu;
    if (!heap_slot(lock,&cpu)) return;
    irq_latency_heap_event_t *e=&g_irq_heap_open[cpu];
    if (!e->operation || e->operation>IRQ_HEAP_OPERATIONS) { boundary_failure(31);return; }
    e->body.end=interior_point(cpu,lock);e->body.end.tsc=now;
    e->pending_end=g_irq_latency[cpu].pending_count;
    if (!irq_latency_heap_record(&g_irq_heap_max[cpu][e->operation-1],e)) boundary_failure(31);
    if (g_irq_heap_operation_state[cpu]) {
        irq_latency_heap_operation_t *w=&g_irq_heap_operation_open[cpu];
        w->parent.end=e->body.end;
        w->next_mark=e->marks && e->mark[e->marks-1].site==IRQ_HEAP_METADATA ? e->mark[e->marks-1].tsc : 0;
        if (g_irq_heap_operation_state[cpu]!=2 || w->sequence!=e->sequence)
            w->sequence=0;
        if (!irq_latency_heap_operation_record(&g_irq_heap_operation_max[cpu],w)) boundary_failure(31);
        g_irq_heap_operation_state[cpu]=0;
    }
    e->operation=0;
}

irq_latency_heap_operation_token_t irq_latency_heap_operation_prepare(uintptr_t lock)
{
    irq_latency_heap_operation_token_t t={0};uint32_t cpu;
    if (!heap_slot(lock,&cpu)) return t;
    irq_latency_heap_event_t *e=&g_irq_heap_open[cpu];
    if (g_irq_heap_operation_state[cpu] || (e->operation!=IRQ_HEAP_ALLOCATE && e->operation!=IRQ_HEAP_RECHECK) ||
        !e->marks || e->marks>IRQ_HEAP_MARKS || e->mark[e->marks-1].site!=IRQ_HEAP_FIRST_FIT) {
        boundary_failure(31);return t;
    }
    t.context=interior_point(cpu,lock);t.sequence=e->sequence;t.active=true;
    g_irq_heap_operation_open[cpu]=(irq_latency_heap_operation_t){
        .parent=e->body,.sequence=e->sequence,.operation=e->operation,
        .previous_mark=e->mark[e->marks-1].tsc};
    g_irq_heap_operation_state[cpu]=1;
    t.context.tsc=irq_latency_read_tsc();
    return t;
}

void irq_latency_heap_operation_publish(const irq_latency_heap_operation_token_t *t,
    uint64_t begin, uint64_t end, uint64_t header_bytes)
{
    if (!t || !t->active) return;
    uint32_t cpu;
    if (!heap_slot(t->context.lock,&cpu) || cpu!=t->context.cpu ||
        g_irq_heap_operation_state[cpu]!=1 || g_irq_heap_open[cpu].sequence!=t->sequence) {
        boundary_failure(31);return;
    }
    irq_latency_heap_operation_t *w=&g_irq_heap_operation_open[cpu];
    w->interval.begin=t->context;w->prepared=t->context.tsc;w->interval.begin.tsc=begin;
    w->interval.end=interior_point(cpu,t->context.lock);w->interval.end.tsc=end;
    w->interval.paths=UINT64_C(1)<<IRQ_PATH_HEAP;w->header_bytes=header_bytes;
    w->validated=irq_latency_read_tsc();
    if (!irq_latency_interior_valid(&w->interval)) boundary_failure(31);
    g_irq_heap_operation_state[cpu]=2;
}

const irq_latency_heap_operation_record_t *irq_latency_heap_operation_read(unsigned cpu)
{
    if (cpu>=g_cpu_count || !g_irq_frozen[cpu] || g_irq_heap_operation_state[cpu] ||
        g_irq_heap_open[cpu].operation) return 0;
    return &g_irq_heap_operation_max[cpu];
}

const irq_latency_heap_record_t *irq_latency_heap_read(unsigned cpu, unsigned operation)
{
    if (cpu>=g_cpu_count || !operation || operation>IRQ_HEAP_OPERATIONS || !g_irq_frozen[cpu]) return 0;
    if (g_irq_heap_open[cpu].operation) return 0;
    return &g_irq_heap_max[cpu][operation-1];
}

irq_latency_writeback_token_t irq_latency_writeback_begin(uint64_t generation, uint64_t root)
{
    irq_latency_writeback_token_t t = {0};
    uint32_t cpu;
    if (!g_irq_ready || irq_is_enabled() || !slot_now(&cpu) || g_irq_frozen[cpu]) return t;
    if (!g_irq_latency[cpu].active) { boundary_failure(31); return t; }
    t.begin=interior_point(cpu,0);t.request_generation=generation;t.root=root;t.active=true;
    return t;
}

irq_latency_writeback_witness_t irq_latency_writeback_points(irq_latency_writeback_token_t t,
    const irq_latency_writeback_stamps_t *stamps)
{
    if (!t.active) return (irq_latency_writeback_witness_t){0};
    if (t.begin.cpu >= g_cpu_count || !stamps) {
        boundary_failure(31); return (irq_latency_writeback_witness_t){0};
    }
    uint32_t cpu;
    irq_latency_writeback_witness_t w={.begin=t.begin,.request_generation=t.request_generation,.root=t.root};
    if (slot_now(&cpu) && !g_irq_frozen[cpu] && g_irq_latency[cpu].active)
        w.end=interior_point(cpu,0);
    /* Metadata surrounds the assembly interval. Both actual PUSHFQ samples
     * must agree with its masked context; sequence checks exclude transitions. */
    bool masked=!(w.begin.flags & IRQ_LATENCY_IF) && !(w.end.flags & IRQ_LATENCY_IF);
    w.begin.tsc=stamps->begin;w.end.tsc=stamps->end;
    w.begin.flags=stamps->begin_flags;w.end.flags=stamps->end_flags;
    if (!masked) w.end.segment=0;
    return w;
}

void irq_latency_writeback_end(irq_latency_writeback_token_t t,
    const irq_latency_writeback_stamps_t *stamps)
{
    if (!t.active) return;
    irq_latency_writeback_witness_t w=irq_latency_writeback_points(t,stamps);
    if (t.begin.cpu < g_cpu_count) (void)irq_latency_writeback_record(&g_irq_writeback[t.begin.cpu],&w);
}

const irq_latency_writeback_record_t *irq_latency_writeback_read(uint32_t cpu)
{
    return cpu < g_cpu_count && g_irq_frozen[cpu] ? &g_irq_writeback[cpu] : 0;
}

void irq_latency_interior_before_unlock(uintptr_t lock)
{
    uint64_t end = irq_latency_read_tsc();
    uint32_t cpu;
    if (!g_irq_ready || !masked_slot_now(&cpu) || g_irq_frozen[cpu] || !g_irq_lock_depth[cpu]) return;
    uint32_t depth = g_irq_lock_depth[cpu];
    if (depth > IRQ_LATENCY_SCOPE_DEPTH || g_irq_locks[cpu][depth-1].lock != lock) return;
    irq_latency_interior_witness_t w = {.begin=g_irq_interior_open[cpu][depth-1],
        .end=interior_point(cpu,lock),.paths=g_irq_interior_paths[cpu][depth-1] | g_irq_latency[cpu].paths};
    w.end.tsc=end;
    g_irq_interior_open[cpu][depth-1].tsc=0;
    bool valid = irq_latency_interior_valid(&w);
    for (unsigned path=0;path<IRQ_LATENCY_PATHS;++path) if (w.paths & (UINT64_C(1)<<path)) {
        irq_latency_interior_record_t *r=&g_irq_interior_max[cpu][path];
        if (!valid || r->count==UINT64_MAX) { if(r->invalid!=UINT64_MAX)++r->invalid;continue; }
        ++r->count;
        uint64_t cycles=w.end.tsc-w.begin.tsc;
        if (cycles>r->maximum_cycles) { r->maximum_cycles=cycles;r->maximum=w; }
    }
}

const irq_latency_interior_record_t *irq_latency_interior_read(uint32_t cpu,uint32_t path)
{
    if (cpu>=g_cpu_count || path>=IRQ_LATENCY_PATHS || !g_irq_frozen[cpu]) return 0;
    return &g_irq_interior_max[cpu][path];
}
#endif

void irq_latency_lock_acquired(uintptr_t lock, irq_latency_lock_token_t token)
{
    if (!token.active) return;
    uint64_t now = irq_latency_read_tsc();
    uint32_t cpu = token.cpu;
    /* This value lives only across the ticket acquisition with IF continuously
     * clear. It is never stored for reuse by unlock or after IRQ restoration.
     * Entry/switch/outer-generation changes invalidate the adjacent-hook token. */
    if (irq_is_enabled() || cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS ||
        !g_irq_latency[cpu].active || token.epoch != g_irq_latency[cpu].epoch ||
        token.sequence != g_irq_latency[cpu].begin_sequence ||
        token.switches != g_irq_switch_sequence[cpu] || token.entries != g_irq_entry_sequence[cpu] ||
        token.depth != g_irq_lock_depth[cpu] || !token.depth ||
        g_irq_locks[cpu][token.depth-1].lock != lock) {
        boundary_failure(26); return;
    }
    g_irq_locks[cpu][g_irq_lock_depth[cpu]-1].acquired = now;
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    g_irq_interior_paths[cpu][token.depth-1] = g_irq_latency[cpu].paths;
    g_irq_interior_open[cpu][token.depth-1] = interior_point(cpu,lock);
    g_irq_interior_open[cpu][token.depth-1].tsc = irq_latency_read_tsc();
#endif
}

void irq_latency_lock_ticket(uintptr_t lock, uint16_t distance)
{
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return;
    uint32_t cpu;
    if (!masked_slot_now(&cpu) || !g_irq_lock_depth[cpu] ||
        g_irq_locks[cpu][g_irq_lock_depth[cpu]-1].lock != lock) return;
    if (!irq_latency_record_ticket(&g_irq_latency[cpu], lock, distance)) boundary_failure(25);
}

void irq_latency_lock_released(uintptr_t lock)
{
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return;
    uint64_t now = irq_latency_read_tsc();
    uint32_t cpu;
    if (!masked_slot_now(&cpu) || !g_irq_lock_depth[cpu]) return;
    irq_latency_lock_t sample = g_irq_locks[cpu][g_irq_lock_depth[cpu]-1];
    if (sample.lock != lock) return;
    if ((!sample.acquired && now <= sample.wait_start) ||
        (sample.acquired && (sample.acquired <= sample.wait_start || now <= sample.acquired))) {
        boundary_failure(16); return;
    }
    --g_irq_lock_depth[cpu];
    (void)irq_latency_record_components(&g_irq_latency[cpu],
        (sample.acquired ? sample.acquired : now)-sample.wait_start,
        sample.acquired ? now-sample.acquired : 0);
}

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
uint64_t irq_latency_interrupt_enter_checkpoint(uint64_t start, uint64_t vector,
    uint64_t saved_flags, uint64_t interrupted_rsp, uint64_t early)
{
    uint64_t result = irq_latency_interrupt_enter(start, vector, saved_flags, interrupted_rsp);
    uint32_t cpu;
    if (slot_now(&cpu) && g_irq_probe_active[cpu] && vector == INT_VECTOR_SPURIOUS)
        g_irq_probe_checkpoint[cpu] = early;
    return result;
}
#endif

uint64_t irq_latency_interrupt_enter(uint64_t start, uint64_t vector,
                                  uint64_t saved_flags, uint64_t interrupted_rsp)
{
    uint32_t cpu;
    if (!__atomic_load_n(&g_irq_latency_identity_ready, __ATOMIC_ACQUIRE) || !slot_now(&cpu)) return 0;
    if (g_irq_entry_sequence[cpu] == UINT64_MAX)
        boundary_failure(17);
    else ++g_irq_entry_sequence[cpu];
    masked_identity_seed(cpu);
    if ((saved_flags & IRQ_LATENCY_IF) && __atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE))
        (void)irq_latency_record_interrupt(&g_irq_latency[cpu], start, cpu);
    bool probe = vector == INT_VECTOR_SPURIOUS &&
        __atomic_load_n(&g_irq_probe_armed[cpu], __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE) && !probe) return 0;
    if (__atomic_load_n(&g_irq_freeze_requested, __ATOMIC_ACQUIRE) &&
        vector == INT_VECTOR_LAPIC_TIMER) {
        uint64_t *v = g_irq_freeze_probe[cpu];
        uint64_t sequence = __atomic_load_n(&v[0], __ATOMIC_RELAXED);
        __atomic_store_n(&v[0], sequence + 1, __ATOMIC_RELEASE);
        const uint64_t values[] = {start, saved_flags, interrupted_rsp,
            paths_at(interrupted_rsp), g_irq_latency[cpu].active, g_irq_latency[cpu].paths,
            g_irq_latency[cpu].pending_count, g_irq_lock_depth[cpu], g_irq_latency[cpu].begin,
            g_irq_latency[cpu].begin_site, g_irq_latency[cpu].begin_sequence,
            g_irq_latency[cpu].context, g_irq_entry_sequence[cpu]};
        for (unsigned i = 0; i < 13; ++i)
            __atomic_store_n(&v[i+1], values[i], __ATOMIC_RELAXED);
        __atomic_store_n(&v[0], sequence + 2, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&g_irq_freeze_requested, __ATOMIC_ACQUIRE) &&
        vector == INT_VECTOR_LAPIC_TIMER && (saved_flags & IRQ_LATENCY_IF) &&
        !paths_at(interrupted_rsp) && !g_irq_latency[cpu].active && !g_irq_lock_depth[cpu]) {
        (void)irq_latency_record_drain(&g_irq_latency[cpu]);
        if (!g_irq_latency[cpu].pending_count) {
            uint64_t before = irq_latency_read_tsc();
            if (irq_latency_record_snapshot(&g_irq_latency[cpu], &g_irq_terminal[cpu]) &&
                clocksource_terminal_capture(cpu)) {
                g_irq_cutoff[cpu] = start;
                g_irq_snapshot_cycles[cpu] = irq_latency_read_tsc() - before;
                __atomic_store_n(&g_irq_frozen[cpu], 1, __ATOMIC_RELEASE);
                return 0;
            }
        }
    }
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE) && !probe) return 0;
    if (probe) g_irq_probe_active[cpu] = 1;
    else if (saved_flags & IRQ_LATENCY_IF) g_irq_started[cpu] = 1;
    uint32_t stack = stack_slot((uintptr_t)&cpu);
    stack_owner_view_t owner;
    bool scoped = stack < STACK_OWNER_COUNT && g_irq_scope_depth[stack] < 4 &&
        kernel_stack_read(stack, &owner) && irq_latency_scope_interrupt(&g_irq_scopes[stack],
            stack, owner.generation, &g_irq_scope_barriers[stack][g_irq_scope_depth[stack]]);
    if (scoped) ++g_irq_scope_depth[stack];
    else boundary_failure(18);
    irq_latency_recorder_t *r = recorder_for(cpu);
    (void)irq_latency_record_drain(r);
    if (!irq_latency_record_begin_envelope(r, saved_flags, start, vector,
            0, probe ? IRQ_LATENCY_TEST_PROBE : IRQ_LATENCY_HARD_IRQ,
            start, cpu, IRQ_BOUNDARY_IRQ_ADJACENT))
        boundary_failure(19);
    return IRQ_ENTRY_ADMITTED | (scoped ? IRQ_ENTRY_SCOPED : 0);
}

irq_latency_tail_t irq_latency_interrupt_exit(uint64_t vector, uint64_t saved_flags, uint64_t entry)
{
    irq_latency_tail_t tail = {0};
    uint32_t cpu;
    if (!__atomic_load_n(&g_irq_latency_identity_ready, __ATOMIC_ACQUIRE) || !masked_slot_now(&cpu)) return tail;
    if (entry > (IRQ_ENTRY_ADMITTED | IRQ_ENTRY_SCOPED) || entry == IRQ_ENTRY_SCOPED) {
        boundary_failure(30); return tail;
    }
    bool scoped = (entry & IRQ_ENTRY_SCOPED) != 0;
    masked_identity_leave();
    g_irq_last_exit[cpu] = irq_latency_read_tsc();
    g_irq_last_exit_entry[cpu] = g_irq_entry_sequence[cpu];
    g_irq_last_exit_switch[cpu] = g_irq_switch_sequence[cpu];
    bool probe = g_irq_probe_active[cpu] && vector == INT_VECTOR_SPURIOUS;
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE) && !probe) return tail;
    irq_latency_recorder_t *r = recorder_for(cpu);
    uint32_t stack = stack_slot((uintptr_t)&cpu);
    if (scoped && stack < STACK_OWNER_COUNT && g_irq_scope_depth[stack]) {
        irq_latency_scope_token_t *token = &g_irq_scope_barriers[stack][g_irq_scope_depth[stack]-1];
        if (irq_latency_scope_pop(&g_irq_scopes[stack], stack, token->owner_generation, token))
            --g_irq_scope_depth[stack];
        else boundary_failure(20);
    } else if (scoped)
        boundary_failure(21);
    if (__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE) && !probe) return tail;
    if (!(saved_flags & IRQ_LATENCY_IF)) return tail;
    /* The frame retains admission at entry across global activation. An IRQ
     * that began before activation has no interval to close unless a later
     * observed mask on this CPU opened one. Admitted missing begins still fail. */
    if (!(entry & IRQ_ENTRY_ADMITTED) && !r->active) return tail;
    uint64_t begin = r->begin;
    irq_latency_ticket_t ticket = irq_latency_record_close(r, saved_flags, vector);
    tail.pending = irq_latency_record_prepare_tail(r, ticket, cpu,
                                                   probe ? 0 : g_irq_boundary_allowance[cpu]);
    if (!tail.pending) boundary_failure(22);
    if (probe) {
        __atomic_store_n(&g_irq_probe_begin[cpu], begin, __ATOMIC_RELAXED);
        tail.probe_end = &g_irq_probe_tail[cpu][0];
        __atomic_add_fetch(&g_irq_probe_sequence[cpu], 1, __ATOMIC_RELEASE);
        g_irq_probe_active[cpu] = 0;
        __atomic_store_n(&g_irq_probe_armed[cpu], 0, __ATOMIC_RELEASE);
    }
    return tail;
}

void irq_latency_test_probe_role(void)
{
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return;
    uint32_t cpu;
    if (!masked_slot_now(&cpu) || !g_irq_latency[cpu].active) {
        boundary_failure(23); return;
    }
    /* Only fixed non-production request opcodes call this hook. Ordinary
     * allocator, integrity and cache-maintenance work never selects this role. */
    __atomic_store_n(&g_irq_latency[cpu].context, IRQ_LATENCY_TEST_PROBE, __ATOMIC_RELEASE);
}

#ifdef HOBBYOS_SELFTEST
bool irq_latency_irq_probe(irq_latency_irq_probe_t *out)
{
    if (!out) return false;
    *out = (irq_latency_irq_probe_t){.cpu = UINT32_MAX, .end_cpu = UINT32_MAX};
    if (!irq_is_enabled() || !irq_latency_identity_prepare()) return false;
    uint64_t flags = irq_save();
    bool ok = irq_latency_identity_sample(&out->cpu, &out->switch_before);
    if (ok) {
        out->sequence_before = __atomic_load_n(&g_irq_probe_sequence[out->cpu], __ATOMIC_ACQUIRE);
        out->irq_before = __atomic_load_n(&g_irq_entry_sequence[out->cpu], __ATOMIC_ACQUIRE);
        ok = !__atomic_exchange_n(&g_irq_probe_armed[out->cpu], 1, __ATOMIC_ACQ_REL);
    }
    irq_restore(flags);
    if (!ok) return false;
    out->outer_begin = irq_latency_read_tsc();
    __asm__ volatile("int $0xff" ::: "memory", "cc");
    out->outer_end = irq_latency_read_tsc();
    ok = irq_latency_identity_sample(&out->end_cpu, &out->switch_after);
    __atomic_store_n(&g_irq_probe_armed[out->cpu], 0, __ATOMIC_RELEASE);
    out->sequence_after = __atomic_load_n(&g_irq_probe_sequence[out->cpu], __ATOMIC_ACQUIRE);
    out->irq_after = __atomic_load_n(&g_irq_entry_sequence[out->cpu], __ATOMIC_ACQUIRE);
    out->inner_begin = __atomic_load_n(&g_irq_probe_begin[out->cpu], __ATOMIC_RELAXED);
    out->inner_end = __atomic_load_n(&g_irq_probe_tail[out->cpu][0], __ATOMIC_RELAXED);
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    out->entry_checkpoint = g_irq_probe_checkpoint[out->cpu];
    out->exit_checkpoint = g_irq_probe_tail[out->cpu][1];
#endif
    out->valid = ok && out->cpu == out->end_cpu && out->switch_before == out->switch_after &&
        out->sequence_before != UINT64_MAX && out->sequence_after == out->sequence_before + 1 &&
        out->irq_before != UINT64_MAX && out->irq_after == out->irq_before + 1 &&
        out->outer_begin <= out->inner_begin && out->inner_begin < out->inner_end &&
        out->inner_end <= out->outer_end;
    return out->valid;
}
#endif

bool irq_latency_set_irq_overhead(uint32_t cpu, uint64_t cycles)
{
    if (cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS || !cycles ||
        __atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE) || g_irq_boundary_allowance[cpu]) return false;
    g_irq_boundary_allowance[cpu] = cycles;
    return true;
}

bool irq_latency_freeze_request(void)
{
    if (!irq_is_enabled() || !__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return false;
    return !__atomic_exchange_n(&g_irq_freeze_requested, 1, __ATOMIC_ACQ_REL);
}

bool irq_latency_terminal_read(uint32_t cpu, const irq_latency_snapshot_t **snapshot,
    const irq_latency_witness_t **failures, uint64_t *cutoff, uint64_t *snapshot_cycles)
{
    if (cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS || !snapshot || !failures || !cutoff ||
        !snapshot_cycles || !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    *snapshot = &g_irq_terminal[cpu]; *failures = g_irq_latency[cpu].failures;
    *cutoff = g_irq_cutoff[cpu]; *snapshot_cycles = g_irq_snapshot_cycles[cpu];
    return true;
}

void irq_latency_boundary_counts(uint64_t *early, uint64_t *invalid)
{
    if (early) *early = __atomic_load_n(&g_irq_early_boundaries, __ATOMIC_ACQUIRE);
    if (invalid) *invalid = __atomic_load_n(&g_irq_invalid_boundaries, __ATOMIC_ACQUIRE);
}

bool irq_latency_nested_elisions(uint32_t cpu, uint64_t *out)
{
    if (!out || cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS ||
        !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    *out = g_irq_nested_elided[cpu].count;
    return true;
}

bool irq_latency_mmio_reuses(uint32_t cpu, uint64_t *out)
{
    if (!out || cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS ||
        !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    *out = g_irq_nested_elided[cpu].mmio_reused;
    return true;
}

bool irq_latency_nested_prechecks(uint32_t cpu, uint64_t *out)
{
    if (cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS || !out ||
        !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    *out = g_irq_nested_elided[cpu].prechecked;
    return true;
}

/* Diagnostic observation of a CPU which did not reach a terminal boundary.
 * A one-attempt read may fail; it never manufactures a frozen snapshot. */
bool irq_latency_freeze_diagnostic(uint32_t cpu, uint64_t out[14])
{
    if (cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS || !out) return false;
    uint64_t before = __atomic_load_n(&g_irq_freeze_probe[cpu][0], __ATOMIC_ACQUIRE);
    if (!before || (before & 1)) return false;
    for (unsigned i = 0; i < 14; ++i)
        out[i] = __atomic_load_n(&g_irq_freeze_probe[cpu][i], __ATOMIC_ACQUIRE);
    return before == __atomic_load_n(&g_irq_freeze_probe[cpu][0], __ATOMIC_ACQUIRE);
}

uint64_t irq_latency_invalid_site(unsigned site)
{
    return site < 32 ? __atomic_load_n(&g_irq_invalid_sites[site], __ATOMIC_ACQUIRE) : 0;
}

bool irq_latency_owner_failure(uint32_t cpu, uint64_t out[4])
{
    if (cpu >= g_cpu_count || !out || !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    for (unsigned i = 0; i < 4; ++i) out[i] = g_irq_owner_failure[cpu][i];
    return out[0] != 0;
}

irq_latency_callback_token_t irq_latency_callback_begin(uint32_t operation, uint64_t generation, uint64_t root)
{
    irq_latency_callback_token_t token = {0};
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return token;
    if (irq_is_enabled() || !masked_slot_now(&token.cpu) || !operation || operation > 4) {
        boundary_failure(26); return token;
    }
    if (__atomic_load_n(&g_irq_frozen[token.cpu], __ATOMIC_ACQUIRE)) return token;
    token.operation = operation; token.generation = generation; token.root = root;
    token.switches = g_irq_switch_sequence[token.cpu];
    token.context = g_irq_latency[token.cpu].context;
    token.active = true;
    token.begin = irq_latency_read_tsc();
    return token;
}

void irq_latency_callback_end(irq_latency_callback_token_t token, bool success)
{
    uint64_t end = irq_latency_read_tsc();
    if (!token.active) return;
    uint32_t cpu;
    bool valid = !irq_is_enabled() && masked_slot_now(&cpu) && cpu == token.cpu &&
        g_irq_switch_sequence[cpu] == token.switches;
    if (!valid || !irq_latency_component_add(&g_irq_callbacks[token.cpu][token.operation-1],
            token.begin, end, token.generation, token.root, token.context, valid, success))
        boundary_failure(27);
}

void irq_latency_request_result(uint32_t operation, uint64_t generation, uint64_t root,
    uint64_t begin_ns, uint64_t end_ns, bool success)
{
    if (!__atomic_load_n(&g_irq_ready, __ATOMIC_ACQUIRE)) return;
    uint64_t flags = irq_save();
    uint32_t cpu;
    if (!masked_slot_now(&cpu) || !operation || operation > 4) boundary_failure(28);
    else if (!__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE) &&
        !irq_latency_component_add(&g_irq_requests[cpu][operation-1], begin_ns, end_ns,
            generation, root, IRQ_LATENCY_THREAD, (flags & IRQ_LATENCY_IF) != 0, success))
        boundary_failure(29);
    irq_restore(flags);
}

bool irq_latency_components(uint32_t cpu, uint32_t operation,
    irq_latency_component_t *callback, irq_latency_component_t *request)
{
    if (cpu >= g_cpu_count || !operation || operation > 4 || !callback || !request ||
        !__atomic_load_n(&g_irq_frozen[cpu], __ATOMIC_ACQUIRE)) return false;
    *callback = g_irq_callbacks[cpu][operation-1];
    *request = g_irq_requests[cpu][operation-1];
    return true;
}

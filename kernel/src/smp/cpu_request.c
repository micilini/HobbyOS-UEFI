#include "../core/irq_latency.h"
#include "cpu_request.h"
#include "smp_boot.h"
#include "smp_topology.h"
#include "../apic/lapic.h"
#include "../core/clock.h"
#include "../core/interrupt_context.h"
#include "../core/irq_bootstrap.h"
#include "../core/panic.h"
#include "../core/runtime_ready.h"
#include "../core/scheduler.h"
#include "../drivers/serial.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "../memory/kernel_tlb.h"
#if CPU_REQUEST_TIMING_ENABLED
#include "../core/irq_latency_tsc.h"
#include "../cpu/cpu.h"
#endif

extern volatile int g_panic_in_progress;
static cpu_request_storage_t g_request;
static uint64_t g_request_root;
static uint8_t g_request_initialized, g_mapping_ready;
#if CPU_REQUEST_TIMING_ENABLED
static uint8_t g_tsc_capability[HOBBYOS_MAX_CPUS];
static cpu_request_send_cycles_t g_send_cycles[HOBBYOS_MAX_CPUS];
static uint64_t timing_epoch(uint32_t cpu)
{
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    return irq_latency_request_epoch(cpu);
#else
    (void)cpu; return 0;
#endif
}
static uint32_t timing_prepare(uint32_t cpu, uint32_t apic)
{
    if (irq_is_enabled()) return CPU_REQUEST_TIMING_CONTEXT;
    if (cpu >= g_cpu_count || cpu >= HOBBYOS_MAX_CPUS ||
        g_cpus[cpu].slot != cpu || g_cpus[cpu].apic_id != apic)
        return CPU_REQUEST_TIMING_IDENTITY;
    if (!g_tsc_capability[cpu]) {
        bool available=irq_latency_tsc_available();
        g_tsc_capability[cpu]=available ? 2 : 1;
    }
    return g_tsc_capability[cpu]==2 ? CPU_REQUEST_TIMING_INCOMPLETE : CPU_REQUEST_TIMING_NO_TSC;
}
#endif
#ifdef HOBBYOS_SELFTEST
/* Diagnostic storage never overwrites a generation. Only the serialized
 * initiator writes it, before releasing request ownership; IRQs never log. */
static cpu_request_audit_t g_audit[CPU_REQUEST_AUDIT_CAPACITY];
static uint64_t g_audit_admitted, g_audit_completed;
static bool g_audit_overflow;
static cpu_request_timing_t g_timing_audit[CPU_REQUEST_AUDIT_CAPACITY][HOBBYOS_MAX_CPUS];
bool cpu_request_audit_snapshot(uint64_t *admitted, uint64_t *completed)
{
    if (!admitted || !completed || __atomic_load_n(&g_audit_overflow, __ATOMIC_ACQUIRE)) return false;
    *completed = __atomic_load_n(&g_audit_completed, __ATOMIC_ACQUIRE);
    *admitted = __atomic_load_n(&g_audit_admitted, __ATOMIC_ACQUIRE);
    return *completed <= *admitted;
}
bool cpu_request_audit_copy(uint64_t generation, cpu_request_audit_t *out)
{
    if (!out || !generation || generation > CPU_REQUEST_AUDIT_CAPACITY ||
        generation > __atomic_load_n(&g_audit_completed, __ATOMIC_ACQUIRE)) return false;
    *out = g_audit[generation-1]; return out->spec.generation == generation && out->result.generation == generation;
}
bool cpu_request_timing_copy(uint64_t generation, uint32_t target, cpu_request_timing_t *out)
{
    if (!out || target >= HOBBYOS_MAX_CPUS || !generation || generation > CPU_REQUEST_AUDIT_CAPACITY ||
        generation > __atomic_load_n(&g_audit_completed,__ATOMIC_ACQUIRE) ||
        !(g_audit[generation-1].spec.targets & (1u << target))) return false;
    *out=g_timing_audit[generation-1][target]; return true;
}
bool cpu_request_timing_format(char *out, size_t capacity, uint64_t generation,
    uint64_t root, uint32_t target, const cpu_request_timing_t *timing)
{
    if (!out || !timing || !irq_is_enabled()) return false;
    const cpu_request_local_cycles_t *l=&timing->local;
    const cpu_request_send_cycles_t *s=&timing->send;
#define T64(v) ((unsigned long long)(v))
    int n=ksnprintf(out,capacity,
        "%llu:%u:%llu:%llu:%llu:%u:%u:%llu:%llu:%u:%llu:%llu:%llu:%llu:%llu:%llu:%u:%u:%llu:%llu:%u:%llu:%llu:%llu:%llu",
        T64(generation),target,T64(root),T64(l->generation),T64(l->root),l->cpu,l->apic,T64(l->epoch),T64(l->end_epoch),l->status,
        T64(l->begin_flags),T64(l->end_flags),T64(l->handler_begin),T64(l->operation_begin),
        T64(l->operation_end),T64(l->handler_end),s->cpu,s->apic,T64(s->epoch),T64(s->end_epoch),s->status,
        T64(s->begin_flags),T64(s->end_flags),T64(s->begin),T64(s->end));
#undef T64
    return n>0 && (size_t)n<capacity;
}
static void audit_publish(bool mapping)
{
    uint64_t generation = g_request.spec.generation;
    if (!generation || generation > CPU_REQUEST_AUDIT_CAPACITY || generation != g_audit_admitted+1) {
        __atomic_store_n(&g_audit_overflow, true, __ATOMIC_RELEASE); return;
    }
    g_audit[generation-1].spec = g_request.spec;
    g_audit[generation-1].mapping = mapping;
    __atomic_store_n(&g_audit_admitted, generation, __ATOMIC_RELEASE);
}
static void audit_complete(const cpu_request_result_t *result)
{
    uint64_t generation = result->generation;
    if (!generation || generation > CPU_REQUEST_AUDIT_CAPACITY || generation != g_audit_completed+1) {
        __atomic_store_n(&g_audit_overflow, true, __ATOMIC_RELEASE); return;
    }
    g_audit[generation-1].result = *result;
    for (unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot) {
        if (!(result->requested & (1u << slot))) continue;
        cpu_request_timing_t *t=&g_timing_audit[generation-1][slot];
        if ((result->acknowledged | result->failed) & (1u << slot)) t->local=g_request.samples[slot].timing.local;
        else t->local.status=CPU_REQUEST_TIMING_INCOMPLETE;
        t->send=g_send_cycles[slot];
    }
    __atomic_store_n(&g_audit_completed, generation, __ATOMIC_RELEASE);
}
static uint32_t g_delayed_slot = UINT32_MAX, g_delayed_entries;
bool cpu_request_test_delay(uint32_t slot)
{
    if (!cpu_request_context_allowed() || slot >= g_cpu_count ||
        __atomic_load_n(&g_request.active, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&g_delayed_entries, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_delayed_slot, slot, __ATOMIC_RELEASE);
    return true;
}
uint32_t cpu_request_test_delayed_entries(void)
{ return __atomic_load_n(&g_delayed_entries, __ATOMIC_ACQUIRE); }
void cpu_request_test_release_delay(void)
{ __atomic_store_n(&g_delayed_slot, UINT32_MAX, __ATOMIC_RELEASE); }
#endif
/* A stalled clock is a separate failure, not a claimed elapsed-time interval. */
#define REQUEST_STALLED_SAMPLE_LIMIT 1000000u

bool cpu_request_runtime_init(uint64_t root)
{
    if (g_request_initialized || !root || (root & 4095) || irq_is_enabled() ||
        irq_bootstrap_state() != IRQ_BOOTSTRAP_CPUS_PREPARED ||
        smp_online_cpu_count() != g_cpu_count || smp_failed_cpu_count()) return false;
    g_request_root = root;
    __atomic_store_n(&g_request_initialized, 1, __ATOMIC_RELEASE);
    return true;
}

static bool context_allowed(bool mapping)
{
    irq_flags_t flags = irq_save();
    cpu_slot_t slot;
    interrupt_cpu_snapshot_t irq;
    bool identified = smp_current_cpu_slot(&slot) &&
        interrupt_context_snapshot(slot, &irq);
    bool valid = cpu_request_context_valid((flags & 512u) != 0,
        identified ? irq.depth : UINT32_MAX,
        scheduler_current_context_can_block(),
        __atomic_load_n(&g_request_initialized, __ATOMIC_ACQUIRE) &&
        (irq_bootstrap_services_are_ready() ||
         (mapping && __atomic_load_n(&g_mapping_ready, __ATOMIC_ACQUIRE))) && clock_monotonic_is_ready(),
        __atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE),
        identified && irq.preempt_epilogue);
    irq_restore(flags);
    return valid;
}

bool cpu_request_context_allowed(void) { return context_allowed(false); }
bool cpu_request_mapping_context_allowed(void) { return context_allowed(true); }

bool cpu_request_mapping_enable(void)
{
    if (!irq_is_enabled() || !g_request_initialized || g_mapping_ready ||
        irq_bootstrap_state() != IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE || !clock_monotonic_is_ready() ||
        smp_online_cpu_count() != g_cpu_count || smp_failed_cpu_count()) return false;
    for (unsigned slot = 0; slot < g_cpu_count; ++slot)
        if (!irq_bootstrap_cpu_irq_ready(slot)) return false;
    __atomic_store_n(&g_mapping_ready, 1, __ATOMIC_RELEASE);
    return true;
}

static bool execute_local(void *context, const cpu_request_spec_t *spec,
                           cpu_request_sample_t *sample)
{
    (void)context;
    if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) return false;
#if CPU_REQUEST_TIMING_ENABLED
    cpu_request_local_cycles_t *t=&sample->timing.local;
    *t=(cpu_request_local_cycles_t){.generation=spec->generation,.root=spec->root,
        .epoch=timing_epoch(sample->slot),.cpu=sample->slot,.apic=sample->apic,
        .begin_flags=irq_is_enabled() ? 512u : 0u};
    t->status=timing_prepare(sample->slot,sample->apic);
    if (t->status==CPU_REQUEST_TIMING_INCOMPLETE) t->handler_begin=irq_latency_read_tsc();
#endif
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    irq_latency_callback_token_t timing = irq_latency_callback_begin(spec->operation, spec->generation, spec->root);
#endif
    bool success = kernel_tlb_execute_local(spec, sample);
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    irq_latency_callback_end(timing, success);
#endif
#if CPU_REQUEST_TIMING_ENABLED
    if (t->status==CPU_REQUEST_TIMING_INCOMPLETE) t->handler_end=irq_latency_read_tsc();
    t->end_flags=irq_is_enabled() ? 512u : 0u;
    t->end_epoch=timing_epoch(sample->slot);
    if (t->status==CPU_REQUEST_TIMING_INCOMPLETE) {
        t->status=cpu_request_local_cycles_check(t,spec->generation,spec->root,
            t->end_epoch,sample->slot,sample->apic);
    }
#endif
    return success;
}

void cpu_request_irq(void)
{
    cpu_slot_t slot;
    if (smp_current_cpu_slot(&slot)) {
#ifdef HOBBYOS_SELFTEST
        if (slot == __atomic_load_n(&g_delayed_slot, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&g_delayed_entries, 1, __ATOMIC_RELEASE);
            return;
        }
#endif
        (void)cpu_request_handle(&g_request, slot, lapic_get_id(), execute_local, NULL);
    }
}

static void failure_record(const cpu_request_spec_t *spec,
                            const cpu_request_result_t *result)
{
    char line[384];
    int n = ksnprintf(line, sizeof(line),
        "[CPU_REQUEST][FAIL] generation=%llu operation=%u first=0x%llx last=0x%llx "
        "root=0x%llx expected=0x%x acknowledged=0x%x missing=0x%x failed=0x%x send_failed=0x%x status=%u\n",
        (unsigned long long)result->generation, (unsigned)spec->operation,
        (unsigned long long)spec->interval.first, (unsigned long long)spec->interval.last,
        (unsigned long long)spec->root, result->requested, result->acknowledged,
        result->missing, result->failed, result->send_failed, (unsigned)result->status);
    if (n > 0 && (size_t)n < sizeof(line)) serial_write_all(line);
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
        if (!((result->missing | result->failed | result->send_failed) & (1u << slot))) continue;
        n = ksnprintf(line, sizeof(line),
            "[CPU_REQUEST][TARGET_FAILURE] generation=%llu slot=%u apic=%u\n",
            (unsigned long long)result->generation, slot, spec->apic[slot]);
        if (n > 0 && (size_t)n < sizeof(line)) serial_write_all(line);
    }
}

static cpu_request_result_t run(cpu_request_operation_t operation,
    const tlb_interval_t *interval, cpu_request_sample_t *samples, uint32_t capacity, bool mapping)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN);
    cpu_request_result_t result = {.status = CPU_REQUEST_INVALID, .timing_schema=CPU_REQUEST_TIMING_SCHEMA};
    if (!tlb_interval_valid(interval) || (samples && capacity < HOBBYOS_MAX_CPUS)) return result;
    if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) {
        result.status = CPU_REQUEST_PANIC; return result;
    }
    bool context = context_allowed(mapping);
    KBUG_ON(!context);
    if (!context) { result.status = CPU_REQUEST_CONTEXT; return result; }
    if (operation == CPU_REQUEST_INVALIDATE && !interval->pages) {
        result.status = CPU_REQUEST_OK; return result;
    }
    uint64_t started = 0;
    bool clock_valid = clock_monotonic_read_ns(&started);
    uint64_t previous = started;
    if (!clock_valid || !started || started > UINT64_MAX - CPU_REQUEST_DEADLINE_NS) {
        result.status = CPU_REQUEST_UNAVAILABLE; return result;
    }
    uint64_t deadline = started + CPU_REQUEST_DEADLINE_NS;
    uint32_t stalled = 0;
    while (!cpu_request_admit(&g_request)) {
        if (result.admission_spins != UINT32_MAX) ++result.admission_spins;
        if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) {
            result.status = CPU_REQUEST_PANIC; return result;
        }
        uint64_t now = 0;
        bool clock_valid = clock_monotonic_read_ns(&now);
        stalled = now == previous ? stalled + 1 : 0;
        if (!clock_valid || !now || now < previous || now >= deadline || stalled >= REQUEST_STALLED_SAMPLE_LIMIT) {
            result.status = CPU_REQUEST_BUSY; return result;
        }
        previous = now;
        __asm__ volatile("pause" ::: "memory");
    }
    cpu_request_spec_t spec = {.root = g_request_root, .interval = *interval, .operation = operation};
    irq_flags_t flags = irq_save();
    bool topology = g_cpu_count && g_cpu_count <= HOBBYOS_MAX_CPUS;
    for (unsigned slot = 0; topology && slot < g_cpu_count; ++slot) {
        const SmpCpuInfo *cpu = &g_cpus[slot];
        if (cpu->slot != slot || __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) != CPU_STATE_ONLINE)
            topology = false;
        else { spec.targets |= 1u << slot; spec.apic[slot] = cpu->apic_id; }
    }
    irq_restore(flags);
    if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) result.status = CPU_REQUEST_PANIC;
    else if (!topology) result.status = CPU_REQUEST_UNAVAILABLE;
    else result.status = cpu_request_publish(&g_request, &spec);
    if (result.status != CPU_REQUEST_OK) {
        __atomic_store_n(&g_request.owner, 0, __ATOMIC_RELEASE);
        return result;
    }
#ifdef HOBBYOS_SELFTEST
    audit_publish(mapping);
#endif
#if CPU_REQUEST_TIMING_ENABLED
    memset(g_send_cycles,0,sizeof(g_send_cycles));
    for (unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot)
        if (spec.targets & (1u << slot)) g_send_cycles[slot].status=CPU_REQUEST_TIMING_INCOMPLETE;
#endif
    /* Each iteration pins only identity plus local execution/sending. Migration
     * between iterations cannot remove any CPU from the immutable target set. */
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
        if (!(spec.targets & (1u << slot))) continue;
        flags = irq_save();
        if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) {
            irq_restore(flags); result.status = CPU_REQUEST_PANIC; break;
        }
        uint32_t actor_apic=lapic_get_id();
#if CPU_REQUEST_TIMING_ENABLED
        cpu_slot_t actor=CPU_SLOT_INVALID;
        cpu_request_send_cycles_t *send=&g_send_cycles[slot];
        bool identified=smp_cpu_slot_from_apic_id(actor_apic,&actor);
        *send=(cpu_request_send_cycles_t){.cpu=actor,.apic=actor_apic,
            .epoch=identified ? timing_epoch(actor) : 0,.begin_flags=irq_is_enabled() ? 512u : 0u};
        send->status=identified ? timing_prepare(actor,actor_apic) : CPU_REQUEST_TIMING_IDENTITY;
        if (send->status==CPU_REQUEST_TIMING_INCOMPLETE) send->begin=irq_latency_read_tsc();
#endif
        if (actor_apic == spec.apic[slot]) cpu_request_irq();
        else {
            result.sent |= 1u << slot;
            ++result.notifications;
            if (lapic_send_ipi(spec.apic[slot], INT_VECTOR_CPU_REQUEST) != LAPIC_DELIVERY_SUBMITTED)
                result.send_failed |= 1u << slot;
        }
#if CPU_REQUEST_TIMING_ENABLED
        if (send->status==CPU_REQUEST_TIMING_INCOMPLETE) send->end=irq_latency_read_tsc();
        send->end_flags=irq_is_enabled() ? 512u : 0u;
        send->end_epoch=timing_epoch(actor);
        if (send->status==CPU_REQUEST_TIMING_INCOMPLETE) {
            send->status=cpu_request_send_cycles_check(send,send->end_epoch,actor,actor_apic);
        }
#endif
        irq_restore(flags);
    }
    for (;;) {
        cpu_request_collect(&g_request, &result);
        if (__atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) {
            result.status = CPU_REQUEST_PANIC; break;
        }
        uint64_t now = 0;
        bool clock_valid = clock_monotonic_read_ns(&now);
        stalled = now == previous ? stalled + 1 : 0;
        if (!clock_valid || !now || now < previous || now >= deadline || stalled >= REQUEST_STALLED_SAMPLE_LIMIT) {
            result.status = CPU_REQUEST_TIMEOUT; break;
        }
        previous = now;
        if (!result.missing) {
            result.status = result.send_failed ? CPU_REQUEST_SEND_FAILED :
                result.failed ? CPU_REQUEST_LOCAL_FAILED : CPU_REQUEST_OK;
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    uint64_t ended = 0;
    bool ended_valid = clock_monotonic_read_ns(&ended);
    result.elapsed_ns = ended_valid && ended > started ? ended - started : UINT64_MAX;
    if (result.status == CPU_REQUEST_OK && (!ended_valid || ended <= started || ended < previous || ended >= deadline))
        result.status = CPU_REQUEST_TIMEOUT;
    if (samples && result.status == CPU_REQUEST_OK)
        for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
            samples[slot] = g_request.samples[slot];
#if CPU_REQUEST_TIMING_ENABLED
            samples[slot].timing.send=g_send_cycles[slot];
#endif
        }
    /* Copy diagnostic operands before releasing admission; late handlers retain
     * the original storage after failure. No ordinary lock spans this wait. */
    spec = g_request.spec;
#ifdef HOBBYOS_SELFTEST
    audit_complete(&result);
#endif
    (void)cpu_request_retire(&g_request, result.status == CPU_REQUEST_OK);
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    irq_latency_request_result(operation, result.generation, spec.root, started, ended, result.status == CPU_REQUEST_OK);
#endif
    if (result.status != CPU_REQUEST_OK && result.status != CPU_REQUEST_PANIC)
        failure_record(&spec, &result);
    return result;
}

cpu_request_result_t cpu_request_run(cpu_request_operation_t operation,
                                     const tlb_interval_t *interval)
{ return run(operation, interval, NULL, 0, false); }
#ifdef HOBBYOS_SELFTEST
cpu_request_result_t cpu_request_run_observed(cpu_request_operation_t operation,
    const tlb_interval_t *interval, cpu_request_sample_t *out, uint32_t capacity)
{ return run(operation, interval, out, capacity, false); }
#endif

cpu_request_result_t cpu_request_mapping_run(cpu_request_operation_t operation,
                                             const tlb_interval_t *interval)
{
    if (operation != CPU_REQUEST_INVALIDATE && operation != CPU_REQUEST_CACHE_WRITEBACK)
        return (cpu_request_result_t){.status = CPU_REQUEST_INVALID};
    return run(operation, interval, NULL, 0, true);
}

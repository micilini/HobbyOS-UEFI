#include "../core/irq_latency.h"
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
#include "../core/irq_latency_interior.h"
#endif
#include "kernel_tlb.h"
#include "paging_private.h"
#include "../core/idt.h"
#include "../core/semaphore.h"
#include "../core/scheduler.h"
#include "../core/panic.h"
#include "../core/interrupt_context.h"
#include "../core/irq_bootstrap.h"
#if CPU_REQUEST_TIMING_ENABLED
#include "../core/irq_latency_tsc.h"
#endif
#ifdef HOBBYOS_SELFTEST
#include "tlb_fixture.h"
#include "protection_probe.h"
#endif

extern PageTable *g_kernel_pml4;
extern volatile int g_panic_in_progress;
static uint8_t g_bootstrap_closed, g_worker_ready;
static tlb_deferred_queue_t g_deferred;
static semaphore_t g_deferred_wake;
static uint32_t g_deferred_wake_pending;
#ifdef HOBBYOS_SELFTEST
static cpu_request_sample_t g_deferred_samples[TLB_DEFERRED_CAPACITY][HOBBYOS_MAX_CPUS];
bool kernel_tlb_deferred_samples(tlb_completion_t handle, cpu_request_sample_t *out,
                                 uint32_t capacity)
{
    cpu_request_result_t result;
    if (!out || capacity < HOBBYOS_MAX_CPUS ||
        kernel_tlb_poll(handle, &result) != TLB_DEFERRED_COMPLETE) return false;
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot)
        out[slot] = g_deferred_samples[handle.slot][slot];
    return true;
}
#endif

static uint64_t read_cr3(void *context)
{ (void)context; uint64_t value; __asm__ volatile("mov %%cr3, %0" : "=r"(value) :: "memory"); return value; }
static uint64_t read_cr4(void *context)
{ (void)context; uint64_t value; __asm__ volatile("mov %%cr4, %0" : "=r"(value) :: "memory"); return value; }
static void invalidate_page(void *context, uint64_t address)
{ (void)context; __asm__ volatile("invlpg (%0)" :: "r"(address) : "memory"); }
static void reload_cr3(void *context, uint64_t root)
{ (void)context; __asm__ volatile("mov %0, %%cr3" :: "r"(root) : "memory"); }
static const tlb_local_ops_t g_local_ops = {read_cr3, read_cr4, invalidate_page, reload_cr3};

bool kernel_tlb_execute_local(const cpu_request_spec_t *spec,
                              cpu_request_sample_t *sample)
{
    if (!spec || !sample) return false;
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    if (spec->operation != CPU_REQUEST_INVALIDATE && spec->operation != CPU_REQUEST_CACHE_WRITEBACK)
        irq_latency_test_probe_role();
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_SHOOTDOWN) |
        (spec->operation == CPU_REQUEST_CACHE_WRITEBACK ? (UINT64_C(1) << IRQ_PATH_CACHE_WRITEBACK) : 0));
#endif
    sample->cr3 = read_cr3(NULL);
    if (spec->root != paging_kernel_root_physical()) return false;
#if CPU_REQUEST_TIMING_ENABLED
    if (sample->timing.local.status==CPU_REQUEST_TIMING_INCOMPLETE)
        sample->timing.local.operation_begin=irq_latency_read_tsc();
#endif
    bool success = false;
    if (spec->operation == CPU_REQUEST_CACHE_WRITEBACK && read_cr4(NULL) == 0x20 &&
        (sample->cr3 & ~UINT64_C(0x18)) == spec->root) {
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
        irq_latency_writeback_token_t token=irq_latency_writeback_begin(spec->generation,spec->root);
        irq_latency_writeback_stamps_t stamps;
        irq_latency_writeback_execute(&stamps);
        irq_latency_writeback_end(token,&stamps);
#else
        __asm__ volatile("wbinvd" ::: "memory");
#endif
        success = true;
    }
    else if (spec->operation == CPU_REQUEST_INVALIDATE)
        success = tlb_local_execute(&spec->interval, spec->root, &g_local_ops, NULL);
#ifdef HOBBYOS_SELFTEST
    else if (spec->operation == CPU_REQUEST_SAMPLE && read_cr4(NULL) == 0x20 &&
             (sample->cr3 & ~UINT64_C(0x18)) == spec->root)
        success = tlb_fixture_load(&spec->interval, sample);
    else if (spec->operation==CPU_REQUEST_PROTECTION && read_cr4(NULL)==0x20 && sample->cr3==spec->root)
        success=protection_probe_execute(spec,sample);
#endif
#if CPU_REQUEST_TIMING_ENABLED
    if (sample->timing.local.status==CPU_REQUEST_TIMING_INCOMPLETE)
        sample->timing.local.operation_end=irq_latency_read_tsc();
#endif
    return success;
}

bool kernel_tlb_bootstrap_invalidate(uint64_t start, uint64_t length)
{
    tlb_interval_t interval;
    if (g_bootstrap_closed || irq_is_enabled() || !g_kernel_pml4 ||
        !tlb_interval_normalize(start, length, &interval)) return false;
    return tlb_local_execute(&interval, paging_kernel_root_physical(), &g_local_ops, NULL);
}

void kernel_tlb_close_bootstrap(void)
{ __atomic_store_n(&g_bootstrap_closed, 1, __ATOMIC_RELEASE); }

cpu_request_result_t kernel_tlb_invalidate(uint64_t start, uint64_t length)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN);
    tlb_interval_t interval;
    if (!tlb_interval_normalize(start, length, &interval))
        return (cpu_request_result_t){.status = CPU_REQUEST_INVALID};
    return cpu_request_run(CPU_REQUEST_INVALIDATE, &interval);
}

static void deferred_worker(void *unused)
{
    (void)unused;
    for (;;) {
        sem_wait(&g_deferred_wake);
        /* Clear before scanning: publication after a passed slot schedules the
         * next sweep, while earlier publication is visible in this sweep. */
        __atomic_store_n(&g_deferred_wake_pending, 0, __ATOMIC_RELEASE);
        for (unsigned slot = 0; slot < TLB_DEFERRED_CAPACITY; ++slot) {
            tlb_completion_t handle;
            tlb_interval_t interval;
            if (!tlb_deferred_claim(&g_deferred, slot, &handle, &interval)) continue;
            cpu_request_result_t result;
#ifdef HOBBYOS_SELFTEST
            result = cpu_request_run_observed(CPU_REQUEST_INVALIDATE, &interval,
                g_deferred_samples[slot], HOBBYOS_MAX_CPUS);
#else
            result = cpu_request_run(CPU_REQUEST_INVALIDATE, &interval);
#endif
            if (!tlb_deferred_finish(&g_deferred, handle, &result))
                kpanic("TLB deferred completion ownership failure");
        }
    }
}

bool kernel_tlb_runtime_init(void)
{
    if (!g_bootstrap_closed || g_worker_ready ||
        !cpu_request_runtime_init(paging_kernel_root_physical())) return false;
    sem_init(&g_deferred_wake, 0);
    if (!thread_create_named_with_class_flags(deferred_worker, NULL,
        TASK_CLASS_NORMAL, "memory-invalidation", TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED)) return false;
    __atomic_store_n(&g_worker_ready, 1, __ATOMIC_RELEASE);
    return true;
}

tlb_deferred_status_t kernel_tlb_defer(uint64_t start, uint64_t length,
                                     tlb_completion_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN);
    tlb_interval_t interval;
    if (!out || !tlb_interval_normalize(start, length, &interval) ||
        !__atomic_load_n(&g_worker_ready, __ATOMIC_ACQUIRE) || !irq_bootstrap_services_are_ready() ||
        __atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE)) return TLB_DEFERRED_INVALID;
    irq_flags_t flags = irq_save();
    cpu_slot_t slot;
    interrupt_cpu_snapshot_t context;
    bool eligible = smp_current_cpu_slot(&slot) && interrupt_context_snapshot(slot, &context) &&
        context.depth <= 1 && !context.preempt_epilogue;
    tlb_deferred_status_t status = eligible ?
        tlb_deferred_enqueue(&g_deferred, &interval, out) : TLB_DEFERRED_INVALID;
    irq_restore(flags);
    if (status == TLB_DEFERRED_QUEUED &&
        !__atomic_exchange_n(&g_deferred_wake_pending, 1, __ATOMIC_ACQ_REL) &&
        !sem_signal(&g_deferred_wake))
        kpanic("TLB deferred worker notification failure");
    return status;
}

tlb_deferred_status_t kernel_tlb_poll(tlb_completion_t handle,
                                     cpu_request_result_t *out)
{ return tlb_deferred_poll(&g_deferred, handle, out); }
bool kernel_tlb_release(tlb_completion_t handle)
{ return tlb_deferred_release(&g_deferred, handle); }

bool kernel_cache_bootstrap_writeback(void)
{
    if (g_bootstrap_closed || irq_is_enabled() || read_cr4(NULL) != 0x20 ||
        (read_cr3(NULL) & ~UINT64_C(0x18)) != paging_kernel_root_physical()) return false;
    __asm__ volatile("wbinvd" ::: "memory");
    return true;
}

cpu_request_result_t kernel_cache_writeback(void)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_SHOOTDOWN) | (UINT64_C(1) << IRQ_PATH_CACHE_WRITEBACK));
    tlb_interval_t interval;
    if (!tlb_interval_normalize(0, 4096, &interval))
        return (cpu_request_result_t){.status = CPU_REQUEST_INVALID};
    return cpu_request_mapping_run(CPU_REQUEST_CACHE_WRITEBACK, &interval);
}

cpu_request_result_t kernel_mapping_invalidate(uint64_t start, uint64_t length)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN);
    tlb_interval_t interval;
    if (!tlb_interval_normalize(start, length, &interval))
        return (cpu_request_result_t){.status = CPU_REQUEST_INVALID};
    return cpu_request_mapping_run(CPU_REQUEST_INVALIDATE, &interval);
}

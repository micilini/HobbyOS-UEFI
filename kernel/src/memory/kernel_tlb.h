#ifndef KERNEL_TLB_H
#define KERNEL_TLB_H
#include "tlb_deferred.h"

/* This kernel shares one root with PGE/PCIDE/LA57 disabled. Synchronous
 * invalidation requires IF=1 and eligible thread context, outside hard IRQ,
 * NMI, panic, CPU pin and every spinlock/resource a target may need while IF=0.
 * Release the paging lock before waiting. An IF check is not general lockdep.
 * No frame/VA reuse is allowed before CPU_REQUEST_OK; failure is not rollback. */
cpu_request_result_t kernel_tlb_invalidate(uint64_t start, uint64_t length);
/* Boot-only local path is permanently sealed before AP startup. */
bool kernel_tlb_bootstrap_invalidate(uint64_t start, uint64_t length);
void kernel_tlb_close_bootstrap(void);
/* Owned first-adoption maintenance only; the caller has withdrawn previous
 * aliases and completed translation invalidation before this operation. */
bool kernel_cache_bootstrap_writeback(void);
cpu_request_result_t kernel_cache_writeback(void);
cpu_request_result_t kernel_mapping_invalidate(uint64_t start, uint64_t length);
bool kernel_tlb_runtime_init(void);

/* Allocation-free bounded submission from thread or maskable IRQ context.
 * Operands are copied. Caller retains all affected resources until COMPLETE.
 * Handles are explicit owned resources; a failed handle cannot be released. */
tlb_deferred_status_t kernel_tlb_defer(uint64_t start, uint64_t length,
                                     tlb_completion_t *out);
tlb_deferred_status_t kernel_tlb_poll(tlb_completion_t handle,
                                     cpu_request_result_t *out);
bool kernel_tlb_release(tlb_completion_t handle);
#ifdef HOBBYOS_SELFTEST
/* The fixture owns this token exclusively until after copying the completed
 * observations; release by another consumer is outside this test contract. */
bool kernel_tlb_deferred_samples(tlb_completion_t handle, cpu_request_sample_t *out,
                                 uint32_t capacity);
#endif

/* Internal handler entry; performs no allocation, scheduler operation or wait. */
bool kernel_tlb_execute_local(const cpu_request_spec_t *spec,
                              cpu_request_sample_t *sample);
#endif

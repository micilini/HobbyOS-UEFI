#ifndef TLB_FIXTURE_H
#define TLB_FIXTURE_H
#include <stdbool.h>
#include <stdint.h>
#include "../smp/cpu_request.h"
#ifdef HOBBYOS_SELFTEST
#define TLB_FIXTURE_WINDOWS 2u
#define TLB_FIXTURE_PAGES 65u
#define TLB_FIXTURE_ROUNDS 256u
#define TLB_FIXTURE_CONCURRENT_ROUNDS 64u
#define TLB_FIXTURE_BASE UINT64_C(0xffffc000001ff000)
#define TLB_FIXTURE_STRIDE UINT64_C(0x40000000)
bool paging_tlb_fixture_prepare(unsigned window, uint64_t frame);
bool paging_tlb_fixture_replace(unsigned window, unsigned pages,
    uint64_t expected, uint64_t replacement, uint64_t *old_entry, uint64_t *new_entry);
bool paging_tlb_fixture_clear(unsigned window);
uint64_t paging_tlb_fixture_entry(unsigned window);
uint32_t paging_tlb_fixture_tables(void);
bool tlb_fixture_load(const tlb_interval_t *interval, cpu_request_sample_t *sample);
int tlb_fixture_run(void);
bool tlb_fixture_proof_prepare(bool deferred, uint64_t session, unsigned invocations);
bool tlb_fixture_proof_export(uint64_t service_generation);
bool tlb_fixture_proof_configured(void);
int tlb_fixture_context_failure(void);
int tlb_fixture_delayed_response(void);
#endif
#endif

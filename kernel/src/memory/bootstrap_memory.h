#ifndef BOOTSTRAP_MEMORY_H
#define BOOTSTRAP_MEMORY_H
#include <stdbool.h>
#include <stdint.h>
#include "../smp/cpu_limits.h"
enum { BOOTSTRAP_TRAMPOLINE_OWNER = 1 };

typedef struct {
    uint64_t physical, bytes, generation, origin_first, origin_end;
    uint32_t origin_type, owner;
} bootstrap_claim_t;
typedef struct {
    uint64_t previous_root, root, rsp, gdtr, idtr, tss, ist1, ist2, ist3;
    uint64_t stack_first, stack_bytes, generation;
    uint32_t apic_id, slot;
    uint16_t cs, tr;
} bootstrap_departure_t;
enum { BOOTSTRAP_NEW, BOOTSTRAP_ACTIVE, BOOTSTRAP_SEALED,
       BOOTSTRAP_RETIRED, BOOTSTRAP_RETAINED, BOOTSTRAP_NOT_NEEDED };
typedef struct {
    uint64_t generation, root, main_root, intended;
    uint32_t state, count;
    struct {
        uint32_t apic_id, started, completed;
        bootstrap_departure_t departure;
    } cpus[HOBBYOS_MAX_CPUS];
} bootstrap_lifetime_t;

bool bootstrap_lifetime_begin(bootstrap_lifetime_t *life, uint64_t generation,
    uint64_t root, uint64_t main_root, uint64_t intended, const uint32_t *apic_ids, unsigned count);
bool bootstrap_lifetime_start(bootstrap_lifetime_t *life, unsigned slot, uint32_t apic_id, uint64_t generation);
bool bootstrap_lifetime_retry(const bootstrap_lifetime_t *life, unsigned slot, uint32_t apic_id, uint64_t generation);
bool bootstrap_lifetime_depart(bootstrap_lifetime_t *life, const bootstrap_departure_t *departure);
bool bootstrap_lifetime_seal(bootstrap_lifetime_t *life, uint64_t bsp_root);
bool bootstrap_lifetime_finish(bootstrap_lifetime_t *life);
bool bootstrap_lifetime_not_needed(bootstrap_lifetime_t *life, uint64_t main_root, unsigned cpu_count);

bool pmm_claim_bootstrap(bootstrap_claim_t *out);
bool paging_bootstrap_create(const bootstrap_claim_t *claim, uint64_t generation,
    uint64_t intended, const uint32_t *apic_ids, unsigned count, uint64_t *root);
bool paging_bootstrap_start(unsigned slot, uint32_t apic_id, uint64_t generation);
bool paging_bootstrap_retry(unsigned slot, uint32_t apic_id, uint64_t generation);
bool paging_bootstrap_handoff_ready(const bootstrap_departure_t *departure);
bool paging_bootstrap_depart(const bootstrap_departure_t *departure);
bool paging_bootstrap_retire(volatile uint64_t *trampoline_root);
void paging_bootstrap_not_needed(void);
const bootstrap_lifetime_t *paging_bootstrap_lifetime(void);
#ifdef HOBBYOS_SELFTEST
bool paging_bootstrap_snapshot_entry(unsigned index, uint64_t *physical, uint64_t *hash);
#endif
#endif

#ifndef BOOTSTRAP_CLAIM_H
#define BOOTSTRAP_CLAIM_H
#include "bootstrap_memory.h"
#include "physical_internal.h"
#include "boot_address.h"
bool bootstrap_claim_candidate(const physical_contract_t *contract, const boot_physical_objects_t *boot,
    uint64_t bitmap, uint64_t bitmap_bytes, uint64_t physical, bootstrap_claim_t *out);
bool bootstrap_claim_adopt(bootstrap_claim_t *owner, const bootstrap_claim_t *candidate);
#endif

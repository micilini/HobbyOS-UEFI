#ifndef CPU_CONTROL_POLICY_H
#define CPU_CONTROL_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "control_policy_bits.h"

typedef struct {
    uint32_t basic_edx, extended_edx, apic_id;
    bool basic_valid, extended_valid, identity_valid;
} cpu_policy_features_t;

typedef struct {
    uint64_t cr0_protected, cr0_final, cr4, efer_write, pat;
    uint32_t required_basic, required_extended, page1g_supported, reserved;
} cpu_policy_t;

typedef struct {
    uint64_t cr0, cr3, cr4, efer, pat;
    uint32_t apic_id;
} cpu_policy_readback_t;

typedef enum {
    CPU_POLICY_OK, CPU_POLICY_FEATURES, CPU_POLICY_INVALID,
    CPU_POLICY_INCOMING, CPU_POLICY_MAPPING, CPU_POLICY_READ,
    CPU_POLICY_WRITE, CPU_POLICY_READBACK, CPU_POLICY_IDENTITY,
    CPU_POLICY_DUPLICATE
} cpu_policy_status_t;

cpu_policy_status_t cpu_policy_build(const cpu_policy_features_t *features,
                                     cpu_policy_t *out);
cpu_policy_status_t cpu_policy_validate(const cpu_policy_t *policy,
                                        const cpu_policy_features_t *features);
bool cpu_policy_pat_valid(uint64_t pat);
bool cpu_policy_preserves_slots(uint64_t before, uint64_t after, uint8_t used);
bool cpu_policy_bootstrap_map(const uint64_t *frames, size_t bytes,
                              uint64_t physical_root, uint64_t cr3);
cpu_policy_status_t cpu_policy_match(const cpu_policy_t *policy,
                                     const cpu_policy_features_t *features,
                                     const cpu_policy_readback_t *actual,
                                     uint64_t root, uint32_t expected_apic);
cpu_policy_status_t cpu_policy_publish_ready(uint8_t *publication,
                                             const cpu_policy_t *policy,
                                             const cpu_policy_features_t *features,
                                             const cpu_policy_readback_t *actual,
                                             uint64_t root, uint32_t expected_apic);

/* Boot-only, local operation. No AP may use the BSP policy until publication.
 * The caller supplies a previously validated mapping-selector set and root.
 * Entry IF is restored on every return, including failed validation/readback. */
cpu_policy_status_t cpu_policy_apply(const cpu_policy_t *policy,
                                     const cpu_policy_features_t *features,
                                     uint64_t root, uint8_t used_slots,
                                     cpu_policy_readback_t *out);

#ifndef HOBBYOS_CONTROL_POLICY_HOST_TEST
void cpu_policy_init_bsp(void);
const cpu_policy_t *cpu_policy_get(void);
bool cpu_policy_confirm_local(uint32_t expected_apic, uint64_t root,
                              const char *role);
#else
/* Only hosted builds substitute privileged operations; the sequence is shared. */
uint64_t cpu_policy_test_read_cr(unsigned index);
void cpu_policy_test_write_cr(unsigned index, uint64_t value);
bool cpu_policy_test_read_msr(uint32_t msr, uint64_t *value);
bool cpu_policy_test_write_msr(uint32_t msr, uint64_t value);
uint64_t cpu_policy_test_irq_save(void);
void cpu_policy_test_irq_restore(uint64_t flags);
void cpu_policy_test_wbinvd(void);
#endif
#endif

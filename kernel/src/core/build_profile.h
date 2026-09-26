#ifndef BUILD_PROFILE_H
#define BUILD_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t bsp_seen;
    uint8_t bsp_rsp_mod16;
    uint8_t bsp_df;
    uint32_t ap_seen_mask;
    uint32_t ap_bad_rsp_mask;
    uint32_t ap_bad_df_mask;
    uint64_t irq_entries;
    uint64_t irq_df_violations;
    uint64_t irq_probe_entries;
    uint64_t irq_probe_df_violations;
} build_profile_snapshot_t;

void build_profile_record_bsp_entry(uint64_t rsp_mod16, uint64_t df);
void build_profile_record_ap_entry(uint32_t slot, uint64_t rsp_mod16,
                                   uint64_t df);
void build_profile_record_irq_entry(uint8_t vector, uint64_t df);
bool build_profile_snapshot(build_profile_snapshot_t *out);

#endif

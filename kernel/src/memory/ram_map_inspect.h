#ifndef RAM_MAP_INSPECT_H
#define RAM_MAP_INSPECT_H
#include "ram_map_plan.h"
typedef struct { uint64_t virtual_address, physical, bytes, flags; unsigned shift; } ram_leaf_run_t;
typedef bool (*ram_leaf_consumer_t)(void *context, const ram_leaf_run_t *run);
typedef struct {
    const ram_map_plan_t *plan;
    const ram_mtrr_inputs_t *mtrrs;
    uint64_t root, table_pointer, epoch, initial_tables;
} ram_map_snapshot_t;
bool paging_ram_snapshot(ram_map_snapshot_t *out, ram_leaf_consumer_t consumer, void *context);
bool paging_ram_scratch_table(uint64_t table, uint64_t data, uint64_t *cpu, uint64_t *entry);
int ram_map_fixture_run(bool high_memory);
#endif

#ifndef PAGING_INSPECT_H
#define PAGING_INSPECT_H
#include "../cpu/cpu_control_snapshot.h"

typedef struct {
    uint64_t epoch_before, epoch_after, allocations, root;
    uint32_t reachable, extra_roots;
    bool complete, consistent;
} paging_table_count_t;

void paging_inspect(uint64_t va, cpu_control_sample_t *cpu, pt_result_t *out);
bool paging_inspect_tables(paging_table_count_t *out);
uint64_t paging_inspect_initial_allocations(void);
bool paging_inspect_copy_physical(uint64_t address, void *out, uint32_t size);

#endif

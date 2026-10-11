#ifndef BOOTSTRAP_TABLES_H
#define BOOTSTRAP_TABLES_H
#include "paging_build.h"
#include "bootstrap_memory.h"
#define BOOTSTRAP_PRIVATE_TABLES 4u
typedef struct {
    mapping_table_t tables[BOOTSTRAP_PRIVATE_TABLES];
    bootstrap_claim_t claim;
    const paging_build_t *main;
    mapping_table_ops_t ops;
    void *context;
    unsigned count;
    bool verified, published;
} bootstrap_tables_t;
#ifdef HOBBYOS_SELFTEST
typedef struct {
    uint64_t generation, root, physical[BOOTSTRAP_PRIVATE_TABLES];
    PageTable bytes[BOOTSTRAP_PRIVATE_TABLES];
    uint32_t count;
} bootstrap_snapshot_t;
#endif
bool bootstrap_tables_create(bootstrap_tables_t *tables, const paging_build_t *main,
    const bootstrap_claim_t *claim, const mapping_table_ops_t *ops, void *context);
bool bootstrap_tables_verify(bootstrap_tables_t *tables);
bool bootstrap_tables_abort(bootstrap_tables_t *tables);
bool bootstrap_tables_retire(bootstrap_tables_t *tables, const bootstrap_lifetime_t *life);
#endif

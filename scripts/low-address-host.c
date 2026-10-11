#define main direct_map_host_main
#include "direct-map-host.c"
#undef main

int main(void)
{
    initialize();
    const mapping_table_ops_t ops = {allocate, release};
    CHECK(paging_build_create(&build, &contract, &plan, &permissions, NULL, &ops, NULL));
    for (unsigned i = 0; i < 256; ++i)
        CHECK(build.tables[0].table.cpu->entries[i] == 0);
    CHECK(verify());
    static const unsigned slots[] = {0, 127, 255};
    for (unsigned n = 0; n < sizeof(slots)/sizeof(slots[0]); ++n) {
        uint64_t *entry = &build.tables[0].table.cpu->entries[slots[n]];
        reject_mutation(entry, 2);
        reject_mutation(entry, build.tables[1].table.physical | 3);
    }
    uint64_t physical = 77; bool nx = false;
    CHECK(!paging_build_lookup(&build, 0, &physical, &nx) && physical == 77);
    CHECK(!paging_build_lookup(&build, UINT64_C(0x7ffffffff000), &physical, &nx));
    CHECK(paging_build_activate(&build, load, NULL) && loads == 1);
    build.activated = false;
    reset_build();
    printf("LOW_ADDRESS_HOST checks=%u status=PASS\n", checks);
    return 0;
}

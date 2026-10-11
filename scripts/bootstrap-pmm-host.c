#define main direct_transition_host_main
#include "direct-map-transition-host.c"
#undef main
static boot_physical_objects_t boot;
const boot_physical_objects_t *physical_boot_objects(void) { return &boot; }

int main(void)
{
    assert(direct_transition_host_main()==0);
    assert(physical_translation_retire_identity());
    assert(!physical_translation_retire_identity());
    uint8_t *storage=malloc(g_bitmap_storage_size); assert(storage);
    memset(storage,255,g_bitmap_storage_size);
    g_bitmap_buffer=g_bitmap.buffer=storage;
    g_reserved_bitmap_buffer=g_reserved_bitmap.buffer=storage+g_bitmap_size;
    assert(bitmap_clear_range(&g_bitmap,1024,37));
    assert(bitmap_clear_range(&g_reserved_bitmap,1024,37));
    assert(pmm_validate_integrity());
    bootstrap_claim_t out={0},first={0};
    size_t free_before=g_free_frames;
    uint64_t saved_flags=flags;
    assert(pmm_claim_bootstrap(&out) && out.physical==4096 && out.owner==BOOTSTRAP_TRAMPOLINE_OWNER);
    first=out;
    assert(pmm_claim_bootstrap(&out) && memcmp(&out,&first,sizeof(out))==0);
    assert(g_free_frames==free_before && flags==saved_flags && locks==unlocks && !g_pmm_lock.locked);
    assert(pmm_validate_integrity());
    bitmap_set(&g_reserved_bitmap,1,false);
    out=(bootstrap_claim_t){.physical=99};
    assert(!pmm_claim_bootstrap(&out) && out.physical==99 && g_free_frames==free_before);
    bitmap_set(&g_reserved_bitmap,1,true);
    boot.map=4096; boot.map_bytes=4096;
    assert(!pmm_claim_bootstrap(&out) && out.physical==99);
    boot=(boot_physical_objects_t){0};
    g_bootstrap_claim=(bootstrap_claim_t){0};
    g_physical_contract.regions[0].type=EFI_ACPI_MEMORY_NVS;
    assert(!pmm_claim_bootstrap(&out) && out.physical==99 && !g_bootstrap_claim.owner);
    g_physical_contract.regions[0].type=EFI_CONVENTIONAL_MEMORY;
    assert(pmm_claim_bootstrap(&out) && out.physical==4096);
    assert(g_free_frames==free_before && locks==unlocks && flags==saved_flags && pmm_validate_integrity());
    void *pointer=(void *)99;
    assert(physical_pointer_range(0,4096,PHYSICAL_RAM,&pointer) && (uintptr_t)pointer==RAM_DIRECT_BASE);
    uint64_t physical=99;
    assert(!physical_contract_inverse(&g_physical_contract,4096,4096,&physical) && physical==99);
    assert(!physical_identity_range_present(4096,4096));
    free(storage);
    puts("BOOTSTRAP_PMM_HOST status=PASS cases=15 full_frames=268435456 reservation=ADOPTED free_count=UNCHANGED if=RESTORED");
    return 0;
}

#include "bootstrap_claim.h"
#include "ram_map_plan.h"

static bool conflict(uint64_t physical, uint64_t first, uint64_t bytes)
{
    uint64_t end;
    if (!bytes) return false;
    if (!physical_range_valid(first,bytes,&end)) return true;
    return physical<end && first<physical+4096;
}

bool bootstrap_claim_candidate(const physical_contract_t *c, const boot_physical_objects_t *b,
    uint64_t bitmap, uint64_t bitmap_bytes, uint64_t physical, bootstrap_claim_t *out)
{
    if (!c || !c->ready || !c->identity_retired || !c->direct_map || !b || !out || !physical ||
        physical>=0xa0000 || (physical&4095) || !ram_map_contains(c->direct_map,physical,4096)) return false;
    const physical_region_t *origin=NULL;
    for (unsigned i=0; i<c->count; ++i)
        if (c->regions[i].first<=physical && c->regions[i].end>=physical+4096 &&
            c->regions[i].type==EFI_CONVENTIONAL_MEMORY) origin=&c->regions[i];
    if (!origin || conflict(physical,bitmap,bitmap_bytes) ||
        conflict(physical,c->image_first-KERNEL_IMAGE_OFFSET,c->image_end-c->image_first) ||
        conflict(physical,b->handoff,sizeof(BootInfo)) ||
        conflict(physical,b->framebuffer_descriptor,sizeof(Framebuffer)) ||
        conflict(physical,b->framebuffer,b->framebuffer_bytes) ||
        conflict(physical,b->map_descriptor,sizeof(MemoryMap)) || conflict(physical,b->map,b->map_bytes) ||
        conflict(physical,b->font_descriptor,sizeof(Psf1_Font)) || conflict(physical,b->font_header,sizeof(Psf1_Header)) ||
        conflict(physical,b->glyphs,b->glyph_bytes) || conflict(physical,b->logo_descriptor,sizeof(SimpleImage)) ||
        conflict(physical,b->pixels,b->pixel_bytes) || conflict(physical,b->rsdp,b->rsdp_bytes)) return false;
    *out=(bootstrap_claim_t){physical,4096,1,origin->first,origin->end,origin->type,BOOTSTRAP_TRAMPOLINE_OWNER};
    return true;
}

bool bootstrap_claim_adopt(bootstrap_claim_t *owner, const bootstrap_claim_t *c)
{
    if (!owner || !c || c->owner!=BOOTSTRAP_TRAMPOLINE_OWNER || c->generation!=1 ||
        !c->physical || c->physical>=0xa0000 || (c->physical&4095) || c->bytes!=4096 ||
        c->origin_type!=EFI_CONVENTIONAL_MEMORY || c->origin_first>c->physical ||
        c->origin_end<c->physical+4096) return false;
    if (owner->owner)
        return owner->owner==c->owner && owner->generation==c->generation && owner->physical==c->physical &&
            owner->bytes==c->bytes && owner->origin_type==c->origin_type &&
            owner->origin_first==c->origin_first && owner->origin_end==c->origin_end;
    *owner=*c; return true;
}

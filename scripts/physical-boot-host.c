#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define HOBBYOS_PHYSICAL_HOST 1
#include "../kernel/src/memory/boot_address.c"
#include "../kernel/src/memory/ram_map_plan.h"

/* Fake physical addresses are deliberately unrelated to these host buffers. */
static unsigned char storage[0x20000], original[sizeof(storage)];
static bool framebuffer_mapped;
static unsigned firmware_acquisitions;
mapping_status_t firmware_mapping_acquire(uint64_t physical,size_t bytes,mapping_view_t *out)
{
    assert(physical==0x10c000 && bytes==sizeof(AcpiRsdp));
    ++firmware_acquisitions;
    *out=(mapping_view_t){{1,1,1},(void *)0xffffd00000001000,physical,bytes};
    return MAPPING_OK;
}
const void *physical_host_backing(uint64_t physical, size_t bytes)
{
    if (physical < 0x100000 || physical - 0x100000 >= sizeof(storage) ||
        bytes > sizeof(storage) - (physical - 0x100000)) return NULL;
    return storage + physical - 0x100000;
}
bool mmio_origin_resolve(mmio_origin_kind_t kind,uint64_t source,mmio_origin_t *out)
{
    if(kind!=MMIO_FRAMEBUFFER || source!=0x104000)return false;
    *out=(mmio_origin_t){kind,0xe0000000,4096,source,1};return true;
}
mapping_status_t mmio_mapping_acquire(const mmio_origin_t *origin,cache_attribute_t cache,mapping_view_t *out)
{
    assert(origin->physical==0xe0000000 && origin->bytes==4096 && cache==CACHE_WRITE_COMBINING);
    if(!framebuffer_mapped)return MAPPING_INVALID;
    *out=(mapping_view_t){{1,0,1},(void *)0xffffd00000000000,origin->physical,origin->bytes};return MAPPING_OK;
}
bool owned_mapping_physical(const void *cpu,size_t bytes,uint64_t *out)
{
    if((uintptr_t)cpu!=0xffffd00000000000 || bytes>4096)return false;
    *out=0xe0000000;return true;
}
bool paging_query_range(uint64_t address,size_t bytes,uint64_t *out)
{ return framebuffer_mapped && owned_mapping_physical((void *)(uintptr_t)address,bytes,out); }

void kpanic(const char *message)
{ fprintf(stderr, "Unexpected panic: %s\n", message); abort(); }
static void put(uint64_t physical, const void *value, size_t bytes)
{ memcpy((void *)physical_host_backing(physical, bytes), value, bytes); }

static void scenario(unsigned kind)
{
    BootInfo raw = {.framebuffer=(void *)0x104000, .font=(void *)0x105000,
        .logo=(void *)0x10a000, .rsdp=(void *)0x10c000, .memory_map=(void *)0x102000};
    EfiMemoryDescriptor descriptors[5] = {
        {.Type=EFI_CONVENTIONAL_MEMORY, .PhysicalStart=0, .NumberOfPages=0xe0000},
        {.Type=EFI_MEMORY_MAPPED_IO, .PhysicalStart=0xe0000000, .NumberOfPages=0x1000},
        {.Type=EFI_RESERVED_MEMORY_TYPE, .PhysicalStart=0xff00000000, .NumberOfPages=0x100000}};
    MemoryMap map = {.Base=(void *)0x103000, .Size=3*sizeof(descriptors[0]),
                     .DescriptorSize=sizeof(EfiMemoryDescriptor)};
    Framebuffer framebuffer = {.BaseAddress=(void *)0xe0000000, .BufferSize=4096,
        .Width=16, .Height=16, .PixelsPerScanLine=16};
    Psf1_Font font = {.psf_header=(void *)0x106000, .glyph_buffer=(void *)0x107000};
    Psf1_Header header = {.magic={0x36,0x04}, .mode=1, .charsize=16};
    SimpleImage logo = {.Width=16,.Height=16,.Size=1024,.PixelBuffer=(void *)0x10b000};
    AcpiRsdp rsdp = {.revision=2,.length=sizeof(AcpiRsdp)};
    bool expected = kind <= 1 || kind>=12;
    if (kind==13) {
        descriptors[3]=(EfiMemoryDescriptor){.Type=EFI_RESERVED_MEMORY_TYPE,.PhysicalStart=0x10c000,.NumberOfPages=1};
        descriptors[4]=descriptors[0];descriptors[4].PhysicalStart=0x10d000;
        descriptors[4].NumberOfPages-=0x10d;
        descriptors[0].NumberOfPages=0x10c;
        map.Size=sizeof(descriptors);
    }
    if (kind == 1) { raw.font=NULL; raw.logo=NULL; }
    if (kind == 2) raw.memory_map=(void *)0x100000000;
    if (kind == 3) map.DescriptorSize=sizeof(EfiMemoryDescriptor)-1;
    if (kind == 4) font.glyph_buffer=(void *)0x11ffff;
    if (kind == 5) header.charsize=0;
    if (kind == 6) logo.Size=1;
    if (kind == 7) framebuffer.BufferSize=1;
    if (kind == 8) raw.rsdp=NULL;
    if (kind == 9) rsdp.length=UINT32_MAX;
    if (kind == 10) descriptors[0].NumberOfPages=256;
    if (kind == 11) map.Base=(void *)(UINT64_MAX-8);
    put(0x101000,&raw,sizeof(raw)); put(0x102000,&map,sizeof(map));
    put(0x103000,descriptors,sizeof(descriptors)); put(0x104000,&framebuffer,sizeof(framebuffer));
    put(0x105000,&font,sizeof(font)); put(0x106000,&header,sizeof(header));
    put(0x10a000,&logo,sizeof(logo)); put(0x10c000,&rsdp,sizeof(rsdp));
    memcpy(original,storage,sizeof(storage));
    BootInfo *view=(void *)0x1234;
    assert(physical_boot_adapt(0x101000,&view)==expected);
    assert(!memcmp(original,storage,sizeof(storage)));
    if (!expected) assert(view==(void *)0x1234 && !physical_boot_objects());
    else {
        assert(view && view != (void *)0x101000 && view->memory_map->Base != map.Base);
        assert(view->framebuffer != raw.framebuffer && !view->framebuffer->BaseAddress);
        assert(view->rsdp==physical_host_backing(0x10c000,sizeof(rsdp)));
        if (kind != 1) {
            assert(view->font->psf_header != font.psf_header);
            assert(view->font->glyph_buffer==physical_host_backing(0x107000,8192));
            assert(view->logo->PixelBuffer==physical_host_backing(0x10b000,1024));
        } else assert(!view->font && !view->logo);
        assert(physical_allocation_ceiling()==UINT64_C(0x100000000));
        assert(!early_copy(0x101000,&raw,sizeof(raw)));
        static ram_map_plan_t plan;
        if (kind>=12) {
            ram_features_t f;ram_mtrr_inputs_t m={.available=true,.defaults=0x806};
            assert(ram_features_decode(0x80000008,(1u<<29)|(1u<<26)|(1u<<20),40,true,&f));
            assert(ram_map_plan_create(&plan,&g_physical_contract,&f,&m,0xe0000000,4096));
            physical_borrow_t old={.physical=0x107000,.bytes=8192,.epoch=g_physical_contract.epoch};
            assert(physical_translation_activate_direct(&plan));
            uint64_t rejected=77;
            assert(!physical_contract_borrow(&g_physical_contract,&old,PHYSICAL_RAM,&rejected) && rejected==77);
        } else assert(physical_translation_activate(UINT64_C(0x100000000)));
        assert(!physical_boot_activate());
        assert(!view->framebuffer->BaseAddress);
        framebuffer_mapped=true;
        assert(physical_boot_activate() && (uintptr_t)view->framebuffer->BaseAddress==0xffffd00000000000);
        void *pointer=(void *)0x1234;
        if (kind>=12) {
            assert(physical_pointer_range(0,1,PHYSICAL_RAM,&pointer) && (uintptr_t)pointer==RAM_DIRECT_BASE);
            assert((uintptr_t)view->font->glyph_buffer==RAM_DIRECT_BASE+0x107000);
            assert((uintptr_t)view->logo->PixelBuffer==RAM_DIRECT_BASE+0x10b000);
            assert((uintptr_t)view->rsdp==(kind==13?UINT64_C(0xffffd00000001000):RAM_DIRECT_BASE+0x10c000));
            assert(firmware_acquisitions==(kind==13?2u:0u));
            assert(view==&g_view && view->memory_map==&g_map && view->memory_map->Base==(void *)g_map_bytes);
            assert(view->font==&g_font && view->font->psf_header==&g_font_header && view->logo==&g_logo);
        } else assert(!physical_pointer_range(0,1,PHYSICAL_RAM,&pointer) && pointer==(void *)0x1234);
        assert(!physical_device_pointer(0xe0000000,8192,PHYSICAL_DEVICE_FRAMEBUFFER,&pointer));
    }
    assert(!physical_boot_adapt(0x101000,&view));
}
int main(void)
{
    for (unsigned i=0;i<14;i++) {
        pid_t child=fork(); assert(child>=0);
        if (!child) { scenario(i); _exit(0); }
        int status=0; assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    puts("status=PASS cases=14 scope=actual_boot_adapter physical_backing=SUBSTITUTED nonidentity_host_buffers=1");
    return 0;
}

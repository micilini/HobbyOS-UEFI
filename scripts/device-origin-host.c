/* Real admission and physical provenance; page/CPU operations are substituted. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/cpu/control_policy.h"
const cpu_policy_t *cpu_policy_get(void);
#include "../kernel/src/memory/owned_mapping.c"
#include "../kernel/src/memory/physical_translation.c"
#include "../kernel/src/memory/device_origin.c"
#include "../kernel/src/memory/image_permissions.c"
static unsigned effects, checks;
static boot_physical_objects_t boot;
static union { HpetTable table; unsigned char bytes[4096]; } hpet_storage;
#define hpet hpet_storage.table
static cpu_policy_t policy;
static union { McfgHeader table; unsigned char bytes[4096]; } mcfg_storage;
static uint64_t pci_source,pci_base,pci_bytes;
static bool fail_flush;
static unsigned sequence[32],sequence_count;
static void operation(unsigned kind){++effects;if(sequence_count<32)sequence[sequence_count++]=kind;}
#define C(x) do {++checks;assert(x);} while(0)
const boot_physical_objects_t *physical_boot_objects(void){return &boot;}
const AcpiSdtHeader *acpi_retained_table(const char *s,uint64_t *p)
{if(!memcmp(s,"HPET",4)){*p=0x50000;return &hpet.header;}
 if(!memcmp(s,"MCFG",4)){*p=0x60000;return (void *)&mcfg_storage.table;}return NULL;}
bool lapic_discovered_extent(uint64_t s,uint64_t *p,uint64_t *b)
{if(s!=0xfee00800)return false;*p=0xfee00000;*b=4096;return true;}
uint32_t madt_get_ioapic_count(void){return 0;}
bool madt_ioapic_at(uint32_t i,madt_ioapic_t *out){(void)i;(void)out;return false;}
bool pci_discovered_xhci_extent(uint64_t s,uint64_t *p,uint64_t *b)
{if(!pci_source||s!=pci_source)return false;*p=pci_base;*b=pci_bytes;return true;}
bool acpi_wdat_discovered_extent(uint64_t s,uint64_t *p,uint64_t *b){(void)s;(void)p;(void)b;return false;}
const cpu_policy_t *cpu_policy_get(void){return &policy;}
bool cpu_request_mapping_context_allowed(void){return true;}
irq_flags_t irq_save(void){++effects;return 512;}
void irq_restore(irq_flags_t f){C(f==512);++effects;}
irq_flags_t spin_lock_irqsave(spinlock_t *l){C(!l->locked);l->locked=1;return 512;}
void spin_unlock_irqrestore(spinlock_t *l,irq_flags_t f){C(l->locked&&f==512);l->locked=0;}
bool paging_owned_withdraw_identity(uint64_t p,uint64_t b){(void)p;(void)b;operation(1);return true;}
static const image_permissions_t protected_image={.first=0x2000000,.end=0x2400000,
 .input={.boot_first=0x1ff0000,.boot_end=0x2000000},.ready=true};
bool paging_owned_extent_allowed(uint64_t p,uint64_t b) { return !image_permissions_overlaps(&protected_image,p,b); }
bool paging_owned_install(uint64_t v,uint64_t p,uint64_t n,uint64_t f){(void)v;(void)p;(void)n;(void)f;operation(3);return true;}
bool kernel_tlb_bootstrap_invalidate(uint64_t a,uint64_t b){(void)a;(void)b;operation(2);return !fail_flush;}
bool kernel_cache_bootstrap_writeback(void){operation(4);return true;}
bool paging_owned_remove(uint64_t v,uint64_t p,uint64_t n,uint64_t f)
{(void)v;(void)p;(void)n;(void)f;operation(5);return true;}
cpu_request_result_t kernel_mapping_invalidate(uint64_t a,uint64_t b){(void)a;(void)b;++effects;return (cpu_request_result_t){.status=CPU_REQUEST_OK};}
cpu_request_result_t kernel_cache_writeback(void){++effects;return (cpu_request_result_t){.status=CPU_REQUEST_OK};}
int main(void)
{
 g_physical_contract=(physical_contract_t){.ready=true,.epoch=1,.image_first=KERNEL_IMAGE_OFFSET+0x100000,.image_end=KERNEL_IMAGE_OFFSET+0x200000,.identity_limit=0x100000000};
 g_physical_contract.regions[0]=(physical_region_t){.first=0x91000000,.end=0x91001000,.kind=PHYSICAL_FIRMWARE};g_physical_contract.count=1;
 cpu_policy_features_t features={.basic_edx=CPU_FEATURE_REQUIRED,.extended_edx=CPU_FEATURE_LM|CPU_FEATURE_NX,.basic_valid=true,.extended_valid=true,.identity_valid=true};
 C(cpu_policy_build(&features,&policy)==CPU_POLICY_OK);
 mapping_view_t out,saved;memset(&out,0xa5,sizeof(out));saved=out;
 mmio_origin_t bad={MMIO_PCI_BAR,0x90000000,4096,1,1};
 C(mmio_mapping_acquire(&bad,CACHE_WRITE_COMBINING,&out)==MAPPING_INVALID);
 C(effects==0&&!memcmp(&out,&saved,sizeof(out))&&g_generation==0);
 bad.physical=0x91000000;C(mmio_mapping_acquire(&bad,CACHE_WRITE_COMBINING,&out)==MAPPING_INVALID);
 C(effects==0&&g_generation==0);
 boot.framebuffer=0x90000000;boot.framebuffer_bytes=4096;boot.framebuffer_descriptor=0x40000;
 mmio_origin_t real;C(mmio_origin_resolve(MMIO_FRAMEBUFFER,0x40000,&real));C(mmio_origin_valid(&real));
 bad=real;bad.generation++;C(!mmio_origin_valid(&bad));bad=real;bad.physical++;C(!mmio_origin_valid(&bad));
 bad=real;bad.kind=MMIO_PCI_BAR;C(!mmio_origin_valid(&bad));
 sequence_count=0;
 C(mmio_mapping_acquire(&real,CACHE_WRITE_COMBINING,&out)==MAPPING_OK);C(effects>0&&out.physical==0x90000000);
 const unsigned expected[]={1,2,3,2,4};C(sequence_count==5&&!memcmp(sequence,expected,sizeof(expected)));
 mapping_view_t second;C(mmio_mapping_acquire(&real,CACHE_WRITE_COMBINING,&second)==MAPPING_OK);
 C(second.cpu!=out.cpu);C(owned_mapping_release(out.handle)==MAPPING_OK);
 C(owned_mapping_release(out.handle)==MAPPING_STALE);
 void *sub=NULL;C(owned_mapping_subview(second.handle,0,4096,&sub)&&sub==second.cpu);
 C(!owned_mapping_subview(second.handle,4095,2,&sub));
 unsigned saved_effects=effects;uint64_t saved_generation=g_generation;
 C(mmio_mapping_acquire(&real,CACHE_UNCACHEABLE,&out)==MAPPING_CONFLICT);
 C(effects==saved_effects+2&&g_generation==saved_generation);
 C(owned_mapping_release(second.handle)==MAPPING_OK);
 hpet.header.length=sizeof(hpet);hpet.address=0xfed00000;
 C(mmio_origin_resolve(MMIO_HPET,0x50000,&real));C(real.bytes==0x120&&mmio_origin_valid(&real));
 hpet.address_space_id=1;C(!mmio_origin_valid(&real));hpet.address_space_id=0;
 bad=real;bad.bytes=0x121;C(!mmio_origin_valid(&bad));
 C(mmio_origin_resolve(MMIO_LAPIC,0xfee00800,&real));C(mmio_origin_valid(&real));
 unsigned before=effects;bad=real;bad.source=1;C(mmio_mapping_acquire(&bad,CACHE_UNCACHEABLE,&out)==MAPPING_INVALID);C(effects==before);
 McfgHeader *mcfg=&mcfg_storage.table;mcfg->Header.length=sizeof(*mcfg)+sizeof(McfgDeviceConfig);
 McfgDeviceConfig *ecam=(void *)(mcfg_storage.bytes+sizeof(*mcfg));
 ecam->BaseAddress=0xe0000000;ecam->StartBus=2;ecam->EndBus=7;
 C(mmio_origin_resolve(MMIO_PCI_ECAM,0x60000+sizeof(*mcfg),&real));
 C(real.physical==0xe0200000&&real.bytes==6*1048576&&mmio_origin_valid(&real));
 bad=real;bad.bytes++;C(!mmio_origin_valid(&bad));bad=real;bad.source++;C(!mmio_origin_valid(&bad));
 ecam->StartBus=8;C(!mmio_origin_valid(&real));ecam->StartBus=2;
 ecam->BaseAddress=UINT64_MAX-1048575;C(!mmio_origin_valid(&real));
 mapping_view_t firmware;C(firmware_mapping_acquire(0x91000001,16,&firmware)==MAPPING_OK);
 pci_source=0x123400; pci_base=0x91000020; pci_bytes=8;
 C(mmio_origin_resolve(MMIO_PCI_BAR,pci_source,&real));
 saved_effects=effects;saved_generation=g_generation;out=saved;
 C(mmio_mapping_acquire(&real,CACHE_WRITE_COMBINING,&out)==MAPPING_CONFLICT);
 C(effects==saved_effects+2&&g_generation==saved_generation&&!memcmp(&out,&saved,sizeof(out)));
 C(owned_mapping_release(firmware.handle)==MAPPING_OK);
 g_physical_contract.regions[1]=(physical_region_t){.first=0x92000000,.end=0x92001000,.kind=PHYSICAL_RAM};
 g_physical_contract.count=2;pci_base=0x92000020;C(!mmio_origin_resolve(MMIO_PCI_BAR,pci_source,&real));
 pci_base=0x2001000;C(mmio_origin_resolve(MMIO_PCI_BAR,pci_source,&real));
 saved_effects=effects;saved_generation=g_generation;out=saved;
 C(mmio_mapping_acquire(&real,CACHE_UNCACHEABLE,&out)==MAPPING_INVALID);
 C(effects==saved_effects&&g_generation==saved_generation&&!memcmp(&out,&saved,sizeof(out)));
 pci_base=0x90000000;C(mmio_origin_resolve(MMIO_PCI_BAR,pci_source,&real));
 fail_flush=true;saved_generation=g_generation;out=saved;
 C(mmio_mapping_acquire(&real,CACHE_UNCACHEABLE,&out)==MAPPING_QUARANTINED);
 C(g_generation==saved_generation+1&&!memcmp(&out,&saved,sizeof(out)));
 fail_flush=false;C(mmio_mapping_acquire(&real,CACHE_UNCACHEABLE,&out)==MAPPING_CONFLICT);
 printf("DEVICE_ORIGIN_HOST checks=%u status=PASS\n",checks);
}

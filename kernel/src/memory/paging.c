#include "../core/irq_latency.h"
#include "paging_private.h"
#include "physical_internal.h"
#include "mapping_core.h"
#include "ram_map_boot.h"
#include "boot_address.h"
#include "bootstrap_tables.h"
#include "image_walk.h"
#include "stack_internal.h"
#include "../libc/string.h"
#include "../core/panic.h"
#include "../libc/memory.h"
#include "../cpu/cpu.h"
#include "../cpu/tss.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"
#ifdef HOBBYOS_SELFTEST
#include "tlb_fixture.h"
#include "ram_map_inspect.h"
#endif
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
#include "paging_inspect.h"
#define PAGING_OBSERVER_CAPACITY 8192u
static uint64_t g_observer_tables[PAGING_OBSERVER_CAPACITY];
static uint64_t g_observer_reachable[PAGING_OBSERVER_CAPACITY];
static uint32_t g_observer_count;
static uint64_t g_observer_limit, g_observer_epoch, g_observer_initial;
static PageTable *g_observer_boot_root;
static bool g_observer_failed, g_observer_nonidentity;
#endif

extern uint64_t _kernel_start;
extern uint64_t _kernel_end;
extern uint8_t bsp_stack_bottom[], bsp_stack_top[];

PageTable *g_kernel_pml4 = NULL;
static uint64_t g_kernel_root_physical;
static spinlock_t g_paging_lock;
static mapping_window_t g_owned_window;
static ram_map_plan_t g_ram_plan;
static ram_mtrr_inputs_t g_ram_mtrrs;
static paging_build_t g_boot_build;
static image_permissions_t g_image_permissions;
static uint64_t g_image_mapping_epoch;
static paging_boot_requirements_t g_boot_requirements;
static bootstrap_tables_t g_bootstrap_tables;
static bootstrap_lifetime_t g_bootstrap_life;
static uint64_t g_bootstrap_freed, g_bootstrap_free_before, g_bootstrap_free_after;
#ifdef HOBBYOS_SELFTEST
static bootstrap_snapshot_t g_bootstrap_snapshot;
#endif

#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x) (((x) >> 12) & 0x1FF)

static PageTable *alloc_table()
{
    void *paddr = pmm_alloc_frame();
    if (!paddr)
    {
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
        g_observer_failed = true;
#endif
        return NULL;
    }
    void *pointer;
    if (!physical_pointer_range((uintptr_t)paddr, PAGE_SIZE, PHYSICAL_RAM, &pointer)) {
        pmm_free_frame(paddr);
        return NULL;
    }
    memset(pointer, 0, PAGE_SIZE);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (g_observer_count < PAGING_OBSERVER_CAPACITY)
        g_observer_tables[g_observer_count++] = (uint64_t)paddr;
    else
        g_observer_failed = true;
#endif
    return pointer;
}

static PageTable *table_pointer(uint64_t physical)
{
    void *pointer;
    if ((physical & 4095) || !physical_pointer_range(physical, PAGE_SIZE, PHYSICAL_RAM, &pointer)) return NULL;
    return pointer;
}

static bool query_page_locked(uint64_t address, uint64_t *physical, uint64_t *remaining)
{
    PageTable *table = g_kernel_pml4;
    for (unsigned level = 4; table && level; --level) {
        uint64_t entry = table->entries[(address >> (12 + (level - 1) * 9)) & 511];
        if (!(entry & PAGE_PRESENT) || (entry & UINT64_C(0x7ff0000000000000))) return false;
        if (level == 1 || (entry & PAGE_HUGE)) {
            if (level == 4) return false;
            uint64_t size = UINT64_C(1) << (12 + (level - 1) * 9);
            uint64_t reserved = (size - 1) & PAGE_ADDR_MASK;
            if (level > 1) reserved &= ~UINT64_C(0x1000);
            if (entry & reserved) return false;
            *physical = (entry & PAGE_ADDR_MASK & ~(size - 1)) | (address & (size - 1));
            *remaining = size - (address & (size - 1));
            return true;
        }
        table = table_pointer(entry & PAGE_ADDR_MASK);
    }
    return false;
}

bool paging_query_range(uint64_t virtual_address, size_t bytes, uint64_t *physical)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!physical || !physical_canonical_range(virtual_address, bytes)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    uint64_t offset = 0, first = 0;
    bool valid = true;
    while (offset < bytes) {
        uint64_t actual, remaining;
        if (!query_page_locked(virtual_address + offset, &actual, &remaining) ||
            (offset && (first > UINT64_MAX - offset || actual != first + offset))) {
            valid = false; break;
        }
        if (!offset) first = actual;
        uint64_t chunk = bytes - offset < remaining ? bytes - offset : remaining;
        offset += chunk;
    }
    spin_unlock_irqrestore(&g_paging_lock, flags);
    if (valid) *physical = first;
    return valid;
}

uint64_t paging_get_physical_address(uint64_t vaddr)
{
    uint64_t physical = 0;
    (void)paging_query_range(vaddr, 1, &physical);
    return physical;
}

uint64_t paging_kernel_root_physical(void) { return g_kernel_root_physical; }

void paging_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (g_boot_build.activated)
        kpanic("Unowned mappings are forbidden after verified root publication");
    if (g_physical_contract.identity_retired && vaddr < UINT64_C(0xffff800000000000))
        kpanic("A low mapping was requested after identity retirement");
    irq_flags_t irq_flags = spin_lock_irqsave(&g_paging_lock);

    if (!g_kernel_pml4)
    {
        spin_unlock_irqrestore(&g_paging_lock, irq_flags);
        return;
    }

    uint64_t pml4_idx = PML4_INDEX(vaddr);

    if (!(g_kernel_pml4->entries[pml4_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table)
        {
            spin_unlock_irqrestore(&g_paging_lock, irq_flags);
            return;
        }
        g_kernel_pml4->entries[pml4_idx] = physical_require_address(new_table, PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pdpt = table_pointer(g_kernel_pml4->entries[pml4_idx] & PAGE_ADDR_MASK);
    if (!pdpt) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }

    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    if (!(pdpt->entries[pdpt_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table)
        {
            spin_unlock_irqrestore(&g_paging_lock, irq_flags);
            return;
        }
        pdpt->entries[pdpt_idx] = physical_require_address(new_table, PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pd = table_pointer(pdpt->entries[pdpt_idx] & PAGE_ADDR_MASK);
    if (!pd) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }

    uint64_t pd_idx = PD_INDEX(vaddr);
    if (!(pd->entries[pd_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table)
        {
            spin_unlock_irqrestore(&g_paging_lock, irq_flags);
            return;
        }
        pd->entries[pd_idx] = physical_require_address(new_table, PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pt = table_pointer(pd->entries[pd_idx] & PAGE_ADDR_MASK);
    if (!pt) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }

    pt->entries[PT_INDEX(vaddr)] = (paddr & PAGE_ADDR_MASK) | flags;
    physical_translation_mapping(vaddr, paddr, flags);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    g_observer_epoch++;
    if (vaddr < g_observer_limit &&
        ((vaddr & PAGE_ADDR_MASK) != (paddr & PAGE_ADDR_MASK) ||
         !(flags & PAGE_PRESENT)))
        g_observer_nonidentity = true;
#endif

    __asm__ volatile("invlpg (%0)" ::"r"(vaddr) : "memory");

    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
}

void paging_load_kernel_root(void)
{
    uint64_t physical = g_kernel_root_physical;
    __asm__ volatile("mov %0, %%cr3" ::"r"(physical) : "memory");
}

static bool boot_table_allocate(void *context, mapping_table_t *out)
{
    (void)context;
    uint64_t physical; void *cpu;
    if (!pmm_alloc_constrained_frames(1, 1, PHYSICAL_ENTRY_LIMIT, &physical)) return false;
    if (!physical_pointer_range(physical, PAGE_SIZE, PHYSICAL_RAM, &cpu)) {
        pmm_free_frame((void *)(uintptr_t)physical); return false;
    }
    *out = (mapping_table_t){cpu, physical}; return true;
}
static void boot_table_release(void *context, mapping_table_t table)
{ (void)context; pmm_free_frame((void *)(uintptr_t)table.physical); }
static void boot_root_load(void *context, uint64_t physical)
{
    (void)context;
    __asm__ volatile("mov %0, %%cr3" :: "r"(physical) : "memory");
}

const bootstrap_lifetime_t *paging_bootstrap_lifetime(void) { return &g_bootstrap_life; }

#ifdef HOBBYOS_SELFTEST
bool paging_bootstrap_snapshot_entry(unsigned index, uint64_t *physical, uint64_t *hash)
{
    if (!physical || !hash || index>=g_bootstrap_snapshot.count ||
        g_bootstrap_snapshot.generation!=g_bootstrap_life.generation ||
        g_bootstrap_snapshot.root!=g_bootstrap_life.root) return false;
    *physical=g_bootstrap_snapshot.physical[index];
    *hash=paging_table_hash(&g_bootstrap_snapshot.bytes[index]);
    return true;
}
#endif

void paging_bootstrap_not_needed(void)
{
    if (g_bootstrap_tables.count || !bootstrap_lifetime_not_needed(&g_bootstrap_life,g_kernel_root_physical,1))
        kpanic("Bootstrap absence conflicts with existing ownership");
    serial_write_all("[AP_ROOT] NOT_NEEDED private=0 freed=0\n");
}

bool paging_bootstrap_create(const bootstrap_claim_t *claim, uint64_t generation,
    uint64_t intended, const uint32_t *ids, unsigned count, uint64_t *root)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!root || !paging_low_half_empty() || g_bootstrap_life.state!=BOOTSTRAP_NEW) return false;
    bootstrap_claim_t owned;
    if (!claim || !pmm_claim_bootstrap(&owned) || memcmp(claim,&owned,sizeof(owned))) return false;
    const mapping_table_ops_t ops={boot_table_allocate,boot_table_release};
    irq_flags_t flags=spin_lock_irqsave(&g_paging_lock);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (g_observer_count>PAGING_OBSERVER_CAPACITY-BOOTSTRAP_PRIVATE_TABLES) {
        spin_unlock_irqrestore(&g_paging_lock,flags); return false;
    }
#endif
    bool ok=bootstrap_tables_create(&g_bootstrap_tables,&g_boot_build,claim,&ops,NULL);
    if (ok) ok=bootstrap_lifetime_begin(&g_bootstrap_life,generation,g_bootstrap_tables.tables[0].physical,
        g_kernel_root_physical,intended,ids,count);
    if (!ok) {
        bootstrap_tables_abort(&g_bootstrap_tables);
        spin_unlock_irqrestore(&g_paging_lock,flags); return false;
    }
#ifdef HOBBYOS_SELFTEST
    g_bootstrap_snapshot.generation=generation;
    g_bootstrap_snapshot.root=g_bootstrap_life.root;
    for (unsigned i=0; i<BOOTSTRAP_PRIVATE_TABLES; ++i) {
        g_bootstrap_snapshot.physical[i]=g_bootstrap_tables.tables[i].physical;
        memcpy(&g_bootstrap_snapshot.bytes[i],g_bootstrap_tables.tables[i].cpu,PAGE_SIZE);
    }
    g_bootstrap_snapshot.count=BOOTSTRAP_PRIVATE_TABLES;
#endif
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    for (unsigned i=0; i<BOOTSTRAP_PRIVATE_TABLES; ++i)
        g_observer_tables[g_observer_count++]=g_bootstrap_tables.tables[i].physical;
    g_observer_boot_root=g_bootstrap_tables.tables[0].cpu;
    ++g_observer_epoch;
#endif
    g_bootstrap_tables.published=true;
    *root=g_bootstrap_life.root;
    spin_unlock_irqrestore(&g_paging_lock,flags);
    char line[256];
    int n=ksnprintf(line,sizeof(line),
        "[AP_ROOT] CREATED generation=%llu root=%016llx main=%016llx intended=%016llx trampoline=%016llx private=4 low_pages=1 verified=1\n",
        (unsigned long long)generation,(unsigned long long)*root,(unsigned long long)g_kernel_root_physical,
        (unsigned long long)intended,(unsigned long long)claim->physical);
    if (n<=0 || (size_t)n>=sizeof(line)) kpanic("Bootstrap record overflow");
    serial_write_all(line); return true;
}

bool paging_bootstrap_start(unsigned slot, uint32_t id, uint64_t generation)
{ return bootstrap_lifetime_start(&g_bootstrap_life,slot,id,generation); }
bool paging_bootstrap_retry(unsigned slot, uint32_t id, uint64_t generation)
{ return bootstrap_lifetime_retry(&g_bootstrap_life,slot,id,generation); }

bool paging_bootstrap_handoff_ready(const bootstrap_departure_t *d)
{
    uint64_t physical;
    if (!d || d->previous_root!=g_bootstrap_life.root ||
        !bootstrap_lifetime_retry(&g_bootstrap_life,d->slot,d->apic_id,d->generation) ||
        d->cs!=0x08 || d->tr!=0x28 || d->stack_bytes!=16384 ||
        d->rsp<d->stack_first || d->rsp-d->stack_first>=d->stack_bytes ||
        !paging_low_half_empty() ||
        !paging_query_range(d->stack_first,d->stack_bytes,&physical) ||
        !paging_query_range(d->gdtr,4096,&physical) || !paging_query_range(d->idtr,4096,&physical) ||
        !paging_query_range(d->tss,sizeof(Tss64),&physical) ||
        !paging_query_range(d->ist1-32768,32768,&physical) ||
        !paging_query_range(d->ist2-32768,32768,&physical) ||
        !paging_query_range(d->ist3-32768,32768,&physical)) return false;
    return true;
}

bool paging_bootstrap_depart(const bootstrap_departure_t *d)
{
    if (!paging_bootstrap_handoff_ready(d) || !bootstrap_lifetime_depart(&g_bootstrap_life,d)) return false;
    char line[448];
    int n=ksnprintf(line,sizeof(line),
        "[AP_ROOT] DEPART generation=%llu slot=%u apic=%u previous=%016llx root=%016llx rsp=%016llx stack=%016llx gdtr=%016llx idtr=%016llx tss=%016llx ist1=%016llx ist2=%016llx ist3=%016llx cs=%x tr=%x\n",
        (unsigned long long)d->generation,d->slot,d->apic_id,(unsigned long long)d->previous_root,
        (unsigned long long)d->root,(unsigned long long)d->rsp,(unsigned long long)d->stack_first,
        (unsigned long long)d->gdtr,(unsigned long long)d->idtr,(unsigned long long)d->tss,
        (unsigned long long)d->ist1,(unsigned long long)d->ist2,(unsigned long long)d->ist3,d->cs,d->tr);
    if (n<=0 || (size_t)n>=sizeof(line)) kpanic("Bootstrap departure record overflow");
    serial_write_all(line); return true;
}

bool paging_bootstrap_retire(volatile uint64_t *trampoline_root)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    uint64_t actual, parameter;
    __asm__ volatile("mov %%cr3,%0" : "=r"(actual));
    if (!trampoline_root || !physical_address_range((const void *)trampoline_root,8,&parameter) ||
        parameter<g_bootstrap_tables.claim.physical ||
        parameter-g_bootstrap_tables.claim.physical>4096-8 || *trampoline_root!=g_bootstrap_life.root ||
        !bootstrap_tables_verify(&g_bootstrap_tables) || !bootstrap_lifetime_seal(&g_bootstrap_life,actual)) return false;
    /* Every admitted AP has switched with PGE=PCIDE=0. The existing trampoline
     * rejects zero CR3, and no owned start/retry is admitted after sealing. */
    __atomic_store_n(trampoline_root,0,__ATOMIC_RELEASE);
    irq_flags_t flags=spin_lock_irqsave(&g_paging_lock);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    for (unsigned i=0; i<BOOTSTRAP_PRIVATE_TABLES; ++i) {
        unsigned found=0;
        for (unsigned j=0; j<g_observer_count; ++j)
            if (g_observer_tables[j]==g_bootstrap_tables.tables[i].physical) ++found;
        if (found!=1) { spin_unlock_irqrestore(&g_paging_lock,flags); return false; }
    }
    g_observer_boot_root=NULL;
    for (unsigned i=0; i<BOOTSTRAP_PRIVATE_TABLES; ++i)
        for (unsigned j=0; j<g_observer_count; ++j)
            if (g_observer_tables[j]==g_bootstrap_tables.tables[i].physical) {
                g_observer_tables[j]=g_observer_tables[--g_observer_count];
                g_observer_tables[g_observer_count]=0; break;
            }
    ++g_observer_epoch;
#endif
    g_bootstrap_free_before=pmm_get_free_memory();
    bool ok=bootstrap_tables_retire(&g_bootstrap_tables,&g_bootstrap_life);
    g_bootstrap_free_after=pmm_get_free_memory();
    ok=ok && g_bootstrap_free_after==g_bootstrap_free_before+BOOTSTRAP_PRIVATE_TABLES*PAGE_SIZE;
    if (ok) { g_bootstrap_freed=BOOTSTRAP_PRIVATE_TABLES; ok=bootstrap_lifetime_finish(&g_bootstrap_life); }
    spin_unlock_irqrestore(&g_paging_lock,flags);
    if (!ok) return false;
    char line[256];
    int n=ksnprintf(line,sizeof(line),
        "[AP_ROOT] RETIRED generation=%llu root=%016llx intended=%016llx private=0 freed=%llu free_before=%llu free_after=%llu observer_live=0\n",
        (unsigned long long)g_bootstrap_life.generation,(unsigned long long)g_bootstrap_life.root,
        (unsigned long long)g_bootstrap_life.intended,(unsigned long long)g_bootstrap_freed,
        (unsigned long long)g_bootstrap_free_before,(unsigned long long)g_bootstrap_free_after);
    if (n<=0 || (size_t)n>=sizeof(line)) kpanic("Bootstrap retirement record overflow");
    serial_write_all(line); return true;
}

bool paging_boot_requirements(paging_boot_requirements_t *out)
{
    if (!out || !g_boot_build.activated || !g_physical_contract.direct_map) return false;
    *out = g_boot_requirements; return true;
}

void init_paging(uint64_t fb_base, uint64_t fb_size)
{
    spinlock_init(&g_paging_lock);
    uint64_t flags, stack;
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr, idtr;
    __asm__ volatile("pushfq; popq %0; mov %%rsp,%1; sgdt %2; sidt %3"
        : "=r"(flags), "=r"(stack), "=m"(gdtr), "=m"(idtr) :: "memory");
    if (flags & (1u << 9)) kpanic("RAM map activation requires the boot interrupt boundary");
    if ((uintptr_t)bsp_stack_top-(uintptr_t)bsp_stack_bottom != 16384 ||
        stack < (uintptr_t)bsp_stack_bottom || stack >= (uintptr_t)bsp_stack_top)
        kpanic("BSP paging transition is outside the high entry stack");
    const boot_physical_objects_t *objects = physical_boot_objects();
    ram_features_t features;
    if (!objects || fb_base != objects->framebuffer || fb_size != objects->framebuffer_bytes)
        kpanic("RAM map framebuffer origin differs from the boot handoff");
    if (!ram_map_boot_inputs(&features, &g_ram_mtrrs))
        kpanic("RAM map CPU or read-only cache inputs are unavailable");
    if (!ram_map_plan_create(&g_ram_plan, &g_physical_contract, &features, &g_ram_mtrrs, fb_base, fb_size)) {
        char reason[224];
        ksnprintf(reason, sizeof(reason), "[RAM_MAP] REJECT bits=%u extended=%08x cap=%llx defaults=%llx ranges=%u descriptors=%u\n",
            features.physical_bits, features.extended_edx, (unsigned long long)g_ram_mtrrs.capability,
            (unsigned long long)g_ram_mtrrs.defaults, g_ram_mtrrs.count, g_physical_contract.count);
        serial_write_all(reason);
        for (unsigned i=0; i<g_ram_mtrrs.count; ++i) {
            ksnprintf(reason, sizeof(reason), "[RAM_MAP] CACHE_INPUT index=%u base=%016llx mask=%016llx\n",
                i, (unsigned long long)g_ram_mtrrs.base[i], (unsigned long long)g_ram_mtrrs.mask[i]);
            serial_write_all(reason);
        }
        for (unsigned i=0; i<g_physical_contract.count; ++i) {
            const physical_region_t *region = &g_physical_contract.regions[i];
            ksnprintf(reason, sizeof(reason),
                "[RAM_MAP] INPUT_REGION index=%u type=%u first=%016llx end=%016llx attributes=%016llx\n",
                i, region->type, (unsigned long long)region->first,
                (unsigned long long)region->end, (unsigned long long)region->attributes);
            serial_write_all(reason);
        }
        kpanic("RAM descriptor or cache boundary plan is invalid");
    }
    if (!pmm_direct_map_ready(&g_ram_plan)) kpanic("RAM bitmap storage is not covered by the candidate plan");
    if (!pmm_reserve_boot_framebuffer()) kpanic("RAM framebuffer reservation conflicts with bitmap storage");
    if (!image_permissions_from_linker(&g_image_permissions,&g_physical_contract))
        kpanic("Linked image permission boundaries are invalid");
    uint64_t cr0,efer;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    if (cr0!=CPU_CR0_FINAL || !cpu_read_msr_safe(CPU_MSR_EFER,&efer) ||
        efer!=(uint64_t)(CPU_EFER_LME|CPU_EFER_LMA|(features.nx ? CPU_EFER_NXE : 0)))
        kpanic("Image protection requires the established local WP and NX controls");
    const mapping_table_ops_t ops = {boot_table_allocate, boot_table_release};
    const stack_layout_t *stacks=kernel_stack_prepare(&g_image_permissions,&g_ram_plan);
    if (!stacks) kpanic("Guarded stack preparation failed before root construction");
    if (!paging_build_create(&g_boot_build, &g_physical_contract, &g_ram_plan, &g_image_permissions,stacks,&ops, NULL))
        kpanic("Unpublished RAM page-table construction failed");
    const Tss64 *tss = tss_get();
    const paging_required_path_t paths[] = {
        {(uintptr_t)&init_paging, 1, true,false}, {(uintptr_t)bsp_stack_bottom, 16384, false,true},
        {gdtr.base, (uint64_t)gdtr.limit+1, false,true}, {idtr.base, (uint64_t)idtr.limit+1, false,true},
        {(uintptr_t)tss, sizeof(*tss), false,true},
        {tss->ist1-32768, 32768, false,true}, {tss->ist2-32768, 32768, false,true},
        {tss->ist3-32768, 32768, false,true},
        {g_image_permissions.input.sections[IMAGE_TEXT].first+KERNEL_IMAGE_OFFSET,
         g_image_permissions.input.sections[IMAGE_TEXT].end-g_image_permissions.input.sections[IMAGE_TEXT].first,true,false}
    };
    ram_extent_t framebuffer = {fb_base & ~UINT64_C(4095), (fb_base+fb_size+4095) & ~UINT64_C(4095)};
    if (!paging_build_verify(&g_boot_build, &g_physical_contract, &g_ram_mtrrs, framebuffer,
                            paths, sizeof(paths)/sizeof(paths[0]))) {
        paging_build_abort(&g_boot_build);
        kpanic("Unpublished RAM root failed independent validation");
    }
    char line[320];
    int n = ksnprintf(line, sizeof(line),
        "[RAM_MAP] PRELOAD verified=1 root=%016llx tables=%u ram_bytes=%llu leaves_4k=%llu leaves_2m=%llu leaves_1g=%llu nx=%u physical_bits=%u\n",
        (unsigned long long)g_boot_build.tables[0].table.physical, g_boot_build.count,
        (unsigned long long)g_boot_build.direct_bytes, (unsigned long long)g_boot_build.leaves[0],
        (unsigned long long)g_boot_build.leaves[1], (unsigned long long)g_boot_build.leaves[2],
        (unsigned)features.nx, (unsigned)features.physical_bits);
    if (n <= 0 || (size_t)n >= sizeof(line)) kpanic("RAM preload record overflow");
    serial_write_all(line);
    n=ksnprintf(line,sizeof(line),"[IMAGE_PROTECTION] PRELOAD verified=1 cr0=%016llx efer=%016llx sections=%u write=ENFORCED execute=%s\n",
        (unsigned long long)cr0,(unsigned long long)efer,IMAGE_ROLE_COUNT,features.nx ? "ENFORCED" : "UNAVAILABLE_NX");
    if (n<=0 || (size_t)n>=sizeof(line)) kpanic("Image preload record overflow");
    serial_write_all(line);
    n = ksnprintf(line, sizeof(line),
        "[LOW_ROOT] PRELOAD low_slots=256 empty=1 rsp=%016llx stack_first=%016llx stack_bytes=16384 gdtr=%016llx idtr=%016llx tss=%016llx\n",
        (unsigned long long)stack, (unsigned long long)(uintptr_t)bsp_stack_bottom,
        (unsigned long long)gdtr.base, (unsigned long long)idtr.base,
        (unsigned long long)(uintptr_t)tss);
    if (n <= 0 || (size_t)n >= sizeof(line)) kpanic("Low-root preload record overflow");
    serial_write_all(line);
    if (!paging_build_activate(&g_boot_build, boot_root_load, NULL)) kpanic("RAM root was not verified");
    uint64_t actual;
    __asm__ volatile("mov %%cr3,%0" : "=r"(actual));
    g_kernel_root_physical = g_boot_build.tables[0].table.physical;
    if (actual != g_kernel_root_physical) kpanic("RAM root activation readback differs");
    /* Verify actual backing through the new alias before replacing any owner.
     * Reads do not touch device bytes or alter table entries. */
    for (unsigned i = 0; i < g_boot_build.count; ++i) {
        mapping_table_t *table = &g_boot_build.tables[i].table;
        uint64_t pointer;
        if (!ram_map_pointer(&g_ram_plan, table->physical, PAGE_SIZE, &pointer))
            kpanic("Construction table is outside verified RAM");
        if (paging_table_hash((const PageTable *)(uintptr_t)pointer) != g_boot_build.tables[i].verified_hash)
            kpanic("RAM alias table readback differs");
        table->cpu = (PageTable *)(uintptr_t)pointer;
    }
    for (unsigned i = 0; i < MAPPING_WINDOW_TABLES; ++i)
        g_boot_build.window.tables[i].cpu = (PageTable *)(uintptr_t)(RAM_DIRECT_BASE+g_boot_build.window.tables[i].physical);
    for (unsigned i=0;i<STACK_WINDOW_TABLES;++i)
        g_boot_build.stacks.tables[i].cpu=(PageTable *)(uintptr_t)(RAM_DIRECT_BASE+g_boot_build.stacks.tables[i].physical);
    g_owned_window = g_boot_build.window;
    g_kernel_pml4 = g_boot_build.tables[0].table.cpu;
    if (!pmm_direct_map_activate(&g_ram_plan)) kpanic("RAM bitmap and coverage publication failed");
    if (!physical_translation_retire_identity()) kpanic("Identity boundary retirement failed");
    if (!kernel_stack_activate()) kpanic("BSP guarded stack or IST handoff failed");
    g_boot_requirements = (paging_boot_requirements_t){
        g_boot_build.leaves[2] ? CPU_FEATURE_PAGE1G : 0, features.physical_bits};
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    g_observer_limit = g_ram_plan.ceiling;
    for (unsigned i = 0; i < g_boot_build.count; ++i)
        g_observer_tables[g_observer_count++] = g_boot_build.tables[i].table.physical;
    g_observer_initial = g_observer_count;
    ++g_observer_epoch;
#endif
    n = ksnprintf(line, sizeof(line),
        "[RAM_MAP] ACTIVE root=%016llx base=%016llx span=%016llx ceiling=%016llx epoch=%llu bitmap_rebased=1 identity_retained=0 nx=%s\n",
        (unsigned long long)actual, (unsigned long long)RAM_DIRECT_BASE, (unsigned long long)RAM_DIRECT_BYTES,
        (unsigned long long)g_ram_plan.ceiling, (unsigned long long)g_physical_contract.epoch,
        features.nx ? "ENABLED" : "NX_UNAVAILABLE");
    if (n <= 0 || (size_t)n >= sizeof(line)) kpanic("RAM activation record overflow");
    serial_write_all(line);
    __asm__ volatile("mov %%rsp,%0" : "=r"(stack));
    n = ksnprintf(line, sizeof(line), "[LOW_ROOT] ACTIVE root=%016llx rsp=%016llx low_slots=256 empty=1 table_readback=COMPLETE\n",
        (unsigned long long)actual, (unsigned long long)stack);
    if (n <= 0 || (size_t)n >= sizeof(line)) kpanic("Low-root activation record overflow");
    serial_write_all(line);
    if (!paging_image_boot_check("ACTIVATION")) kpanic("Activated image protection scan failed");
}

#ifdef HOBBYOS_SELFTEST
typedef struct {
    ram_leaf_run_t pending;
    ram_leaf_consumer_t consume;
    void *context;
} ram_walk_t;
static bool ram_walk_table(uint64_t physical, unsigned level, uint64_t base, ram_walk_t *walk)
{
    const PageTable *table = table_pointer(physical);
    if (!table) return false;
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t raw = __atomic_load_n(&table->entries[i], __ATOMIC_RELAXED);
        if (!raw) continue;
        paging_entry_view_t entry;
        if (!paging_entry_decode(&g_ram_plan.features, level, raw, &entry)) return false;
        uint64_t va = base + ((uint64_t)i << (12+9*(level-1)));
        if (!entry.leaf) {
            if (!ram_walk_table(entry.physical, level-1, va, walk)) return false;
            continue;
        }
        uint64_t flags = raw & ~PAGE_ADDR_MASK & ~UINT64_C(0x60);
        /* Large-leaf PAT is an attribute at bit 12, not an address bit. */
        if (level > 1) flags |= raw & UINT64_C(0x1000);
        unsigned shift = 12+9*(level-1);
        ram_leaf_run_t *p = &walk->pending;
        if (p->bytes && p->shift == shift && p->flags == flags &&
            p->virtual_address+p->bytes == va && p->physical+p->bytes == entry.physical) {
            p->bytes += entry.bytes; continue;
        }
        if (p->bytes && !walk->consume(walk->context,p)) return false;
        *p = (ram_leaf_run_t){va, entry.physical, entry.bytes, flags, shift};
    }
    return true;
}

bool paging_ram_snapshot(ram_map_snapshot_t *out, ram_leaf_consumer_t consume, void *context)
{
    if (!out || !consume || !g_boot_build.activated || !g_physical_contract.direct_map) return false;
    uint64_t active;
    __asm__ volatile("mov %%cr3,%0" : "=r"(active));
    if (active != g_kernel_root_physical) return false;
    ram_walk_t walk = {.consume=consume,.context=context};
    for (unsigned i = 256; i < 384; ++i) {
        uint64_t raw = __atomic_load_n(&g_kernel_pml4->entries[i], __ATOMIC_RELAXED);
        if (!raw) continue;
        paging_entry_view_t entry;
        if (!paging_entry_decode(&g_ram_plan.features,4,raw,&entry) || entry.leaf ||
            entry.nx != g_ram_plan.features.nx || !ram_walk_table(entry.physical,3,
                RAM_DIRECT_BASE+((uint64_t)(i-256)<<39),&walk)) return false;
    }
    if (walk.pending.bytes && !consume(context,&walk.pending)) return false;
    *out = (ram_map_snapshot_t){&g_ram_plan,&g_ram_mtrrs,active,(uintptr_t)g_kernel_pml4,
                               g_physical_contract.epoch,g_boot_build.count};
    return true;
}

bool paging_ram_scratch_table(uint64_t frame, uint64_t data, uint64_t *cpu, uint64_t *entry)
{
    if (!cpu || !entry || frame < PHYSICAL_ENTRY_LIMIT || data < PHYSICAL_ENTRY_LIMIT ||
        frame == data || (frame & 4095) || (data & 4095) ||
        pmm_is_frame_free(frame) || pmm_is_frame_free(data)) return false;
    PageTable *table = table_pointer(frame);
    if (!table) return false;
    uint64_t encoded;
    if (!paging_entry_encode(&g_ram_plan.features,1,true,data,0,true,g_ram_plan.features.nx,&encoded)) return false;
    memset(table,0,PAGE_SIZE);
    table->entries[511] = encoded;
    paging_entry_view_t decoded;
    uint64_t raw = __atomic_load_n(&table->entries[511],__ATOMIC_ACQUIRE);
    bool ok = paging_entry_decode(&g_ram_plan.features,1,raw,&decoded) && decoded.physical == data;
    table->entries[511] = 0;
    if (ok) { *cpu=(uintptr_t)table; *entry=raw; }
    return ok;
}

/* These entries belong exclusively to two fixed test windows. Tables remain
 * registered and owned for the boot; data frames are separately retired after
 * acknowledged invalidation. No caller-supplied virtual address is accepted. */
static PageEntry *g_tlb_fixture_entries[TLB_FIXTURE_WINDOWS][TLB_FIXTURE_PAGES];
static uint64_t g_tlb_fixture_frames[TLB_FIXTURE_WINDOWS][TLB_FIXTURE_PAGES];
static PageTable *g_tlb_fixture_root;
static uint32_t g_tlb_fixture_tables;
#define FIXTURE_STRUCTURE_CAPACITY (TLB_FIXTURE_WINDOWS*(3+(TLB_FIXTURE_PAGES+511)/512))
static uint64_t g_tlb_fixture_structure[FIXTURE_STRUCTURE_CAPACITY];
static uint8_t g_tlb_fixture_levels[FIXTURE_STRUCTURE_CAPACITY];

static bool fixture_table(uint64_t frame)
{
    if (!frame || (frame & 4095) || frame >= g_observer_limit) return false;
    for (unsigned index = 0; index < g_observer_count; ++index)
        if (g_observer_tables[index] == frame) return true;
    return false;
}

static bool fixture_frame(uint64_t frame)
{
    return frame && !(frame & 4095) && frame < physical_allocation_ceiling() &&
        frame < pmm_get_total_memory() && !pmm_is_frame_free(frame) &&
        !image_permissions_overlaps(&g_image_permissions,frame,PAGE_SIZE);
}

typedef struct { uint64_t frame; PageTable *pointer; } fixture_prepared_table_t;
#define FIXTURE_PREPARE_CAPACITY (2u+(PT_INDEX(TLB_FIXTURE_BASE)+TLB_FIXTURE_PAGES+511u)/512u)
_Static_assert(PDPT_INDEX(TLB_FIXTURE_BASE)==
    PDPT_INDEX(TLB_FIXTURE_BASE+(TLB_FIXTURE_PAGES-1u)*PAGE_SIZE),
    "fixed fixture window must stay within one PDPT entry");
_Static_assert((TLB_FIXTURE_STRIDE & ((UINT64_C(1)<<30)-1)) == 0 &&
    PML4_INDEX(TLB_FIXTURE_BASE)==PML4_INDEX(TLB_FIXTURE_BASE+
        (TLB_FIXTURE_WINDOWS-1u)*TLB_FIXTURE_STRIDE+(TLB_FIXTURE_PAGES-1u)*PAGE_SIZE),
    "fixed fixture windows must share the declared root entry");

static bool fixture_prepare_table(fixture_prepared_table_t *out)
{
    uint64_t frame = (uint64_t)pmm_alloc_frame();
    if (!frame) return false;
    if ((frame & 4095) || frame >= g_observer_limit ||
        g_observer_limit - frame < PAGE_SIZE) {
        pmm_free_frame((void *)frame);
        return false;
    }
    PageTable *pointer = table_pointer(frame);
    if (!pointer) { pmm_free_frame((void *)frame); return false; }
    memset(pointer, 0, PAGE_SIZE);
    *out = (fixture_prepared_table_t){frame, pointer};
    return true;
}

static PageTable *fixture_publish_table(fixture_prepared_table_t *prepared,
                                      unsigned count, unsigned *used, unsigned level)
{
    if (*used >= count || g_observer_count >= PAGING_OBSERVER_CAPACITY ||
        g_tlb_fixture_tables >= FIXTURE_STRUCTURE_CAPACITY) return NULL;
    fixture_prepared_table_t *item = &prepared[(*used)++];
    g_observer_tables[g_observer_count++] = item->frame;
    g_tlb_fixture_structure[g_tlb_fixture_tables] = item->frame;
    g_tlb_fixture_levels[g_tlb_fixture_tables++] = level;
    return item->pointer;
}

static uint64_t fixture_data_flags(void)
{ return PAGE_PRESENT|PAGE_RW|(g_ram_plan.features.nx ? PAGE_NX : 0); }

/* Tables are retained for the boot, so concurrent preparation can only
 * reduce this missing-table bound. Publication rechecks every entry. */
static bool fixture_missing_tables(unsigned window, unsigned *missing)
{
    uint64_t previous[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    PageEntry root = g_kernel_pml4->entries[PML4_INDEX(TLB_FIXTURE_BASE)];
    if (g_tlb_fixture_root ?
        (root & ~PAGE_ACCESSED) != (physical_require_address(g_tlb_fixture_root, PAGE_SIZE) | fixture_data_flags()) : root != 0)
        return false;
    *missing = 0;
    for (unsigned page = 0; page < TLB_FIXTURE_PAGES; ++page) {
        uint64_t va = TLB_FIXTURE_BASE + window * TLB_FIXTURE_STRIDE + page * 4096u;
        PageTable *table = g_kernel_pml4;
        for (unsigned level = 0; level < 3; ++level) {
            unsigned shift = 39u-9u*level;
            PageEntry entry = table ? table->entries[(va >> shift) & 511u] : 0;
            if (!entry) {
                uint64_t key = va >> shift;
                if (key != previous[level]) { ++*missing; previous[level] = key; }
                table = NULL;
            } else {
                if ((entry & ~(PAGE_ADDR_MASK | PAGE_ACCESSED)) != fixture_data_flags() ||
                    !fixture_table(entry & PAGE_ADDR_MASK)) return false;
                table = table_pointer(entry & PAGE_ADDR_MASK);
                if (!table) return false;
            }
        }
        PageEntry *leaf = table ? &table->entries[PT_INDEX(va)] : NULL;
        if ((leaf && *leaf) || g_tlb_fixture_frames[window][page] ||
            (g_tlb_fixture_entries[window][page] && g_tlb_fixture_entries[window][page] != leaf))
            return false;
    }
    return *missing <= FIXTURE_PREPARE_CAPACITY;
}

bool paging_tlb_fixture_prepare(unsigned window, uint64_t frame)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (window >= TLB_FIXTURE_WINDOWS || !fixture_frame(frame)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    unsigned missing;
    bool ready = fixture_missing_tables(window, &missing);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    if (!ready) return false;
    fixture_prepared_table_t prepared[FIXTURE_PREPARE_CAPACITY];
    unsigned count = 0, used = 0;
    while (count < missing && fixture_prepare_table(&prepared[count])) ++count;
    if (count < missing) {
        for (unsigned i = 0; i < count; ++i) pmm_free_frame((void *)prepared[i].frame);
        return false;
    }
    flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    PageEntry root = g_kernel_pml4->entries[PML4_INDEX(TLB_FIXTURE_BASE)];
    bool valid = g_tlb_fixture_root ?
        (root & ~PAGE_ACCESSED) == (physical_require_address(g_tlb_fixture_root, PAGE_SIZE) | fixture_data_flags()) : root == 0;
    for (unsigned page = 0; valid && page < TLB_FIXTURE_PAGES; ++page) {
        uint64_t va = TLB_FIXTURE_BASE + window * TLB_FIXTURE_STRIDE + page * 4096u;
        const unsigned indices[] = {PML4_INDEX(va), PDPT_INDEX(va), PD_INDEX(va), PT_INDEX(va)};
        PageTable *table = g_kernel_pml4;
        for (unsigned level = 0; valid && level < 3; ++level) {
            PageEntry *entry = &table->entries[indices[level]];
            if (!(*entry & PAGE_PRESENT)) {
                if (*entry) { valid = false; break; }
                PageTable *child = fixture_publish_table(prepared, count, &used, 3-level);
                if (!child) { valid = false; break; }
                *entry = physical_require_address(child, PAGE_SIZE) | fixture_data_flags();
                if (level == 0) g_tlb_fixture_root = child;
            }
            if ((*entry & ~(PAGE_ADDR_MASK | PAGE_ACCESSED)) != (fixture_data_flags())) {
                valid = false; break;
            }
            if (!fixture_table(*entry & PAGE_ADDR_MASK)) { valid = false; break; }
            table = table_pointer(*entry & PAGE_ADDR_MASK);
            if (!table) { valid = false; break; }
        }
        if (!valid) break;
        PageEntry *leaf = &table->entries[indices[3]];
        if (*leaf || g_tlb_fixture_frames[window][page] ||
            (g_tlb_fixture_entries[window][page] && g_tlb_fixture_entries[window][page] != leaf)) {
            valid = false; break;
        }
        g_tlb_fixture_entries[window][page] = leaf;
    }
    if (valid) {
        for (unsigned page = 0; page < TLB_FIXTURE_PAGES; ++page) {
            *g_tlb_fixture_entries[window][page] = frame | fixture_data_flags();
            g_tlb_fixture_frames[window][page] = frame;
        }
        ++g_observer_epoch;
    }
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    for (unsigned i = used; i < count; ++i) pmm_free_frame((void *)prepared[i].frame);
    return valid;
}

bool paging_tlb_fixture_replace(unsigned window, unsigned pages,
    uint64_t expected, uint64_t replacement, uint64_t *old_entry, uint64_t *new_entry)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (window >= TLB_FIXTURE_WINDOWS || !pages || pages > TLB_FIXTURE_PAGES ||
        !old_entry || !new_entry || !fixture_frame(replacement) ||
        (expected && !fixture_frame(expected))) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    bool valid = true;
    for (unsigned page = 0; page < pages; ++page) {
        PageEntry *leaf = g_tlb_fixture_entries[window][page];
        uint64_t frame = g_tlb_fixture_frames[window][page];
        if (!leaf || !frame || (expected && frame != expected) ||
            (*leaf & ~(PAGE_ACCESSED | PAGE_DIRTY)) != (frame | fixture_data_flags())) valid = false;
    }
    if (valid) {
        *old_entry = *g_tlb_fixture_entries[window][0];
        for (unsigned page = 0; page < pages; ++page) {
            PageEntry *leaf = g_tlb_fixture_entries[window][page];
            *leaf = (*leaf & ~PAGE_ADDR_MASK) | replacement;
            g_tlb_fixture_frames[window][page] = replacement;
        }
        *new_entry = *g_tlb_fixture_entries[window][0];
        ++g_observer_epoch;
    }
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return valid;
}

bool paging_tlb_fixture_clear(unsigned window)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_MAPPING) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    if (window >= TLB_FIXTURE_WINDOWS) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    bool valid = true;
    for (unsigned page = 0; page < TLB_FIXTURE_PAGES; ++page) {
        PageEntry *leaf = g_tlb_fixture_entries[window][page];
        if (!leaf || (*leaf & ~(PAGE_ACCESSED | PAGE_DIRTY)) !=
            (g_tlb_fixture_frames[window][page] | fixture_data_flags())) valid = false;
    }
    if (valid) {
        for (unsigned page = 0; page < TLB_FIXTURE_PAGES; ++page) {
            *g_tlb_fixture_entries[window][page] = 0;
            g_tlb_fixture_frames[window][page] = 0;
        }
        ++g_observer_epoch;
    }
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return valid;
}

uint64_t paging_tlb_fixture_entry(unsigned window)
{ return window < TLB_FIXTURE_WINDOWS && g_tlb_fixture_entries[window][0] ?
    physical_require_address(g_tlb_fixture_entries[window][0], sizeof(PageEntry)) : 0; }

uint32_t paging_tlb_fixture_tables(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    uint32_t count = g_tlb_fixture_tables;
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return count;
}
#endif

#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
/*
 * PMM allocates only conventional RAM. Every recorded frame was initialized by
 * alloc_table and is never freed. The existing low identity map is the access
 * contract, not an assumption that all low physical addresses are RAM. Any
 * failed allocation, registry overflow, or nonidentity low remap closes access.
 * Only the current kernel root and its known shared bootstrap clone qualify.
 */
static bool observer_root(uint64_t root)
{
    return !g_observer_failed && !g_observer_nonidentity &&
        (root == g_kernel_root_physical ||
         (g_observer_boot_root && root == physical_require_address(g_observer_boot_root, PAGE_SIZE)));
}

static bool observer_table(uint64_t address)
{
    if (!address || (address & 4095) || g_observer_limit < 4096 ||
        address > g_observer_limit - 4096)
        return false;
    for (uint32_t i = 0; i < g_observer_count; i++)
        if (g_observer_tables[i] == address) return true;
    return false;
}

static bool observer_read(void *context, uint64_t table, uint16_t index,
                           uint64_t *value)
{
    (void)context;
    if (index >= 512 || !observer_table(table)) return false;
    PageTable *pointer = table_pointer(table);
    if (!pointer) return false;
    *value = __atomic_load_n(&pointer->entries[index],
                             __ATOMIC_RELAXED);
    return true;
}

typedef struct { uint64_t table[4], raw[4]; uint16_t index[4]; uint8_t count; } observer_walk_t;

static bool observer_cached(void *context, uint64_t table, uint16_t index,
                             uint64_t *value)
{
    const observer_walk_t *walk = context;
    for (uint8_t i = 0; i < walk->count; i++)
        if (walk->table[i] == table && walk->index[i] == index)
        { *value = walk->raw[i]; return true; }
    return false;
}

void paging_inspect(uint64_t va, cpu_control_sample_t *cpu, pt_result_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    observer_walk_t walk = {0};
    irq_flags_t pin = irq_save();
    cpu_control_read_local(cpu);
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    uint64_t table = cpu->paging.cr3 & PAGE_ADDR_MASK;
    if (observer_root(table) && !(cpu->paging.cr4 & (1u << 12)) && pt_canonical(va, 48))
    {
        for (unsigned level = 4; level; level--)
        {
            uint16_t index = (uint16_t)((va >> (12 + 9 * (level - 1))) & 511);
            uint64_t raw;
            if (!observer_read(0, table, index, &raw)) break;
            unsigned i = walk.count++;
            walk.table[i] = table; walk.index[i] = index; walk.raw[i] = raw;
            if (!(raw & 1) || level == 1 || (raw & PAGE_HUGE)) break;
            /* Reject reserved high address bits before following a child. */
            unsigned width = cpu->paging.physical_bits;
            if (width < 32 || width > 52 ||
                (raw & PAGE_ADDR_MASK & ~((UINT64_C(1) << width) - 1)) ||
                ((raw & PAGE_NX) && !(cpu->paging.efer & (1u << 11)))) break;
            table = raw & PAGE_ADDR_MASK;
        }
    }
    spin_unlock_irqrestore(&g_paging_lock, flags);
    irq_restore(pin);
    page_table_decode(va, &cpu->paging, observer_cached, &walk, out);
}

uint64_t paging_inspect_initial_allocations(void) { return g_observer_initial; }

bool paging_inspect_copy_physical(uint64_t address, void *out, uint32_t size)
{
    if (!out || !size || size > 256 || address > UINT64_MAX - size) return false;
    void *pointer;
    if (!physical_pointer_range(address, size, PHYSICAL_RAM, &pointer)) return false;
    cpu_control_sample_t cpu;
    pt_result_t result;
    paging_inspect((uintptr_t)pointer, &cpu, &result);
    if (result.status != PT_MAPPED || result.physical != address) return false;
    paging_inspect((uintptr_t)pointer + size - 1, &cpu, &result);
    if (result.status != PT_MAPPED || result.physical != address + size - 1) return false;
    memcpy(out, pointer, size);
    return true;
}

bool paging_inspect_tables(paging_table_count_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    uint64_t *queue = g_observer_reachable;
    static uint8_t levels[PAGING_OBSERVER_CAPACITY];
    static uint32_t owner;
    uint32_t expected = 0;
    *out = (paging_table_count_t){0};
    if (!__atomic_compare_exchange_n(&owner, &expected, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    out->root = g_kernel_root_physical;
    out->epoch_before = g_observer_epoch;
    out->allocations = g_observer_count;
    out->extra_roots = g_observer_boot_root ? 1 : 0;
    bool ok = observer_root(out->root);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    uint32_t count = ok ? 1 : 0;
    queue[0] = out->root; levels[0] = 4;
    for (uint32_t cursor = 0; ok && cursor < count; cursor++)
    {
        flags = spin_lock_irqsave(&g_paging_lock);
        ok = observer_table(queue[cursor]);
        spin_unlock_irqrestore(&g_paging_lock, flags);
        if (!ok) break;
        if (levels[cursor] == 1) continue;
        for (uint16_t index = 0; ok && index < 512; index++)
        {
            uint64_t raw;
            /* Each critical section reads one entry; A/D do not affect topology. */
            flags = spin_lock_irqsave(&g_paging_lock);
            ok = observer_read(0, queue[cursor], index, &raw);
            spin_unlock_irqrestore(&g_paging_lock, flags);
            if (!ok || !(raw & PAGE_PRESENT)) continue;
            if (raw & PAGE_HUGE)
            {
                if (levels[cursor] == 4) ok = false;
                continue;
            }
            uint64_t child = raw & PAGE_ADDR_MASK;
            uint32_t at;
            for (at = 0; at < count && queue[at] != child; at++) ;
            if (at < count)
            {
                if (levels[at] != levels[cursor] - 1) ok = false;
                continue;
            }
            if (count == PAGING_OBSERVER_CAPACITY) { ok = false; break; }
            queue[count] = child; levels[count++] = levels[cursor] - 1;
        }
    }
    flags = spin_lock_irqsave(&g_paging_lock);
    out->epoch_after = g_observer_epoch;
    spin_unlock_irqrestore(&g_paging_lock, flags);
    out->reachable = count;
    out->consistent = out->epoch_before == out->epoch_after;
    out->complete = ok && out->consistent;
    __atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
    return out->complete;
}
#endif

static bool owned_table_allocate(void *context, mapping_table_t *out)
{
    (void)context;
    void *physical = pmm_alloc_frame(), *cpu;
    if (!physical) return false;
    if (!physical_pointer_range((uintptr_t)physical, PAGE_SIZE, PHYSICAL_RAM, &cpu)) {
        pmm_free_frame(physical); return false;
    }
    *out = (mapping_table_t){cpu, (uintptr_t)physical};
    return true;
}
static void owned_table_release(void *context, mapping_table_t table)
{ (void)context; pmm_free_frame((void *)(uintptr_t)table.physical); }

bool paging_owned_initialize(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    const mapping_table_ops_t ops = {owned_table_allocate, owned_table_release};
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (g_observer_count > PAGING_OBSERVER_CAPACITY - MAPPING_WINDOW_TABLES) {
        __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags); return false;
    }
#endif
    bool ok = mapping_window_construct(&g_owned_window, g_kernel_pml4, &ops, NULL,g_ram_plan.features.nx) == MAPPING_OK;
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (ok) {
        for (unsigned i = 0; i < MAPPING_WINDOW_TABLES; ++i)
            g_observer_tables[g_observer_count++] = g_owned_window.tables[i].physical;
        ++g_observer_epoch;
    }
#endif
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ok;
}

bool paging_owned_install(uint64_t first, uint64_t physical, uint64_t pages, uint64_t attributes)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!pages || pages>PHYSICAL_ADDRESS_LIMIT/PAGE_SIZE ||
        !paging_owned_extent_allowed(physical,pages*PAGE_SIZE)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    bool ok = mapping_window_install(&g_owned_window, first, physical, pages, attributes) == MAPPING_OK;
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (ok) ++g_observer_epoch;
#endif
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ok;
}

bool paging_owned_extent_allowed(uint64_t physical, uint64_t bytes)
{ return !image_permissions_overlaps(&g_image_permissions,physical,bytes); }

typedef struct { image_leaf_consumer_t consume; image_owner_consumer_t owners; void *context; } image_reader_context_t;
static const PageTable *image_main_table(void *context,uint64_t frame,unsigned level)
{
    (void)context;
    for (unsigned i=0;i<g_boot_build.count;++i)
        if (g_boot_build.tables[i].table.physical==frame)
            return g_boot_build.tables[i].level==level ? g_boot_build.tables[i].table.cpu : NULL;
#ifdef HOBBYOS_SELFTEST
    for (unsigned i=0;i<g_tlb_fixture_tables;++i)
        if (g_tlb_fixture_structure[i]==frame)
            return g_tlb_fixture_levels[i]==level ? table_pointer(frame) : NULL;
#endif
    return NULL;
}
static bool image_consume(void *context,const image_leaf_run_t *run)
{
    image_reader_context_t *reader=context;
    return !reader->consume || reader->consume(reader->context,run);
}

static bool image_consume_owner(void *context,const stack_owner_view_t *owner)
{
    image_reader_context_t *reader=context;
    return !reader->owners || reader->owners(reader->context,owner);
}

bool paging_image_snapshot_owners(image_walk_result_t *out,image_leaf_consumer_t consume,
                                 image_owner_consumer_t owners,void *context)
{
    static image_walk_workspace_t workspace;
    static uint32_t owner;
    uint32_t free=0;
    if (!out || !g_boot_build.activated || !image_permissions_verify(&g_image_permissions,&g_physical_contract) ||
        !__atomic_compare_exchange_n(&owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return false;
    uint64_t before=__atomic_load_n(&g_image_mapping_epoch,__ATOMIC_ACQUIRE),root;
    __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    image_reader_context_t reader={consume,owners,context};
    image_walk_inputs_t in={.permissions=&g_image_permissions,.ram=&g_ram_plan,.stacks=kernel_stack_layout(),
        .table=image_main_table,.consume=image_consume,.owner_consume=image_consume_owner,.context=&reader};
#ifdef HOBBYOS_SELFTEST
    for (unsigned i=0;i<TLB_FIXTURE_WINDOWS;++i) in.scratch[i]=(ram_extent_t){
        TLB_FIXTURE_BASE+i*TLB_FIXTURE_STRIDE,TLB_FIXTURE_BASE+i*TLB_FIXTURE_STRIDE+TLB_FIXTURE_PAGES*4096};
#endif
    image_walk_result_t result;
    bool ok=!(before&1) && root==g_kernel_root_physical && image_walk_main(&in,root,&workspace,&result) &&
        before==__atomic_load_n(&g_image_mapping_epoch,__ATOMIC_ACQUIRE);
    if (ok) *out=result;
    __atomic_store_n(&owner,0,__ATOMIC_RELEASE);return ok;
}

bool paging_image_snapshot(image_walk_result_t *out,image_leaf_consumer_t consume,void *context)
{ return paging_image_snapshot_owners(out,consume,NULL,context); }

const image_permissions_t *paging_image_plan(void)
{ return g_boot_build.activated ? &g_image_permissions : NULL; }

bool paging_stack_install(stack_handle_t h,bool prepared)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    const stack_layout_t *layout=kernel_stack_layout();
    if (!layout || h.slot>=STACK_REGION_SLOTS || !g_boot_build.activated) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    stack_owner_view_t v;
    bool ok=kernel_stack_reserved_backing(h,&v) &&
        stack_backing_allowed(layout,&g_image_permissions,&g_ram_plan,&v);
    if (ok) ok=stack_window_install(&g_boot_build.stacks,&v) &&
        stack_owner_mapped(&layout->owners[h.slot],h,prepared);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock,flags);return ok;
}

bool paging_stack_remove(stack_handle_t h,uint64_t id,uint64_t lifecycle,bool unpublished)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_MAPPING) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    const stack_layout_t *layout=kernel_stack_layout();
    if (!layout || h.slot>=STACK_REGION_SLOTS || !g_boot_build.activated) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    stack_owner_view_t v;
    bool ok=stack_owner_read(&layout->owners[h.slot],&v) && v.generation==h.generation &&
        stack_backing_allowed(layout,&g_image_permissions,&g_ram_plan,&v) &&
        stack_window_check(&g_boot_build.stacks,g_kernel_pml4,&v) &&
        stack_owner_begin_retire(&layout->owners[h.slot],h,id,lifecycle,unpublished);
    if (ok) {v.state=STACK_RETIRING;ok=stack_window_remove(&g_boot_build.stacks,&v);}
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock,flags);return ok;
}

bool paging_stack_validate(stack_handle_t h)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    const stack_layout_t *layout=kernel_stack_layout();
    if (!layout || h.slot>=STACK_OWNER_COUNT || !g_boot_build.activated) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_paging_lock);
    stack_owner_view_t v;bool ok=stack_owner_read(&layout->owners[h.slot],&v) && v.generation==h.generation;
    if (ok && h.slot<STACK_REGION_SLOTS)
        ok=stack_backing_allowed(layout,&g_image_permissions,&g_ram_plan,&v) &&
            stack_window_check(&g_boot_build.stacks,g_kernel_pml4,&v);
    else if (ok) {
        uint64_t physical;bool writable,nx;
        ok=v.guard==g_image_permissions.input.bsp_guard_first+KERNEL_IMAGE_OFFSET &&
            !paging_build_rights(&g_boot_build,v.guard,&physical,&writable,&nx);
        for (uint64_t va=v.base;ok && va<v.end;va+=4096)
            ok=paging_build_rights(&g_boot_build,va,&physical,&writable,&nx) && writable &&
                nx==g_ram_plan.features.nx && physical==v.physical+va-v.base;
    }
    spin_unlock_irqrestore(&g_paging_lock,flags);return ok;
}

bool paging_image_target(uint64_t linked,uint64_t bytes,image_role_t role,
                         uint64_t *physical,uint64_t *direct,bool *nx)
{
    if (!g_boot_build.activated || !physical || !direct || !nx || role>=IMAGE_ROLE_COUNT ||
        !bytes || linked<g_image_permissions.linked_first || linked>=g_physical_contract.image_end ||
        bytes>g_physical_contract.image_end-linked) return false;
    const image_section_t *section=&g_image_permissions.input.sections[role];
    uint64_t first=linked-KERNEL_IMAGE_OFFSET,alias;
    if (first<section->first || first>=section->end || bytes>section->end-first ||
        !ram_map_pointer(&g_ram_plan,first,bytes,&alias)) return false;
    for (uint64_t at=0;at<bytes;) {
        uint64_t a,b;bool aw,an,bw,bn;
        if (!paging_build_rights(&g_boot_build,linked+at,&a,&aw,&an) ||
            !paging_build_rights(&g_boot_build,alias+at,&b,&bw,&bn) || a!=first+at || a!=b ||
            !image_permissions_check_leaf(&g_image_permissions,(linked+at)&~UINT64_C(4095),a&~UINT64_C(4095),4096,aw,an,g_ram_plan.features.nx) ||
            !image_permissions_check_leaf(&g_image_permissions,(alias+at)&~UINT64_C(4095),b&~UINT64_C(4095),4096,bw,bn,g_ram_plan.features.nx)) return false;
        uint64_t chunk=4096-((linked+at)&4095);if (chunk>bytes-at) chunk=bytes-at;at+=chunk;
    }
    *physical=first;*direct=alias;*nx=g_ram_plan.features.nx;return true;
}

bool paging_image_boot_check(const char *stage)
{
    image_walk_result_t snapshot;
    if (!stage || !paging_image_snapshot(&snapshot,NULL,NULL)) return false;
    char line[384];
    int n=ksnprintf(line,sizeof(line),
        "[IMAGE_PROTECTION] SCAN stage=%s root=%016llx tables=%llu leaves=%llu bytes=%llu image_bytes=%llu direct_bytes=%llu managed_bytes=%llu scratch_bytes=%llu rwx_bytes=%llu nx=%u status=PASS\n",
        stage,(unsigned long long)snapshot.root,(unsigned long long)snapshot.tables,(unsigned long long)snapshot.leaves,
        (unsigned long long)snapshot.bytes,(unsigned long long)snapshot.image_bytes,(unsigned long long)snapshot.direct_bytes,
        (unsigned long long)snapshot.managed_bytes,(unsigned long long)snapshot.scratch_bytes,
        (unsigned long long)snapshot.rwx_bytes,snapshot.nx);
    if (n<=0 || (size_t)n>=sizeof(line)) return false;
    serial_write_all(line);return true;
}

bool paging_owned_remove(uint64_t first, uint64_t physical, uint64_t pages, uint64_t attributes)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_MAPPING) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_ACQ_REL);
    bool ok = mapping_window_remove(&g_owned_window, first, physical, pages, attributes) == MAPPING_OK;
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
    if (ok) ++g_observer_epoch;
#endif
    __atomic_add_fetch(&g_image_mapping_epoch,1,__ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ok;
}

static PageEntry *identity_leaf(uint64_t address)
{
    PageTable *table = g_kernel_pml4;
    for (unsigned level = 4; table && level > 1; --level) {
        uint64_t entry = table->entries[(address >> (12 + (level - 1) * 9)) & 511];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) return NULL;
        table = table_pointer(entry & PAGE_ADDR_MASK);
    }
    return table ? &table->entries[(address >> 12) & 511] : NULL;
}

static bool low_half_empty(void)
{
    if (!g_kernel_pml4 || !g_physical_contract.identity_retired) return false;
    for (unsigned i = 0; i < 256; ++i)
        if (g_kernel_pml4->entries[i]) return false;
    return true;
}

bool paging_low_half_empty(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    bool empty = low_half_empty();
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return empty;
}

#ifdef HOBBYOS_SELFTEST
#include "mapping_probe.h"
bool paging_owned_leaf_pair(uint64_t address, uint64_t physical,
                            uint64_t *managed, uint64_t *identity)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!managed || !identity || address < DEVICE_WINDOW_BASE ||
        address-DEVICE_WINDOW_BASE >= DEVICE_WINDOW_BYTES || (address & 4095) || (physical & 4095)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = mapping_window_entry(&g_owned_window, (address-DEVICE_WINDOW_BASE)/4096, managed);
    if (g_physical_contract.identity_retired) {
        ok = ok && low_half_empty();
        if (ok) *identity = 0;
    } else {
        PageEntry *old = physical < g_physical_contract.identity_limit ? identity_leaf(physical) : NULL;
        if (ok) *identity = old ? *old : 0;
    }
    spin_unlock_irqrestore(&g_paging_lock, flags); return ok;
}
#endif

bool paging_owned_withdraw_identity(uint64_t physical, uint64_t bytes)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!physical_device_extent_allowed(physical, bytes) || (physical & 4095) || (bytes & 4095)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    if (g_physical_contract.identity_retired) {
        bool empty = low_half_empty();
        spin_unlock_irqrestore(&g_paging_lock, flags);
        return empty;
    }
    uint64_t end = physical + bytes;
    if (end > g_physical_contract.identity_limit) end = g_physical_contract.identity_limit;
    bool ok = true;
    for (uint64_t at = physical; at < end; at += PAGE_SIZE) {
        PageEntry *entry = identity_leaf(at);
        if (!entry || (*entry && (*entry & ~(uint64_t)(PAGE_ACCESSED | PAGE_DIRTY | PAGE_PAT)) !=
                                 (at | PAGE_PRESENT | PAGE_RW))) { ok = false; break; }
        if (!*entry && physical_identity_range_present(at, PAGE_SIZE)) { ok = false; break; }
    }
    if (ok) ok = physical_identity_hole(physical, bytes);
    if (ok) {
        for (uint64_t at = physical; at < end; at += PAGE_SIZE)
            __atomic_store_n(identity_leaf(at), 0, __ATOMIC_RELEASE);
#if HOBBYOS_DEBUG_ASSERT || defined(HOBBYOS_SELFTEST)
        ++g_observer_epoch;
#endif
    }
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ok;
}

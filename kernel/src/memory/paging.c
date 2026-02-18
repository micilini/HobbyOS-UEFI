#include "paging.h"
#include "../libc/memory.h"
#include "../cpu/cpu.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"

extern uint64_t _kernel_start;
extern uint64_t _kernel_end;

#define KERNEL_VIRT_OFFSET 0xFFFFFFFF80000000ULL


PageTable *g_kernel_pml4 = NULL;
static spinlock_t g_paging_lock;

#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x)   (((x) >> 21) & 0x1FF)
#define PT_INDEX(x)   (((x) >> 12) & 0x1FF)

static PageTable *alloc_table()
{
    void *paddr = pmm_alloc_frame();
    if (!paddr) return NULL;
    memset(paddr, 0, PAGE_SIZE);
    return (PageTable *)paddr;
}

uint64_t paging_get_physical_address(uint64_t vaddr)
{
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);
    uint64_t ret = 0;

    if (!g_kernel_pml4) goto out;

    uint64_t pml4_idx = PML4_INDEX(vaddr);
    if (!(g_kernel_pml4->entries[pml4_idx] & PAGE_PRESENT)) goto out;

    PageTable *pdpt = (PageTable *)(g_kernel_pml4->entries[pml4_idx] & PAGE_ADDR_MASK);
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    if (!(pdpt->entries[pdpt_idx] & PAGE_PRESENT)) goto out;

    PageTable *pd = (PageTable *)(pdpt->entries[pdpt_idx] & PAGE_ADDR_MASK);
    uint64_t pd_idx = PD_INDEX(vaddr);
    if (!(pd->entries[pd_idx] & PAGE_PRESENT)) goto out;

    PageTable *pt = (PageTable *)(pd->entries[pd_idx] & PAGE_ADDR_MASK);
    uint64_t pt_idx = PT_INDEX(vaddr);
    if (!(pt->entries[pt_idx] & PAGE_PRESENT)) goto out;

    ret = (pt->entries[pt_idx] & PAGE_ADDR_MASK) + (vaddr & 0xFFF);

out:
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ret;
}

void paging_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    irq_flags_t irq_flags = spin_lock_irqsave(&g_paging_lock);

    if (!g_kernel_pml4) {
        spin_unlock_irqrestore(&g_paging_lock, irq_flags);
        return;
    }

    uint64_t pml4_idx = PML4_INDEX(vaddr);
    
    if (!(g_kernel_pml4->entries[pml4_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }
        g_kernel_pml4->entries[pml4_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pdpt = (PageTable *)(g_kernel_pml4->entries[pml4_idx] & PAGE_ADDR_MASK);

    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    if (!(pdpt->entries[pdpt_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }
        pdpt->entries[pdpt_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pd = (PageTable *)(pdpt->entries[pdpt_idx] & PAGE_ADDR_MASK);

    uint64_t pd_idx = PD_INDEX(vaddr);
    if (!(pd->entries[pd_idx] & PAGE_PRESENT))
    {
        PageTable *new_table = alloc_table();
        if (!new_table) { spin_unlock_irqrestore(&g_paging_lock, irq_flags); return; }
        pd->entries[pd_idx] = (uint64_t)new_table | PAGE_PRESENT | PAGE_RW;
    }
    PageTable *pt = (PageTable *)(pd->entries[pd_idx] & PAGE_ADDR_MASK);

    pt->entries[PT_INDEX(vaddr)] = (paddr & PAGE_ADDR_MASK) | flags;

    __asm__ volatile("invlpg (%0)" ::"r"(vaddr) : "memory");

    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
}

void paging_load_map(PageTable *pml4)
{
    __asm__ volatile("mov %0, %%cr3" ::"r"(pml4) : "memory");
}


PageTable* paging_create_bootstrap_table()
{
    serial_write_all("[PAGING] Creating Robust Bootstrap Page Table...\n");
    irq_flags_t flags = spin_lock_irqsave(&g_paging_lock);

    PageTable *boot_pml4 = alloc_table();
    if (!boot_pml4) {
        serial_write_all("[PAGING] FAIL: No mem for Bootstrap PML4\n");
        spin_unlock_irqrestore(&g_paging_lock, flags);
        return NULL;
    }

    
    
    for (int i = 256; i < 512; i++) {
        boot_pml4->entries[i] = g_kernel_pml4->entries[i];
    }

    
    
    
    
    boot_pml4->entries[0] = g_kernel_pml4->entries[0];

    serial_write_all("[PAGING] Bootstrap Table: Cloned Identity Map (Entry 0) & Higher Half.\n");
    
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return boot_pml4;
}

void init_paging(uint64_t fb_base, uint64_t fb_size)
{
    spinlock_init(&g_paging_lock);

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    uint64_t cr0_nc = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0_nc) : "memory");
    __asm__ volatile("wbinvd");

    uint64_t pat = 0x0007040600070406ULL; 
    cpu_write_msr(0x277, pat);

    __asm__ volatile("wbinvd");
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

    g_kernel_pml4 = alloc_table();

    uint64_t safe_limit = 0x100000000ULL; 
    uint64_t total_mem = pmm_get_total_memory(); 
    if (total_mem < safe_limit) safe_limit = total_mem;

    
    
    for (uint64_t addr = 0; addr < safe_limit; addr += PAGE_SIZE)
    {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    uint64_t fb_end = (fb_base + fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t addr = fb_base; addr < fb_end; addr += PAGE_SIZE)
    {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PAT);
    }

    uint64_t virt_start = (uint64_t)&_kernel_start;
    uint64_t virt_end   = (uint64_t)&_kernel_end;

    virt_start &= ~(PAGE_SIZE - 1);
    virt_end = (virt_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t vaddr = virt_start; vaddr < virt_end; vaddr += PAGE_SIZE)
    {
        uint64_t paddr = vaddr - KERNEL_VIRT_OFFSET;
        paging_map(vaddr, paddr, PAGE_PRESENT | PAGE_RW);
    }

    paging_load_map(g_kernel_pml4);
}
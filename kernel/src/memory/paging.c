#include "paging.h"
#include "../libc/memory.h"


static PageTable* g_kernel_pml4 = NULL;



#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x)   (((x) >> 21) & 0x1FF)
#define PT_INDEX(x)   (((x) >> 12) & 0x1FF)


static PageTable* alloc_table() {
    void* paddr = pmm_alloc_frame();
    if (!paddr) return NULL;
    
    
    memset(paddr, 0, PAGE_SIZE);
    return (PageTable*)paddr;
}

void paging_map(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (!g_kernel_pml4) return;

    
    uint64_t pml4_idx = PML4_INDEX(vaddr);
    if (!(g_kernel_pml4->entries[pml4_idx] & PAGE_PRESENT)) {
        PageTable* new_pdpt = alloc_table();
        if (!new_pdpt) return; 
        
        
        g_kernel_pml4->entries[pml4_idx] = (uint64_t)new_pdpt | PAGE_PRESENT | PAGE_RW;
    }
    
    PageTable* pdpt = (PageTable*)(g_kernel_pml4->entries[pml4_idx] & PAGE_ADDR_MASK);

    
    uint64_t pdpt_idx = PDPT_INDEX(vaddr);
    if (!(pdpt->entries[pdpt_idx] & PAGE_PRESENT)) {
        PageTable* new_pd = alloc_table();
        if (!new_pd) return;
        pdpt->entries[pdpt_idx] = (uint64_t)new_pd | PAGE_PRESENT | PAGE_RW;
    }
    PageTable* pd = (PageTable*)(pdpt->entries[pdpt_idx] & PAGE_ADDR_MASK);

    
    uint64_t pd_idx = PD_INDEX(vaddr);
    if (!(pd->entries[pd_idx] & PAGE_PRESENT)) {
        PageTable* new_pt = alloc_table();
        if (!new_pt) return;
        pd->entries[pd_idx] = (uint64_t)new_pt | PAGE_PRESENT | PAGE_RW;
    }
    PageTable* pt = (PageTable*)(pd->entries[pd_idx] & PAGE_ADDR_MASK);

    
    
    uint64_t pt_idx = PT_INDEX(vaddr);
    pt->entries[pt_idx] = (paddr & PAGE_ADDR_MASK) | flags;

    
    __asm__ volatile("invlpg (%0)" :: "r" (vaddr) : "memory");
}

void paging_load_map(PageTable* pml4) {
    
    __asm__ volatile("mov %0, %%cr3" :: "r" (pml4) : "memory");
}

void init_paging(uint64_t fb_base, uint64_t fb_size) {
    
    g_kernel_pml4 = alloc_table();
    
    
    
    
    uint64_t total_mem = pmm_get_total_memory();
    
    for (uint64_t addr = 0; addr < total_mem; addr += PAGE_SIZE) {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    
    
    
    
    uint64_t fb_end = fb_base + fb_size;
    
    
    for (uint64_t addr = fb_base; addr < fb_end; addr += PAGE_SIZE) {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    
    
    paging_load_map(g_kernel_pml4);
}
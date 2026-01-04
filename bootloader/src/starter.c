#include "starter.h"
#include "mem.h"


typedef void (*KernelStartFunc)(BootInfo*);

void start_kernel(EFI_HANDLE ImageHandle, void* entry_point, Framebuffer* fb, Psf1_Font* font, void* logo, void* rsdp, MemoryMap* mem_map) {
    EFI_STATUS status;
    
    
    
    
    BootInfo* boot_info;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(BootInfo), (void**)&boot_info);
    
    
    boot_info->framebuffer = fb;
    boot_info->font = font; 
    boot_info->logo = (SimpleImage*)logo; 
    boot_info->rsdp = rsdp;
    boot_info->memory_map = mem_map; 

    
    
    
    
    UINTN map_key;
    UINTN descriptor_size;
    UINT32 descriptor_version;
    
    
    int retry = 0;
    while (retry < 10) {
        
        
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, 
            &mem_map->Size, 
            mem_map->Base, 
            &mem_map->MapKey, 
            &mem_map->DescriptorSize, 
            &descriptor_version
        );
        
        
        

        if (!EFI_ERROR(status)) {
            
            status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mem_map->MapKey);
            
            if (!EFI_ERROR(status)) {
                
                
                
                KernelStartFunc kernel = (KernelStartFunc)entry_point;
                kernel(boot_info);
                
                
                while(1); 
            }
        }
        
        
        retry++;
    }

    Print(L"[-] Critical: Failed to Exit Boot Services after retries.\n");
    while(1);
}
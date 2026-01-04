#ifndef MEM_MAP_H
#define MEM_MAP_H

#include <stdint.h>


typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} __attribute__((packed)) EfiMemoryDescriptor;


typedef struct {
    EfiMemoryDescriptor* Base; 
    uint64_t Size;             
    uint64_t DescriptorSize;   
    uint64_t MapKey;
} MemoryMap;


#define EFI_RESERVED_MEMORY_TYPE 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7  
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11


static inline uint64_t mmap_get_entry_count(MemoryMap* map) {
    if (map->DescriptorSize == 0) return 0;
    return map->Size / map->DescriptorSize;
}


static inline EfiMemoryDescriptor* mmap_get_descriptor(MemoryMap* map, uint64_t i) {
    return (EfiMemoryDescriptor*)((uint8_t*)map->Base + (i * map->DescriptorSize));
}

#endif
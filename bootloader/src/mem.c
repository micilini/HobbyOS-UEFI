#include "mem.h"

MemoryMap *get_memory_map()
{
    EFI_STATUS status;
    MemoryMap *mmap_info;
    UINT32 descriptor_version = 0;

    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(MemoryMap), (void **)&mmap_info);
    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Failed to allocate MemoryMap struct\n");
        return NULL;
    }

    mmap_info->Base = NULL;
    mmap_info->Size = 0;
    mmap_info->MapKey = 0;
    mmap_info->DescriptorSize = 0;

    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               (UINTN *)&mmap_info->Size,
                               (EFI_MEMORY_DESCRIPTOR *)mmap_info->Base,
                               (UINTN *)&mmap_info->MapKey,
                               (UINTN *)&mmap_info->DescriptorSize,
                               &descriptor_version);

    if (status != EFI_BUFFER_TOO_SMALL)
    {
        Print(L"[-] Error: First GetMemoryMap failed (Status: %r)\n", status);
        return NULL;
    }

    mmap_info->Size += (2 * mmap_info->DescriptorSize);

    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, mmap_info->Size, (void **)&mmap_info->Base);
    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Failed to allocate buffer for Memory Map\n");
        return NULL;
    }

    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               (UINTN *)&mmap_info->Size,
                               (EFI_MEMORY_DESCRIPTOR *)mmap_info->Base,
                               (UINTN *)&mmap_info->MapKey,
                               (UINTN *)&mmap_info->DescriptorSize,
                               &descriptor_version);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Final GetMemoryMap failed (Status: %r)\n", status);
        uefi_call_wrapper(BS->FreePool, 1, mmap_info->Base);
        return NULL;
    }

    return mmap_info;
}
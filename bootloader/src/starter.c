#include "starter.h"
#include "mem.h"

typedef void (*KernelStartFunc)(BootInfo *);

void start_kernel(EFI_HANDLE ImageHandle, void *entry_point, BootInfo *prepared_boot_info)
{
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
    if (EFI_ERROR(status))
    {
        Print(L"[!] Warning: Failed to disable UEFI Watchdog (SetWatchdogTimer). Status=%r\n", status);
    }
    else
    {
        Print(L"[*] UEFI Watchdog disabled.\n");
    }

    if (prepared_boot_info == NULL)
    {
        Print(L"[-] Critical: prepared_boot_info is NULL.\n");
        while (1) { }
    }

    
    BootInfo *boot_info = NULL;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(BootInfo), (void **)&boot_info);
    if (EFI_ERROR(status) || boot_info == NULL)
    {
        Print(L"[-] Critical: AllocatePool(BootInfo) failed. Status=%r\n", status);
        while (1) { }
    }

    
    ZeroMem(boot_info, sizeof(BootInfo));
    CopyMem(boot_info, prepared_boot_info, sizeof(BootInfo));

    
    if (boot_info->serial.count > HOBBYOS_MAX_SERIAL_PORTS)
    {
        boot_info->serial.count = 0;
    }

    MemoryMap *mem_map = boot_info->memory_map;
    if (mem_map == NULL)
    {
        Print(L"[-] Critical: boot_info->memory_map is NULL.\n");
        while (1) { }
    }

    int retry = 0;
    while (retry < 10)
    {
        UINT32 descriptor_version;

        status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                                   &mem_map->Size,
                                   mem_map->Base,
                                   &mem_map->MapKey,
                                   &mem_map->DescriptorSize,
                                   &descriptor_version);

        if (!EFI_ERROR(status))
        {
            status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, mem_map->MapKey);

            if (!EFI_ERROR(status))
            {
                KernelStartFunc kernel = (KernelStartFunc)entry_point;
                kernel(boot_info);

                while (1) { }
            }
        }

        retry++;
    }

    Print(L"[-] Critical: Failed to Exit Boot Services after retries.\n");
    while (1) { }
}

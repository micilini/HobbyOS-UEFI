#include "commands.h"
#include "utils.h"
#include "filesystem.h"

void print_header(EFI_SYSTEM_TABLE *SystemTable) {
    SetColor(SystemTable, COLOR_CYAN);
    Print(L"\n  HOBBYOS V0.1 UEFI - (X64 Bare Metal Version)\n");
    Print(L"  (c) Portal Micilini - All Rights Reserved.\n\n");
    SetColor(SystemTable, COLOR_RESET);
}

void cmd_ver(EFI_SYSTEM_TABLE *SystemTable) {
    SetColor(SystemTable, COLOR_CYAN);
    Print(L"\n  HobbyOS v0.1 UEFI Edition - (X64 Bare Metal Version)\n");
    Print(L"  Development by: Micilini\n");
    Print(L"  Architecture: x64 Bare Metal\n\n");
    SetColor(SystemTable, COLOR_RESET);
}

void cmd_shutdown(EFI_SYSTEM_TABLE *SystemTable) {
    SetColor(SystemTable, COLOR_RED);
    Print(L"\n  Shutdown the system...\n");
    
    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 1000000); 

    uefi_call_wrapper((void*)SystemTable->RuntimeServices->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}

void cmd_restart(EFI_SYSTEM_TABLE *SystemTable) {
    SetColor(SystemTable, COLOR_YELLOW);
    Print(L"\n  Restarting...\n");
    
    uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 1000000);

    uefi_call_wrapper((void*)SystemTable->RuntimeServices->ResetSystem, 4, EfiResetWarm, EFI_SUCCESS, 0, NULL);
}

void cmd_date(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_TIME Time;
    EFI_STATUS Status;

    Status = uefi_call_wrapper(SystemTable->RuntimeServices->GetTime, 2, &Time, NULL);

    if (EFI_ERROR(Status)) {
        Print(L"Error when reading clock (RTC).\n");
        return;
    }

    SetColor(SystemTable, COLOR_CYAN);

    Print(L"  Date: %02d/%02d/%04d  Hour: %02d:%02d:%02d\n", 
        Time.Day, Time.Month, Time.Year, 
        Time.Hour, Time.Minute, Time.Second);
    SetColor(SystemTable, COLOR_WHITE);
}

void cmd_mem(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;

    Status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5, 
        &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    if (Status != EFI_BUFFER_TOO_SMALL) {
        Print(L"Error querying RAM..\n");
        return;
    }

    MemoryMapSize += 2 * DescriptorSize;

    Status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, 
        EfiLoaderData, MemoryMapSize, (void**)&MemoryMap);
    
    if (EFI_ERROR(Status)) {
        Print(L"Allocation error for memory map.\n");
        return;
    }

    Status = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5, 
        &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    if (EFI_ERROR(Status)) {
        Print(L"Error retrieving map.\n");
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, MemoryMap);
        return;
    }

    UINT64 TotalRam = 0;
    UINT64 FreeRam = 0;

    UINT8 *Ptr = (UINT8*)MemoryMap;
    UINT8 *End = Ptr + MemoryMapSize;

    while (Ptr < End) {
        EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR*)Ptr;
        
        UINT64 SizeInBytes = Desc->NumberOfPages * 4096;
        TotalRam += SizeInBytes;

        if (Desc->Type == EfiConventionalMemory) {
            FreeRam += SizeInBytes;
        }

        Ptr += DescriptorSize;
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, MemoryMap);

    SetColor(SystemTable, COLOR_YELLOW);
    Print(L"  Total RAM: %ld MB\n", TotalRam / (1024 * 1024));
    SetColor(SystemTable, COLOR_GREEN);
    Print(L"  Free RAM: %ld MB\n", FreeRam / (1024 * 1024));
    SetColor(SystemTable, COLOR_WHITE);
}

void cmd_hexdump(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName) {
    if (FileName[0] == 0) {
        Print(L"Usage: hexdump <file>\n");
        return;
    }

    EFI_FILE_PROTOCOL *Root = fs_get_root();
    if (!Root) {
        Print(L"Error: File system not initialized.\n");
        return;
    }

    CHAR16 FullPath[256];
    ResolvePath(FileName, FullPath);

    EFI_FILE_PROTOCOL *File;
    EFI_STATUS Status = uefi_call_wrapper(Root->Open, 5, Root, &File, FullPath, EFI_FILE_MODE_READ, 0);

    if (EFI_ERROR(Status)) {
        Print(L"File not found: %s\n", FullPath);
        return;
    }

    UINT8 Buffer[16];
    UINTN ReadSize = sizeof(Buffer);
    UINTN Offset = 0;

    Print(L"\nOFFSET    00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  ASCII\n");
    Print(L"--------  -----------------------------------------------  ----------------\n");

    BOOLEAN Stop = FALSE;
    while (!Stop) {
        ReadSize = sizeof(Buffer);
        Status = uefi_call_wrapper(File->Read, 3, File, &ReadSize, Buffer);

        if (EFI_ERROR(Status) || ReadSize == 0) break;

        SetColor(SystemTable, COLOR_CYAN);
        Print(L"%08x  ", Offset);
        SetColor(SystemTable, COLOR_WHITE);

        for (UINTN i = 0; i < 16; i++) {
            if (i < ReadSize) {
                if (Buffer[i] < 0x10) Print(L"0");
                Print(L"%x ", Buffer[i]);
            } else {
                Print(L"   ");
            }
        }
        
        Print(L" ");

        SetColor(SystemTable, COLOR_YELLOW);
        for (UINTN i = 0; i < ReadSize; i++) {
            CHAR16 c = (CHAR16)Buffer[i];
            if (c >= 32 && c <= 126) {
                CHAR16 Str[2] = {c, 0};
                Print(Str);
            } else {
                Print(L".");
            }
        }
        SetColor(SystemTable, COLOR_WHITE);
        Print(L"\n");

        Offset += ReadSize;

       if (Offset > 2048) { 
           Print(L"\n... (Very large file, showing only the first 2KB.) ...\n");
           Stop = TRUE; 
       }
    }
    Print(L"\n");
    uefi_call_wrapper(File->Close, 1, File);
}